#include "EyeTrackerLoadServiceHook.hpp"
#include "ImuLoadServiceHook.hpp"
#include "TimestampLoadService.hpp"
#include "VstLoadServiceHook.hpp"

namespace DualIC4Varjo {
namespace {

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
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
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

    return result;
}
