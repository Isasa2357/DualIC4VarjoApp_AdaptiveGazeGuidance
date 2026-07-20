#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint REGION_SHAPE_CIRCLE = 0;
static const uint REGION_SHAPE_RECT = 1;

static const uint REGION_SELECTION_INSIDE = 0;
static const uint REGION_SELECTION_OUTSIDE = 1;

cbuffer RegionBlurBlendConstants : register(b0)
{
    uint SrcWidth;
    uint SrcHeight;
    uint DstWidth;
    uint DstHeight;

    int SrcX;
    int SrcY;
    int DstX;
    int DstY;

    uint Shape;
    uint Selection;
    uint Reserved0;
    uint Reserved1;

    float CenterX;
    float CenterY;
    float Radius;
    float EdgeSoftness;

    float RectX;
    float RectY;
    float RectWidth;
    float RectHeight;

    float BlurStrength;
    float Reserved2;
    float Reserved3;
    float Reserved4;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> Blurred : register(t1);
RWTexture2D<float4> Dst : register(u0);

bool InDispatchRect(uint2 p)
{
    return p.x < DstWidth && p.y < DstHeight;
}

float CircleInsideFactor(float2 pos)
{
    float d = length(pos - float2(CenterX, CenterY));
    if (EdgeSoftness <= 0.00001f) {
        return d <= Radius ? 1.0f : 0.0f;
    }
    return 1.0f - smoothstep(Radius, Radius + EdgeSoftness, d);
}

float RectInsideFactor(float2 pos)
{
    float2 halfSize = float2(RectWidth, RectHeight) * 0.5f;
    float2 center = float2(RectX, RectY) + halfSize;
    float2 q = abs(pos - center) - halfSize;
    float outsideDistance = length(max(q, float2(0.0f, 0.0f)));
    float insideDistance = min(max(q.x, q.y), 0.0f);
    float signedDistance = outsideDistance + insideDistance;

    if (EdgeSoftness <= 0.00001f) {
        return signedDistance <= 0.0f ? 1.0f : 0.0f;
    }
    return 1.0f - smoothstep(0.0f, EdgeSoftness, signedDistance);
}

float RegionMask(float2 dstPixelCenter)
{
    float inside = Shape == REGION_SHAPE_RECT ? RectInsideFactor(dstPixelCenter) : CircleInsideFactor(dstPixelCenter);
    return Selection == REGION_SELECTION_OUTSIDE ? (1.0f - inside) : inside;
}

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    uint2 srcPos = uint2((uint)SrcX, (uint)SrcY) + p;
    uint2 dstPos = uint2((uint)DstX, (uint)DstY) + p;
    float2 dstCenter = float2((float)dstPos.x + 0.5f, (float)dstPos.y + 0.5f);

    float4 original = Original.Load(int3((int)srcPos.x, (int)srcPos.y, 0));
    float4 blurred = Blurred.Load(int3((int)p.x, (int)p.y, 0));
    float mask = saturate(RegionMask(dstCenter) * BlurStrength);

    Dst[dstPos] = saturate(lerp(original, blurred, mask));
}
