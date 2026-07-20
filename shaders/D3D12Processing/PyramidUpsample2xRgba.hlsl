#include "PyramidCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    float4 color;
    if (Filter == PROCESSING_FILTER_POINT) {
        color = SampleSource(int2((int)p.x / 2, (int)p.y / 2));
    } else {
        float2 srcPos = ((float2)p + 0.5f) * 0.5f - 0.5f;
        color = SampleSourceLinear(srcPos);
    }

    StorePixel(p, color);
}
