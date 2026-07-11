#include "ExperimentOutput.hpp"

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace DualIC4Varjo {
namespace {

std::mutex gOutputLayoutMutex;
std::optional<ExperimentOutputLayout> gActiveOutputLayout;

void ValidateProjectName(const std::string& projectName)
{
    if (projectName.empty()) {
        throw std::invalid_argument("--project must not be empty");
    }

    const std::filesystem::path projectPath(projectName);
    if (projectPath.is_absolute() || projectPath.has_parent_path() ||
        projectPath == "." || projectPath == "..") {
        throw std::invalid_argument(
            "--project must be a single folder name, not a path");
    }
}

ExperimentOutputLayout AllocateExperimentOutputLayout(
    const std::filesystem::path& baseDirectory,
    const std::string& projectName,
    const std::filesystem::path& requestedRenderedFramesCsv)
{
    static_cast<void>(requestedRenderedFramesCsv);

    ValidateProjectName(projectName);
    if (baseDirectory.empty()) {
        throw std::invalid_argument("--dir must not be empty");
    }

    std::error_code ec;
    const std::filesystem::path absoluteBase =
        std::filesystem::absolute(baseDirectory, ec);
    if (ec) {
        throw std::runtime_error(
            "failed to resolve --dir: " + ec.message());
    }

    std::filesystem::create_directories(absoluteBase, ec);
    if (ec) {
        throw std::runtime_error(
            "failed to create --dir directory '" + absoluteBase.string() +
            "': " + ec.message());
    }

    ExperimentOutputLayout result;
    result.requestedProjectName = projectName;

    for (std::uint32_t suffix = 0;
         suffix < std::numeric_limits<std::uint32_t>::max();
         ++suffix) {
        const std::string resolvedName = suffix == 0
            ? projectName
            : projectName + "_" + std::to_string(suffix);
        const std::filesystem::path candidate = absoluteBase / resolvedName;

        ec.clear();
        if (std::filesystem::create_directory(candidate, ec)) {
            result.directory = candidate;
            result.resolvedProjectName = resolvedName;
            result.renderedFramesCsv =
                candidate / (resolvedName + "_rendered_frames.csv");
            return result;
        }

        if (ec) {
            throw std::runtime_error(
                "failed to create experiment directory '" + candidate.string() +
                "': " + ec.message());
        }
    }

    throw std::runtime_error("could not allocate a unique experiment directory");
}

} // namespace

ExperimentOutputLayout ReserveExperimentOutputLayout(
    const std::filesystem::path& baseDirectory,
    const std::string& projectName,
    const std::filesystem::path& requestedRenderedFramesCsv)
{
    std::lock_guard<std::mutex> lock(gOutputLayoutMutex);
    if (gActiveOutputLayout.has_value()) {
        return *gActiveOutputLayout;
    }

    gActiveOutputLayout = AllocateExperimentOutputLayout(
        baseDirectory,
        projectName,
        requestedRenderedFramesCsv);
    return *gActiveOutputLayout;
}

std::optional<ExperimentOutputLayout> ActiveExperimentOutputLayout()
{
    std::lock_guard<std::mutex> lock(gOutputLayoutMutex);
    return gActiveOutputLayout;
}

ExperimentOutputLayout CreateExperimentOutputLayout(
    const std::filesystem::path& baseDirectory,
    const std::string& projectName,
    const std::filesystem::path& requestedRenderedFramesCsv)
{
    {
        std::lock_guard<std::mutex> lock(gOutputLayoutMutex);
        if (gActiveOutputLayout.has_value()) {
            return *gActiveOutputLayout;
        }
    }

    return ReserveExperimentOutputLayout(
        baseDirectory,
        projectName,
        requestedRenderedFramesCsv);
}

} // namespace DualIC4Varjo
