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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

namespace DualIC4Varjo::CalibrationRuntimeBridge {

inline std::atomic_bool gCalibrationActive{false};
inline VarjoXR::XRPlane* gPlane = nullptr;

struct PlanePostProcessRuntimeConfig {
    StereoPostProcessSettings settings{};
    float revealOpenSeconds = 0.5f;
    float revealHoldSeconds = 3.0f;
    float revealCloseSeconds = 0.5f;
};

inline std::mutex& PostProcessConfigMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

inline PlanePostProcessRuntimeConfig& PostProcessConfigStorage() noexcept
{
    static PlanePostProcessRuntimeConfig config;
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

inline float PositiveOr(float value, float fallback) noexcept
{
    return std::isfinite(value) && value > 0.0f ? value : fallback;
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

inline void ApplyPlanePostProcessFromKeyboard(VarjoXR::XRPlane& plane)
{
    using Clock = std::chrono::steady_clock;

    static thread_local bool dDown = false;
    static thread_local bool pulseActive = false;
    static thread_local Clock::time_point pulseStart{};

    const auto config = GetPostProcessRuntimeConfig();
    const auto now = Clock::now();
    if (KeyPressedEdge('D', dDown)) {
        pulseActive = true;
        pulseStart = now;
        std::cout
            << "[POSTPROCESS] reveal pulse started: "
            << "open=" << config.revealOpenSeconds
            << "s, hold=" << config.revealHoldSeconds
            << "s, close=" << config.revealCloseSeconds << "s\n";
    }

    float revealAmount = 0.0f;
    if (pulseActive) {
        const float openSeconds = PositiveOr(config.revealOpenSeconds, 0.5f);
        const float holdSeconds = std::max(0.0f, config.revealHoldSeconds);
        const float closeSeconds = PositiveOr(config.revealCloseSeconds, 0.5f);
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

    UpdatePlanePostProcessState(plane, config.settings, revealAmount);
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
    std::cout
        << "[CALIB] OpenCV preview disabled\n"
        << "[CALIB] controls: Q=finish, R=clear observations, Esc=abort\n"
        << "[CALIB] move plane: arrows; resize/distance: Shift+arrows\n"
        << "[CALIB] hide/show Varjo plane: O\n"
        << "[CALIB] reveal darkened surroundings: D\n";

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
