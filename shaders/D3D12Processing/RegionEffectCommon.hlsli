#ifndef D3D12_PROCESSING_REGION_EFFECT_COMMON_HLSLI
#define D3D12_PROCESSING_REGION_EFFECT_COMMON_HLSLI

#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint REGION_SHAPE_CIRCLE = 0;
static const uint REGION_SHAPE_RECT = 1;

static const uint REGION_SELECTION_INSIDE = 0;
static const uint REGION_SELECTION_OUTSIDE = 1;

static const uint REGION_EFFECT_DARKEN = 0;
static const uint REGION_EFFECT_TINT = 1;
static const uint REGION_EFFECT_GRAYSCALE = 2;
static const uint REGION_EFFECT_HIGHLIGHT = 3;
static const uint REGION_EFFECT_ALPHA_FADE = 4;
static const uint REGION_EFFECT_VIGNETTE = 5;

cbuffer RegionEffectConstants : register(b0)
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
    uint Shape;
    uint Selection;

    uint Effect;
    uint Reserved0;
    uint Reserved1;
    uint Reserved2;

    float CenterX;
    float CenterY;
    float Radius;
    float EdgeSoftness;

    float RectX;
    float RectY;
    float RectWidth;
    float RectHeight;

    float Strength;
    float Reserved3;
    float Reserved4;
    float Reserved5;

    float4 TintColor;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

bool InDispatchRect(uint2 p)
{
    return p.x < DstWidth && p.y < DstHeight;
}

float CircleInsideAmount(float2 pixel)
{
    float dist = length(pixel - float2(CenterX, CenterY));
    if (EdgeSoftness <= 0.0f) {
        return dist <= Radius ? 1.0f : 0.0f;
    }
    return 1.0f - smoothstep(max(0.0f, Radius - EdgeSoftness), Radius, dist);
}

float RectInsideAmount(float2 pixel)
{
    float left = pixel.x - RectX;
    float right = (RectX + RectWidth) - pixel.x;
    float top = pixel.y - RectY;
    float bottom = (RectY + RectHeight) - pixel.y;
    float signedDistanceToEdge = min(min(left, right), min(top, bottom));

    if (EdgeSoftness <= 0.0f) {
        return signedDistanceToEdge >= 0.0f ? 1.0f : 0.0f;
    }

    return saturate(signedDistanceToEdge / EdgeSoftness);
}

float RegionAmount(uint2 localDst)
{
    float2 pixel = float2((float)(DstX + (int)localDst.x) + 0.5f,
                          (float)(DstY + (int)localDst.y) + 0.5f);

    float inside = (Shape == REGION_SHAPE_RECT) ? RectInsideAmount(pixel) : CircleInsideAmount(pixel);
    return (Selection == REGION_SELECTION_OUTSIDE) ? (1.0f - inside) : inside;
}

float3 Grayscale(float3 rgb)
{
    float y = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
    return y.xxx;
}

float4 ApplyRegionEffect(float4 src, float amount)
{
    float s = saturate(Strength);
    float a = saturate(amount);
    float4 target = src;

    if (Effect == REGION_EFFECT_DARKEN) {
        target.rgb = src.rgb * s;
    } else if (Effect == REGION_EFFECT_TINT) {
        target.rgb = lerp(src.rgb, TintColor.rgb, s);
    } else if (Effect == REGION_EFFECT_GRAYSCALE) {
        target.rgb = lerp(src.rgb, Grayscale(src.rgb), s);
    } else if (Effect == REGION_EFFECT_HIGHLIGHT) {
        target.rgb = saturate(src.rgb + TintColor.rgb * s);
    } else if (Effect == REGION_EFFECT_ALPHA_FADE) {
        target.a = src.a * s;
    } else if (Effect == REGION_EFFECT_VIGNETTE) {
        target.rgb = src.rgb * (1.0f - s);
    }

    return lerp(src, target, a);
}

#endif
