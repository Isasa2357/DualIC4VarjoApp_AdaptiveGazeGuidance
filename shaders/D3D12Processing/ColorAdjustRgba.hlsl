#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

cbuffer ColorAdjustConstants : register(b0)
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
    uint PreserveAlpha;
    uint Reserved0;

    float Brightness;
    float Contrast;
    float Gamma;
    float Saturation;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

float3 ApplyColorAdjust(float3 rgb)
{
    rgb = (rgb - 0.5) * Contrast + 0.5 + Brightness;

    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    rgb = lerp(float3(luma, luma, luma), rgb, Saturation);

    rgb = saturate(rgb);
    rgb = pow(rgb, 1.0 / Gamma);
    return saturate(rgb);
}

float ApplyScalarAdjust(float v)
{
    v = (v - 0.5) * Contrast + 0.5 + Brightness;
    v = saturate(v);
    v = pow(v, 1.0 / Gamma);
    return saturate(v);
}

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= DstWidth || p.y >= DstHeight) {
        return;
    }

    uint2 srcPos = uint2((uint)SrcX, (uint)SrcY) + p;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;

    float4 c = Src.Load(int3((int)srcPos.x, (int)srcPos.y, 0));
    float originalAlpha = c.a;
    c.rgb = ApplyColorAdjust(c.rgb);
    c.a = (PreserveAlpha != 0) ? originalAlpha : ApplyScalarAdjust(c.a);
    Dst[dstPos] = c;
}
