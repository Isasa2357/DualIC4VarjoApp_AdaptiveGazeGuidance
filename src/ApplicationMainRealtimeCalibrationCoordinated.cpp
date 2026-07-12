#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CoordinatedCameraCaptureThread.hpp"
#include "RawStereoNvencRecordingIntegration.hpp"
#include "CalibrationRuntimeBridge.hpp"

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
#define RenderedFrameMetadataLogger RecordingRenderedFrameMetadataLogger

// Register the Plane immediately after XRSpace::createPlane() returns. The
// render-token replacement applies calibration-only keyboard input on the
// Varjo render thread, avoiding concurrent XRPlane mutation from the OpenCV
// calibration thread.
#define createPlane(...) createPlane(__VA_ARGS__); \
    DualIC4Varjo::CalibrationRuntimeBridge::RegisterPlane(plane)
#define render() render(); \
    DualIC4Varjo::CalibrationRuntimeBridge::ApplyPlaneInputAfterRender(plane)

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
#undef RenderedFrameMetadataLogger
#undef StereoDisplayTextureRing
#undef D3D12FrameSyncThread
#undef D3D12CameraCaptureThread
