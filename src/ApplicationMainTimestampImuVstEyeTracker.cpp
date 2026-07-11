#include "ExperimentOutput.hpp"
#include "EyeTrackerLoadServiceHook.hpp"
#include "ImuLoadServiceHook.hpp"
#include "KeyInputService.hpp"
#include "TimestampLoadService.hpp"
#include "VstLoadServiceHook.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace DualIC4Varjo {
namespace {

std::optional<std::string> FindArgumentValue(
    int argc,
    char** argv,
    const std::string& option)
{
    for (int index = 1; index < argc; ++index) {
        if (argv[index] && option == argv[index]) {
            if (index + 1 >= argc || !argv[index + 1]) return std::nullopt;
            return std::string(argv[index + 1]);
        }
    }
    return std::nullopt;
}

bool HasFlag(int argc, char** argv, const std::string& option)
{
    for (int index = 1; index < argc; ++index) {
        if (argv[index] && option == argv[index]) return true;
    }
    return false;
}

ExperimentOutputLayout ReserveOutputFromArguments(int argc, char** argv)
{
    const auto directory = FindArgumentValue(argc, argv, "--dir");
    const auto project = FindArgumentValue(argc, argv, "--project");
    const auto metadata = FindArgumentValue(argc, argv, "--metadata-csv");

    if (!directory.has_value() || directory->empty()) {
        throw std::invalid_argument("--dir is required and must not be empty");
    }
    if (!project.has_value() || project->empty()) {
        throw std::invalid_argument("--project is required and must not be empty");
    }

    return ReserveExperimentOutputLayout(
        std::filesystem::path(*directory),
        *project,
        metadata.has_value()
            ? std::filesystem::path(*metadata)
            : std::filesystem::path("rendered_frames.csv"));
}

bool ValidateOutputFile(
    const char* label,
    const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        std::cerr
            << "[OUTPUT] missing " << label << ": " << path.string();
        if (error) std::cerr << " (" << error.message() << ')';
        std::cerr << '\n';
        return false;
    }

    error.clear();
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0) {
        std::cerr
            << "[OUTPUT] empty/unreadable " << label << ": "
            << path.string();
        if (error) std::cerr << " (" << error.message() << ')';
        std::cerr << '\n';
        return false;
    }

    std::cout
        << "[OUTPUT] verified " << label << ": "
        << size << " bytes, " << path.string() << '\n';
    return true;
}

void SubmitFrameInfoServices(
    const VarjoFrameInfoSnapshot& snapshot,
    const std::shared_ptr<varjo_Session>& session) noexcept
{
    ImuLoadServiceHook::submit(snapshot, session);
    EyeTrackerLoadServiceHook::submit(snapshot, session);
}

} // namespace
} // namespace DualIC4Varjo

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#ifdef NOMINMAX
#undef NOMINMAX
#endif

#define main DualIC4VarjoBaseMain
#define markRendered(...)                                                     \
    markRendered(__VA_ARGS__),                                                \
        DualIC4Varjo::SubmitFrameInfoServices(                                \
            d3dBackend.frameInfoSnapshot(),                                   \
            session->shared()),                                               \
        DualIC4Varjo::VstLoadServiceHook::ensureStarted(                      \
            session->shared())
#include "ApplicationMainPlaneControl.cpp"
#undef markRendered
#undef main

#include <exception>
#include <stdexcept>

int main(int argc, char** argv)
{
    if (DualIC4Varjo::HasFlag(argc, argv, "--help") ||
        DualIC4Varjo::HasFlag(argc, argv, "-h")) {
        return DualIC4VarjoBaseMain(argc, argv);
    }

    DualIC4Varjo::ExperimentOutputLayout outputLayout;
    try {
        outputLayout = DualIC4Varjo::ReserveOutputFromArguments(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Output setup failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout
        << "[OUTPUT] requested project : "
        << outputLayout.requestedProjectName << '\n'
        << "[OUTPUT] resolved project  : "
        << outputLayout.resolvedProjectName << '\n'
        << "[OUTPUT] directory         : "
        << outputLayout.directory.string() << '\n';

    DualIC4Varjo::ImuLoadServiceHook::configure(argc, argv);
    DualIC4Varjo::VstLoadServiceHook::configure(argc, argv);
    DualIC4Varjo::EyeTrackerLoadServiceHook::configure(argc, argv);

    DualIC4Varjo::KeyInputService keyInputService(
        outputLayout.directory /
        (outputLayout.resolvedProjectName + "_key_input.csv"));
    if (!keyInputService.start()) {
        std::cerr
            << "Key input service failed to start: "
            << keyInputService.lastError() << '\n';
        return 1;
    }

    DualIC4Varjo::TimestampLoadService timestampService(argc, argv);
    if (!timestampService.start()) {
        std::cerr
            << "Timestamp service failed to start: "
            << timestampService.lastError() << '\n';
        keyInputService.stop();
        return 1;
    }

    int result = 1;
    try {
        result = DualIC4VarjoBaseMain(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr
            << "Unhandled application exception: "
            << exception.what() << '\n';
        result = 1;
    } catch (...) {
        std::cerr << "Unhandled unknown application exception.\n";
        result = 1;
    }

    DualIC4Varjo::EyeTrackerLoadServiceHook::stop();
    DualIC4Varjo::VstLoadServiceHook::stop();
    DualIC4Varjo::ImuLoadServiceHook::stop();
    timestampService.stop();
    keyInputService.stop();

    std::cout
        << "[TIMESTAMP] samples=" << timestampService.sampleCount()
        << " failed=" << timestampService.failedSampleCount()
        << " CSV=" << timestampService.outputPath().string() << '\n';

    std::cout
        << "[IMU] received=" << DualIC4Varjo::ImuLoadServiceHook::receivedCount()
        << " processed=" << DualIC4Varjo::ImuLoadServiceHook::processedCount()
        << " written=" << DualIC4Varjo::ImuLoadServiceHook::writtenCount()
        << " dropped=" << DualIC4Varjo::ImuLoadServiceHook::droppedCount()
        << " CSV=" << DualIC4Varjo::ImuLoadServiceHook::outputPath().string()
        << '\n';

    std::cout
        << "[VST] leftReceived=" << DualIC4Varjo::VstLoadServiceHook::leftReceivedCount()
        << " rightReceived=" << DualIC4Varjo::VstLoadServiceHook::rightReceivedCount()
        << " leftProcessed=" << DualIC4Varjo::VstLoadServiceHook::leftProcessedCount()
        << " rightProcessed=" << DualIC4Varjo::VstLoadServiceHook::rightProcessedCount()
        << " dropped=" << DualIC4Varjo::VstLoadServiceHook::droppedCount()
        << " writeFailures=" << DualIC4Varjo::VstLoadServiceHook::writeFailureCount()
        << " output=" << DualIC4Varjo::VstLoadServiceHook::outputDirectory().string()
        << '\n';

    std::cout
        << "[EYETRACKER] received="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::receivedSampleCount()
        << " processed="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::processedSampleCount()
        << " written="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::writtenSampleCount()
        << " droppedSamples="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::droppedSampleCount()
        << " submittedFrameInfo="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::submittedFrameInfoCount()
        << " evictedFrameInfoHistory="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::droppedFrameInfoCount()
        << " CSV="
        << DualIC4Varjo::EyeTrackerLoadServiceHook::outputPath().string()
        << '\n';

    std::cout
        << "[KEYINPUT] events=" << keyInputService.eventCount()
        << " CSV=" << keyInputService.outputPath().string() << '\n';

    const std::string imuError = DualIC4Varjo::ImuLoadServiceHook::lastError();
    const std::string vstError = DualIC4Varjo::VstLoadServiceHook::lastError();
    const std::string eyeError = DualIC4Varjo::EyeTrackerLoadServiceHook::lastError();
    const std::string keyError = keyInputService.lastError();
    if (!imuError.empty()) std::cerr << "[IMU] last error: " << imuError << '\n';
    if (!vstError.empty()) std::cerr << "[VST] last error: " << vstError << '\n';
    if (!eyeError.empty()) std::cerr << "[EYETRACKER] last error: " << eyeError << '\n';
    if (!keyError.empty()) std::cerr << "[KEYINPUT] last error: " << keyError << '\n';

    const auto vstLeftMetadata =
        outputLayout.directory /
        (outputLayout.resolvedProjectName + "_vst_left_metadata.csv");
    const auto vstRightMetadata =
        outputLayout.directory /
        (outputLayout.resolvedProjectName + "_vst_right_metadata.csv");

    bool requiredOutputsValid = true;
    requiredOutputsValid &= DualIC4Varjo::ValidateOutputFile(
        "rendered frame CSV", outputLayout.renderedFramesCsv);
    requiredOutputsValid &= DualIC4Varjo::ValidateOutputFile(
        "timestamp CSV", timestampService.outputPath());
    requiredOutputsValid &= DualIC4Varjo::ValidateOutputFile(
        "IMU CSV", DualIC4Varjo::ImuLoadServiceHook::outputPath());
    requiredOutputsValid &= DualIC4Varjo::ValidateOutputFile(
        "key input CSV", keyInputService.outputPath());
    requiredOutputsValid &= DualIC4Varjo::ValidateOutputFile(
        "VST left metadata CSV", vstLeftMetadata);
    requiredOutputsValid &= DualIC4Varjo::ValidateOutputFile(
        "VST right metadata CSV", vstRightMetadata);

    if (!requiredOutputsValid && result == 0) result = 1;
    return result;
}
