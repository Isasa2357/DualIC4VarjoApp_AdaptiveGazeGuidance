#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CoordinatedCameraCaptureThread.hpp"
#include "RawStereoNvencRecordingIntegration.hpp"
#include "BlackCirclePlaneIntegration.hpp"
#include "CalibrationRuntimeBridge.hpp"
#include "DebugCenterDotPlaneIntegration.hpp"
#include "FadeOutPostProcessIntegration.hpp"
#include "GracefulShutdownIntegration.hpp"
#include "GuiPerformanceStats.hpp"
#include "GuiPlaneControlIntegration.hpp"
#include "KeyboardLockIntegration.hpp"
#include "PersonalizationSettings.hpp"
#include "PreCalibrationDisplayCalibration.hpp"
#include "PostProcessControlWindow.hpp"
#include "PostProcessDefaultOverrides.hpp"

// The included application defines these macros itself. Undefine them here to
// avoid C4005 while keeping Windows headers already parsed with NOMINMAX.
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX

// Replace only application-level implementation classes in this translation
// unit. Keep D3D12SyncedFrameQueue unchanged so existing functions such as
// ImGuiStereoPreview and RunCheckerboardStereoCalibration keep their ABI.
#define D3D12CameraCaptureThread CoordinatedD3D12CameraCaptureThread
#define D3D12FrameSyncThread RecordingD3D12FrameSyncThread
#define StereoDisplayTextureRing RecordingStereoDisplayTextureRing
#define RenderedFrameMetadataLogger FadeOutRenderedFrameMetadataLogger

// Register our console handler instead of the legacy handler that directly sets
// gStopRequested. Ctrl+C now publishes an application-exit request; the normal
// Esc polling path keeps rendering until the VST fade has completed.
#define SetConsoleCtrlHandler(handler, add) \
    ::SetConsoleCtrlHandler( \
        DualIC4Varjo::GracefulShutdownIntegration::ConsoleControlHandler, \
        add)

// Intercept only calls inside the included application translation unit. Esc,
// GUI Exit, ImGui window close, and Ctrl+C all enter the same two-second fade;
// ordinary keyboard Plane operations are suppressed while the GUI keyboard lock
// is enabled. The postprocess reveal key remains handled by the runtime bridge.
#define GetAsyncKeyState DualIC4Varjo::KeyboardLockIntegration::GetAsyncKeyState

// Register the video Plane immediately after XRSpace::createPlane() returns and
// create VarjoXR helper Planes for the black circle and debug postprocess center
// dot. Helper Planes stay transparent until synchronized to the video Plane.
#define createPlane(...) createPlane(__VA_ARGS__); \
    auto& blackCirclePlane = space.createPlane({1.0f, 1.0f}); \
    DualIC4Varjo::BlackCirclePlaneIntegration::Initialize(blackCirclePlane); \
    auto& postProcessDebugDotPlane = space.createPlane({1.0f, 1.0f}); \
    DualIC4Varjo::DebugCenterDotPlaneIntegration::Initialize(postProcessDebugDotPlane); \
    DualIC4Varjo::CalibrationRuntimeBridge::RegisterPlane(plane); \
    DualIC4Varjo::FadeOutPostProcessIntegration::RegisterRuntime( \
        session->shared(), core->GetDirectCommandQueue())

// The render-token replacement applies GUI/keyboard input on the Varjo render
// thread, applies an existing --calib JSON before the checkerboard prompt/phase,
// synchronizes the VarjoXR helper Planes for the next rendered frame,
// starts/updates the VST blur+darken mask from the same Plane projection,
// publishes camera counters, captures personalization state, and applies shutdown
// Plane transparency.
#define render() render(); \
    DualIC4Varjo::PostProcessDefaultOverrides::ApplyOnce(); \
    DualIC4Varjo::PostProcessControlWindow::EnsureStarted(); \
    DualIC4Varjo::PreCalibrationDisplayCalibration::ApplyIfAvailable(plane); \
    DualIC4Varjo::GuiPerformanceStats::SubmitCameraReadFrames( \
        leftCamera.stats().readFrames, rightCamera.stats().readFrames); \
    DualIC4Varjo::GuiPlaneControlIntegration::ApplyPlaneInputAfterRender(plane); \
    DualIC4Varjo::BlackCirclePlaneIntegration::SyncToVideoPlane(plane, blackCirclePlane); \
    DualIC4Varjo::DebugCenterDotPlaneIntegration::SyncToPostProcessCenter(plane, postProcessDebugDotPlane); \
    DualIC4Varjo::PersonalizationSettings::CapturePlaneState(plane); \
    DualIC4Varjo::FadeOutPostProcessIntegration::UpdatePlaneMaskFromFrame( \
        plane, d3dBackend.frameInfoSnapshot()); \
    DualIC4Varjo::FadeOutPostProcessIntegration::ApplyPlaneFadeVisibility(plane)

// GUI close/Ctrl+C can occur while OpenCV calibration owns the main thread.
// This wrapper aborts that loop and waits for the VST fade before returning to
// the caller, which may then set gStopRequested and perform normal cleanup.
#define RunCheckerboardStereoCalibration(inputQueue, backend, options, initialDocument) \
    DualIC4Varjo::GracefulShutdownIntegration::RunHeadlessCheckerboardStereoCalibration( \
        inputQueue, backend, options, initialDocument)

#include "ApplicationMainRealtimeCalibration.cpp"

#undef RunCheckerboardStereoCalibration
#undef render
#undef createPlane
#undef GetAsyncKeyState
#undef SetConsoleCtrlHandler
#undef RenderedFrameMetadataLogger
#undef StereoDisplayTextureRing
#undef D3D12FrameSyncThread
#undef D3D12CameraCaptureThread
