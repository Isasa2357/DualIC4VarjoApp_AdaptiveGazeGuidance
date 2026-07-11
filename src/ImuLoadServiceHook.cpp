#include "ImuLoadServiceHook.hpp"
#include "ExperimentOutput.hpp"

#include <VarjoToolkit/Services/IMU/VarjoIMUService.hpp>

#include <Windows.h>

#include <atomic>
#include <exception>
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
std::atomic<bool> gStartAttempted{false};
std::atomic<std::uint64_t> gFinalReceived{0};
std::atomic<std::uint64_t> gFinalProcessed{0};
std::atomic<std::uint64_t> gFinalWritten{0};
std::atomic<std::uint64_t> gFinalDropped{0};

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};

    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "<wide-to-utf8 conversion failed>";

    std::string result(static_cast<std::size_t>(required), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr);
    return converted == required
        ? result
        : std::string("<wide-to-utf8 conversion failed>");
}

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
    if (const auto active = ActiveExperimentOutputLayout()) {
        return active->directory /
               (active->resolvedProjectName + "_imu.csv");
    }

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
    if (gServiceRaw.load(std::memory_order_acquire)) return true;

    bool expected = false;
    if (!gStartAttempted.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (!session) {
            gLastError = "renderer Varjo session is null";
            return false;
        }

        gService = std::make_unique<VarjoIMUService>(
            session,
            gOutputPath.wstring(),
            180);
        if (!gService->start()) {
            gLastError = WideToUtf8(gService->lastError());
            gService.reset();
            return false;
        }

        gServiceRaw.store(gService.get(), std::memory_order_release);
        std::cout
            << "[IMU] service started from renderer FrameInfo snapshots\n"
            << "[IMU] CSV: " << gOutputPath.string() << '\n';
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
    gStartAttempted.store(false, std::memory_order_release);
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
        try { service->stop(); } catch (...) {}
        gFinalReceived.store(service->receivedSampleCount(), std::memory_order_release);
        gFinalProcessed.store(service->processedSampleCount(), std::memory_order_release);
        gFinalWritten.store(service->writtenSampleCount(), std::memory_order_release);
        gFinalDropped.store(service->droppedSampleCount(), std::memory_order_release);
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
    VarjoIMUService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->receivedSampleCount() : gFinalReceived.load(std::memory_order_acquire);
}

std::uint64_t ImuLoadServiceHook::processedCount() noexcept
{
    VarjoIMUService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->processedSampleCount() : gFinalProcessed.load(std::memory_order_acquire);
}

std::uint64_t ImuLoadServiceHook::writtenCount() noexcept
{
    VarjoIMUService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->writtenSampleCount() : gFinalWritten.load(std::memory_order_acquire);
}

std::uint64_t ImuLoadServiceHook::droppedCount() noexcept
{
    VarjoIMUService* service = gServiceRaw.load(std::memory_order_acquire);
    return service ? service->droppedSampleCount() : gFinalDropped.load(std::memory_order_acquire);
}

} // namespace DualIC4Varjo
