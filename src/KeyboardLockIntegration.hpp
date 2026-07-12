#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "FadeOutPostProcessIntegration.hpp"
#include "GuiControlBridge.hpp"

#include <Windows.h>

namespace DualIC4Varjo::KeyboardLockIntegration {

inline SHORT GetAsyncKeyState(int virtualKey)
{
    // Esc must keep the graceful shutdown/fade-out behavior even while the
    // keyboard operation lock is enabled.
    if (virtualKey == VK_ESCAPE) {
        return FadeOutPostProcessIntegration::GetAsyncKeyState(virtualKey);
    }

    // The legacy application body also reads arrow/shift keys directly through
    // GetAsyncKeyState before the GUI/Plane integration hook runs. When the GUI
    // lock is enabled, suppress those reads at the wrapper level so keyboard
    // input can still be observed by logging/UI, but it no longer mutates the
    // Plane. Postprocess reveal is handled by CalibrationRuntimeBridge, which was
    // included before this macro replacement and still reads the physical key.
    if (GuiControlBridge::KeyboardControlLocked()) {
        return 0;
    }

    return ::GetAsyncKeyState(virtualKey);
}

} // namespace DualIC4Varjo::KeyboardLockIntegration
