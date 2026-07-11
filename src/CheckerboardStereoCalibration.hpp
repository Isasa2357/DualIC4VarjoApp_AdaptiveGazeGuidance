#pragma once

#include "StereoCalibrationSupport.hpp"

#include <IC4Ext/IC4Ext.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace DualIC4Varjo {

struct CheckerboardCalibrationOptions {
    int innerCornersColumns = 9;
    int innerCornersRows = 6;
    std::size_t minimumSamplePairs = 12;
    int captureIntervalMs = 500;
    double minimumCornerMotionPixels = 20.0;
};

struct CheckerboardCalibrationResult {
    bool ok = false;
    bool aborted = false;
    std::size_t capturedSamplePairs = 0;
    double averageEpipolarErrorPixels = 0.0;
    StereoCalibrationDocument document;
    std::string error;
};

CheckerboardCalibrationResult RunCheckerboardStereoCalibration(
    IC4Ext::D3D12SyncedFrameQueue& inputQueue,
    const IC4Ext::D3D12BackendContext& backend,
    const CheckerboardCalibrationOptions& options,
    const std::optional<StereoCalibrationDocument>& initialDocument = std::nullopt);

} // namespace DualIC4Varjo
