#include "ProcessingCommon.hlsli"
#include "YuvPrimitives.hlsli"

Texture2D<float> YPlane : register(t0);
Texture2D<float2> UVPlane : register(t1);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InRect(p, DstWidth, DstHeight)) {
        return;
    }

    const uint2 srcPos = OffsetPosition(SrcX, SrcY, p);
    const uint2 dstPos = OffsetPosition(DstX, DstY, p);
    const float3 rgb = D3D12LoadYuv420Rgb(
        YPlane,
        UVPlane,
        srcPos,
        SrcFormat,
        SrcRange,
        SrcMatrix);
    Dst[dstPos] = FromLogicalRgba(float4(rgb, 1.0f), DstFormat);
}
