#include "StereoCalibrationSupport.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

using Json = nlohmann::json;

constexpr const char* kExpectedFormat =
    "vdca.stereo_rectification";
constexpr int kExpectedVersion = 1;

CalibrationImageSize ParseImageSize(
    const Json& value,
    const char* label)
{
    if (!value.is_object()) {
        throw std::invalid_argument(
            std::string(label) + " must be an object");
    }

    CalibrationImageSize result;
    result.width = value.at("width").get<std::uint32_t>();
    result.height = value.at("height").get<std::uint32_t>();
    if (!result.valid()) {
        throw std::invalid_argument(
            std::string(label) + " must be non-zero");
    }
    return result;
}

CalibrationHomography ParseHomography(
    const Json& value,
    const char* label)
{
    if (!value.is_object() ||
        !value.contains("rows") ||
        !value.at("rows").is_array() ||
        value.at("rows").size() != 3) {
        throw std::invalid_argument(
            std::string(label) +
            " must contain a 3x3 rows array");
    }

    CalibrationHomography result;
    const auto& rows = value.at("rows");
    for (std::size_t row = 0; row < 3; ++row) {
        if (!rows.at(row).is_array() ||
            rows.at(row).size() != 3) {
            throw std::invalid_argument(
                std::string(label) +
                " must contain a 3x3 rows array");
        }
        for (std::size_t column = 0;
             column < 3;
             ++column) {
            const double element =
                rows.at(row).at(column).get<double>();
            if (!std::isfinite(element)) {
                throw std::invalid_argument(
                    std::string(label) +
                    " contains a non-finite value");
            }
            result.rows[row * 3 + column] = element;
        }
    }
    return result;
}

CalibrationProfile ParseProfile(
    const Json& value,
    const std::string& profileName)
{
    if (!value.is_object()) {
        throw std::invalid_argument(
            "calibration profile is not an object: " +
            profileName);
    }

    CalibrationProfile result;
    result.method = value.value("method", profileName);

    const auto& eyes = value.at("eyes");
    const auto& left = eyes.at("left");
    const auto& right = eyes.at("right");
    result.leftInverse = ParseHomography(
        left.at("inverse_pixel_homography"),
        "left inverse_pixel_homography");
    result.rightInverse = ParseHomography(
        right.at("inverse_pixel_homography"),
        "right inverse_pixel_homography");
    return result;
}

void ValidateDocument(
    const StereoCalibrationDocument& document)
{
    if (document.format != kExpectedFormat) {
        throw std::invalid_argument(
            "unsupported calibration format: " +
            document.format);
    }
    if (document.version != kExpectedVersion) {
        throw std::invalid_argument(
            "unsupported calibration version: " +
            std::to_string(document.version));
    }
    if (!document.sourceSize.valid() ||
        !document.calibrationInputSize.valid() ||
        !document.rectifiedOutputSize.valid()) {
        throw std::invalid_argument(
            "calibration image geometry is invalid");
    }
    if (document.resizeMode != "none") {
        throw std::invalid_argument(
            "this stage supports only preprocess.resize.mode=none");
    }
    if (document.rightOrder != "same") {
        throw std::invalid_argument(
            "this stage supports only preprocess.right_order=same");
    }
    if (document.samplingFilter != "linear") {
        throw std::invalid_argument(
            "this stage supports only sampling.filter=linear");
    }
    if (document.borderMode != "constant") {
        throw std::invalid_argument(
            "this stage supports only sampling.border_mode=constant");
    }
    for (float value : document.borderRgba) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(
                "sampling.border_rgba contains a non-finite value");
        }
    }
    if (document.defaultProfile.empty() ||
        !document.hasProfile(document.defaultProfile)) {
        throw std::invalid_argument(
            "calibration default_profile does not exist in profiles");
    }
}

struct alignas(16) RectificationConstants {
    std::array<float, 4> inverseRow0{};
    std::array<float, 4> inverseRow1{};
    std::array<float, 4> inverseRow2{};
    std::array<float, 4> borderRgba{};
};

static_assert(
    sizeof(RectificationConstants) == 64,
    "Unexpected rectification constant layout");

float CheckedFloat(double value)
{
    const float result = static_cast<float>(value);
    if (!std::isfinite(result)) {
        throw std::invalid_argument(
            "calibration matrix contains a non-finite value");
    }
    return result;
}

RectificationConstants MakeConstants(
    const CalibrationHomography& inverse,
    const std::array<float, 4>& borderRgba)
{
    RectificationConstants result;
    for (std::size_t column = 0;
         column < 3;
         ++column) {
        result.inverseRow0[column] =
            CheckedFloat(inverse.rows[column]);
        result.inverseRow1[column] =
            CheckedFloat(inverse.rows[3 + column]);
        result.inverseRow2[column] =
            CheckedFloat(inverse.rows[6 + column]);
    }
    result.borderRgba = borderRgba;
    return result;
}

const char* RemapHlsl() noexcept
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
        pixel.x >= (int)srcWidth ||
        pixel.y >= (int)srcHeight) {
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
    const float4 c10 = LoadWithConstantBorder(
        basePixel + int2(1, 0));
    const float4 c01 = LoadWithConstantBorder(
        basePixel + int2(0, 1));
    const float4 c11 = LoadWithConstantBorder(
        basePixel + int2(1, 1));
    return lerp(
        lerp(c00, c10, fraction.x),
        lerp(c01, c11, fraction.x),
        fraction.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dstWidth || id.y >= dstHeight) return;

    const float3 destinationPixel =
        float3((float)id.x, (float)id.y, 1.0f);
    const float3 sourceH = float3(
        dot(inverseRow0.xyz, destinationPixel),
        dot(inverseRow1.xyz, destinationPixel),
        dot(inverseRow2.xyz, destinationPixel));

    if (!isfinite(sourceH.z) ||
        abs(sourceH.z) < 1.0e-8f) {
        xrOutput[id.xy] = borderRgba;
        return;
    }

    const float2 sourcePixel = sourceH.xy / sourceH.z;
    if (!all(isfinite(sourcePixel)) ||
        sourcePixel.x <= -1.0f ||
        sourcePixel.y <= -1.0f ||
        sourcePixel.x >= (float)srcWidth ||
        sourcePixel.y >= (float)srcHeight) {
        xrOutput[id.xy] = borderRgba;
        return;
    }

    xrOutput[id.xy] = SampleLinear(sourcePixel);
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
    result.timing =
        VarjoXR::ProcessingTiming::OnTextureChanged;
    result.hlsl = RemapHlsl();
    result.entryPoint = "main";
    result.target = "cs_5_0";
    result.sourceName =
        "DualIC4Varjo_StaticStereoRemap.hlsl";
    result.outputSize = {
        outputSize.width,
        outputSize.height,
    };
    result.userConstants.registerIndex = 0;
    result.userConstants.set(
        MakeConstants(inverse, borderRgba));
    result.frameConstants.enabled = true;
    result.frameConstants.registerIndex = 1;
    return result;
}

} // namespace

bool StereoCalibrationDocument::hasProfile(
    const std::string& name) const noexcept
{
    return profiles.find(name) != profiles.end();
}

const CalibrationProfile& StereoCalibrationDocument::profile(
    const std::string& name) const
{
    const auto it = profiles.find(name);
    if (it == profiles.end()) {
        throw std::out_of_range(
            "calibration profile not found: " + name);
    }
    return it->second;
}

StereoCalibrationDocument LoadStereoCalibrationJson(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(
            "could not open calibration JSON: " +
            path.string());
    }

    Json root;
    try {
        stream >> root;
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "failed to parse calibration JSON " +
            path.string() +
            ": " +
            exception.what());
    }

    try {
        StereoCalibrationDocument result;
        result.format = root.at("format").get<std::string>();
        result.version = root.at("version").get<int>();
        result.defaultProfile =
            root.at("default_profile").get<std::string>();

        const auto& geometry = root.at("image_geometry");
        result.sourceSize = ParseImageSize(
            geometry.at("source_size"),
            "image_geometry.source_size");
        result.calibrationInputSize = ParseImageSize(
            geometry.at("calibration_input_size"),
            "image_geometry.calibration_input_size");
        result.rectifiedOutputSize = ParseImageSize(
            geometry.at("rectified_output_size"),
            "image_geometry.rectified_output_size");

        const auto& preprocess = root.at("preprocess");
        result.resizeMode =
            preprocess.at("resize").at("mode")
                .get<std::string>();
        result.rightOrder =
            preprocess.at("right_order")
                .get<std::string>();

        const auto& sampling = root.at("sampling");
        result.samplingFilter =
            sampling.at("filter").get<std::string>();
        result.borderMode =
            sampling.at("border_mode").get<std::string>();
        const auto& border = sampling.at("border_rgba");
        if (!border.is_array() || border.size() != 4) {
            throw std::invalid_argument(
                "sampling.border_rgba must contain four values");
        }
        for (std::size_t index = 0;
             index < 4;
             ++index) {
            result.borderRgba[index] =
                border.at(index).get<float>();
        }

        const auto& profiles = root.at("profiles");
        if (!profiles.is_object() || profiles.empty()) {
            throw std::invalid_argument(
                "profiles must be a non-empty object");
        }
        for (const auto& item : profiles.items()) {
            result.profiles.emplace(
                item.key(),
                ParseProfile(item.value(), item.key()));
        }

        ValidateDocument(result);
        return result;
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "invalid calibration JSON " +
            path.string() +
            ": " +
            exception.what());
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
        message
            << "calibration JSON source/input size "
            << document.sourceSize.width
            << 'x'
            << document.sourceSize.height
            << " does not match IC4 output texture size "
            << inputWidth
            << 'x'
            << inputHeight;
        throw std::invalid_argument(message.str());
    }
}

void ApplyCalibrationToPlane(
    VarjoXR::XRPlane& plane,
    const StereoCalibrationDocument& document,
    const std::string& profileName)
{
    ValidateDocument(document);
    const CalibrationProfile& selected =
        document.profile(profileName);

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
}

void UpdatePlaneAspectFromCalibration(
    VarjoXR::XRPlane& plane,
    const StereoCalibrationDocument& document)
{
    if (!document.rectifiedOutputSize.valid()) {
        throw std::invalid_argument(
            "calibration rectified output size is invalid");
    }

    const auto current = plane.size();
    const float width = std::max(0.001f, current.x);
    const float aspect =
        static_cast<float>(
            document.rectifiedOutputSize.height) /
        static_cast<float>(
            document.rectifiedOutputSize.width);
    plane.setSize({width, width * aspect});
}

} // namespace DualIC4Varjo
