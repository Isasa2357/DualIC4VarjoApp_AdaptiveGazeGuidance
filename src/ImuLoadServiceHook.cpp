#include "ImuLoadServiceHook.hpp"

#include <VarjoToolkit/Services/IMU/VarjoIMUService.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace DualIC4Varjo {
namespace {

std::mutex gStateMutex;
std::filesystem::path gOutputPath;
std::string gLastError;
std::unique_ptr<VarjoIMUService> gService;
std::atomic<VarjoIMUService*> gServiceRaw{nullptr};
std::atomic<std::uint64_t> gFinalReceived{0};
std::atomic<std::uint64_t> gFinalProcessed{0};
std::atomic<std::uint64_t> gFinalWritten{0};
std::atomic<std::uint64_t> gFinalDropped{0};

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
    return value.empty() ? std::string("imu_test") : value;
}

std::filesystem::path ResolveOutputPath(int argc, char** argv)
{
    const std::filesystem::path baseDirectory =
        FindArgumentValue(argc, argv, "--dir", "logs");
    const std::string project = SanitizeFilename(
        FindArgumentValue(argc, argv, "--project", "imu_test"));
    return baseDirectory /
           "service_load" /
           (project + "_imu.csv");
}

bool EnsureStarted(
    const std::shared_ptr<varjo_Session>& session) noexcept
{
    if (gServiceRaw.load(std::memory_order_acquire)) {
        return true;
    }

    try {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gService) {
            gServiceRaw.store(gService.get(), std::memory_order_release);
            return true;
        }
        if (!session) {
            gLastError = "renderer Varjo session is null";
            return false;
        }

        gService = std::make_unique<VarjoIMUService>(
            session,
            gOutputPath.wstring(),
            180);
        if (!gService->start()) {
            const std::wstring error = gService->lastError();
            gLastError.assign(error.begin(), error.end());
            gService.reset();
            return false;
        }

        gServiceRaw.store(gService.get(), std::memory_order_release);
        std::cout
            << "[IMU] service started from renderer FrameInfo snapshots\n"
            << "[IMU] CSV: "
            << gOutputPath.string()
            << '\n';
        return true;
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = exception.what();
        gService.reset();
        gServiceRaw.store(nullptr, std::memory_order_release);
        return false;
    } catch (...) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = "unknown IMU service initialization failure";
        gService.reset();
        gServiceRaw.store(nullptr, std::memory_order_release);
        return false;
    }
}

} // namespace

void ImuLoadServiceHook::configure(int argc, char** argv)
{
    stop();
    std::lock_guard<std::mutex> lock(gStateMutex);
    gOutputPath = ResolveOutputPath(argc, argv);
    gLastError.clear();
    gFinalReceived.store(0, std::memory_order_release);
    gFinalProcessed.store(0, std::memory_order_release);
    gFinalWritten.store(0, std::memory_order_release);
    gFinalDropped.store(0, std::memory_order_release);
}

void ImuLoadServiceHook::submit(
    const VarjoFrameInfoSnapshot& snapshot,
    const std::shared_ptr<varjo_Session>& session) noexcept
{
    if (!snapshot.valid) return;

    VarjoIMUService* service =
        gServiceRaw.load(std::memory_order_acquire);
    if (!service) {
        if (!EnsureStarted(session)) return;
        service = gServiceRaw.load(std::memory_order_acquire);
    }

    if (service && !service->submitFrameInfo(snapshot)) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (gLastError.empty()) {
            gLastError = "VarjoIMUService rejected a renderer snapshot";
        }
    }
}

void ImuLoadServiceHook::stop() noexcept
{
    gServiceRaw.store(nullptr, std::memory_order_release);

    std::unique_ptr<VarjoIMUService> service;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        service = std::move(gService);
    }
    if (service) {
        try {
            service->stop();
        } catch (...) {
        }
        gFinalReceived.store(
            service->receivedSampleCount(),
            std::memory_order_release);
        gFinalProcessed.store(
            service->processedSampleCount(),
            std::memory_order_release);
        gFinalWritten.store(
            service->writtenSampleCount(),
            std::memory_order_release);
        gFinalDropped.store(
            service->droppedSampleCount(),
            std::memory_order_release);
    }
}

std::filesystem::path ImuLoadServiceHook::outputPath()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gOutputPath;
}

std::string ImuLoadServiceHook::lastError()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gLastError;
}

std::uint64_t ImuLoadServiceHook::receivedCount() noexcept
{
    VarjoIMUService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->receivedSampleCount()
        : gFinalReceived.load(std::memory_order_acquire);
}

std::uint64_t ImuLoadServiceHook::processedCount() noexcept
{
    VarjoIMUService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->processedSampleCount()
        : gFinalProcessed.load(std::memory_order_acquire);
}

std::uint64_t ImuLoadServiceHook::writtenCount() noexcept
{
    VarjoIMUService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->writtenSampleCount()
        : gFinalWritten.load(std::memory_order_acquire);
}

std::uint64_t ImuLoadServiceHook::droppedCount() noexcept
{
    VarjoIMUService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->droppedSampleCount()
        : gFinalDropped.load(std::memory_order_acquire);
}

} // namespace DualIC4Varjo
