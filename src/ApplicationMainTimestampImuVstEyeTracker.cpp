#include "EyeTrackerLoadServiceHook.hpp"
#include "ImuLoadServiceHook.hpp"
#include "TimestampLoadService.hpp"
#include "VstLoadServiceHook.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace DualIC4Varjo {
namespace {

struct NonEyeTrackerCsvOutputs {
    std::filesystem::path timestamp;
    std::filesystem::path imu;
    std::filesystem::path vstLeftMetadata;
    std::filesystem::path vstRightMetadata;
};

std::string FindArgumentValue(
    int argc,
    char** argv,
    const std::string& option,
    const std::string& fallback)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] && option == argv[index]) {
            return argv[index + 1] ? argv[index + 1] : fallback;
        }
    }
    return fallback;
}

std::string SanitizeServiceFilename(std::string value)
{
    for (char& character : value) {
        switch (character) {
        case '<': case '>': case ':': case '"': case '/':
        case '\\': case '|': case '?': case '*':
            character = '_';
            break;
        default:
            break;
        }
    }
    return value.empty() ? std::string("service_test") : value;
}

NonEyeTrackerCsvOutputs ResolveNonEyeTrackerCsvOutputs(
    int argc,
    char** argv)
{
    const std::filesystem::path baseDirectory =
        FindArgumentValue(argc, argv, "--dir", "logs");
    const std::string project = SanitizeServiceFilename(
        FindArgumentValue(argc, argv, "--project", "service_test"));
    const std::filesystem::path serviceDirectory =
        baseDirectory / "service_load";
    const std::filesystem::path vstDirectory =
        serviceDirectory / (project + "_vst");

    NonEyeTrackerCsvOutputs outputs;
    outputs.timestamp =
        serviceDirectory / (project + "_timestamp_mapping.csv");
    outputs.imu =
        serviceDirectory / (project + "_imu.csv");
    outputs.vstLeftMetadata =
        vstDirectory / (project + "_vst_left_metadata.csv");
    outputs.vstRightMetadata =
        vstDirectory / (project + "_vst_right_metadata.csv");
    return outputs;
}

bool PrepareCsvFile(
    const char* label,
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            std::cerr
                << "[CSV] failed to create directory for "
                << label << ": " << parent.string()
                << " (" << error.message() << ")\n";
            return false;
        }
    }

    error.clear();
    std::filesystem::remove(path, error);
    if (error) {
        std::cerr
            << "[CSV] failed to remove old "
            << label << ": " << path.string()
            << " (" << error.message() << ")\n";
        return false;
    }
    return true;
}

bool PrepareNonEyeTrackerCsvOutputs(
    const NonEyeTrackerCsvOutputs& outputs)
{
    const bool timestampReady =
        PrepareCsvFile("timestamp CSV", outputs.timestamp);
    const bool imuReady =
        PrepareCsvFile("IMU CSV", outputs.imu);
    const bool vstLeftReady =
        PrepareCsvFile("VST left metadata CSV", outputs.vstLeftMetadata);
    const bool vstRightReady =
        PrepareCsvFile("VST right metadata CSV", outputs.vstRightMetadata);

    if (!(timestampReady && imuReady && vstLeftReady && vstRightReady)) {
        return false;
    }

    std::cout
        << "[CSV] non-EyeTracker service CSV writing enabled\n"
        << "[CSV] timestamp          : "
        << outputs.timestamp.string() << '\n'
        << "[CSV] IMU                : "
        << outputs.imu.string() << '\n'
        << "[CSV] VST left metadata  : "
        << outputs.vstLeftMetadata.string() << '\n'
        << "[CSV] VST right metadata : "
        << outputs.vstRightMetadata.string() << '\n';
    return true;
}

bool ValidateCsvFile(
    const char* label,
    const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        std::cerr
            << "[CSV] " << label
            << " was not created: " << path.string();
        if (error) std::cerr << " (" << error.message() << ')';
        std::cerr << '\n';
        return false;
    }

    error.clear();
    if (!std::filesystem::is_regular_file(path, error) || error) {
        std::cerr
            << "[CSV] " << label
            << " is not a regular file: " << path.string();
        if (error) std::cerr << " (" << error.message() << ')';
        std::cerr << '\n';
        return false;
    }

    error.clear();
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0) {
        std::cerr
            << "[CSV] " << label
            << " is empty or unreadable: " << path.string();
        if (error) std::cerr << " (" << error.message() << ')';
        std::cerr << '\n';
        return false;
    }

    std::cout
        << "[CSV] verified " << label
        << ": " << size << " bytes, "
        << path.string() << '\n';
    return true;
}

bool ValidateNonEyeTrackerCsvOutputs(
    const NonEyeTrackerCsvOutputs& outputs)
{
    const bool timestampValid =
        ValidateCsvFile("timestamp CSV", outputs.timestamp);
    const bool imuValid =
        ValidateCsvFile("IMU CSV", outputs.imu);
    const bool vstLeftValid =
        ValidateCsvFile(
            "VST left metadata CSV",
            outputs.vstLeftMetadata);
    const bool vstRightValid =
        ValidateCsvFile(
            "VST right metadata CSV",
            outputs.vstRightMetadata);
    return timestampValid && imuValid && vstLeftValid && vstRightValid;
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

int main(int argc, char** argv)
{
    const auto nonEyeTrackerCsvOutputs =
        DualIC4Varjo::ResolveNonEyeTrackerCsvOutputs(argc, argv);
    if (!DualIC4Varjo::PrepareNonEyeTrackerCsvOutputs(
            nonEyeTrackerCsvOutputs)) {
        return 1;
    }

    DualIC4Varjo::ImuLoadServiceHook::configure(argc, argv);
    DualIC4Varjo::VstLoadServiceHook::configure(argc, argv);
    DualIC4Varjo::EyeTrackerLoadServiceHook::configure(argc, argv);

    DualIC4Varjo::TimestampLoadService timestampService(argc, argv);
    if (!timestampService.start()) {
        std::cerr
            << "Timestamp service failed to start: "
            << timestampService.lastError()
            << '\n';
        return 1;
    }

    int result = 1;
    try {
        result = DualIC4VarjoBaseMain(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr
            << "Unhandled application exception: "
            << exception.what()
            << '\n';
        result = 1;
    } catch (...) {
        std::cerr << "Unhandled unknown application exception.\n";
        result = 1;
    }

    // Stop the highest-rate services first. EyeTracker drains gaze processing and
    // its CSV before the lighter IMU/timestamp services are finalized.
    DualIC4Varjo::EyeTrackerLoadServiceHook::stop();
    DualIC4Varjo::VstLoadServiceHook::stop();
    DualIC4Varjo::ImuLoadServiceHook::stop();
    timestampService.stop();

    std::cout
        << "[TIMESTAMP] samples="
        << timestampService.sampleCount()
        << " failed="
        << timestampService.failedSampleCount()
        << " CSV="
        << timestampService.outputPath().string()
        << '\n';

    std::cout
        << "[IMU] received="
        << DualIC4Varjo::ImuLoadServiceHook::receivedCount()
        << " processed="
        << DualIC4Varjo::ImuLoadServiceHook::processedCount()
        << " written="
        << DualIC4Varjo::ImuLoadServiceHook::writtenCount()
        << " dropped="
        << DualIC4Varjo::ImuLoadServiceHook::droppedCount()
        << " CSV="
        << DualIC4Varjo::ImuLoadServiceHook::outputPath().string()
        << '\n';

    std::cout
        << "[VST] leftReceived="
        << DualIC4Varjo::VstLoadServiceHook::leftReceivedCount()
        << " rightReceived="
        << DualIC4Varjo::VstLoadServiceHook::rightReceivedCount()
        << " leftProcessed="
        << DualIC4Varjo::VstLoadServiceHook::leftProcessedCount()
        << " rightProcessed="
        << DualIC4Varjo::VstLoadServiceHook::rightProcessedCount()
        << " dropped="
        << DualIC4Varjo::VstLoadServiceHook::droppedCount()
        << " writeFailures="
        << DualIC4Varjo::VstLoadServiceHook::writeFailureCount()
        << " output="
        << DualIC4Varjo::VstLoadServiceHook::outputDirectory().string()
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

    const std::string imuError =
        DualIC4Varjo::ImuLoadServiceHook::lastError();
    if (!imuError.empty()) {
        std::cerr << "[IMU] last error: " << imuError << '\n';
    }

    const std::string vstError =
        DualIC4Varjo::VstLoadServiceHook::lastError();
    if (!vstError.empty()) {
        std::cerr << "[VST] last error: " << vstError << '\n';
    }

    const std::string eyeTrackerError =
        DualIC4Varjo::EyeTrackerLoadServiceHook::lastError();
    if (!eyeTrackerError.empty()) {
        std::cerr
            << "[EYETRACKER] last error: "
            << eyeTrackerError
            << '\n';
    }

    const bool nonEyeTrackerCsvValid =
        DualIC4Varjo::ValidateNonEyeTrackerCsvOutputs(
            nonEyeTrackerCsvOutputs);
    if (!nonEyeTrackerCsvValid && result == 0) {
        result = 1;
    }

    return result;
}
