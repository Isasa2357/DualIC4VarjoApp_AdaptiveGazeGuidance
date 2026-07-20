#include "ThresholdCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    float v = SelectChannel(LoadSrc(p), Channel);
    float t = NormalizeRange(v, MinValue, MaxValue);
    float3 rgb = HeatmapColor(t, Mode);
    Store(p, float4(rgb, saturate(t * Opacity)));
}
