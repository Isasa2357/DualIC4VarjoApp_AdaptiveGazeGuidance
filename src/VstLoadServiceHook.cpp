#include "VstLoadServiceHook.hpp"

#include "ExperimentOutput.hpp"

#include <iostream>
#include <string>

namespace DualIC4Varjo {
namespace {

std::string gLastError;

} // namespace

void VstLoadServiceHook::configure(int, char**)
{
    gLastError.clear();
    std::cout
        << "[VST] recording disabled; no VST video or VST metadata CSV will be created\n";
}

void VstLoadServiceHook::ensureStarted(
    const std::shared_ptr<varjo_Session>&) noexcept
{
    // Intentionally disabled. The application still uses Varjo MR/VST
    // postprocess for the visual blur/darken effect, but it no longer starts
    // VarjoVSTService, ffmpeg pipes, *_vst_left/right.mp4, or *_vst_*metadata.csv.
}

void VstLoadServiceHook::stop() noexcept
{
}

std::filesystem::path VstLoadServiceHook::outputDirectory()
{
    // Compatibility path for UI/status code that was originally written around
    // the VST recorder. VST recording is disabled, but the raw camera recorder
    // writes *_left_raw_metadata.csv and *_right_raw_metadata.csv into the active
    // experiment directory. Returning that directory keeps the ImGui video-codec
    // FPS graph connected to the current camera recording metadata.
    if (const auto layout = ActiveExperimentOutputLayout()) {
        return layout->directory;
    }
    return {};
}

std::string VstLoadServiceHook::lastError()
{
    return gLastError;
}

std::uint64_t VstLoadServiceHook::leftReceivedCount() noexcept
{
    return 0;
}

std::uint64_t VstLoadServiceHook::rightReceivedCount() noexcept
{
    return 0;
}

std::uint64_t VstLoadServiceHook::leftProcessedCount() noexcept
{
    return 0;
}

std::uint64_t VstLoadServiceHook::rightProcessedCount() noexcept
{
    return 0;
}

std::uint64_t VstLoadServiceHook::droppedCount() noexcept
{
    return 0;
}

std::uint64_t VstLoadServiceHook::writeFailureCount() noexcept
{
    return 0;
}

} // namespace DualIC4Varjo
