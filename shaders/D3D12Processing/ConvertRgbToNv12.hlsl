#include "ProcessingCommon.hlsli"
#include "YuvPrimitives.hlsli"

Texture2D<float4> Src : register(t0);
RWTexture2D<float> YPlane : register(u0);
RWTexture2D<float2> UVPlane : register(u1);

float3 LoadLogicalRgb(uint2 srcPos)
{
    return saturate(D3D12LoadLogicalRgba(Src, srcPos, SrcFormat).rgb);
}

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;

    if (p.x < DstWidth && p.y < DstHeight) {
        const uint2 srcPos = OffsetPosition(SrcX, SrcY, p);
        const uint2 dstPos = OffsetPosition(DstX, DstY, p);
        D3D12StoreYuv420Luma(
            YPlane,
            dstPos,
            LoadLogicalRgb(srcPos),
            DstFormat,
            DstRange,
            DstMatrix);
    }

    if (p.x < (DstWidth / 2u) && p.y < (DstHeight / 2u)) {
        const uint2 baseSrc = OffsetPosition(SrcX, SrcY, p * 2u);
        const float3 rgb00 = LoadLogicalRgb(baseSrc + uint2(0, 0));
        const float3 rgb10 = LoadLogicalRgb(baseSrc + uint2(1, 0));
        const float3 rgb01 = LoadLogicalRgb(baseSrc + uint2(0, 1));
        const float3 rgb11 = LoadLogicalRgb(baseSrc + uint2(1, 1));
        const float3 averageRgb = (rgb00 + rgb10 + rgb01 + rgb11) * 0.25f;
        const uint2 uvDst = (uint2((uint)DstX, (uint)DstY) / 2u) + p;
        D3D12StoreYuv420Chroma(
            UVPlane,
            uvDst,
            averageRgb,
            DstFormat,
            DstRange,
            DstMatrix);
    }
}
