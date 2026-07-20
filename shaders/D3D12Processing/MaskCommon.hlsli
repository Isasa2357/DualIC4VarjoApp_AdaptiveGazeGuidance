#ifndef D3D12_PROCESSING_MASK_COMMON_HLSLI
#define D3D12_PROCESSING_MASK_COMMON_HLSLI

#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint MASK_CHANNEL_RED = 0;
static const uint MASK_CHANNEL_GREEN = 1;
static const uint MASK_CHANNEL_BLUE = 2;
static const uint MASK_CHANNEL_ALPHA = 3;
static const uint MASK_CHANNEL_LUMA = 4;

static const uint MASK_APPLY_ALPHA = 0;
static const uint MASK_MULTIPLY_RGB = 1;
static const uint MASK_MULTIPLY_RGBA = 2;
static const uint MASK_REPLACE_ALPHA = 3;

static const uint MASK_COMBINE_ADD = 0;
static const uint MASK_COMBINE_MULTIPLY = 1;
static const uint MASK_COMBINE_MAX = 2;
static const uint MASK_COMBINE_MIN = 3;
static const uint MASK_COMBINE_SUBTRACT = 4;

cbuffer MaskConstants : register(b0)
{
    uint Width;
    uint Height;
    int SrcX;
    int SrcY;

    int MaskX;
    int MaskY;
    int DstX;
    int DstY;

    int OverlayX;
    int OverlayY;
    uint Mode;
    uint Channel;

    uint ChannelB;
    uint Invert;
    uint InvertB;
    uint Reserved0;

    float Strength;
    float Opacity;
    float Scale;
    float Bias;
};

float MaskChannelValue(float4 c, uint channel)
{
    if (channel == MASK_CHANNEL_RED) return c.r;
    if (channel == MASK_CHANNEL_GREEN) return c.g;
    if (channel == MASK_CHANNEL_BLUE) return c.b;
    if (channel == MASK_CHANNEL_ALPHA) return c.a;
    return dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
}

float ReadMask(Texture2D<float4> maskTex, uint2 p, uint channel, bool invertMask)
{
    uint2 pos = uint2((uint)MaskX, (uint)MaskY) + p;
    float m = saturate(MaskChannelValue(maskTex.Load(int3((int)pos.x, (int)pos.y, 0)), channel));
    return invertMask ? (1.0 - m) : m;
}

float ReadMaskAt(Texture2D<float4> maskTex, uint2 p, int x, int y, uint channel, bool invertMask)
{
    uint2 pos = uint2((uint)x, (uint)y) + p;
    float m = saturate(MaskChannelValue(maskTex.Load(int3((int)pos.x, (int)pos.y, 0)), channel));
    return invertMask ? (1.0 - m) : m;
}

bool InDispatch(uint2 p)
{
    return p.x < Width && p.y < Height;
}

#endif
