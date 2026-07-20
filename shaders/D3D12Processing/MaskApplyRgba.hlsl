#include "MaskCommon.hlsli"

Texture2D<float4> Src : register(t0);
Texture2D<float4> MaskTex : register(t1);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatch(p)) return;

    uint2 srcPos = uint2((uint)SrcX, (uint)SrcY) + p;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    float4 src = Src.Load(int3((int)srcPos.x, (int)srcPos.y, 0));
    float m = ReadMask(MaskTex, p, Channel, Invert != 0);
    float s = saturate(Strength);

    float4 applied = src;
    if (Mode == MASK_APPLY_ALPHA) {
        applied.a = src.a * m;
    } else if (Mode == MASK_MULTIPLY_RGB) {
        applied.rgb = src.rgb * m;
    } else if (Mode == MASK_MULTIPLY_RGBA) {
        applied = src * m;
    } else if (Mode == MASK_REPLACE_ALPHA) {
        applied.a = m;
    }

    Dst[dstPos] = saturate(lerp(src, applied, s));
}
