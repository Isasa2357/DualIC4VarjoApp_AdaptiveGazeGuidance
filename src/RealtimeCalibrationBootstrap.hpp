#pragma once

#include "ExperimentOutput.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace DualIC4Varjo {

struct RealtimeCalibrationBootstrapResult {
    bool ok = false;
    bool aborted = false;
    bool calibrationPerformed = false;
    std::filesystem::path calibrationPath;
    std::vector<std::string> forwardedArguments;
    std::string error;
};

// Runs before every logging/service stage. It starts IC4 cameras and a raw ImGui
// preview first. A D3D12FrameSyncThread is created only after realtime
// calibration has been selected or forced by --calib - / a missing file.
RealtimeCalibrationBootstrapResult RunRealtimeCalibrationBootstrap(
    int argc,
    char** argv,
    const ExperimentOutputLayout& outputLayout);

} // namespace DualIC4Varjo
