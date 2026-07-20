#include "ProcessingCommon.hlsli"
#include "YuvPrimitives.hlsli"

Texture2D<float> YPlane : register(t0);
Texture2D<float2> UVPlane : register(t1);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint2 p = tid.xy;
    if (!InRect(p, DstWidth, DstHeight)) {
        return;
    }

    const uint2 sourceOrigin = uint2((uint)SrcX, (uint)SrcY);
    const uint2 sourceSize = uint2(SrcWidth, SrcHeight);
    const float2 scale = float2(ScaleX, ScaleY);

    const float3 rgb = (Filter == PROCESSING_FILTER_POINT)
        ? D3D12SampleYuv420RgbPoint(
            YPlane,
            UVPlane,
            p,
            sourceOrigin,
            sourceSize,
            scale,
            SrcFormat,
            SrcRange,
            SrcMatrix)
        : D3D12SampleYuv420RgbLinear(
            YPlane,
            UVPlane,
            p,
            sourceOrigin,
            sourceSize,
            scale,
            SrcFormat,
            SrcRange,
            SrcMatrix);

    const uint2 dstPos = OffsetPosition(DstX, DstY, p);
    Dst[dstPos] = FromLogicalRgba(saturate(float4(rgb, 1.0f)), DstFormat);
}
