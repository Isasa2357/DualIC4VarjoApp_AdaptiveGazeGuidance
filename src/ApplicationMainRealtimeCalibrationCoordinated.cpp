#include "CoordinatedCameraCaptureThread.hpp"

// ApplicationMainRealtimeCalibration.cpp uses IC4Ext::D3D12CameraCaptureThread.
// Replace only that token in this translation unit with the application-local
// compatibility wrapper. IC4Ext headers were already included above, so their
// declarations are not rewritten by this macro.
#define D3D12CameraCaptureThread CoordinatedD3D12CameraCaptureThread
#include "ApplicationMainRealtimeCalibration.cpp"
#undef D3D12CameraCaptureThread
