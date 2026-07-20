#include "ProcessingExtendedCommon.hlsli"

Texture2D<float4> Base : register(t0);
Texture2D<float4> Overlay : register(t1);
RWTexture2D<float4> Dst : register(u0);

float4 Blend(float4 baseColor, float4 overlayColor)
{
    float opacity = saturate(Opacity);

    if (BlendMode == PROCESSING_COMPOSITE_COPY) {
        float4 copied = overlayColor;
        copied.a *= opacity;
        return copied;
    }

    if (BlendMode == PROCESSING_COMPOSITE_ADD) {
        float3 rgb = baseColor.rgb + overlayColor.rgb * (overlayColor.a * opacity);
        float a = max(baseColor.a, overlayColor.a * opacity);
        return float4(rgb, a);
    }

    float alpha = saturate(overlayColor.a * opacity);

    if (BlendMode == PROCESSING_COMPOSITE_PREMULTIPLIED_ALPHA) {
        float3 rgb = overlayColor.rgb * opacity + baseColor.rgb * (1.0f - alpha);
        float a = alpha + baseColor.a * (1.0f - alpha);
        return float4(rgb, a);
    }

    float3 rgbStraight = overlayColor.rgb * alpha + baseColor.rgb * (1.0f - alpha);
    float aStraight = alpha + baseColor.a * (1.0f - alpha);
    return float4(rgbStraight, aStraight);
}

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InRect(p, DstWidth, DstHeight)) {
        return;
    }

    uint2 basePos = OffsetPosition(SrcX, SrcY, p);
    uint2 overlayPos = OffsetPosition(OverlayX, OverlayY, p);
    uint2 dstPos = OffsetPosition(DstX, DstY, p);

    float4 baseColor = ToLogicalRgba(Base.Load(LoadCoord(basePos)), SrcFormat);
    float4 overlayColor = ToLogicalRgba(Overlay.Load(LoadCoord(overlayPos)), OverlayFormat);

    float4 outColor = Blend(baseColor, overlayColor);
    Dst[dstPos] = FromLogicalRgba(saturate(outColor), DstFormat);
}
