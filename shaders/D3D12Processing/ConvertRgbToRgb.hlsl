#include "ProcessingCommon.hlsli"

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InRect(p, DstWidth, DstHeight)) {
        return;
    }

    uint2 srcPos = OffsetPosition(SrcX, SrcY, p);
    uint2 dstPos = OffsetPosition(DstX, DstY, p);
    float4 c = Src.Load(LoadCoord(srcPos));
    c = ToLogicalRgba(c, SrcFormat);
    c = FromLogicalRgba(c, DstFormat);
    Dst[dstPos] = saturate(c);
}
