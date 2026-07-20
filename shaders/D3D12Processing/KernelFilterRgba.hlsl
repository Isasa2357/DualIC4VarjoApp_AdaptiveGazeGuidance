#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint KERNEL_EDGE_CLAMP = 0;
static const uint KERNEL_EDGE_CONSTANT = 1;

cbuffer KernelFilterConstants : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;

    int SrcX;
    int SrcY;
    int DstX;
    int DstY;

    uint EdgeMode;
    uint PreserveAlpha;
    uint Reserved0;
    uint Reserved1;

    float Scale;
    float Bias;
    float Reserved2;
    float Reserved3;

    float4 BorderColor;

    float4 Kernel0;
    float4 Kernel1;
    float4 Kernel2;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

float KernelAt(uint index)
{
    if (index < 4) {
        return Kernel0[index];
    }
    if (index < 8) {
        return Kernel1[index - 4];
    }
    return Kernel2[index - 8];
}

bool ResolveSampleCoord(int2 localPos, out uint2 samplePos)
{
    if (localPos.x >= 0 && localPos.y >= 0 &&
        localPos.x < (int)SrcWidth && localPos.y < (int)SrcHeight) {
        samplePos = uint2((uint)SrcX, (uint)SrcY) + uint2(localPos);
        return true;
    }

    if (EdgeMode == KERNEL_EDGE_CLAMP) {
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

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= DstWidth || p.y >= DstHeight) {
        return;
    }

    int2 basePos = int2((int)p.x, (int)p.y);
    float4 center = SampleSource(basePos);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);

    [unroll]
    for (int ky = -1; ky <= 1; ++ky) {
        [unroll]
        for (int kx = -1; kx <= 1; ++kx) {
            uint index = (uint)((ky + 1) * 3 + (kx + 1));
            sum += SampleSource(basePos + int2(kx, ky)) * KernelAt(index);
        }
    }

    float4 outColor = saturate(sum * Scale + Bias);
    if (PreserveAlpha != 0) {
        outColor.a = center.a;
    }

    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = outColor;
}
