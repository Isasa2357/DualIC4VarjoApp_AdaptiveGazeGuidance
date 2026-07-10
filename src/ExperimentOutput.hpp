#pragma once

#include <filesystem>
#include <string>

namespace DualIC4Varjo {

struct ExperimentOutputLayout {
    std::filesystem::path directory;
    std::string requestedProjectName;
    std::string resolvedProjectName;
    std::filesystem::path renderedFramesCsv;
};

ExperimentOutputLayout CreateExperimentOutputLayout(
    const std::filesystem::path& baseDirectory,
    const std::string& projectName,
    const std::filesystem::path& requestedRenderedFramesCsv);

} // namespace DualIC4Varjo
