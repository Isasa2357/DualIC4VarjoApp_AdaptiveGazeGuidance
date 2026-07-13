#include "StereoCalibrationSupport.hpp"

#include <VarjoXR/VarjoXR.hpp>

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

const char* RectificationHlsl() noexcept
{
    return R"hlsl(
Texture2D<float4> xrInput : register(t0);
RWTexture2D<float4> xrOutput : register(u0);

cbuffer RectificationConstants : register(b0)
{
    float4 inverseRow0;
    float4 inverseRow1;
    float4 inverseRow2;
    float4 borderRgba;
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

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dstWidth || id.y >= dstHeight) return;
    const float2 destinationPixel = float2((float)id.x, (float)id.y);
    xrOutput[id.xy] = SampleRemappedOutputPixel(destinationPixel);
}
)hlsl";
}

const char* PlanePostProcessPixelShaderHlsl() noexcept
{
    return R"hlsl(
float OutsideMaskForUv(float2 uv, float2 textureSize)
{
    const float shortAxis = max(1.0f, min(textureSize.x, textureSize.y));
    const float2 center01 = params0.xy;
    const float2 radii = max(params0.zw, float2(1.0e-5f, 1.0e-5f));
    const float2 deltaShortAxis = (uv - center01) * textureSize / shortAxis;
    const float ellipseDistance = length(deltaShortAxis / radii);
    const float maxRadius = max(max(radii.x, radii.y), 1.0e-5f);
    const float softness = max(1.0e-5f, params1.y / maxRadius);
    return smoothstep(1.0f - softness, 1.0f + softness, ellipseDistance);
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
    if (mode <= 0) {
        return color * tint;
    }

    const float outsideMask = OutsideMaskForUv(uv, textureSize);
    if (mode == 1) {
        const float outsideBrightness = saturate(params1.x);
        color.rgb *= lerp(1.0f, outsideBrightness, outsideMask);
    } else if (mode == 2) {
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
    result.userConstants.set(MakeConstants(inverse, borderRgba));
    result.frameConstants.enabled = true;
    result.frameConstants.registerIndex = 1;
    return result;
}

void ApplyMaterialPostProcessParams(
    VarjoXR::XRPlane& plane,
    VarjoXR::Eye eye,
    StereoPostProcessSettings settings) noexcept
{
    settings = SanitizePostProcessSettings(settings);
    const float mode = settings.enabled ? ModeToFloat(settings.mode) : 0.0f;
    const float encodedModeAndBlurStrength =
        mode > 0.0f ? mode + std::clamp(settings.blurStrength01, 0.0f, 1.0f) * 0.1f : 0.0f;

    auto& material = plane.material(eye);
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

void UpdatePostProcessStateForEye(
    VarjoXR::XRPlane& plane,
    VarjoXR::Eye eye,
    const StereoPostProcessSettings& settings,
    float /*dynamicAmount01*/) noexcept
{
    ApplyMaterialPostProcessParams(plane, eye, settings);
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
    float dynamicAmount01)
{
    UpdatePostProcessStateForEye(plane, VarjoXR::Eye::Left, settings, dynamicAmount01);
    UpdatePostProcessStateForEye(plane, VarjoXR::Eye::Right, settings, dynamicAmount01);
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
