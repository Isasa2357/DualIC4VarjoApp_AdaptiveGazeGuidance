#include "MaskCommon.hlsli"

Texture2D<float4> MaskA : register(t0);
Texture2D<float4> MaskB : register(t1);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatch(p)) return;

    float a = ReadMaskAt(MaskA, p, SrcX, SrcY, Channel, Invert != 0);
    float b = ReadMaskAt(MaskB, p, MaskX, MaskY, ChannelB, InvertB != 0);

    float v = 0.0;
    if (Mode == MASK_COMBINE_ADD) v = a + b;
    else if (Mode == MASK_COMBINE_MULTIPLY) v = a * b;
    else if (Mode == MASK_COMBINE_MAX) v = max(a, b);
    else if (Mode == MASK_COMBINE_MIN) v = min(a, b);
    else if (Mode == MASK_COMBINE_SUBTRACT) v = a - b;

    v = saturate(v * Scale + Bias);
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = float4(v, v, v, v);
}
