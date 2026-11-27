#include "StereoCalibrationSupport.hpp"
#include "BlurWaveShader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

using Json = nlohmann::json;

constexpr const char* kExpectedFormat = "vdca.stereo_rectification";
constexpr int kExpectedVersion = 1;

CalibrationImageSize ParseImageSize(const Json& value, const char* label)
{
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(label) + " must be an object");
    }
    CalibrationImageSize result;
    result.width = value.at("width").get<std::uint32_t>();
    result.height = value.at("height").get<std::uint32_t>();
    if (!result.valid()) {
        throw std::invalid_argument(std::string(label) + " must be non-zero");
    }
    return result;
}

CalibrationHomography ParseHomography(const Json& value, const char* label)
{
    if (!value.is_object() || !value.contains("rows") ||
        !value.at("rows").is_array() || value.at("rows").size() != 3) {
        throw std::invalid_argument(
            std::string(label) + " must contain a 3x3 rows array");
    }

    CalibrationHomography result;
    const auto& rows = value.at("rows");
    for (std::size_t row = 0; row < 3; ++row) {
        if (!rows.at(row).is_array() || rows.at(row).size() != 3) {
            throw std::invalid_argument(
                std::string(label) + " must contain a 3x3 rows array");
        }
        for (std::size_t column = 0; column < 3; ++column) {
            const double element = rows.at(row).at(column).get<double>();
            if (!std::isfinite(element)) {
                throw std::invalid_argument(
                    std::string(label) + " contains a non-finite value");
            }
            result.rows[row * 3 + column] = element;
        }
    }
    return result;
}

CalibrationProfile ParseProfile(const Json& value, const std::string& profileName)
{
    if (!value.is_object()) {
        throw std::invalid_argument(
            "calibration profile is not an object: " + profileName);
    }

    CalibrationProfile result;
    result.method = value.value("method", profileName);
    const auto& eyes = value.at("eyes");
    result.leftInverse = ParseHomography(
        eyes.at("left").at("inverse_pixel_homography"),
        "left inverse_pixel_homography");
    result.rightInverse = ParseHomography(
        eyes.at("right").at("inverse_pixel_homography"),
        "right inverse_pixel_homography");
    return result;
}

void ValidateDocument(const StereoCalibrationDocument& document)
{
    if (document.format != kExpectedFormat) {
        throw std::invalid_argument("unsupported calibration format: " + document.format);
    }
    if (document.version != kExpectedVersion) {
        throw std::invalid_argument(
            "unsupported calibration version: " + std::to_string(document.version));
    }
    if (!document.sourceSize.valid() || !document.calibrationInputSize.valid() ||
        !document.rectifiedOutputSize.valid()) {
        throw std::invalid_argument("calibration image geometry is invalid");
    }
    if (document.resizeMode != "none") {
        throw std::invalid_argument("this stage supports only preprocess.resize.mode=none");
    }
    if (document.rightOrder != "same") {
        throw std::invalid_argument("this stage supports only preprocess.right_order=same");
    }
    if (document.samplingFilter != "linear") {
        throw std::invalid_argument("this stage supports only sampling.filter=linear");
    }
    if (document.borderMode != "constant") {
        throw std::invalid_argument("this stage supports only sampling.border_mode=constant");
    }
    for (float value : document.borderRgba) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("sampling.border_rgba contains a non-finite value");
        }
    }
    if (document.defaultProfile.empty() ||
        !document.hasProfile(document.defaultProfile)) {
        throw std::invalid_argument("calibration default_profile does not exist in profiles");
    }
}

// Matches the trailing fields of the rectification shader cbuffer.
struct alignas(16) BlurWaveConstants {
    std::array<float, 4> roiPixels{}; // center X/Y, ellipse radii X/Y
    std::array<float, 4> rings{};     // three radii in pixels, Gaussian kernel radius
    std::array<float, 4> blend{};     // overall strength, remaining components reserved
    std::array<float, 40> weights{};  // normalized half-kernel, packed as float4[10]
};

struct alignas(16) RectificationConstants {
    std::array<float, 4> inverseRow0{};
    std::array<float, 4> inverseRow1{};
    std::array<float, 4> inverseRow2{};
    std::array<float, 4> borderRgba{};
};

static_assert(sizeof(RectificationConstants) == 64,
              "Unexpected rectification constant layout");

float CheckedFloat(double value)
{
    const float result = static_cast<float>(value);
    if (!std::isfinite(result)) {
        throw std::invalid_argument("calibration matrix contains a non-finite value");
    }
    return result;
}

float FiniteOr(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

float ModeToFloat(StereoPostProcessMode mode) noexcept
{
    switch (mode) {
    case StereoPostProcessMode::Darken: return 1.0f;
    case StereoPostProcessMode::Blur: return 2.0f;
    case StereoPostProcessMode::BlurWave: return 3.0f;
    case StereoPostProcessMode::OutsideBlur: return 4.0f;
    case StereoPostProcessMode::None:
    default:
        return 0.0f;
    }
}

StereoPostProcessSettings SanitizePostProcessSettings(
    StereoPostProcessSettings settings) noexcept
{
    if (!settings.enabled) {
        settings.mode = StereoPostProcessMode::None;
    }
    settings.centerX01 = std::clamp(FiniteOr(settings.centerX01, 0.5f), 0.0f, 1.0f);
    settings.centerY01 = std::clamp(FiniteOr(settings.centerY01, 0.5f), 0.0f, 1.0f);
    settings.radiusShortAxis01 = std::clamp(
        FiniteOr(settings.radiusShortAxis01, 0.2f), 0.0f, 4.0f);
    settings.radiusXShortAxis01 = std::clamp(
        FiniteOr(settings.radiusXShortAxis01, 0.2f), 0.0f, 4.0f);
    settings.radiusYShortAxis01 = std::clamp(
        FiniteOr(settings.radiusYShortAxis01, 0.15f), 0.0f, 4.0f);
    settings.edgeSoftnessShortAxis01 = std::clamp(
        FiniteOr(settings.edgeSoftnessShortAxis01, 0.03f), 0.0f, 4.0f);
    settings.outsideBrightness = std::clamp(
        FiniteOr(settings.outsideBrightness, 0.25f), 0.0f, 1.0f);
    settings.blurRadiusPixels = std::clamp(
        FiniteOr(settings.blurRadiusPixels, 6.0f), 1.0f, 128.0f);
    settings.blurSigmaPixels = std::clamp(
        FiniteOr(settings.blurSigmaPixels, 3.0f), 0.01f, 128.0f);
    settings.blurStrength01 = std::clamp(
        FiniteOr(settings.blurStrength01, 1.0f), 0.0f, 1.0f);

    if (settings.radiusShortAxis01 >
        std::max(settings.radiusXShortAxis01, settings.radiusYShortAxis01)) {
        settings.radiusXShortAxis01 = settings.radiusShortAxis01;
        settings.radiusYShortAxis01 = settings.radiusShortAxis01;
    }
    return settings;
}

RectificationConstants MakeConstants(
    const CalibrationHomography& inverse,
    const std::array<float, 4>& borderRgba)
{
    RectificationConstants result;
    for (std::size_t column = 0; column < 3; ++column) {
        result.inverseRow0[column] = CheckedFloat(inverse.rows[column]);
        result.inverseRow1[column] = CheckedFloat(inverse.rows[3 + column]);
        result.inverseRow2[column] = CheckedFloat(inverse.rows[6 + column]);
    }
    result.borderRgba = borderRgba;
    return result;
}

const std::string& RectificationHlsl()
{
    static const std::string shader = std::string(R"hlsl(
Texture2D<float4> xrInput : register(t0);
RWTexture2D<float4> xrOutput : register(u0);

cbuffer RectificationConstants : register(b0)
{
    float4 inverseRow0;
    float4 inverseRow1;
    float4 inverseRow2;
    float4 borderRgba;
    float4 roiPixels;
    float4 rings;
    float4 blend;
    float4 waveWeights[10];
};

cbuffer XRTextureProcessingFrameConstants : register(b1)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float4 frameParams;
};

float4 LoadWithConstantBorder(int2 pixel)
{
    if (pixel.x < 0 || pixel.y < 0 ||
        pixel.x >= (int)srcWidth || pixel.y >= (int)srcHeight) {
        return borderRgba;
    }
    return xrInput.Load(int3(pixel, 0));
}

float4 SampleLinear(float2 sourcePixel)
{
    const float2 baseFloat = floor(sourcePixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = sourcePixel - baseFloat;
    const float4 c00 = LoadWithConstantBorder(basePixel);
    const float4 c10 = LoadWithConstantBorder(basePixel + int2(1, 0));
    const float4 c01 = LoadWithConstantBorder(basePixel + int2(0, 1));
    const float4 c11 = LoadWithConstantBorder(basePixel + int2(1, 1));
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

float4 SampleRemappedOutputPixel(float2 destinationPixel)
{
    const float3 destination = float3(destinationPixel, 1.0f);
    const float3 sourceH = float3(
        dot(inverseRow0.xyz, destination),
        dot(inverseRow1.xyz, destination),
        dot(inverseRow2.xyz, destination));

    if (!isfinite(sourceH.z) || abs(sourceH.z) < 1.0e-8f) {
        return borderRgba;
    }

    const float2 sourcePixel = sourceH.xy / sourceH.z;
    if (!all(isfinite(sourcePixel)) ||
        sourcePixel.x <= -1.0f || sourcePixel.y <= -1.0f ||
        sourcePixel.x >= (float)srcWidth || sourcePixel.y >= (float)srcHeight) {
        return borderRgba;
    }

    return SampleLinear(sourcePixel);
}

)hlsl") + kBlurWaveComputeHlsl;
    return shader;
}

const char* PlanePostProcessPixelShaderHlsl() noexcept
{
    return R"hlsl(
float OutsideMaskForUv(float2 uv, float2 textureSize, bool protectInterior)
{
    const float shortAxis = max(1.0f, min(textureSize.x, textureSize.y));
    const float2 center01 = params0.xy;
    const float2 radii = max(params0.zw, float2(1.0e-5f, 1.0e-5f));
    const float2 deltaShortAxis = (uv - center01) * textureSize / shortAxis;
    const float ellipseDistance = length(deltaShortAxis / radii);
    const float maxRadius = max(max(radii.x, radii.y), 1.0e-5f);
    const float softness = max(1.0e-5f, params1.y / maxRadius);
    return smoothstep(protectInterior ? 1.0f : 1.0f - softness, 1.0f + softness, ellipseDistance);
}

float4 Blur9Rectified(float2 uv, float2 textureSize)
{
    const float r = max(params1.z, 1.0f);
    const float2 texel = 1.0f / max(textureSize, float2(1.0f, 1.0f));
    const float2 d = texel * r;

    float4 sum = 0.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2(-1.0f, -1.0f))) * 1.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2( 0.0f, -1.0f))) * 2.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2( 1.0f, -1.0f))) * 1.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2(-1.0f,  0.0f))) * 2.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv)) * 4.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2( 1.0f,  0.0f))) * 2.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2(-1.0f,  1.0f))) * 1.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2( 0.0f,  1.0f))) * 2.0f;
    sum += xrTexture.Sample(xrSampler, saturate(uv + d * float2( 1.0f,  1.0f))) * 1.0f;
    return sum / 16.0f;
}

float4 main(float2 uv : TEXCOORD0) : SV_TARGET
{
    uint width = 1;
    uint height = 1;
    xrTexture.GetDimensions(width, height);
    const float2 textureSize = max(float2((float)width, (float)height), float2(1.0f, 1.0f));

    float4 color = xrTexture.Sample(xrSampler, uv);
    const float encodedMode = params1.w;
    const int mode = (int)floor(encodedMode + 1.0e-4f);
    if (mode <= 0 || mode == 3) { // BlurWave is composited in rectified pixel coordinates.
        return color * tint;
    }

    const float outsideMask = OutsideMaskForUv(uv, textureSize, mode == 4);
    if (mode == 1) {
        const float outsideBrightness = saturate(params1.x);
        color.rgb *= lerp(1.0f, outsideBrightness, outsideMask);
    } else if (mode == 2 || mode == 4) {
        const float blurStrength = saturate((encodedMode - floor(encodedMode)) * 10.0f);
        const float blurAmount = outsideMask * blurStrength;
        if (blurAmount > 1.0e-5f) {
            const float4 blurred = Blur9Rectified(uv, textureSize);
            color = lerp(color, blurred, blurAmount);
        }
    }

    return color * tint;
}
)hlsl";
}

VarjoXR::TextureProcessingDesc MakeProcessing(
    const CalibrationHomography& inverse,
    const std::array<float, 4>& borderRgba,
    CalibrationImageSize outputSize)
{
    VarjoXR::TextureProcessingDesc result{};
    result.enabled = true;
    result.timing = VarjoXR::ProcessingTiming::BeforeRenderEachFrame;
    result.hlsl = RectificationHlsl();
    result.entryPoint = "main";
    result.target = "cs_5_0";
    result.sourceName = "DualIC4Varjo_StereoRectificationPass.hlsl";
    result.outputSize = {outputSize.width, outputSize.height};
    result.userConstants.registerIndex = 0;
    struct alignas(16) ProcessingConstants {
        RectificationConstants rectification;
        BlurWaveConstants wave;
    };
    static_assert(offsetof(ProcessingConstants, wave) == 64 && sizeof(ProcessingConstants) == 272);
    result.userConstants.set(ProcessingConstants{MakeConstants(inverse, borderRgba), {}});
    result.frameConstants.enabled = true;
    result.frameConstants.registerIndex = 1;
    return result;
}

void ApplyMaterialPostProcessParams(
    VarjoXR::XRPlane& plane,
    VarjoXR::Eye eye,
    StereoPostProcessSettings settings)
{
    settings.mode = settings.modeForEye(eye);
    settings = SanitizePostProcessSettings(settings);
    const float mode = settings.enabled ? ModeToFloat(settings.mode) : 0.0f;
    const float encodedModeAndBlurStrength =
        mode > 0.0f ? mode + std::clamp(settings.blurStrength01, 0.0f, 1.0f) * 0.1f : 0.0f;

    auto& material = plane.material(eye);
    if (settings.mode == StereoPostProcessMode::OutsideBlur && material.planePixelShaderHlsl.empty()) {
        // Raw camera display has no calibration pass to install the Plane shader.
        plane.setPixelShaderHLSL(eye, PlanePostProcessPixelShaderHlsl());
    }
    material.params0 = {
        settings.centerX01,
        settings.centerY01,
        settings.radiusXShortAxis01,
        settings.radiusYShortAxis01,
    };
    material.params1 = {
        settings.outsideBrightness,
        settings.edgeSoftnessShortAxis01,
        settings.blurRadiusPixels,
        encodedModeAndBlurStrength,
    };
}

bool UsesBlurWave(const VarjoXR::XRMaterial& material) noexcept
{
    return static_cast<int>(material.params1.w) == static_cast<int>(StereoPostProcessMode::BlurWave);
}

BlurWaveConstants MakeBlurWaveConstants(
    const VarjoXR::XRMaterial& material, const std::optional<glm::vec2>& gazeUv,
    double elapsedSeconds, double& phase)
{
    BlurWaveConstants wave{};
    const glm::vec2 size(material.processing.outputSize);
    const glm::vec2 last = size - 1.f;
    const glm::vec2 center = glm::vec2(material.params0) * last;
    const glm::vec2 radii = glm::max(glm::vec2(1.f),
        glm::vec2(material.params0.z, material.params0.w) * std::min(size.x, size.y));
    wave.roiPixels = {center.x, center.y, radii.x, radii.y};
    wave.blend[0] = std::clamp((material.params1.w - ModeToFloat(StereoPostProcessMode::BlurWave)) *
                               10.f, 0.f, 1.f);

    const glm::vec2 delta = gazeUv ? *gazeUv * last - center : glm::vec2(0.f);
    const double ellipseDistance = std::hypot(delta.x / radii.x, delta.y / radii.y);
    if (!(ellipseDistance > 1.0) || wave.blend[0] <= 0.f) {
        phase = 0.0;
        return wave;
    }

    // The reference Viewer uses a 1 m wide virtual image viewed from 1 m.
    const glm::vec2 farthest = glm::max(center, last - center);
    const double thetaMax = std::atan2(std::hypot(farthest.x, farthest.y), size.x);
    if (thetaMax <= 0.0) return wave;
    const double distance = std::hypot(delta.x, delta.y);
    const double thetaRoi = std::atan2(distance / ellipseDistance, size.x);
    const double u = thetaMax > thetaRoi
        ? std::clamp((std::atan2(distance, size.x) - thetaRoi) / (thetaMax - thetaRoi), 0.0, 1.0)
        : 0.0;
    constexpr double radiansPerDegree = 0.017453292519943295;
    const double spacing = thetaMax / 3.0;
    const double speed = (5.0 + 25.0 * u) * radiansPerDegree;
    phase = std::fmod(phase + speed * std::max(0.0, elapsedSeconds), spacing);
    for (int i = 0; i < 3; ++i) {
        wave.rings[i] = static_cast<float>(size.x * std::tan((i + 1) * spacing - phase));
    }

    const double sigma = 5.0 + 8.0 * u;
    const int radius = static_cast<int>(std::ceil(3.0 * sigma));
    wave.rings[3] = static_cast<float>(radius);
    double sum = 1.0;
    wave.weights[0] = 1.f;
    for (int i = 1; i <= radius; ++i) {
        const double weight = std::exp(-0.5 * i * i / (sigma * sigma));
        sum += 2.0 * weight;
        wave.weights[i] = static_cast<float>(weight);
    }
    for (int i = 0; i <= radius; ++i) wave.weights[i] /= static_cast<float>(sum);
    return wave;
}

} // namespace

bool StereoCalibrationDocument::hasProfile(const std::string& name) const noexcept
{
    return profiles.find(name) != profiles.end();
}

const CalibrationProfile& StereoCalibrationDocument::profile(const std::string& name) const
{
    const auto it = profiles.find(name);
    if (it == profiles.end()) {
        throw std::out_of_range("calibration profile not found: " + name);
    }
    return it->second;
}

StereoCalibrationDocument LoadStereoCalibrationJson(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("could not open calibration JSON: " + path.string());
    }

    Json root;
    try {
        stream >> root;
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "failed to parse calibration JSON " + path.string() + ": " + exception.what());
    }

    try {
        StereoCalibrationDocument result;
        result.format = root.at("format").get<std::string>();
        result.version = root.at("version").get<int>();
        result.defaultProfile = root.at("default_profile").get<std::string>();

        const auto& geometry = root.at("image_geometry");
        result.sourceSize = ParseImageSize(geometry.at("source_size"), "image_geometry.source_size");
        result.calibrationInputSize = ParseImageSize(geometry.at("calibration_input_size"), "image_geometry.calibration_input_size");
        result.rectifiedOutputSize = ParseImageSize(geometry.at("rectified_output_size"), "image_geometry.rectified_output_size");

        const auto& preprocess = root.at("preprocess");
        result.resizeMode = preprocess.at("resize").at("mode").get<std::string>();
        result.rightOrder = preprocess.at("right_order").get<std::string>();

        const auto& sampling = root.at("sampling");
        result.samplingFilter = sampling.at("filter").get<std::string>();
        result.borderMode = sampling.at("border_mode").get<std::string>();
        const auto& border = sampling.at("border_rgba");
        if (!border.is_array() || border.size() != 4) {
            throw std::invalid_argument("sampling.border_rgba must contain four values");
        }
        for (std::size_t index = 0; index < 4; ++index) {
            result.borderRgba[index] = border.at(index).get<float>();
        }

        const auto& profiles = root.at("profiles");
        if (!profiles.is_object() || profiles.empty()) {
            throw std::invalid_argument("profiles must be a non-empty object");
        }
        for (const auto& item : profiles.items()) {
            result.profiles.emplace(item.key(), ParseProfile(item.value(), item.key()));
        }

        ValidateDocument(result);
        return result;
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "invalid calibration JSON " + path.string() + ": " + exception.what());
    }
}

void ValidateCalibrationInputGeometry(
    const StereoCalibrationDocument& document,
    std::uint32_t inputWidth,
    std::uint32_t inputHeight)
{
    ValidateDocument(document);
    if (document.sourceSize.width != inputWidth ||
        document.sourceSize.height != inputHeight ||
        document.calibrationInputSize.width != inputWidth ||
        document.calibrationInputSize.height != inputHeight) {
        std::ostringstream message;
        message << "calibration JSON source/input size "
                << document.sourceSize.width << 'x' << document.sourceSize.height
                << " does not match IC4 output texture size "
                << inputWidth << 'x' << inputHeight;
        throw std::invalid_argument(message.str());
    }
}

void ApplyCalibrationToPlane(
    VarjoXR::XRPlane& plane,
    const StereoCalibrationDocument& document,
    const std::string& profileName,
    const StereoPostProcessSettings& postProcessSettings)
{
    ValidateDocument(document);
    const CalibrationProfile& selected = document.profile(profileName);
    plane.setProcessing(
        VarjoXR::Eye::Left,
        MakeProcessing(
            selected.leftInverse,
            document.borderRgba,
            document.rectifiedOutputSize));
    plane.setProcessing(
        VarjoXR::Eye::Right,
        MakeProcessing(
            selected.rightInverse,
            document.borderRgba,
            document.rectifiedOutputSize));
    plane.setPixelShaderHLSL(PlanePostProcessPixelShaderHlsl());
    UpdatePlanePostProcessState(plane, postProcessSettings, 0.0f);
}

void UpdatePlanePostProcessState(
    VarjoXR::XRPlane& plane,
    const StereoPostProcessSettings& settings,
    float /*dynamicAmount01*/)
{
    ApplyMaterialPostProcessParams(plane, VarjoXR::Eye::Left, settings);
    ApplyMaterialPostProcessParams(plane, VarjoXR::Eye::Right, settings);
}

void UpdatePlanePostProcessReveal(
    VarjoXR::XRPlane& plane,
    const StereoPostProcessSettings& settings,
    float revealAmount01,
    float expandedRadiusShortAxis01)
{
    const float reveal = std::clamp(FiniteOr(revealAmount01, 0.0f), 0.0f, 1.0f);
    for (auto eye : {VarjoXR::Eye::Left, VarjoXR::Eye::Right}) {
        auto animated = settings;
        switch (settings.modeForEye(eye)) {
        case StereoPostProcessMode::Darken: {
            const float base = std::clamp(FiniteOr(settings.radiusShortAxis01, 0.25f), 0.0f, 4.0f);
            const float expanded = FiniteOr(expandedRadiusShortAxis01, 4.0f);
            const float target = std::clamp(expanded > 0.0f ? expanded : 4.0f, base, 4.0f);
            animated.radiusShortAxis01 = base + (target - base) * reveal;
            break;
        }
        case StereoPostProcessMode::Blur:
            animated.blurStrength01 = 1.0f - reveal;
            break;
        case StereoPostProcessMode::BlurWave:
            animated.blurStrength01 *= 1.0f - reveal;
            break;
        case StereoPostProcessMode::OutsideBlur:
        case StereoPostProcessMode::None:
            break;
        }
        ApplyMaterialPostProcessParams(plane, eye, animated);
    }
}

bool PlaneUsesBlurWave(const VarjoXR::XRPlane& plane) noexcept
{
    return UsesBlurWave(plane.material(VarjoXR::Eye::Left)) ||
           UsesBlurWave(plane.material(VarjoXR::Eye::Right));
}

void UpdateBlurWaveFrame(
    VarjoXR::XRPlane& plane,
    BlurWaveAnimation& animation,
    const std::optional<glm::vec2>& gazeUv,
    double elapsedSeconds)
{
    for (auto eye : {VarjoXR::Eye::Left, VarjoXR::Eye::Right}) {
        auto& material = plane.material(eye);
        auto& processing = material.processing;
        const bool selected = UsesBlurWave(material);
        const auto index = VarjoXR::EyeIndex(eye);
        if (!selected && !animation.active[index]) continue;
        if (selected && !processing.enabled && material.texture) {
            // Without --calib the displayed camera image already uses Plane UVs.
            processing = MakeProcessing(CalibrationHomography{}, {0.f, 0.f, 0.f, 1.f},
                                        {material.texture->width(), material.texture->height()});
        }
        auto& data = processing.userConstants.data;
        auto& phase = animation.phaseRadians[index];
        if (!processing.enabled ||
            data.size() != sizeof(RectificationConstants) + sizeof(BlurWaveConstants)) {
            phase = 0.0;
            animation.active[index] = false;
            continue;
        }
        auto* destination = data.data() + sizeof(RectificationConstants);
        if (!selected) {
            const BlurWaveConstants disabled{};
            std::memcpy(destination, &disabled, sizeof(disabled));
            animation.active[index] = false;
            phase = 0.0;
            continue;
        }

        const auto wave = MakeBlurWaveConstants(material, gazeUv, elapsedSeconds, phase);
        std::memcpy(destination, &wave, sizeof(wave));
        animation.active[index] = true;
    }
}

void UpdatePlaneAspectFromCalibration(
    VarjoXR::XRPlane& plane,
    const StereoCalibrationDocument& document)
{
    if (!document.rectifiedOutputSize.valid()) {
        throw std::invalid_argument("calibration rectified output size is invalid");
    }

    const auto current = plane.size();
    const float width = std::max(0.001f, current.x);
    const float aspect =
        static_cast<float>(document.rectifiedOutputSize.height) /
        static_cast<float>(document.rectifiedOutputSize.width);
    plane.setSize({width, width * aspect});
}

} // namespace DualIC4Varjo
