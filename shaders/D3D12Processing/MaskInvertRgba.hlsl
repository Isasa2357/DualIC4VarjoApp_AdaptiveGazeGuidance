#include "MaskCommon.hlsli"

Texture2D<float4> MaskTex : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatch(p)) return;

    float m = ReadMaskAt(MaskTex, p, MaskX, MaskY, Channel, false);
    float v = 1.0 - m;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = float4(v, v, v, v);
}
