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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace DualIC4Varjo::GuiPlaneControlIntegration {
namespace detail {

inline constexpr float kGuiPostProcessTransitionSeconds = 0.5f;

inline void ApplyVisibleState(VarjoXR::XRPlane& plane, bool visible)
{
    // VST pass-through is now selected explicitly by
    // FadeOutPostProcessIntegration from GuiControlBridge::PlaneVisible(). Keep
    // the real Plane geometry unchanged while it is hidden so metadata and later
    // size/depth edits continue to describe the actual Plane.
    plane.setTint({1.0f, 1.0f, 1.0f, visible ? 1.0f : 0.0f});
    GuiControlBridge::SetPlaneVisible(visible);
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

    const float depthDelta =
        static_cast<float>(nearSteps - farSteps) * moveStep;
    if (depthDelta != 0.0f) {
        const float newZ = (std::min)(
            -minimumDistance,
            position.z + depthDelta);
        if (newZ != position.z) {
            position.z = newZ;
            changed = true;
        }
    }

    const float widthDelta =
        static_cast<float>(sizeUpSteps - sizeDownSteps) * resizeStep;
    if (widthDelta != 0.0f) {
        const glm::vec2 size = plane.size();
        if (size.x > 0.0f && size.y > 0.0f) {
            const float newWidth = (std::max)(
                minimumWidth,
                size.x + widthDelta);
            if (newWidth != size.x) {
                plane.setSize({newWidth, newWidth * (size.y / size.x)});
                changed = true;
            }
        }
    }

    return changed;
}

inline void PrintPlaneState(
    const char* reason,
    const VarjoXR::XRPlane& plane)
{
    const glm::vec2 size = plane.size();
    const auto& position = plane.transform().position;
    std::cout << std::fixed << std::setprecision(3)
              << "[PLANE][" << (reason ? reason : "GUI") << "] x="
              << position.x
              << " m, y=" << position.y
              << " m, z=" << position.z
              << " m, distance=" << (std::max)(0.0f, -position.z)
              << " m, width=" << size.x
              << " m, height=" << size.y
              << " m, visible="
              << (GuiControlBridge::PlaneVisible() ? 1 : 0)
              << '\n';
}

inline void ApplyAnimatedPostProcessSettings(
    VarjoXR::XRPlane& plane,
    const CalibrationRuntimeBridge::PlanePostProcessRuntimeConfig& config,
    float revealAmount01)
{
    const float revealAmount = std::clamp(
        std::isfinite(revealAmount01) ? revealAmount01 : 0.0f,
        0.0f,
        1.0f);

    if (config.settings.mode == StereoPostProcessMode::Darken) {
        const StereoPostProcessSettings animatedSettings =
            CalibrationRuntimeBridge::MakeRadiusRevealSettings(
                config,
                revealAmount);
        UpdatePlanePostProcessState(plane, animatedSettings, 0.0f);
    } else if (config.settings.mode == StereoPostProcessMode::Blur) {
        const StereoPostProcessSettings animatedSettings =
            CalibrationRuntimeBridge::MakeBlurRevealSettings(
                config,
                revealAmount);
        UpdatePlanePostProcessState(plane, animatedSettings, 0.0f);
    }
}

inline void ApplyPostProcessWithGuiTransition(VarjoXR::XRPlane& plane)
{
    using Clock = std::chrono::steady_clock;

    // Keep the existing keyboard reveal path active. GUI check/uncheck only
    // overrides the normal postprocess state while its 0.5 s transition is
    // running.
    CalibrationRuntimeBridge::ApplyPlanePostProcessFromKeyboard(plane);

    static thread_local bool previousDesiredApply = false;
    static thread_local bool transitionActive = false;
    static thread_local bool transitionToApply = false;
    static thread_local Clock::time_point transitionStart{};
    static thread_local StereoPostProcessMode transitionMode = StereoPostProcessMode::None;
    static thread_local bool haveLastActiveConfig = false;
    static thread_local CalibrationRuntimeBridge::PlanePostProcessRuntimeConfig transitionConfig{};
    static thread_local CalibrationRuntimeBridge::PlanePostProcessRuntimeConfig lastActiveConfig{};

    const auto config = CalibrationRuntimeBridge::GetPostProcessRuntimeConfig();
    const bool desiredApply =
        config.settings.enabled &&
        config.settings.mode != StereoPostProcessMode::None;
    const auto now = Clock::now();

    if (desiredApply) {
        lastActiveConfig = config;
        haveLastActiveConfig = true;
    }

    if (desiredApply != previousDesiredApply) {
        transitionActive = true;
        transitionToApply = desiredApply;
        transitionStart = now;
        transitionMode = desiredApply
            ? config.settings.mode
            : (haveLastActiveConfig
                ? lastActiveConfig.settings.mode
                : config.settings.mode);
        transitionConfig = desiredApply ? config : lastActiveConfig;
        if (transitionConfig.settings.mode != StereoPostProcessMode::None) {
            transitionConfig.settings.enabled = true;
        }

        std::cout
            << "[POSTPROCESS] "
            << (desiredApply ? "apply" : "remove")
            << " transition started from GUI checkbox; duration="
            << kGuiPostProcessTransitionSeconds << "s\n";
    }
    previousDesiredApply = desiredApply;

    if (!transitionActive ||
        transitionConfig.settings.mode == StereoPostProcessMode::None) {
        return;
    }

    if (transitionMode != transitionConfig.settings.mode) {
        transitionStart = now;
        transitionMode = transitionConfig.settings.mode;
    }

    const float elapsedSeconds = std::chrono::duration<float>(
        now - transitionStart).count();
    const float t = std::clamp(
        elapsedSeconds / kGuiPostProcessTransitionSeconds,
        0.0f,
        1.0f);

    if (t >= 1.0f) {
        transitionActive = false;
        if (!transitionToApply) {
            StereoPostProcessSettings disabledSettings = transitionConfig.settings;
            disabledSettings.enabled = false;
            UpdatePlanePostProcessState(plane, disabledSettings, 0.0f);
        }
        return;
    }

    // revealAmount == 1 means postprocess is effectively removed; revealAmount
    // == 0 means the selected command-line postprocess is fully applied.
    const float revealAmount = transitionToApply ? (1.0f - t) : t;
    ApplyAnimatedPostProcessSettings(plane, transitionConfig, revealAmount);
}

} // namespace detail

inline void ApplyPlaneInputAfterRender(VarjoXR::XRPlane& plane)
{
    // Postprocess reveal remains keyboard-triggerable while ordinary keyboard
    // Plane operations are locked. GUI check/uncheck uses a fixed 0.5 s
    // transition in both directions.
    detail::ApplyPostProcessWithGuiTransition(plane);

    const auto commands = GuiControlBridge::ConsumePlaneCommands();
    bool changed = false;

    if ((commands.visibilityToggles % 2u) != 0u) {
        const bool nextVisible = !GuiControlBridge::PlaneVisible();
        detail::ApplyVisibleState(plane, nextVisible);
        changed = true;
        std::cout
            << "[PLANE][GUI] Plane "
            << (nextVisible
                    ? "visible; VST blur+darken mask restored"
                    : "hidden; VST postprocess pass-through")
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
    const bool oPressed =
        CalibrationRuntimeBridge::KeyPressedEdge('O', oDown);
    if (oPressed && !GuiControlBridge::KeyboardControlLocked()) {
        const bool nextVisible = !GuiControlBridge::PlaneVisible();
        detail::ApplyVisibleState(plane, nextVisible);
        changed = true;
        std::cout
            << "[PLANE][KEY] Plane "
            << (nextVisible
                    ? "visible; VST blur+darken mask restored"
                    : "hidden; VST postprocess pass-through")
            << '\n';
    }

    // Consume key edges even while locked so unlocking never replays stale input.
    static thread_local CalibrationRuntimeBridge::ArrowEdgeState keys;
    const bool shift =
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    const bool left = keys.left();
    const bool right = keys.right();
    const bool up = keys.up();
    const bool down = keys.down();

    if (CalibrationRuntimeBridge::gCalibrationActive.load(
            std::memory_order_acquire) &&
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
