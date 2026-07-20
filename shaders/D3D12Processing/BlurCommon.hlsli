#ifndef D3D12_PROCESSING_BLUR_COMMON_HLSLI
#define D3D12_PROCESSING_BLUR_COMMON_HLSLI

#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint BLUR_EDGE_CLAMP = 0;
static const uint BLUR_EDGE_CONSTANT = 1;

cbuffer BlurConstants : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;

    int SrcX;
    int SrcY;
    int DstX;
    int DstY;

    uint Radius;
    uint EdgeMode;
    uint Reserved0;
    uint Reserved1;

    float4 BorderColor;

    float4 Weights0;
    float4 Weights1;
    float4 Weights2;
    float4 Weights3;
    float4 Weights4;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

bool InDispatchRect(uint2 p)
{
    return p.x < DstWidth && p.y < DstHeight;
}

float WeightAt(uint index)
{
    if (index < 4) {
        return Weights0[index];
    }
    if (index < 8) {
        return Weights1[index - 4];
    }
    if (index < 12) {
        return Weights2[index - 8];
    }
    if (index < 16) {
        return Weights3[index - 12];
    }
    return Weights4[index - 16];
}

bool ResolveSampleCoord(int2 localPos, out uint2 samplePos)
{
    if (localPos.x >= 0 && localPos.y >= 0 &&
        localPos.x < (int)SrcWidth && localPos.y < (int)SrcHeight) {
        samplePos = uint2((uint)SrcX, (uint)SrcY) + uint2(localPos);
        return true;
    }

    if (EdgeMode == BLUR_EDGE_CLAMP) {
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

float4 BlurPixel(uint2 p, int2 direction)
{
    int2 basePos = int2((int)p.x, (int)p.y);
    float4 sum = SampleSource(basePos) * WeightAt(0);

    [loop]
    for (uint i = 1; i <= Radius; ++i) {
        int2 delta = direction * (int)i;
        float w = WeightAt(i);
        sum += SampleSource(basePos + delta) * w;
        sum += SampleSource(basePos - delta) * w;
    }

    return sum;
}

void StoreBlurredPixel(uint2 p, int2 direction)
{
    if (!InDispatchRect(p)) {
        return;
    }

    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = BlurPixel(p, direction);
}

#endif
