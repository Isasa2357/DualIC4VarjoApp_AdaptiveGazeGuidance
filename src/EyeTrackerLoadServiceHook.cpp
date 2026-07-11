#include "EyeTrackerLoadServiceHook.hpp"
#include "ExperimentOutput.hpp"

#include <VarjoToolkit/Services/EyeTracking/VarjoEyeTrackingService.hpp>

#include <atomic>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace DualIC4Varjo {
namespace {

std::mutex gStateMutex;
std::filesystem::path gOutputPath;
std::string gLastError;
std::unique_ptr<VarjoEyeTrackingService> gService;
std::atomic<VarjoEyeTrackingService*> gServiceRaw{nullptr};
std::thread gStartThread;
std::atomic<bool> gStartRequested{false};
std::atomic<std::uint64_t> gFinalReceived{0};
std::atomic<std::uint64_t> gFinalProcessed{0};
std::atomic<std::uint64_t> gFinalWritten{0};
std::atomic<std::uint64_t> gFinalDropped{0};
std::atomic<std::uint64_t> gFinalSubmittedFrameInfo{0};
std::atomic<std::uint64_t> gFinalDroppedFrameInfo{0};

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

std::string SanitizeFilename(std::string value)
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
    return value.empty() ? std::string("eyetracker_test") : value;
}

std::filesystem::path ResolveOutputPath(int argc, char** argv)
{
    if (const auto active = ActiveExperimentOutputLayout()) {
        return active->directory / "eyetracking.csv";
    }

    const std::filesystem::path baseDirectory =
        FindArgumentValue(argc, argv, "--dir", "logs");
    const std::string project = SanitizeFilename(
        FindArgumentValue(argc, argv, "--project", "eyetracker_test"));
    return baseDirectory /
           "service_load" /
           (project + "_eyetracking.csv");
}

void StartService(std::shared_ptr<varjo_Session> session) noexcept
{
    try {
        std::error_code directoryError;
        std::filesystem::create_directories(
            gOutputPath.parent_path(),
            directoryError);
        if (directoryError) {
            std::lock_guard<std::mutex> lock(gStateMutex);
            gLastError =
                "failed to create EyeTracker output directory: " +
                directoryError.message();
            return;
        }

        std::lock_guard<std::mutex> lock(gStateMutex);
        if (!session) {
            gLastError = "renderer Varjo session is null";
            return;
        }
        if (gService) {
            gServiceRaw.store(gService.get(), std::memory_order_release);
            return;
        }

        gService = std::make_unique<VarjoEyeTrackingService>(
            session,
            VarjoEyeTrackingProvider::OutputFilterType::STANDARD,
            VarjoEyeTrackingProvider::OutputFrequency::MAXIMUM,
            gOutputPath.u8string(),
            20000,
            5);
        if (!gService->start()) {
            gLastError =
                "VarjoEyeTrackingService failed to open its CSV output";
            gService.reset();
            return;
        }

        gServiceRaw.store(gService.get(), std::memory_order_release);
        std::cout
            << "[EYETRACKER] service started: filter=STANDARD frequency=MAXIMUM poll=5 ms\n"
            << "[EYETRACKER] CSV: "
            << gOutputPath.string()
            << '\n';
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = exception.what();
        gService.reset();
        gServiceRaw.store(nullptr, std::memory_order_release);
    } catch (...) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = "unknown EyeTracker service initialization failure";
        gService.reset();
        gServiceRaw.store(nullptr, std::memory_order_release);
    }
}

} // namespace

void EyeTrackerLoadServiceHook::configure(int argc, char** argv)
{
    stop();

    std::lock_guard<std::mutex> lock(gStateMutex);
    gOutputPath = ResolveOutputPath(argc, argv);
    gLastError.clear();
    gStartRequested.store(false, std::memory_order_release);
    gFinalReceived.store(0, std::memory_order_release);
    gFinalProcessed.store(0, std::memory_order_release);
    gFinalWritten.store(0, std::memory_order_release);
    gFinalDropped.store(0, std::memory_order_release);
    gFinalSubmittedFrameInfo.store(0, std::memory_order_release);
    gFinalDroppedFrameInfo.store(0, std::memory_order_release);
}

void EyeTrackerLoadServiceHook::submit(
    const VarjoFrameInfoSnapshot& snapshot,
    const std::shared_ptr<varjo_Session>& session) noexcept
{
    if (!snapshot.valid) return;

    VarjoEyeTrackingService* service =
        gServiceRaw.load(std::memory_order_acquire);
    if (!service) {
        bool expected = false;
        if (gStartRequested.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            try {
                gStartThread = std::thread([session] { StartService(session); });
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(gStateMutex);
                gLastError = exception.what();
            } catch (...) {
                std::lock_guard<std::mutex> lock(gStateMutex);
                gLastError =
                    "failed to create EyeTracker initialization thread";
            }
        }
        return;
    }

    if (!service->submitFrameInfo(snapshot)) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gLastError.empty()) {
            gLastError = "EyeTracker rejected a renderer FrameInfo snapshot";
        }
    }
}

void EyeTrackerLoadServiceHook::stop() noexcept
{
    if (gStartThread.joinable()) {
        try { gStartThread.join(); } catch (...) {}
    }

    gServiceRaw.store(nullptr, std::memory_order_release);

    std::unique_ptr<VarjoEyeTrackingService> service;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        service = std::move(gService);
    }

    if (service) {
        try { service->stop(); } catch (...) {}
        gFinalReceived.store(service->receivedSampleCount(), std::memory_order_release);
        gFinalProcessed.store(service->processedSampleCount(), std::memory_order_release);
        gFinalWritten.store(service->writtenSampleCount(), std::memory_order_release);
        gFinalDropped.store(service->droppedSampleCount(), std::memory_order_release);
        gFinalSubmittedFrameInfo.store(service->submittedFrameInfoCount(), std::memory_order_release);
        gFinalDroppedFrameInfo.store(service->droppedFrameInfoCount(), std::memory_order_release);
    }
}

std::filesystem::path EyeTrackerLoadServiceHook::outputPath()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gOutputPath;
}

std::string EyeTrackerLoadServiceHook::lastError()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gLastError;
}

std::uint64_t EyeTrackerLoadServiceHook::receivedSampleCount() noexcept
{
    VarjoEyeTrackingService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->receivedSampleCount() : gFinalReceived.load(std::memory_order_acquire);
}

std::uint64_t EyeTrackerLoadServiceHook::processedSampleCount() noexcept
{
    VarjoEyeTrackingService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->processedSampleCount() : gFinalProcessed.load(std::memory_order_acquire);
}

std::uint64_t EyeTrackerLoadServiceHook::writtenSampleCount() noexcept
{
    VarjoEyeTrackingService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->writtenSampleCount() : gFinalWritten.load(std::memory_order_acquire);
}

std::uint64_t EyeTrackerLoadServiceHook::droppedSampleCount() noexcept
{
    VarjoEyeTrackingService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->droppedSampleCount() : gFinalDropped.load(std::memory_order_acquire);
}

std::uint64_t EyeTrackerLoadServiceHook::submittedFrameInfoCount() noexcept
{
    VarjoEyeTrackingService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->submittedFrameInfoCount() : gFinalSubmittedFrameInfo.load(std::memory_order_acquire);
}

std::uint64_t EyeTrackerLoadServiceHook::droppedFrameInfoCount() noexcept
{
    VarjoEyeTrackingService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->droppedFrameInfoCount() : gFinalDroppedFrameInfo.load(std::memory_order_acquire);
}

} // namespace DualIC4Varjo
