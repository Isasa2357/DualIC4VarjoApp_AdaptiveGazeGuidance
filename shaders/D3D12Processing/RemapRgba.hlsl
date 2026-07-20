#include "ProcessingExtendedCommon.hlsli"

Texture2D<float4> Src : register(t0);
Texture2D<float2> Map : register(t1);
RWTexture2D<float4> Dst : register(u0);

float2 NormalizeToPixel(float2 coord)
{
    if (CoordinateMode == PROCESSING_REMAP_COORD_NORMALIZED_ZERO_TO_ONE) {
        float sx = (SrcWidth > 1) ? float(SrcWidth - 1) : 0.0f;
        float sy = (SrcHeight > 1) ? float(SrcHeight - 1) : 0.0f;
        return coord * float2(sx, sy);
    }
    return coord;
}

bool OutsideSource(int2 p)
{
    return p.x < 0 || p.y < 0 || p.x >= int(SrcWidth) || p.y >= int(SrcHeight);
}

float4 LoadSourceAt(int2 p)
{
    if (BorderMode == PROCESSING_REMAP_BORDER_CONSTANT && OutsideSource(p)) {
        return BorderColor;
    }

    int2 q = clamp(p, int2(0, 0), int2(int(SrcWidth) - 1, int(SrcHeight) - 1));
    return ToLogicalRgba(Src.Load(int3(q.x + SrcX, q.y + SrcY, 0)), SrcFormat);
}

float4 SamplePoint(float2 coord)
{
    int2 p = int2(round(coord));
    return LoadSourceAt(p);
}

float4 SampleLinear(float2 coord)
{
    float2 baseF = floor(coord);
    float2 f = coord - baseF;

    int2 p00 = int2(baseF);
    int2 p10 = p00 + int2(1, 0);
    int2 p01 = p00 + int2(0, 1);
    int2 p11 = p00 + int2(1, 1);

    float4 c00 = LoadSourceAt(p00);
    float4 c10 = LoadSourceAt(p10);
    float4 c01 = LoadSourceAt(p01);
    float4 c11 = LoadSourceAt(p11);

    float4 cx0 = lerp(c00, c10, f.x);
    float4 cx1 = lerp(c01, c11, f.x);
    return lerp(cx0, cx1, f.y);
}

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InRect(p, DstWidth, DstHeight)) {
        return;
    }
    if (p.x >= MapWidth || p.y >= MapHeight) {
        return;
    }

    float2 mapped = NormalizeToPixel(Map.Load(int3((int)p.x, (int)p.y, 0)));
    float4 c = (Filter == PROCESSING_FILTER_POINT) ? SamplePoint(mapped) : SampleLinear(mapped);
    uint2 dstPos = OffsetPosition(DstX, DstY, p);
    Dst[dstPos] = FromLogicalRgba(saturate(c), DstFormat);
}
