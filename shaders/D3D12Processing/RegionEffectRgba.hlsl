#include "RegionEffectCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    uint2 srcPos = uint2((uint)SrcX, (uint)SrcY) + p;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;

    float4 src = Src.Load(int3((int)srcPos.x, (int)srcPos.y, 0));
    float amount = RegionAmount(p);
    Dst[dstPos] = ApplyRegionEffect(src, amount);
}
