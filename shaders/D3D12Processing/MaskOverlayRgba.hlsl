#include "ThresholdCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    float v = SelectChannel(LoadSrc(p), Channel);
    if (Invert != 0u) {
        v = 1.0 - v;
    }

    float a = saturate(v * Opacity) * OverlayColor.a;
    Store(p, float4(OverlayColor.rgb, a));
}
