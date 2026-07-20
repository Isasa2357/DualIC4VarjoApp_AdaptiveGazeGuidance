#ifndef D3D12_PROCESSING_YUV_PRIMITIVES_HLSLI
#define D3D12_PROCESSING_YUV_PRIMITIVES_HLSLI

#include "ProcessingCommon.hlsli"

// Reusable, format-aware YUV helpers for application-owned fused shaders.
//
// NV12 plane views expose 8-bit codes as R8/R8G8 UNORM samples.
// P010 plane views expose the 16-bit storage words as R16/R16G16 UNORM;
// the 10-bit video code is stored in the most-significant bits (code << 6).
// These helpers convert through integer video-code space so NV12 and P010 use
// the same color equations without treating a P010 sample as ordinary R16.

float D3D12YuvCodeMaximum(uint format)
{
    return (format == DXGI_FORMAT_P010_VALUE) ? 1023.0f : 255.0f;
}

float D3D12YuvEightBitScale(uint format)
{
    return (format == DXGI_FORMAT_P010_VALUE) ? 4.0f : 1.0f;
}

float D3D12YuvChromaCenterCode(uint format)
{
    return 128.0f * D3D12YuvEightBitScale(format);
}

float D3D12YuvSampleToCode(float sampleValue, uint format)
{
    sampleValue = saturate(sampleValue);
    if (format == DXGI_FORMAT_P010_VALUE) {
        // Recover the 10-bit code stored in bits 15:6.
        return clamp(floor(sampleValue * 65535.0f / 64.0f + 0.5f), 0.0f, 1023.0f);
    }
    return clamp(floor(sampleValue * 255.0f + 0.5f), 0.0f, 255.0f);
}

float2 D3D12YuvSampleToCode(float2 sampleValue, uint format)
{
    return float2(
        D3D12YuvSampleToCode(sampleValue.x, format),
        D3D12YuvSampleToCode(sampleValue.y, format));
}

float3 D3D12YuvSampleToCode(float3 sampleValue, uint format)
{
    return float3(
        D3D12YuvSampleToCode(sampleValue.x, format),
        D3D12YuvSampleToCode(sampleValue.y, format),
        D3D12YuvSampleToCode(sampleValue.z, format));
}

float D3D12YuvCodeToSample(float codeValue, uint format)
{
    codeValue = clamp(floor(codeValue + 0.5f), 0.0f, D3D12YuvCodeMaximum(format));
    if (format == DXGI_FORMAT_P010_VALUE) {
        return (codeValue * 64.0f) / 65535.0f;
    }
    return codeValue / 255.0f;
}

float2 D3D12YuvCodeToSample(float2 codeValue, uint format)
{
    return float2(
        D3D12YuvCodeToSample(codeValue.x, format),
        D3D12YuvCodeToSample(codeValue.y, format));
}

float3 D3D12YuvCodeToSample(float3 codeValue, uint format)
{
    return float3(
        D3D12YuvCodeToSample(codeValue.x, format),
        D3D12YuvCodeToSample(codeValue.y, format),
        D3D12YuvCodeToSample(codeValue.z, format));
}

float3 D3D12YuvToRgbSignal(float y, float u, float v, uint matrix)
{
    if (matrix == PROCESSING_MATRIX_BT601) {
        return float3(
            y + 1.402000f * v,
            y - 0.344136f * u - 0.714136f * v,
            y + 1.772000f * u);
    }
    if (matrix == PROCESSING_MATRIX_BT2020) {
        return float3(
            y + 1.474600f * v,
            y - 0.164553f * u - 0.571353f * v,
            y + 1.881400f * u);
    }

    // Identity is intentionally treated as BT.709 for YUV input, preserving the
    // Processing layer's historical fallback behavior.
    return float3(
        y + 1.574800f * v,
        y - 0.187324f * u - 0.468124f * v,
        y + 1.855600f * u);
}

float3 D3D12RgbToYuvSignal(float3 rgb, uint matrix)
{
    rgb = saturate(rgb);

    float y;
    float u;
    float v;
    if (matrix == PROCESSING_MATRIX_BT601) {
        y = dot(rgb, float3(0.299000f, 0.587000f, 0.114000f));
        u = (rgb.b - y) / 1.772000f;
        v = (rgb.r - y) / 1.402000f;
    } else if (matrix == PROCESSING_MATRIX_BT2020) {
        y = dot(rgb, float3(0.262700f, 0.678000f, 0.059300f));
        u = (rgb.b - y) / 1.881400f;
        v = (rgb.r - y) / 1.474600f;
    } else {
        y = dot(rgb, float3(0.212600f, 0.715200f, 0.072200f));
        u = (rgb.b - y) / 1.855600f;
        v = (rgb.r - y) / 1.574800f;
    }
    return float3(y, u, v);
}

float3 D3D12DecodeYuvCode(float3 yuvCode, uint format, uint range, uint matrix)
{
    const float scale = D3D12YuvEightBitScale(format);
    const float maxCode = D3D12YuvCodeMaximum(format);

    float y;
    float u;
    float v;
    if (range == PROCESSING_RANGE_LIMITED) {
        y = (yuvCode.x - 16.0f * scale) / (219.0f * scale);
        u = (yuvCode.y - 128.0f * scale) / (224.0f * scale);
        v = (yuvCode.z - 128.0f * scale) / (224.0f * scale);
    } else {
        y = yuvCode.x / maxCode;
        u = (yuvCode.y - D3D12YuvChromaCenterCode(format)) / maxCode;
        v = (yuvCode.z - D3D12YuvChromaCenterCode(format)) / maxCode;
    }

    return saturate(D3D12YuvToRgbSignal(y, u, v, matrix));
}

float3 D3D12DecodeYuvSample(
    float ySample,
    float2 uvSample,
    uint format,
    uint range,
    uint matrix)
{
    const float3 code = float3(
        D3D12YuvSampleToCode(ySample, format),
        D3D12YuvSampleToCode(uvSample, format));
    return D3D12DecodeYuvCode(code, format, range, matrix);
}

float3 D3D12EncodeRgbToYuvCode(float3 rgb, uint format, uint range, uint matrix)
{
    const float3 signal = D3D12RgbToYuvSignal(rgb, matrix);
    const float scale = D3D12YuvEightBitScale(format);
    const float maxCode = D3D12YuvCodeMaximum(format);

    float3 code;
    if (range == PROCESSING_RANGE_LIMITED) {
        code.x = 16.0f * scale + 219.0f * scale * signal.x;
        code.y = 128.0f * scale + 224.0f * scale * signal.y;
        code.z = 128.0f * scale + 224.0f * scale * signal.z;
        code.x = clamp(code.x, 16.0f * scale, 235.0f * scale);
        code.yz = clamp(code.yz, 16.0f * scale, 240.0f * scale);
    } else {
        code.x = maxCode * signal.x;
        code.y = D3D12YuvChromaCenterCode(format) + maxCode * signal.y;
        code.z = D3D12YuvChromaCenterCode(format) + maxCode * signal.z;
        code = clamp(code, 0.0f, maxCode);
    }

    return floor(code + 0.5f);
}

float3 D3D12EncodeRgbToYuvSample(float3 rgb, uint format, uint range, uint matrix)
{
    return D3D12YuvCodeToSample(
        D3D12EncodeRgbToYuvCode(rgb, format, range, matrix),
        format);
}

float3 D3D12LoadYuv420Rgb(
    Texture2D<float> yPlane,
    Texture2D<float2> uvPlane,
    uint2 absoluteLumaPixel,
    uint format,
    uint range,
    uint matrix)
{
    const float y = yPlane.Load(LoadCoord(absoluteLumaPixel));
    const float2 uv = uvPlane.Load(LoadCoord(absoluteLumaPixel / 2u));
    return D3D12DecodeYuvSample(y, uv, format, range, matrix);
}

float2 D3D12ResizeSourcePosition(uint2 destinationLocalPixel, float2 scale)
{
    return (float2(destinationLocalPixel) + 0.5f) * scale - 0.5f;
}

int2 D3D12ClampPixel(int2 pixel, uint2 size)
{
    return clamp(pixel, int2(0, 0), int2(int(size.x) - 1, int(size.y) - 1));
}

float3 D3D12SampleYuv420RgbPoint(
    Texture2D<float> yPlane,
    Texture2D<float2> uvPlane,
    uint2 destinationLocalPixel,
    uint2 sourceOrigin,
    uint2 sourceSize,
    float2 scale,
    uint format,
    uint range,
    uint matrix)
{
    const float2 sourcePosition = D3D12ResizeSourcePosition(destinationLocalPixel, scale);
    const int2 localPixel = D3D12ClampPixel(int2(round(sourcePosition)), sourceSize);
    return D3D12LoadYuv420Rgb(
        yPlane,
        uvPlane,
        sourceOrigin + uint2(localPixel),
        format,
        range,
        matrix);
}

float3 D3D12SampleYuv420RgbLinear(
    Texture2D<float> yPlane,
    Texture2D<float2> uvPlane,
    uint2 destinationLocalPixel,
    uint2 sourceOrigin,
    uint2 sourceSize,
    float2 scale,
    uint format,
    uint range,
    uint matrix)
{
    const float2 sourcePosition = D3D12ResizeSourcePosition(destinationLocalPixel, scale);
    const float2 basePosition = floor(sourcePosition);
    const float2 fraction = sourcePosition - basePosition;

    const int2 p00 = D3D12ClampPixel(int2(basePosition), sourceSize);
    const int2 p11 = D3D12ClampPixel(int2(basePosition) + int2(1, 1), sourceSize);

    const float3 c00 = D3D12LoadYuv420Rgb(
        yPlane, uvPlane, sourceOrigin + uint2(p00.x, p00.y), format, range, matrix);
    const float3 c10 = D3D12LoadYuv420Rgb(
        yPlane, uvPlane, sourceOrigin + uint2(p11.x, p00.y), format, range, matrix);
    const float3 c01 = D3D12LoadYuv420Rgb(
        yPlane, uvPlane, sourceOrigin + uint2(p00.x, p11.y), format, range, matrix);
    const float3 c11 = D3D12LoadYuv420Rgb(
        yPlane, uvPlane, sourceOrigin + uint2(p11.x, p11.y), format, range, matrix);

    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

float4 D3D12LoadLogicalRgba(
    Texture2D<float4> source,
    uint2 absolutePixel,
    uint format)
{
    return ToLogicalRgba(source.Load(LoadCoord(absolutePixel)), format);
}

float4 D3D12SampleLogicalRgbaPoint(
    Texture2D<float4> source,
    uint2 destinationLocalPixel,
    uint2 sourceOrigin,
    uint2 sourceSize,
    float2 scale,
    uint format)
{
    const float2 sourcePosition = D3D12ResizeSourcePosition(destinationLocalPixel, scale);
    const int2 localPixel = D3D12ClampPixel(int2(round(sourcePosition)), sourceSize);
    return D3D12LoadLogicalRgba(source, sourceOrigin + uint2(localPixel), format);
}

float4 D3D12SampleLogicalRgbaLinear(
    Texture2D<float4> source,
    uint2 destinationLocalPixel,
    uint2 sourceOrigin,
    uint2 sourceSize,
    float2 scale,
    uint format)
{
    const float2 sourcePosition = D3D12ResizeSourcePosition(destinationLocalPixel, scale);
    const float2 basePosition = floor(sourcePosition);
    const float2 fraction = sourcePosition - basePosition;

    const int2 p00 = D3D12ClampPixel(int2(basePosition), sourceSize);
    const int2 p11 = D3D12ClampPixel(int2(basePosition) + int2(1, 1), sourceSize);

    const float4 c00 = D3D12LoadLogicalRgba(source, sourceOrigin + uint2(p00.x, p00.y), format);
    const float4 c10 = D3D12LoadLogicalRgba(source, sourceOrigin + uint2(p11.x, p00.y), format);
    const float4 c01 = D3D12LoadLogicalRgba(source, sourceOrigin + uint2(p00.x, p11.y), format);
    const float4 c11 = D3D12LoadLogicalRgba(source, sourceOrigin + uint2(p11.x, p11.y), format);

    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

void D3D12StoreYuv420Luma(
    RWTexture2D<float> yPlane,
    uint2 absolutePixel,
    float3 rgb,
    uint format,
    uint range,
    uint matrix)
{
    const float3 yuvSample = D3D12EncodeRgbToYuvSample(rgb, format, range, matrix);
    yPlane[absolutePixel] = yuvSample.x;
}

void D3D12StoreYuv420Chroma(
    RWTexture2D<float2> uvPlane,
    uint2 absoluteChromaPixel,
    float3 averageRgb,
    uint format,
    uint range,
    uint matrix)
{
    const float3 yuvSample = D3D12EncodeRgbToYuvSample(averageRgb, format, range, matrix);
    uvPlane[absoluteChromaPixel] = yuvSample.yz;
}

#endif
