#include "PyramidCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    int2 srcBase = int2((int)p.x * 2, (int)p.y * 2);
    float4 color =
        SampleSource(srcBase) +
        SampleSource(srcBase + int2(1, 0)) +
        SampleSource(srcBase + int2(0, 1)) +
        SampleSource(srcBase + int2(1, 1));

    StorePixel(p, color * 0.25f);
}
