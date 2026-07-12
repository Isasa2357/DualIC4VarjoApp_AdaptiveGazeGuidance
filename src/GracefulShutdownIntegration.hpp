#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CalibrationRuntimeBridge.hpp"
#include "FadeOutPostProcessIntegration.hpp"
#include "GuiControlBridge.hpp"

#include <Windows.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <optional>

namespace DualIC4Varjo::GracefulShutdownIntegration {

inline BOOL WINAPI ConsoleControlHandler(DWORD controlType) noexcept
{
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // The Windows console handler runs on a separate system thread. Keep it
        // signal-safe: publish only an atomic request. The application/main loop
        // or calibration wrapper starts the compositor fade on its normal thread.
        GuiControlBridge::RequestApplicationExit();
        return TRUE;
    default:
        return FALSE;
    }
}

inline bool FinishFadeBeforeReturningToCaller(const char* trigger)
{
    if (!FadeOutPostProcessIntegration::RequestGracefulShutdown(trigger)) {
        std::cerr
            << "[FADE] graceful shutdown request failed: "
            << FadeOutPostProcessIntegration::Manager::instance().lastError()
            << " ; caller will continue with immediate cleanup\n";
        return false;
    }

    FadeOutPostProcessIntegration::WaitForFadeCompletion();
    return FadeOutPostProcessIntegration::ShutdownReady();
}

inline CheckerboardCalibrationResult RunHeadlessCheckerboardStereoCalibration(
    IC4Ext::D3D12SyncedFrameQueue& inputQueue,
    const IC4Ext::D3D12BackendContext& backend,
    const CheckerboardCalibrationOptions& options,
    const std::optional<StereoCalibrationDocument>& initialDocument)
{
    // GUI close and Ctrl+C both publish this flag. The calibration bridge watches
    // it and injects Esc into the otherwise blocking OpenCV loop.
    const std::atomic<bool>* externalStop =
        &GuiControlBridge::ApplicationExitRequestedStorage();

    CheckerboardCalibrationResult result =
        CalibrationRuntimeBridge::RunHeadlessCheckerboardStereoCalibration(
            inputQueue,
            backend,
            options,
            initialDocument,
            externalStop);

    // Do not let the caller set gStopRequested until the two-second VST fade has
    // completed. This keeps the Varjo render thread alive while the compositor
    // transitions from the current blur+darken level to black.
    if (result.aborted || GuiControlBridge::ApplicationExitRequested()) {
        GuiControlBridge::RequestApplicationExit();
        FinishFadeBeforeReturningToCaller(
            GuiControlBridge::ApplicationExitRequested()
                ? "GUI/Ctrl+C during calibration"
                : "calibration abort");
    }

    return result;
}

} // namespace DualIC4Varjo::GracefulShutdownIntegration
