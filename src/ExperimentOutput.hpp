#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace DualIC4Varjo {

struct ExperimentOutputLayout {
    std::filesystem::path directory;
    std::string requestedProjectName;
    std::string resolvedProjectName;
    std::filesystem::path renderedFramesCsv;
};

// Allocates the unique experiment folder once and stores it as the active
// process-wide output layout. A collision is resolved as project_1, project_2,
// and so on.
ExperimentOutputLayout ReserveExperimentOutputLayout(
    const std::filesystem::path& baseDirectory,
    const std::string& projectName,
    const std::filesystem::path& requestedRenderedFramesCsv);

// Returns the active layout when one has already been reserved by the combined
// application entry point.
std::optional<ExperimentOutputLayout> ActiveExperimentOutputLayout();

// Existing call site used by the renderer. If an identical active layout has
// already been reserved, this returns that layout instead of allocating a
// second folder.
ExperimentOutputLayout CreateExperimentOutputLayout(
    const std::filesystem::path& baseDirectory,
    const std::string& projectName,
    const std::filesystem::path& requestedRenderedFramesCsv);

} // namespace DualIC4Varjo
