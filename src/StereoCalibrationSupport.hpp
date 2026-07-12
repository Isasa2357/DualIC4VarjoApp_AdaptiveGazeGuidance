#pragma once

#include <VarjoXR/Core/XRPlane.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace DualIC4Varjo {

struct CalibrationImageSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool valid() const noexcept
    {
        return width != 0 && height != 0;
    }
};

struct CalibrationHomography {
    std::array<double, 9> rows{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    };
};

struct CalibrationProfile {
    std::string method;
    CalibrationHomography leftInverse;
    CalibrationHomography rightInverse;
};

struct StereoPostProcessSettings {
    bool enabled = true;
    float centerX01 = 0.5f;
    float centerY01 = 0.5f;
    float radiusShortAxis01 = 0.25f;
    float edgeSoftnessShortAxis01 = 0.03f;
    float outsideBrightness = 0.5f;
};

struct StereoCalibrationDocument {
    std::string format;
    int version = 0;
    std::string defaultProfile;

    CalibrationImageSize sourceSize;
    CalibrationImageSize calibrationInputSize;
    CalibrationImageSize rectifiedOutputSize;

    std::string resizeMode;
    std::string rightOrder;
    std::string samplingFilter;
    std::string borderMode;
    std::array<float, 4> borderRgba{0.0f, 0.0f, 0.0f, 1.0f};

    std::map<std::string, CalibrationProfile> profiles;

    bool hasProfile(const std::string& name) const noexcept;
    const CalibrationProfile& profile(const std::string& name) const;
};

StereoCalibrationDocument LoadStereoCalibrationJson(
    const std::filesystem::path& path);

void SaveStereoCalibrationJson(
    const std::filesystem::path& path,
    const StereoCalibrationDocument& document);

void ValidateCalibrationInputGeometry(
    const StereoCalibrationDocument& document,
    std::uint32_t inputWidth,
    std::uint32_t inputHeight);

void ApplyCalibrationToPlane(
    VarjoXR::XRPlane& plane,
    const StereoCalibrationDocument& document,
    const std::string& profileName,
    const StereoPostProcessSettings& postProcessSettings = {});

void UpdatePlanePostProcessState(
    VarjoXR::XRPlane& plane,
    const StereoPostProcessSettings& settings,
    float revealAmount01);

void UpdatePlaneAspectFromCalibration(
    VarjoXR::XRPlane& plane,
    const StereoCalibrationDocument& document);

} // namespace DualIC4Varjo
