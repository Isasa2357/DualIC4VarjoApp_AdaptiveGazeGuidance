#ifndef D3D12_PROCESSING_EXTENDED_COMMON_HLSLI
#define D3D12_PROCESSING_EXTENDED_COMMON_HLSLI

#define THREAD_GROUP_X 16
#define THREAD_GROUP_Y 16

static const uint DXGI_FORMAT_R8G8B8A8_UNORM_VALUE = 28;
static const uint DXGI_FORMAT_B8G8R8A8_UNORM_VALUE = 87;
static const uint DXGI_FORMAT_NV12_VALUE = 103;

static const uint PROCESSING_FILTER_POINT = 0;
static const uint PROCESSING_FILTER_LINEAR = 1;
static const uint PROCESSING_REMAP_COORD_ABSOLUTE_PIXELS = 0;
static const uint PROCESSING_REMAP_COORD_NORMALIZED_ZERO_TO_ONE = 1;
static const uint PROCESSING_REMAP_BORDER_CLAMP = 0;
static const uint PROCESSING_REMAP_BORDER_CONSTANT = 1;

static const uint PROCESSING_COMPOSITE_COPY = 0;
static const uint PROCESSING_COMPOSITE_ALPHA_BLEND = 1;
static const uint PROCESSING_COMPOSITE_PREMULTIPLIED_ALPHA = 2;
static const uint PROCESSING_COMPOSITE_ADD = 3;

cbuffer ProcessingConstants : register(b0)
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
    uint SrcMatrix;
    uint SrcRange;
    uint DstMatrix;
    uint DstRange;
    uint Filter;
    uint AlphaMode;
    float ScaleX;
    float ScaleY;
    uint Reserved0;
    uint Reserved1;

    uint MapWidth;
    uint MapHeight;
    uint CoordinateMode;
    uint BorderMode;
    int OverlayX;
    int OverlayY;
    uint OverlayFormat;
    uint BlendMode;
    float Opacity;
    uint Reserved2;
    uint Reserved3;
    uint Reserved4;
    float4 BorderColor;
};

bool InRect(uint2 p, uint w, uint h)
{
    return p.x < w && p.y < h;
}

uint2 OffsetPosition(int x, int y, uint2 p)
{
    return uint2((uint)x, (uint)y) + p;
}

int3 LoadCoord(uint2 p)
{
    return int3((int)p.x, (int)p.y, 0);
}

float4 ToLogicalRgba(float4 v, uint format)
{
    return v;
}

float4 FromLogicalRgba(float4 v, uint format)
{
    return v;
}

#endif
