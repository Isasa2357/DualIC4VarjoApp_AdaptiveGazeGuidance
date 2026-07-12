#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CalibrationRuntimeBridge.hpp"
#include "GuiControlBridge.hpp"

#include <VarjoXR/VarjoXR.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>

namespace DualIC4Varjo::GuiPlaneControlIntegration {
namespace detail {

inline std::optional<glm::vec2>& HiddenSavedSize()
{
    static thread_local std::optional<glm::vec2> value;
    return value;
}

inline void ApplyPlaneAlpha(VarjoXR::XRPlane& plane, bool visible)
{
    plane.setTint({1.0f, 1.0f, 1.0f, visible ? 1.0f : 0.0f});
}

inline void ApplyVisibleState(VarjoXR::XRPlane& plane, bool visible)
{
    auto& savedSize = HiddenSavedSize();
    if (visible) {
        if (savedSize) {
            plane.setSize(*savedSize);
            savedSize.reset();
        }
        ApplyPlaneAlpha(plane, true);
    } else {
        if (!savedSize) {
            savedSize = plane.size();
        }
        // Keep the invisible Plane geometry large enough to cover all views, so
        // the VST postprocess mask becomes full-screen pass-through while the
        // Plane is hidden. The saved visible size is restored when shown again.
        plane.setSize({1000.0f, 1000.0f});
        ApplyPlaneAlpha(plane, false);
    }
    GuiControlBridge::SetPlaneVisible(visible);
}

inline glm::vec2& EditablePlaneSize(VarjoXR::XRPlane& plane)
{
    auto& savedSize = HiddenSavedSize();
    if (!GuiControlBridge::PlaneVisible() && savedSize) {
        return *savedSize;
    }
    // This function cannot return plane.size() by reference. Callers must write
    // the returned copy back through SetEditablePlaneSize().
    static thread_local glm::vec2 scratch;
    scratch = plane.size();
    return scratch;
}

inline void SetEditablePlaneSize(VarjoXR::XRPlane& plane, glm::vec2 size)
{
    auto& savedSize = HiddenSavedSize();
    if (!GuiControlBridge::PlaneVisible() && savedSize) {
        *savedSize = size;
        plane.setSize({1000.0f, 1000.0f});
        return;
    }
    plane.setSize(size);
}

inline bool ApplyMoveResizeDepth(
    VarjoXR::XRPlane& plane,
    std::int64_t leftSteps,
    std::int64_t rightSteps,
    std::int64_t upSteps,
    std::int64_t downSteps,
    std::int64_t sizeUpSteps,
    std::int64_t sizeDownSteps,
    std::int64_t nearSteps,
    std::int64_t farSteps)
{
    constexpr float moveStep = 0.01f;
    constexpr float resizeStep = 0.01f;
    constexpr float minimumWidth = 0.05f;
    constexpr float minimumDistance = 0.05f;

    bool changed = false;
    auto& position = plane.transform().position;

    const float dx = static_cast<float>(rightSteps - leftSteps) * moveStep;
    const float dy = static_cast<float>(upSteps - downSteps) * moveStep;
    if (dx != 0.0f) {
        position.x += dx;
        changed = true;
    }
    if (dy != 0.0f) {
        position.y += dy;
        changed = true;
    }

    const float depthDelta = static_cast<float>(nearSteps - farSteps) * moveStep;
    if (depthDelta != 0.0f) {
        const float newZ = (std::min)(-minimumDistance, position.z + depthDelta);
        if (newZ != position.z) {
            position.z = newZ;
            changed = true;
        }
    }

    const float widthDelta = static_cast<float>(sizeUpSteps - sizeDownSteps) * resizeStep;
    if (widthDelta != 0.0f) {
        const glm::vec2 size = EditablePlaneSize(plane);
        if (size.x > 0.0f && size.y > 0.0f) {
            const float newWidth = (std::max)(minimumWidth, size.x + widthDelta);
            if (newWidth != size.x) {
                SetEditablePlaneSize(plane, {newWidth, newWidth * (size.y / size.x)});
                changed = true;
            }
        }
    }

    return changed;
}

inline void PrintPlaneState(const char* reason, const VarjoXR::XRPlane& plane)
{
    const glm::vec2 visibleSize = HiddenSavedSize().value_or(plane.size());
    const auto& position = plane.transform().position;
    std::cout << std::fixed << std::setprecision(3)
              << "[PLANE][" << (reason ? reason : "GUI") << "] x=" << position.x
              << " m, y=" << position.y
              << " m, z=" << position.z
              << " m, distance=" << (std::max)(0.0f, -position.z)
              << " m, width=" << visibleSize.x
              << " m, height=" << visibleSize.y
              << " m, visible=" << (GuiControlBridge::PlaneVisible() ? 1 : 0)
              << '\n';
}

} // namespace detail

inline void ApplyPlaneInputAfterRender(VarjoXR::XRPlane& plane)
{
    // Postprocess reveal is intentionally still keyboard-triggerable when the
    // keyboard operation lock is enabled.
    CalibrationRuntimeBridge::ApplyPlanePostProcessFromKeyboard(plane);

    const auto commands = GuiControlBridge::ConsumePlaneCommands();
    bool changed = false;

    if ((commands.visibilityToggles % 2u) != 0u) {
        const bool nextVisible = !GuiControlBridge::PlaneVisible();
        detail::ApplyVisibleState(plane, nextVisible);
        changed = true;
        std::cout << "[PLANE][GUI] Plane "
                  << (nextVisible ? "visible" : "hidden; VST postprocess pass-through")
                  << '\n';
    }

    changed |= detail::ApplyMoveResizeDepth(
        plane,
        static_cast<std::int64_t>(commands.moveLeft),
        static_cast<std::int64_t>(commands.moveRight),
        static_cast<std::int64_t>(commands.moveUp),
        static_cast<std::int64_t>(commands.moveDown),
        static_cast<std::int64_t>(commands.sizeIncrease),
        static_cast<std::int64_t>(commands.sizeDecrease),
        static_cast<std::int64_t>(commands.moveNear),
        static_cast<std::int64_t>(commands.moveFar));

    static thread_local bool oDown = false;
    const bool oPressed = CalibrationRuntimeBridge::KeyPressedEdge('O', oDown);
    if (oPressed && !GuiControlBridge::KeyboardControlLocked()) {
        const bool nextVisible = !GuiControlBridge::PlaneVisible();
        detail::ApplyVisibleState(plane, nextVisible);
        changed = true;
        std::cout << "[PLANE][KEY] Plane "
                  << (nextVisible ? "visible" : "hidden; VST postprocess pass-through")
                  << '\n';
    }

    // Keep the legacy arrow-key plane calibration behavior, but suppress its
    // effect while keyboard operations are locked. Key edges are still consumed
    // so releasing the lock does not replay old input.
    static thread_local CalibrationRuntimeBridge::ArrowEdgeState keys;
    const bool shift =
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    const bool left = keys.left();
    const bool right = keys.right();
    const bool up = keys.up();
    const bool down = keys.down();

    if (CalibrationRuntimeBridge::gCalibrationActive.load(std::memory_order_acquire) &&
        !GuiControlBridge::KeyboardControlLocked()) {
        if (shift) {
            changed |= detail::ApplyMoveResizeDepth(
                plane,
                0,
                0,
                0,
                0,
                right ? 1 : 0,
                left ? 1 : 0,
                down ? 1 : 0,
                up ? 1 : 0);
        } else {
            changed |= detail::ApplyMoveResizeDepth(
                plane,
                left ? 1 : 0,
                right ? 1 : 0,
                up ? 1 : 0,
                down ? 1 : 0,
                0,
                0,
                0,
                0);
        }
    }

    if (changed) {
        detail::PrintPlaneState("GUI", plane);
    }
}

} // namespace DualIC4Varjo::GuiPlaneControlIntegration
