#ifndef D3D12_PROCESSING_PYRAMID_COMMON_HLSLI
#define D3D12_PROCESSING_PYRAMID_COMMON_HLSLI

#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint PROCESSING_FILTER_POINT = 0;
static const uint PROCESSING_FILTER_LINEAR = 1;
static const uint PYRAMID_EDGE_CLAMP = 0;
static const uint PYRAMID_EDGE_CONSTANT = 1;

cbuffer PyramidConstants : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;

    int SrcX;
    int SrcY;
    int DstX;
    int DstY;

    uint Filter;
    uint EdgeMode;
    uint Reserved0;
    uint Reserved1;

    float4 BorderColor;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

bool InDispatchRect(uint2 p)
{
    return p.x < DstWidth && p.y < DstHeight;
}

bool ResolveSampleCoord(int2 localPos, out uint2 samplePos)
{
    if (localPos.x >= 0 && localPos.y >= 0 &&
        localPos.x < (int)SrcWidth && localPos.y < (int)SrcHeight) {
        samplePos = uint2((uint)SrcX, (uint)SrcY) + uint2(localPos);
        return true;
    }

    if (EdgeMode == PYRAMID_EDGE_CLAMP) {
        int x = clamp(localPos.x, 0, (int)SrcWidth - 1);
        int y = clamp(localPos.y, 0, (int)SrcHeight - 1);
        samplePos = uint2((uint)SrcX, (uint)SrcY) + uint2((uint)x, (uint)y);
        return true;
    }

    samplePos = uint2(0, 0);
    return false;
}

float4 SampleSource(int2 localPos)
{
    uint2 samplePos;
    if (!ResolveSampleCoord(localPos, samplePos)) {
        return BorderColor;
    }

    return Src.Load(int3((int)samplePos.x, (int)samplePos.y, 0));
}

float4 SampleSourceLinear(float2 localPos)
{
    int2 p0 = int2(floor(localPos));
    float2 t = saturate(localPos - (float2)p0);

    float4 c00 = SampleSource(p0);
    float4 c10 = SampleSource(p0 + int2(1, 0));
    float4 c01 = SampleSource(p0 + int2(0, 1));
    float4 c11 = SampleSource(p0 + int2(1, 1));

    float4 cx0 = lerp(c00, c10, t.x);
    float4 cx1 = lerp(c01, c11, t.x);
    return lerp(cx0, cx1, t.y);
}

void StorePixel(uint2 p, float4 color)
{
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = color;
}

#endif
