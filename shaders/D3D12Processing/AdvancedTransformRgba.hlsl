Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

static const uint PROCESSING_FILTER_POINT = 0;
static const uint PROCESSING_FILTER_LINEAR = 1;
static const uint PROCESSING_REMAP_BORDER_CLAMP = 0;
static const uint PROCESSING_REMAP_BORDER_CONSTANT = 1;

cbuffer AdvancedTransformConstants : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;

    int SrcX;
    int SrcY;
    int DstX;
    int DstY;

    uint SrcFormat;
    uint DstFormat;
    uint Filter;
    uint BorderMode;

    float4 MatrixRow0; // m00, m01, m02, unused
    float4 MatrixRow1; // m10, m11, m12, unused
    float4 MatrixRow2; // m20, m21, m22, unused
    float4 BorderColor;
};

float4 ToLogicalRgba(float4 v, uint format)
{
    return v;
}

float4 FromLogicalRgba(float4 v, uint format)
{
    return v;
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
    return LoadSourceAt(int2(round(coord)));
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

float2 TransformDstToSrc(float2 dstLocal, out bool valid)
{
    float sx = dot(MatrixRow0.xyz, float3(dstLocal, 1.0f));
    float sy = dot(MatrixRow1.xyz, float3(dstLocal, 1.0f));
    float sw = dot(MatrixRow2.xyz, float3(dstLocal, 1.0f));

    valid = abs(sw) > 1.0e-8f;
    if (!valid) {
        return float2(0.0f, 0.0f);
    }
    return float2(sx, sy) / sw;
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= DstWidth || p.y >= DstHeight) {
        return;
    }

    bool valid = false;
    float2 srcCoord = TransformDstToSrc(float2(p), valid);
    float4 c = valid ? ((Filter == PROCESSING_FILTER_POINT) ? SamplePoint(srcCoord) : SampleLinear(srcCoord)) : BorderColor;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = FromLogicalRgba(saturate(c), DstFormat);
}
