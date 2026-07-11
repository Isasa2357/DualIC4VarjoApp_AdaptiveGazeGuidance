#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CoordinatedCameraCaptureThread.hpp"
#include "RawStereoNvencQueue.hpp"
#include "RawStereoNvencRecordingIntegration.hpp"

// The included application defines these macros itself. Undefine them here to
// avoid C4005 while keeping Windows headers already parsed with NOMINMAX.
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX

// Replace only application-level types in this translation unit. Their original
// headers were included before these macros, so dependency declarations are not
// rewritten.
#define D3D12CameraCaptureThread CoordinatedD3D12CameraCaptureThread
#define D3D12SyncedFrameQueue RecordingD3D12SyncedFrameQueue
#define D3D12FrameSyncThread RecordingD3D12FrameSyncThread
#define StereoDisplayTextureRing RecordingStereoDisplayTextureRing
#define RenderedFrameMetadataLogger RecordingRenderedFrameMetadataLogger
#include "ApplicationMainRealtimeCalibration.cpp"
#undef RenderedFrameMetadataLogger
#undef StereoDisplayTextureRing
#undef D3D12FrameSyncThread
#undef D3D12SyncedFrameQueue
#undef D3D12CameraCaptureThread
