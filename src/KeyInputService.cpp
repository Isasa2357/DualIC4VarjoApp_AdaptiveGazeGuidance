#include "KeyInputService.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

constexpr auto kPollPeriod = std::chrono::milliseconds(5);

bool IsKeyboardVirtualKey(int virtualKey) noexcept
{
    switch (virtualKey) {
    case VK_LBUTTON:
    case VK_RBUTTON:
    case VK_CANCEL:
    case VK_MBUTTON:
    case VK_XBUTTON1:
    case VK_XBUTTON2:
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_MENU:
        return false;
    default:
        return virtualKey >= VK_BACK && virtualKey <= 0xFE;
    }
}

std::string CsvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char character : value) {
        if (character == '"') escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

std::string VirtualKeyName(int virtualKey)
{
    switch (virtualKey) {
    case VK_LSHIFT: return "LEFT_SHIFT";
    case VK_RSHIFT: return "RIGHT_SHIFT";
    case VK_LCONTROL: return "LEFT_CTRL";
    case VK_RCONTROL: return "RIGHT_CTRL";
    case VK_LMENU: return "LEFT_ALT";
    case VK_RMENU: return "RIGHT_ALT";
    case VK_LEFT: return "LEFT";
    case VK_RIGHT: return "RIGHT";
    case VK_UP: return "UP";
    case VK_DOWN: return "DOWN";
    case VK_ESCAPE: return "ESCAPE";
    case VK_RETURN: return "ENTER";
    case VK_SPACE: return "SPACE";
    case VK_TAB: return "TAB";
    case VK_BACK: return "BACKSPACE";
    case VK_DELETE: return "DELETE";
    default:
        break;
    }

    const UINT scanCode = MapVirtualKeyW(
        static_cast<UINT>(virtualKey),
        MAPVK_VK_TO_VSC);
    LONG parameter = static_cast<LONG>(scanCode << 16u);
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT ||
        virtualKey == VK_UP || virtualKey == VK_DOWN ||
        virtualKey == VK_INSERT || virtualKey == VK_DELETE ||
        virtualKey == VK_HOME || virtualKey == VK_END ||
        virtualKey == VK_PRIOR || virtualKey == VK_NEXT ||
        virtualKey == VK_DIVIDE || virtualKey == VK_NUMLOCK) {
        parameter |= (1L << 24);
    }

    wchar_t wideName[128]{};
    const int length = GetKeyNameTextW(
        parameter,
        wideName,
        static_cast<int>(std::size(wideName)));
    if (length > 0) {
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8, 0, wideName, length,
            nullptr, 0, nullptr, nullptr);
        if (utf8Length > 0) {
            std::string name(static_cast<std::size_t>(utf8Length), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, wideName, length,
                name.data(), utf8Length, nullptr, nullptr);
            return name;
        }
    }

    std::ostringstream stream;
    stream << "VK_0x" << std::uppercase << std::hex
           << std::setw(2) << std::setfill('0') << virtualKey;
    return stream.str();
}

std::string LocalIso8601(
    const std::chrono::system_clock::time_point& timePoint)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        timePoint.time_since_epoch());
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0')
           << (milliseconds.count() % 1000);
    return stream.str();
}

bool IsDown(int virtualKey) noexcept
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool AnyShiftDown() noexcept
{
    return IsDown(VK_LSHIFT) || IsDown(VK_RSHIFT);
}

bool AnyCtrlDown() noexcept
{
    return IsDown(VK_LCONTROL) || IsDown(VK_RCONTROL);
}

bool AnyAltDown() noexcept
{
    return IsDown(VK_LMENU) || IsDown(VK_RMENU);
}

} // namespace

KeyInputService::KeyInputService(std::filesystem::path outputPath)
    : outputPath_(std::move(outputPath))
{
}

KeyInputService::~KeyInputService()
{
    stop();
}

bool KeyInputService::start()
{
    stop();

    try {
        std::error_code error;
        std::filesystem::create_directories(outputPath_.parent_path(), error);
        if (error) {
            setError("failed to create key-input output directory: " + error.message());
            return false;
        }

        output_.open(outputPath_, std::ios::out | std::ios::trunc);
        if (!output_.is_open()) {
            setError("failed to open key-input CSV: " + outputPath_.string());
            return false;
        }

        output_
            << "event_index,system_unix_us,system_local_iso8601,"
            << "event,key_code,key_name,shift_down,ctrl_down,alt_down\n";
        output_.flush();

        eventCount_.store(0, std::memory_order_release);
        stopRequested_.store(false, std::memory_order_release);
        worker_ = std::thread(&KeyInputService::workerMain, this);
        std::cout
            << "[KEYINPUT] service started at 5 ms polling\n"
            << "[KEYINPUT] CSV: " << outputPath_.string() << '\n';
        return true;
    } catch (const std::exception& exception) {
        setError(exception.what());
        stop();
        return false;
    }
}

void KeyInputService::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        try { worker_.join(); } catch (...) {}
    }
    if (output_.is_open()) {
        output_.flush();
        output_.close();
    }
}

std::filesystem::path KeyInputService::outputPath() const
{
    return outputPath_;
}

std::string KeyInputService::lastError() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return lastError_;
}

std::uint64_t KeyInputService::eventCount() const noexcept
{
    return eventCount_.load(std::memory_order_acquire);
}

void KeyInputService::workerMain() noexcept
{
    std::array<bool, 256> previous{};
    for (int virtualKey = 0; virtualKey < 256; ++virtualKey) {
        if (IsKeyboardVirtualKey(virtualKey)) {
            previous[static_cast<std::size_t>(virtualKey)] = IsDown(virtualKey);
        }
    }

    try {
        auto nextPoll = std::chrono::steady_clock::now();
        while (!stopRequested_.load(std::memory_order_acquire)) {
            nextPoll += kPollPeriod;

            for (int virtualKey = 0; virtualKey < 256; ++virtualKey) {
                if (!IsKeyboardVirtualKey(virtualKey)) continue;

                const bool current = IsDown(virtualKey);
                bool& old = previous[static_cast<std::size_t>(virtualKey)];
                if (current == old) continue;
                old = current;

                const auto systemTime = std::chrono::system_clock::now();
                const auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    systemTime.time_since_epoch()).count();
                const std::uint64_t eventIndex =
                    eventCount_.fetch_add(1, std::memory_order_acq_rel);

                output_
                    << eventIndex << ','
                    << unixUs << ','
                    << LocalIso8601(systemTime) << ','
                    << (current ? "down" : "up") << ','
                    << virtualKey << ','
                    << CsvEscape(VirtualKeyName(virtualKey)) << ','
                    << (AnyShiftDown() ? 1 : 0) << ','
                    << (AnyCtrlDown() ? 1 : 0) << ','
                    << (AnyAltDown() ? 1 : 0)
                    << '\n';
                output_.flush();

                if (!output_) {
                    throw std::runtime_error("key-input CSV write failed");
                }
            }

            std::this_thread::sleep_until(nextPoll);
        }
    } catch (const std::exception& exception) {
        setError(exception.what());
        stopRequested_.store(true, std::memory_order_release);
    } catch (...) {
        setError("unknown key-input worker failure");
        stopRequested_.store(true, std::memory_order_release);
    }
}

void KeyInputService::setError(std::string message) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastError_ = std::move(message);
    } catch (...) {
    }
}

} // namespace DualIC4Varjo
