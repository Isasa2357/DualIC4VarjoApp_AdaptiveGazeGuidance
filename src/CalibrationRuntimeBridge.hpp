#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CheckerboardStereoCalibration.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

namespace DualIC4Varjo::CalibrationRuntimeBridge {

inline std::atomic_bool gCalibrationActive{false};
inline VarjoXR::XRPlane* gPlane = nullptr;

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

inline void ApplyPlaneInputAfterRender(VarjoXR::XRPlane& plane)
{
    ApplyPlaneVisibilityFromKeyboard(plane);

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

inline void MoveOpenCvWindowOffscreen(std::atomic_bool& stopRequested) noexcept
{
    constexpr const char* title = "Reference Stereo Calibration (affine_vertical)";
    while (!stopRequested.load(std::memory_order_acquire)) {
        if (HWND window = FindWindowA(nullptr, title)) {
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

inline CheckerboardCalibrationResult RunHeadlessCheckerboardStereoCalibration(
    IC4Ext::D3D12SyncedFrameQueue& inputQueue,
    const IC4Ext::D3D12BackendContext& backend,
    const CheckerboardCalibrationOptions& options,
    const std::optional<StereoCalibrationDocument>& initialDocument)
{
    std::cout
        << "[CALIB] OpenCV preview disabled\n"
        << "[CALIB] controls: Q=finish, R=clear observations, Esc=abort\n"
        << "[CALIB] move plane: arrows; resize/distance: Shift+arrows\n"
        << "[CALIB] hide/show Varjo plane: O\n";

    gCalibrationActive.store(true, std::memory_order_release);
    std::atomic_bool windowThreadStop{false};
    std::thread windowThread([&]() noexcept {
        MoveOpenCvWindowOffscreen(windowThreadStop);
    });

    CheckerboardCalibrationResult result;
    try {
        result = RunCheckerboardStereoCalibration(
            inputQueue,
            backend,
            options,
            initialDocument);
    } catch (...) {
        windowThreadStop.store(true, std::memory_order_release);
        if (windowThread.joinable()) windowThread.join();
        gCalibrationActive.store(false, std::memory_order_release);
        throw;
    }

    windowThreadStop.store(true, std::memory_order_release);
    if (windowThread.joinable()) windowThread.join();
    gCalibrationActive.store(false, std::memory_order_release);

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
