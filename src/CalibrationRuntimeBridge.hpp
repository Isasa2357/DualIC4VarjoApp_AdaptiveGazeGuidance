#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CheckerboardStereoCalibration.hpp"
#include "StereoCalibrationSupport.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace DualIC4Varjo::CalibrationRuntimeBridge {

inline std::atomic_bool gCalibrationActive{false};
inline VarjoXR::XRPlane* gPlane = nullptr;

struct PlanePostProcessRuntimeConfig {
    StereoPostProcessSettings settings{};

    // Total default duration remains 4 seconds:
    // 1s open + 2s hold + 1s close.
    float revealOpenSeconds = 1.0f;
    float revealHoldSeconds = 2.0f;
    float revealCloseSeconds = 1.0f;

    // Used by the darken mode. The reveal animation expands the clear circle to
    // this radius measured against the image short axis. 4.0 covers ordinary
    // aspect ratios even if the center is moved near an edge.
    float revealExpandedRadiusShortAxis01 = 4.0f;

    // Default to F23 so an external keypad / remapper can trigger the effect
    // without colliding with ordinary desktop shortcuts.
    int revealVirtualKey = VK_F23;
};

inline const char* PostProcessModeName(StereoPostProcessMode mode) noexcept
{
    switch (mode) {
    case StereoPostProcessMode::Darken: return "darken";
    case StereoPostProcessMode::Blur: return "blur";
    case StereoPostProcessMode::None:
    default:
        return "none";
    }
}

inline StereoPostProcessSettings MakeDarkenPostProcessSettings() noexcept
{
    StereoPostProcessSettings settings{};
    settings.mode = StereoPostProcessMode::Darken;
    settings.enabled = true;
    settings.centerX01 = 0.5f;
    settings.centerY01 = 0.5f;
    settings.radiusShortAxis01 = 0.25f;
    settings.edgeSoftnessShortAxis01 = 0.03f;
    settings.outsideBrightness = 0.5f;
    settings.blurRadiusPixels = 4.0f;
    settings.blurSigmaPixels = 2.0f;
    settings.blurStrength01 = 1.0f;
    return settings;
}

inline StereoPostProcessSettings MakeBlurPostProcessSettings() noexcept
{
    StereoPostProcessSettings settings{};
    settings.mode = StereoPostProcessMode::Blur;
    settings.enabled = true;
    settings.centerX01 = 0.5f;
    settings.centerY01 = 0.5f;
    settings.radiusShortAxis01 = 0.22f;
    settings.edgeSoftnessShortAxis01 = 0.015f;
    settings.outsideBrightness = 1.0f;
    settings.blurRadiusPixels = 4.0f;
    settings.blurSigmaPixels = 2.0f;
    settings.blurStrength01 = 1.0f;
    return settings;
}

inline std::wstring Lowercase(std::wstring value)
{
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

inline bool ApplyCommandLinePostProcessMode(
    PlanePostProcessRuntimeConfig& config,
    const std::wstring& rawValue) noexcept
{
    const std::wstring value = Lowercase(rawValue);
    if (value == L"none" ||
        value == L"off" ||
        value == L"disabled" ||
        value == L"disable") {
        config.settings = StereoPostProcessSettings{};
        config.settings.mode = StereoPostProcessMode::None;
        config.settings.enabled = false;
        return true;
    }
    if (value == L"darken") {
        config.settings = MakeDarkenPostProcessSettings();
        return true;
    }
    if (value == L"blur" || value == L"blue") {
        config.settings = MakeBlurPostProcessSettings();
        return true;
    }
    return false;
}

inline PlanePostProcessRuntimeConfig MakeInitialPostProcessRuntimeConfig()
{
    PlanePostProcessRuntimeConfig config;
    config.settings = StereoPostProcessSettings{};
    config.settings.mode = StereoPostProcessMode::None;
    config.settings.enabled = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool optionFound = false;
    bool optionValid = true;
    if (argv) {
        for (int index = 1; index < argc; ++index) {
            const std::wstring option = argv[index] ? argv[index] : L"";
            if (option == L"--postprocess") {
                optionFound = true;
                if (index + 1 < argc) {
                    optionValid = ApplyCommandLinePostProcessMode(
                        config,
                        argv[++index] ? argv[index] : L"");
                } else {
                    optionValid = false;
                }
            }
        }
        LocalFree(argv);
    }

    if (!optionFound) {
        std::cout
            << "[POSTPROCESS] mode=none (use --postprocess darken or --postprocess blur/blue to enable)\n";
    } else if (!optionValid) {
        config.settings = StereoPostProcessSettings{};
        config.settings.mode = StereoPostProcessMode::None;
        config.settings.enabled = false;
        std::cout
            << "[POSTPROCESS] invalid --postprocess value; postprocess disabled\n";
    } else {
        std::cout
            << "[POSTPROCESS] mode="
            << PostProcessModeName(config.settings.mode)
            << ", reveal_key=F23, open=" << config.revealOpenSeconds
            << "s, hold=" << config.revealHoldSeconds
            << "s, close=" << config.revealCloseSeconds << "s\n";
    }

    return config;
}

inline std::mutex& PostProcessConfigMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

inline PlanePostProcessRuntimeConfig& PostProcessConfigStorage()
{
    static PlanePostProcessRuntimeConfig config =
        MakeInitialPostProcessRuntimeConfig();
    return config;
}

inline PlanePostProcessRuntimeConfig GetPostProcessRuntimeConfig()
{
    std::lock_guard<std::mutex> lock(PostProcessConfigMutex());
    return PostProcessConfigStorage();
}

inline void SetPostProcessRuntimeConfig(
    const PlanePostProcessRuntimeConfig& config)
{
    std::lock_guard<std::mutex> lock(PostProcessConfigMutex());
    PostProcessConfigStorage() = config;
}

inline void SetPostProcessSettings(
    const StereoPostProcessSettings& settings)
{
    std::lock_guard<std::mutex> lock(PostProcessConfigMutex());
    PostProcessConfigStorage().settings = settings;
}

inline void RegisterPlane(VarjoXR::XRPlane& plane) noexcept
{
    gPlane = &plane;
}

class ArrowEdgeState {
public:
    bool left() { return pressed(VK_LEFT, leftDown_); }
    bool right() { return pressed(VK_RIGHT, rightDown_); }
    bool up() { return pressed(VK_UP, upDown_); }
    bool down() { return pressed(VK_DOWN, downDown_); }

private:
    static bool pressed(int key, bool& previous) noexcept
    {
        const bool current = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool edge = current && !previous;
        previous = current;
        return edge;
    }

    bool leftDown_ = false;
    bool rightDown_ = false;
    bool upDown_ = false;
    bool downDown_ = false;
};

inline bool KeyPressedEdge(int key, bool& previous) noexcept
{
    const bool current = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = current && !previous;
    previous = current;
    return edge;
}

inline int SanitizeRevealVirtualKey(int key) noexcept
{
    return key > 0 ? key : VK_F23;
}

inline const char* RevealVirtualKeyName(int key) noexcept
{
    switch (SanitizeRevealVirtualKey(key)) {
    case VK_F23: return "F23";
    case 'D': return "D";
    case 'F': return "F";
    default: return "custom key";
    }
}

inline float PositiveOr(float value, float fallback) noexcept
{
    return std::isfinite(value) && value > 0.0f ? value : fallback;
}

inline std::atomic<std::uint64_t>& GlobalRevealTriggerCounter() noexcept
{
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic_bool& GlobalRevealKeyDown() noexcept
{
    static std::atomic_bool down{false};
    return down;
}

inline std::atomic_int& GlobalRevealKeyStateKey() noexcept
{
    static std::atomic_int key{VK_F23};
    return key;
}

inline LRESULT CALLBACK GlobalRevealKeyboardHookProc(
    int code,
    WPARAM wParam,
    LPARAM lParam)
{
    if (code == HC_ACTION && lParam != 0) {
        const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const int configuredKey = SanitizeRevealVirtualKey(
            GetPostProcessRuntimeConfig().revealVirtualKey);

        const int previousKey = GlobalRevealKeyStateKey().load(
            std::memory_order_acquire);
        if (previousKey != configuredKey) {
            GlobalRevealKeyStateKey().store(
                configuredKey,
                std::memory_order_release);
            GlobalRevealKeyDown().store(false, std::memory_order_release);
        }

        if (event && static_cast<int>(event->vkCode) == configuredKey) {
            const bool keyDownEvent =
                wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool keyUpEvent =
                wParam == WM_KEYUP || wParam == WM_SYSKEYUP;

            if (keyDownEvent) {
                bool expected = false;
                if (GlobalRevealKeyDown().compare_exchange_strong(
                        expected,
                        true,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    GlobalRevealTriggerCounter().fetch_add(
                        1,
                        std::memory_order_acq_rel);
                }
            } else if (keyUpEvent) {
                GlobalRevealKeyDown().store(false, std::memory_order_release);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

inline void EnsureGlobalRevealKeyboardHookStarted()
{
    static std::atomic_bool started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    std::thread([]() noexcept {
        const HHOOK hook = SetWindowsHookExW(
            WH_KEYBOARD_LL,
            &GlobalRevealKeyboardHookProc,
            GetModuleHandleW(nullptr),
            0);
        if (!hook) {
            std::cerr
                << "[POSTPROCESS] global reveal key hook failed; "
                << "falling back to focused-window polling\n";
            return;
        }

        std::cout
            << "[POSTPROCESS] global reveal key hook active (default F23)\n";

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnhookWindowsHookEx(hook);
    }).detach();
}

inline bool ConsumeRevealKeyPressed(
    int revealVirtualKey,
    bool& focusedPollingPreviousDown)
{
    EnsureGlobalRevealKeyboardHookStarted();

    static thread_local std::uint64_t observedGlobalTriggerCount = 0;
    const std::uint64_t currentGlobalTriggerCount =
        GlobalRevealTriggerCounter().load(std::memory_order_acquire);
    if (currentGlobalTriggerCount != observedGlobalTriggerCount) {
        observedGlobalTriggerCount = currentGlobalTriggerCount;
        focusedPollingPreviousDown =
            (GetAsyncKeyState(revealVirtualKey) & 0x8000) != 0;
        return true;
    }

    return KeyPressedEdge(revealVirtualKey, focusedPollingPreviousDown);
}

inline void ApplyPlaneVisibilityFromKeyboard(VarjoXR::XRPlane& plane)
{
    static thread_local bool oDown = false;
    static thread_local bool visible = true;

    if (!KeyPressedEdge('O', oDown)) return;

    visible = !visible;
    const float alpha = visible ? 1.0f : 0.0f;
    plane.setTint({1.0f, 1.0f, 1.0f, alpha});
    std::cout
        << "[PLANE] Varjo plane "
        << (visible ? "visible" : "transparent")
        << " (rendering continues, O toggles)\n";
}

inline StereoPostProcessSettings MakeRadiusRevealSettings(
    const PlanePostProcessRuntimeConfig& config,
    float revealAmount01) noexcept
{
    StereoPostProcessSettings settings = config.settings;
    const float reveal = std::clamp(
        std::isfinite(revealAmount01) ? revealAmount01 : 0.0f,
        0.0f,
        1.0f);
    const float baseRadius = std::clamp(
        std::isfinite(settings.radiusShortAxis01)
            ? settings.radiusShortAxis01
            : 0.25f,
        0.0f,
        4.0f);
    const float expandedRadius = std::max(
        baseRadius,
        std::clamp(
            PositiveOr(config.revealExpandedRadiusShortAxis01, 4.0f),
            0.0f,
            4.0f));
    settings.radiusShortAxis01 =
        baseRadius + (expandedRadius - baseRadius) * reveal;
    return settings;
}

inline StereoPostProcessSettings MakeBlurRevealSettings(
    const PlanePostProcessRuntimeConfig& config,
    float revealAmount01) noexcept
{
    StereoPostProcessSettings settings = config.settings;
    const float reveal = std::clamp(
        std::isfinite(revealAmount01) ? revealAmount01 : 0.0f,
        0.0f,
        1.0f);
    settings.blurStrength01 = 1.0f - reveal;
    return settings;
}

inline void ApplyPlanePostProcessFromKeyboard(VarjoXR::XRPlane& plane)
{
    using Clock = std::chrono::steady_clock;

    static thread_local bool revealPollingDown = false;
    static thread_local int previousRevealVirtualKey = 0;
    static thread_local bool pulseActive = false;
    static thread_local Clock::time_point pulseStart{};

    const auto config = GetPostProcessRuntimeConfig();
    if (!config.settings.enabled ||
        config.settings.mode == StereoPostProcessMode::None) {
        pulseActive = false;
        UpdatePlanePostProcessState(plane, config.settings, 0.0f);
        return;
    }

    const int revealVirtualKey = SanitizeRevealVirtualKey(config.revealVirtualKey);
    if (previousRevealVirtualKey != revealVirtualKey) {
        revealPollingDown = false;
        previousRevealVirtualKey = revealVirtualKey;
    }

    const char* revealKeyName = RevealVirtualKeyName(revealVirtualKey);
    const auto now = Clock::now();
    const bool revealPressed = ConsumeRevealKeyPressed(
        revealVirtualKey,
        revealPollingDown);
    if (revealPressed && !pulseActive) {
        pulseActive = true;
        pulseStart = now;
        std::cout
            << "[POSTPROCESS] "
            << PostProcessModeName(config.settings.mode)
            << " reveal pulse started by " << revealKeyName << ": "
            << "open=" << config.revealOpenSeconds
            << "s, hold=" << config.revealHoldSeconds
            << "s, close=" << config.revealCloseSeconds << "s\n";
    } else if (revealPressed && pulseActive) {
        std::cout
            << "[POSTPROCESS] reveal pulse is active; "
            << revealKeyName << " input ignored until it closes\n";
    }

    float revealAmount = 0.0f;
    if (pulseActive) {
        const float openSeconds = PositiveOr(config.revealOpenSeconds, 1.0f);
        const float holdSeconds = std::max(0.0f, config.revealHoldSeconds);
        const float closeSeconds = PositiveOr(config.revealCloseSeconds, 1.0f);
        const float elapsedSeconds = std::chrono::duration<float>(
            now - pulseStart).count();
        if (elapsedSeconds < openSeconds) {
            revealAmount = elapsedSeconds / openSeconds;
        } else if (elapsedSeconds < openSeconds + holdSeconds) {
            revealAmount = 1.0f;
        } else if (elapsedSeconds < openSeconds + holdSeconds + closeSeconds) {
            const float closingElapsed = elapsedSeconds - openSeconds - holdSeconds;
            revealAmount = 1.0f - closingElapsed / closeSeconds;
        } else {
            pulseActive = false;
            revealAmount = 0.0f;
        }
        revealAmount = std::clamp(revealAmount, 0.0f, 1.0f);
    }

    if (config.settings.mode == StereoPostProcessMode::Darken) {
        const StereoPostProcessSettings animatedSettings =
            MakeRadiusRevealSettings(config, revealAmount);
        UpdatePlanePostProcessState(plane, animatedSettings, 0.0f);
    } else if (config.settings.mode == StereoPostProcessMode::Blur) {
        const StereoPostProcessSettings animatedSettings =
            MakeBlurRevealSettings(config, revealAmount);
        UpdatePlanePostProcessState(plane, animatedSettings, 0.0f);
    } else {
        UpdatePlanePostProcessState(plane, config.settings, 0.0f);
    }
}

inline void ApplyPlaneInputAfterRender(VarjoXR::XRPlane& plane)
{
    ApplyPlaneVisibilityFromKeyboard(plane);
    ApplyPlanePostProcessFromKeyboard(plane);

    if (!gCalibrationActive.load(std::memory_order_acquire)) return;

    static thread_local ArrowEdgeState keys;
    constexpr float moveStep = 0.01f;
    constexpr float resizeStep = 0.01f;
    constexpr float minimumWidth = 0.05f;
    constexpr float minimumDistance = 0.05f;

    const bool shift =
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    const bool left = keys.left();
    const bool right = keys.right();
    const bool up = keys.up();
    const bool down = keys.down();

    bool changed = false;
    auto& position = plane.transform().position;
    if (shift) {
        float widthDelta = 0.0f;
        if (left) widthDelta -= resizeStep;
        if (right) widthDelta += resizeStep;
        if (widthDelta != 0.0f) {
            const auto size = plane.size();
            if (size.x > 0.0f && size.y > 0.0f) {
                const float newWidth = (std::max)(minimumWidth, size.x + widthDelta);
                if (newWidth != size.x) {
                    plane.setSize({newWidth, newWidth * (size.y / size.x)});
                    changed = true;
                }
            }
        }
        if (up) {
            position.z -= moveStep;
            changed = true;
        }
        if (down) {
            const float newZ = (std::min)(-minimumDistance, position.z + moveStep);
            if (newZ != position.z) {
                position.z = newZ;
                changed = true;
            }
        }
    } else {
        if (left) { position.x -= moveStep; changed = true; }
        if (right) { position.x += moveStep; changed = true; }
        if (up) { position.y += moveStep; changed = true; }
        if (down) { position.y -= moveStep; changed = true; }
    }

    if (changed) {
        const auto size = plane.size();
        std::cout << std::fixed << std::setprecision(3)
                  << "[PLANE][CALIB] x=" << position.x
                  << " m, y=" << position.y
                  << " m, z=" << position.z
                  << " m, distance=" << (std::max)(0.0f, -position.z)
                  << " m, width=" << size.x
                  << " m, height=" << size.y << " m\n";
    }
}

inline HWND FindCalibrationWindow() noexcept
{
    return FindWindowA(nullptr, "Reference Stereo Calibration (affine_vertical)");
}

inline void MoveOpenCvWindowOffscreen(std::atomic_bool& stopRequested) noexcept
{
    while (!stopRequested.load(std::memory_order_acquire)) {
        if (HWND window = FindCalibrationWindow()) {
            LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
            extendedStyle |= WS_EX_TOOLWINDOW;
            extendedStyle &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
            SetWindowLongPtrW(window, GWL_EXSTYLE, extendedStyle);
            SetWindowPos(
                window,
                nullptr,
                -32000,
                -32000,
                1,
                1,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

inline void InjectEscapeForCalibrationAbort() noexcept
{
    if (HWND window = FindCalibrationWindow()) {
        PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(window, WM_KEYUP, VK_ESCAPE, 0);
    }

    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_ESCAPE;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_ESCAPE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

inline void WatchExternalStopForCalibrationAbort(
    std::atomic_bool& stopRequested,
    const std::atomic<bool>* externalStopRequested) noexcept
{
    if (!externalStopRequested) return;

    bool announced = false;
    auto lastInjection = std::chrono::steady_clock::now() -
        std::chrono::seconds(1);
    while (!stopRequested.load(std::memory_order_acquire)) {
        if (externalStopRequested->load(std::memory_order_acquire)) {
            if (!announced) {
                std::cout
                    << "[CALIB] external stop requested; aborting calibration\n";
                announced = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastInjection >= std::chrono::milliseconds(100)) {
                InjectEscapeForCalibrationAbort();
                lastInjection = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

inline CheckerboardCalibrationResult RunHeadlessCheckerboardStereoCalibration(
    IC4Ext::D3D12SyncedFrameQueue& inputQueue,
    const IC4Ext::D3D12BackendContext& backend,
    const CheckerboardCalibrationOptions& options,
    const std::optional<StereoCalibrationDocument>& initialDocument,
    const std::atomic<bool>* externalStopRequested = nullptr)
{
    const auto config = GetPostProcessRuntimeConfig();
    std::cout
        << "[CALIB] OpenCV preview disabled\n"
        << "[CALIB] controls: Q=finish, R=clear observations, Esc=abort\n"
        << "[CALIB] move plane: arrows; resize/distance: Shift+arrows\n"
        << "[CALIB] hide/show Varjo plane: O\n";
    if (config.settings.enabled &&
        config.settings.mode != StereoPostProcessMode::None) {
        std::cout
            << "[CALIB] reveal "
            << PostProcessModeName(config.settings.mode)
            << " postprocess: "
            << RevealVirtualKeyName(config.revealVirtualKey) << "\n";
    } else {
        std::cout << "[CALIB] postprocess disabled\n";
    }

    gCalibrationActive.store(true, std::memory_order_release);
    std::atomic_bool helperThreadStop{false};
    std::thread windowThread([&]() noexcept {
        MoveOpenCvWindowOffscreen(helperThreadStop);
    });
    std::thread stopWatcherThread([&]() noexcept {
        WatchExternalStopForCalibrationAbort(helperThreadStop, externalStopRequested);
    });

    auto stopHelpers = [&]() noexcept {
        helperThreadStop.store(true, std::memory_order_release);
        if (windowThread.joinable()) windowThread.join();
        if (stopWatcherThread.joinable()) stopWatcherThread.join();
        gCalibrationActive.store(false, std::memory_order_release);
    };

    CheckerboardCalibrationResult result;
    try {
        result = RunCheckerboardStereoCalibration(
            inputQueue,
            backend,
            options,
            initialDocument);
    } catch (...) {
        stopHelpers();
        throw;
    }

    stopHelpers();

    if (result.ok) {
        std::cout
            << "[CALIB][QUALITY] selected=affine_vertical"
            << " observations=" << result.capturedSamplePairs
            << " mean_vertical_error=" << result.averageEpipolarErrorPixels
            << " px\n";
    }
    return result;
}

} // namespace DualIC4Varjo::CalibrationRuntimeBridge
