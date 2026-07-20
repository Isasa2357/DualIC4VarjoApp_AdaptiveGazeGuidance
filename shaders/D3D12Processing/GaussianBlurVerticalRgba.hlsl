#include "BlurCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    StoreBlurredPixel(tid.xy, int2(0, 1));
}
