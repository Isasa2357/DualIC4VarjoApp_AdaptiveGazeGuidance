#include "StereoCalibrationSupport.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace DualIC4Varjo {
namespace {

nlohmann::json SizeJson(const CalibrationImageSize& size)
{
    return {
        {"width", size.width},
        {"height", size.height},
    };
}

nlohmann::json HomographyJson(const CalibrationHomography& value)
{
    return {
        {"rows", {
            {value.rows[0], value.rows[1], value.rows[2]},
            {value.rows[3], value.rows[4], value.rows[5]},
            {value.rows[6], value.rows[7], value.rows[8]},
        }},
    };
}

} // namespace

void SaveStereoCalibrationJson(
    const std::filesystem::path& path,
    const StereoCalibrationDocument& document)
{
    if (path.empty()) {
        throw std::invalid_argument("calibration JSON path is empty");
    }
    if (!document.sourceSize.valid() ||
        !document.calibrationInputSize.valid() ||
        !document.rectifiedOutputSize.valid()) {
        throw std::invalid_argument("calibration image geometry is invalid");
    }
    if (document.defaultProfile.empty() ||
        !document.hasProfile(document.defaultProfile)) {
        throw std::invalid_argument("calibration default profile is missing");
    }

    nlohmann::json profiles = nlohmann::json::object();
    for (const auto& [name, profile] : document.profiles) {
        profiles[name] = {
            {"method", profile.method.empty() ? name : profile.method},
            {"eyes", {
                {"left", {
                    {"inverse_pixel_homography", HomographyJson(profile.leftInverse)},
                }},
                {"right", {
                    {"inverse_pixel_homography", HomographyJson(profile.rightInverse)},
                }},
            }},
        };
    }

    nlohmann::json root = {
        {"format", document.format.empty()
            ? "vdca.stereo_rectification"
            : document.format},
        {"version", document.version == 0 ? 1 : document.version},
        {"default_profile", document.defaultProfile},
        {"image_geometry", {
            {"source_size", SizeJson(document.sourceSize)},
            {"calibration_input_size", SizeJson(document.calibrationInputSize)},
            {"rectified_output_size", SizeJson(document.rectifiedOutputSize)},
        }},
        {"preprocess", {
            {"resize", {{"mode", document.resizeMode.empty() ? "none" : document.resizeMode}}},
            {"right_order", document.rightOrder.empty() ? "same" : document.rightOrder},
        }},
        {"sampling", {
            {"filter", document.samplingFilter.empty() ? "linear" : document.samplingFilter},
            {"border_mode", document.borderMode.empty() ? "constant" : document.borderMode},
            {"border_rgba", document.borderRgba},
        }},
        {"profiles", std::move(profiles)},
    };

    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            throw std::runtime_error(
                "failed to create calibration directory: " + error.message());
        }
    }

    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error(
            "failed to open calibration JSON for writing: " + path.string());
    }
    stream << root.dump(2) << '\n';
    if (!stream) {
        throw std::runtime_error(
            "failed to write calibration JSON: " + path.string());
    }
}

} // namespace DualIC4Varjo
