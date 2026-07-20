Texture2D<float4> Src : register(t0);
Texture3D<float4> Lut : register(t1);
RWTexture2D<float4> Dst : register(u0);

cbuffer Lut3DConstants : register(b0)
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
    uint LutWidth;
    uint LutHeight;

    uint LutDepth;
    uint PreserveAlpha;
    float Strength;
    uint Reserved0;
};

float4 ToLogicalRgba(float4 v, uint format)
{
    return v;
}

float4 FromLogicalRgba(float4 v, uint format)
{
    return v;
}

float4 LoadSource(uint2 p)
{
    int2 q = int2((int)p.x + SrcX, (int)p.y + SrcY);
    return ToLogicalRgba(Src.Load(int3(q, 0)), SrcFormat);
}

float4 LoadLut(int3 p)
{
    int3 maxP = int3((int)LutWidth - 1, (int)LutHeight - 1, (int)LutDepth - 1);
    return Lut.Load(int4(clamp(p, int3(0, 0, 0), maxP), 0));
}

float4 SampleLut(float3 rgb)
{
    float3 extent = float3(
        (LutWidth  > 1) ? (float)(LutWidth  - 1) : 0.0f,
        (LutHeight > 1) ? (float)(LutHeight - 1) : 0.0f,
        (LutDepth  > 1) ? (float)(LutDepth  - 1) : 0.0f);

    float3 coord = saturate(rgb) * extent;
    float3 baseF = floor(coord);
    float3 f = coord - baseF;
    int3 p000 = int3(baseF);
    int3 p100 = p000 + int3(1, 0, 0);
    int3 p010 = p000 + int3(0, 1, 0);
    int3 p110 = p000 + int3(1, 1, 0);
    int3 p001 = p000 + int3(0, 0, 1);
    int3 p101 = p000 + int3(1, 0, 1);
    int3 p011 = p000 + int3(0, 1, 1);
    int3 p111 = p000 + int3(1, 1, 1);

    float4 c000 = LoadLut(p000);
    float4 c100 = LoadLut(p100);
    float4 c010 = LoadLut(p010);
    float4 c110 = LoadLut(p110);
    float4 c001 = LoadLut(p001);
    float4 c101 = LoadLut(p101);
    float4 c011 = LoadLut(p011);
    float4 c111 = LoadLut(p111);

    float4 c00 = lerp(c000, c100, f.x);
    float4 c10 = lerp(c010, c110, f.x);
    float4 c01 = lerp(c001, c101, f.x);
    float4 c11 = lerp(c011, c111, f.x);
    float4 c0 = lerp(c00, c10, f.y);
    float4 c1 = lerp(c01, c11, f.y);
    return lerp(c0, c1, f.z);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= DstWidth || p.y >= DstHeight) {
        return;
    }

    float4 src = LoadSource(p);
    float4 lut = SampleLut(src.rgb);
    float4 outColor = lerp(src, lut, saturate(Strength));
    if (PreserveAlpha != 0) {
        outColor.a = src.a;
    }

    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    Dst[dstPos] = FromLogicalRgba(saturate(outColor), DstFormat);
}
