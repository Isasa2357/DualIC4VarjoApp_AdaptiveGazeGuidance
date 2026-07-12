#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CoordinatedCameraCaptureThread.hpp"
#include "RawStereoNvencRecordingIntegration.hpp"
#include "CalibrationRuntimeBridge.hpp"
#include "GuiPerformanceStats.hpp"
#include "PostProcessDefaultOverrides.hpp"
#include "FadeOutPostProcessIntegration.hpp"

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

// Intercept only calls inside the included application translation unit. Before
// the post-calibration fade-out postprocess is created, this delegates directly
// to ::GetAsyncKeyState. After that, Esc starts the 2-second VST fade-out and
// is reported to the application only when the fade is complete.
#define GetAsyncKeyState DualIC4Varjo::FadeOutPostProcessIntegration::GetAsyncKeyState

// Register the Plane immediately after XRSpace::createPlane() returns. The
// render-token replacement applies keyboard input on the Varjo render thread,
// updates the VST postprocess mask from the same frame's Plane projection,
// publishes camera frame counters for the ImGui performance panel, and forces
// the Plane transparent during shutdown fade-out without mutating XRPlane from
// the main thread.
#define createPlane(...) createPlane(__VA_ARGS__); \
    DualIC4Varjo::CalibrationRuntimeBridge::RegisterPlane(plane); \
    DualIC4Varjo::FadeOutPostProcessIntegration::RegisterRuntime( \
        session->shared(), core->GetDirectCommandQueue())
#define render() render(); \
    DualIC4Varjo::PostProcessDefaultOverrides::ApplyOnce(); \
    DualIC4Varjo::GuiPerformanceStats::SubmitCameraReadFrames( \
        leftCamera.stats().readFrames, rightCamera.stats().readFrames); \
    DualIC4Varjo::CalibrationRuntimeBridge::ApplyPlaneInputAfterRender(plane); \
    DualIC4Varjo::FadeOutPostProcessIntegration::UpdatePlaneMaskFromFrame( \
        plane, d3dBackend.frameInfoSnapshot()); \
    DualIC4Varjo::FadeOutPostProcessIntegration::ApplyPlaneFadeVisibility(plane)

// Keep the original Q/R/Esc termination semantics, but move the OpenCV window
// offscreen and report quality through the console. During calibration,
// Ctrl+C only sets gStopRequested; pass that flag to the bridge so the blocking
// calibration loop is also aborted and the normal cleanup path can run.
#define RunCheckerboardStereoCalibration(inputQueue, backend, options, initialDocument) \
    DualIC4Varjo::CalibrationRuntimeBridge::RunHeadlessCheckerboardStereoCalibration( \
        inputQueue, backend, options, initialDocument, &gStopRequested)

#include "ApplicationMainRealtimeCalibration.cpp"

#undef RunCheckerboardStereoCalibration
#undef render
#undef createPlane
#undef GetAsyncKeyState
#undef RenderedFrameMetadataLogger
#undef StereoDisplayTextureRing
#undef D3D12FrameSyncThread
#undef D3D12CameraCaptureThread
