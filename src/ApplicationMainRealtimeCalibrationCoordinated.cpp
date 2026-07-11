#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CoordinatedCameraCaptureThread.hpp"

// ApplicationMainRealtimeCalibration.cpp defines these macros itself. Undefine
// them here to avoid C4005 while keeping Windows headers already parsed with
// NOMINMAX enabled.
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX

// ApplicationMainRealtimeCalibration.cpp uses IC4Ext::D3D12CameraCaptureThread.
// Replace only that token in this translation unit with the application-local
// compatibility wrapper. IC4Ext headers were already included above, so their
// declarations are not rewritten by this macro.
#define D3D12CameraCaptureThread CoordinatedD3D12CameraCaptureThread
#include "ApplicationMainRealtimeCalibration.cpp"
#undef D3D12CameraCaptureThread
