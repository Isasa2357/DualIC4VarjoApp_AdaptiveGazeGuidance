#include "MaskCommon.hlsli"

Texture2D<float4> Base : register(t0);
Texture2D<float4> Overlay : register(t1);
Texture2D<float4> MaskTex : register(t2);
RWTexture2D<float4> Dst : register(u0);

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatch(p)) return;

    uint2 basePos = uint2((uint)SrcX, (uint)SrcY) + p;
    uint2 overlayPos = uint2((uint)OverlayX, (uint)OverlayY) + p;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;

    float4 baseColor = Base.Load(int3((int)basePos.x, (int)basePos.y, 0));
    float4 overlayColor = Overlay.Load(int3((int)overlayPos.x, (int)overlayPos.y, 0));
    float m = saturate(ReadMask(MaskTex, p, Channel, Invert != 0) * saturate(Opacity));

    Dst[dstPos] = saturate(lerp(baseColor, overlayColor, m));
}
