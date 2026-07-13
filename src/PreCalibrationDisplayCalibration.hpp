#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "StereoCalibrationSupport.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace DualIC4Varjo::PreCalibrationDisplayCalibration {
namespace detail {

enum class ApplyState {
    NotChecked,
    Unavailable,
    Applied,
    Failed,
};

inline std::mutex& Mutex() noexcept
{
    static std::mutex value;
    return value;
}

inline ApplyState& State() noexcept
{
    static ApplyState value = ApplyState::NotChecked;
    return value;
}

inline std::optional<std::filesystem::path> ExistingCalibrationPathFromCommandLine()
{
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return std::nullopt;

    std::optional<std::filesystem::path> result;
    for (int index = 1; index + 1 < count; ++index) {
        const std::wstring option = values[index] ? values[index] : L"";
        if (option != L"--calib") continue;

        const std::wstring value = values[index + 1] ? values[index + 1] : L"";
        if (value.empty() || value == L"-") break;

        std::error_code ec;
        std::filesystem::path path = std::filesystem::absolute(
            std::filesystem::path(value),
            ec);
        if (ec) path = std::filesystem::path(value);

        if (std::filesystem::is_regular_file(path, ec)) {
            result = path;
        }
        break;
    }

    LocalFree(values);
    return result;
}

} // namespace detail

inline void ApplyIfAvailable(VarjoXR::XRPlane& plane) noexcept
{
    std::lock_guard<std::mutex> lock(detail::Mutex());
    if (detail::State() != detail::ApplyState::NotChecked) return;

    const auto path = detail::ExistingCalibrationPathFromCommandLine();
    if (!path) {
        detail::State() = detail::ApplyState::Unavailable;
        return;
    }

    try {
        StereoCalibrationDocument document = LoadStereoCalibrationJson(*path);
        ApplyCalibrationToPlane(plane, document, document.defaultProfile);
        UpdatePlaneAspectFromCalibration(plane, document);
        detail::State() = detail::ApplyState::Applied;
        std::cout
            << "[CALIB] applied existing calibration before checkerboard phase: "
            << path->string()
            << " profile=" << document.defaultProfile << '\n';
    } catch (const std::exception& exception) {
        detail::State() = detail::ApplyState::Failed;
        std::cerr
            << "[CALIB] failed to apply existing calibration before checkerboard phase: "
            << exception.what() << '\n';
    } catch (...) {
        detail::State() = detail::ApplyState::Failed;
        std::cerr
            << "[CALIB] failed to apply existing calibration before checkerboard phase: unknown error\n";
    }
}

} // namespace DualIC4Varjo::PreCalibrationDisplayCalibration
