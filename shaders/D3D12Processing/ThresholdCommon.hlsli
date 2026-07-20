#ifndef D3D12_PROCESSING_THRESHOLD_COMMON_HLSLI
#define D3D12_PROCESSING_THRESHOLD_COMMON_HLSLI

#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint MASK_CHANNEL_RED = 0;
static const uint MASK_CHANNEL_GREEN = 1;
static const uint MASK_CHANNEL_BLUE = 2;
static const uint MASK_CHANNEL_ALPHA = 3;
static const uint MASK_CHANNEL_LUMA = 4;

static const uint HEATMAP_GRAYSCALE = 0;
static const uint HEATMAP_RED_GREEN = 1;
static const uint HEATMAP_BLUE_RED = 2;
static const uint HEATMAP_TURBO_APPROX = 3;

cbuffer ThresholdConstants : register(b0)
{
    uint Width;
    uint Height;
    int SrcX;
    int SrcY;

    int DstX;
    int DstY;
    uint Channel;
    uint Invert;

    float Threshold;
    float MinValue;
    float MaxValue;
    float Opacity;

    uint Mode;
    uint ClassCount;
    float ClassScale;
    uint Reserved0;

    float4 ForegroundColor;
    float4 BackgroundColor;
    float4 OverlayColor;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

bool InDispatchRect(uint2 p)
{
    return p.x < Width && p.y < Height;
}

float4 LoadSrc(uint2 p)
{
    uint2 srcPos = uint2((uint)SrcX, (uint)SrcY) + p;
    return Src.Load(int3((int)srcPos.x, (int)srcPos.y, 0));
}

float SelectChannel(float4 v, uint channel)
{
    if (channel == MASK_CHANNEL_RED) {
        return v.r;
    }
    if (channel == MASK_CHANNEL_GREEN) {
        return v.g;
    }
    if (channel == MASK_CHANNEL_BLUE) {
        return v.b;
    }
    if (channel == MASK_CHANNEL_ALPHA) {
        return v.a;
    }
    return dot(v.rgb, float3(0.2126, 0.7152, 0.0722));
}

float NormalizeRange(float v, float minValue, float maxValue)
{
    float denom = max(maxValue - minValue, 1.0e-6);
    return saturate((v - minValue) / denom);
}

float3 TurboApprox(float x)
{
    x = saturate(x);
    float4 kRed = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    float4 kGreen = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    float4 kBlue = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    float4 basis = float4(1.0, x, x * x, x * x * x);
    float r = dot(kRed, basis);
    float g = dot(kGreen, basis);
    float b = dot(kBlue, basis);
    return saturate(float3(r, g, b));
}

float3 HeatmapColor(float x, uint mode)
{
    x = saturate(x);
    if (mode == HEATMAP_GRAYSCALE) {
        return float3(x, x, x);
    }
    if (mode == HEATMAP_RED_GREEN) {
        return lerp(float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), x);
    }
    if (mode == HEATMAP_BLUE_RED) {
        return lerp(float3(0.0, 0.1, 1.0), float3(1.0, 0.0, 0.0), x);
    }
    return TurboApprox(x);
}

float4 Palette16(uint index)
{
    uint i = index & 15u;
    if (i == 0u) return float4(0.000, 0.000, 0.000, 1.0);
    if (i == 1u) return float4(0.902, 0.098, 0.294, 1.0);
    if (i == 2u) return float4(0.235, 0.706, 0.294, 1.0);
    if (i == 3u) return float4(1.000, 0.882, 0.098, 1.0);
    if (i == 4u) return float4(0.000, 0.510, 0.784, 1.0);
    if (i == 5u) return float4(0.961, 0.510, 0.188, 1.0);
    if (i == 6u) return float4(0.568, 0.118, 0.706, 1.0);
    if (i == 7u) return float4(0.275, 0.941, 0.941, 1.0);
    if (i == 8u) return float4(0.941, 0.196, 0.902, 1.0);
    if (i == 9u) return float4(0.824, 0.961, 0.235, 1.0);
    if (i == 10u) return float4(0.980, 0.745, 0.831, 1.0);
    if (i == 11u) return float4(0.000, 0.502, 0.502, 1.0);
    if (i == 12u) return float4(0.902, 0.745, 1.000, 1.0);
    if (i == 13u) return float4(0.667, 0.431, 0.157, 1.0);
    if (i == 14u) return float4(1.000, 0.980, 0.784, 1.0);
    return float4(0.502, 0.502, 0.502, 1.0);
}

void Store(uint2 p, float4 color)
{
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = color;
}

#endif
