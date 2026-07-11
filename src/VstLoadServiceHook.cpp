#include "VstLoadServiceHook.hpp"

#include <VarjoToolkit/Services/VST/VarjoVSTService.hpp>

#include <Windows.h>

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
std::filesystem::path gOutputDirectory;
std::wstring gBaseFilename;
std::string gLastError;
std::unique_ptr<VarjoVSTService> gService;
std::atomic<VarjoVSTService*> gServiceRaw{nullptr};
std::thread gStartThread;
std::atomic<bool> gStartRequested{false};
std::atomic<std::uint64_t> gFinalLeftReceived{0};
std::atomic<std::uint64_t> gFinalRightReceived{0};
std::atomic<std::uint64_t> gFinalLeftProcessed{0};
std::atomic<std::uint64_t> gFinalRightProcessed{0};
std::atomic<std::uint64_t> gFinalDropped{0};
std::atomic<std::uint64_t> gFinalWriteFailures{0};

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
    return value.empty() ? std::string("vst_test") : value;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required);
    return result;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return "wide string conversion failed";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

void StartService(std::shared_ptr<varjo_Session> session) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (!session) {
            gLastError = "renderer Varjo session is null";
            return;
        }
        if (gService) {
            gServiceRaw.store(gService.get(), std::memory_order_release);
            return;
        }

        gService = std::make_unique<VarjoVSTService>(
            session,
            gOutputDirectory.wstring(),
            gBaseFilename,
            180);
        if (!gService->start()) {
            gLastError = WideToUtf8(gService->lastError());
            gService.reset();
            return;
        }

        gServiceRaw.store(gService.get(), std::memory_order_release);
        std::cout
            << "[VST] service started with CPU NV12 left/right capture\n"
            << "[VST] output directory: "
            << gOutputDirectory.string()
            << '\n'
            << "[VST] ffmpeg must be available on PATH\n";
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = exception.what();
        gService.reset();
        gServiceRaw.store(nullptr, std::memory_order_release);
    } catch (...) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = "unknown VST service initialization failure";
        gService.reset();
        gServiceRaw.store(nullptr, std::memory_order_release);
    }
}

} // namespace

void VstLoadServiceHook::configure(int argc, char** argv)
{
    stop();

    const std::filesystem::path baseDirectory =
        FindArgumentValue(argc, argv, "--dir", "logs");
    const std::string project = SanitizeFilename(
        FindArgumentValue(argc, argv, "--project", "vst_test"));

    std::lock_guard<std::mutex> lock(gStateMutex);
    gOutputDirectory =
        baseDirectory /
        "service_load" /
        (project + "_vst");
    gBaseFilename = Utf8ToWide(project);
    gLastError.clear();
    gStartRequested.store(false, std::memory_order_release);
    gFinalLeftReceived.store(0, std::memory_order_release);
    gFinalRightReceived.store(0, std::memory_order_release);
    gFinalLeftProcessed.store(0, std::memory_order_release);
    gFinalRightProcessed.store(0, std::memory_order_release);
    gFinalDropped.store(0, std::memory_order_release);
    gFinalWriteFailures.store(0, std::memory_order_release);
}

void VstLoadServiceHook::ensureStarted(
    const std::shared_ptr<varjo_Session>& session) noexcept
{
    if (gServiceRaw.load(std::memory_order_acquire)) {
        return;
    }

    bool expected = false;
    if (!gStartRequested.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    try {
        gStartThread = std::thread(
            [session] {
                StartService(session);
            });
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = exception.what();
    } catch (...) {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastError = "failed to create VST initialization thread";
    }
}

void VstLoadServiceHook::stop() noexcept
{
    if (gStartThread.joinable()) {
        try {
            gStartThread.join();
        } catch (...) {
        }
    }

    gServiceRaw.store(nullptr, std::memory_order_release);

    std::unique_ptr<VarjoVSTService> service;
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        service = std::move(gService);
    }

    if (service) {
        try {
            service->stop();
        } catch (...) {
        }

        gFinalLeftReceived.store(
            service->leftReceivedFrameCount(),
            std::memory_order_release);
        gFinalRightReceived.store(
            service->rightReceivedFrameCount(),
            std::memory_order_release);
        gFinalLeftProcessed.store(
            service->leftProcessedFrameCount(),
            std::memory_order_release);
        gFinalRightProcessed.store(
            service->rightProcessedFrameCount(),
            std::memory_order_release);
        gFinalDropped.store(
            service->droppedFrameCount(),
            std::memory_order_release);
        gFinalWriteFailures.store(
            service->writeFailureCount(),
            std::memory_order_release);
    }
}

std::filesystem::path VstLoadServiceHook::outputDirectory()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gOutputDirectory;
}

std::string VstLoadServiceHook::lastError()
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gLastError;
}

std::uint64_t VstLoadServiceHook::leftReceivedCount() noexcept
{
    VarjoVSTService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->leftReceivedFrameCount()
        : gFinalLeftReceived.load(std::memory_order_acquire);
}

std::uint64_t VstLoadServiceHook::rightReceivedCount() noexcept
{
    VarjoVSTService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->rightReceivedFrameCount()
        : gFinalRightReceived.load(std::memory_order_acquire);
}

std::uint64_t VstLoadServiceHook::leftProcessedCount() noexcept
{
    VarjoVSTService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->leftProcessedFrameCount()
        : gFinalLeftProcessed.load(std::memory_order_acquire);
}

std::uint64_t VstLoadServiceHook::rightProcessedCount() noexcept
{
    VarjoVSTService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->rightProcessedFrameCount()
        : gFinalRightProcessed.load(std::memory_order_acquire);
}

std::uint64_t VstLoadServiceHook::droppedCount() noexcept
{
    VarjoVSTService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->droppedFrameCount()
        : gFinalDropped.load(std::memory_order_acquire);
}

std::uint64_t VstLoadServiceHook::writeFailureCount() noexcept
{
    VarjoVSTService* service =
        gServiceRaw.load(std::memory_order_acquire);
    return service
        ? service->writeFailureCount()
        : gFinalWriteFailures.load(std::memory_order_acquire);
}

} // namespace DualIC4Varjo
