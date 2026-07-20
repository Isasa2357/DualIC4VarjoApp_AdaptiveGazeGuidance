#ifndef D3D12_PROCESSING_COLOR_SPACE_HLSLI
#define D3D12_PROCESSING_COLOR_SPACE_HLSLI

#include "ProcessingCommon.hlsli"

float3 YuvToRgbMatrix(float y, float u, float v, uint matrix)
{
    if (matrix == PROCESSING_MATRIX_BT601) {
        return float3(
            y + 1.402000 * v,
            y - 0.344136 * u - 0.714136 * v,
            y + 1.772000 * u);
    }
    if (matrix == PROCESSING_MATRIX_BT2020) {
        return float3(
            y + 1.474600 * v,
            y - 0.164553 * u - 0.571353 * v,
            y + 1.881400 * u);
    }
    return float3(
        y + 1.574800 * v,
        y - 0.187324 * u - 0.468124 * v,
        y + 1.855600 * u);
}

float3 RgbToYuvMatrix(float3 rgb, uint matrix)
{
    float y;
    float u;
    float v;
    if (matrix == PROCESSING_MATRIX_BT601) {
        y = dot(rgb, float3(0.299000, 0.587000, 0.114000));
        u = (rgb.b - y) / 1.772000;
        v = (rgb.r - y) / 1.402000;
    } else if (matrix == PROCESSING_MATRIX_BT2020) {
        y = dot(rgb, float3(0.262700, 0.678000, 0.059300));
        u = (rgb.b - y) / 1.881400;
        v = (rgb.r - y) / 1.474600;
    } else {
        y = dot(rgb, float3(0.212600, 0.715200, 0.072200));
        u = (rgb.b - y) / 1.855600;
        v = (rgb.r - y) / 1.574800;
    }
    return float3(y, u, v);
}

float3 DecodeYuv(float ySample, float2 uvSample, uint range, uint matrix)
{
    float y;
    float u;
    float v;
    if (range == PROCESSING_RANGE_LIMITED) {
        y = saturate((ySample * 255.0 - 16.0) / 219.0);
        u = (uvSample.x * 255.0 - 128.0) / 224.0;
        v = (uvSample.y * 255.0 - 128.0) / 224.0;
    } else {
        y = ySample;
        u = uvSample.x - 0.5;
        v = uvSample.y - 0.5;
    }
    return saturate(YuvToRgbMatrix(y, u, v, matrix));
}

float3 EncodeYuv(float3 rgb, uint range, uint matrix)
{
    rgb = saturate(rgb);
    float3 yuv = RgbToYuvMatrix(rgb, matrix);
    if (range == PROCESSING_RANGE_LIMITED) {
        yuv.x = (16.0 + 219.0 * yuv.x) / 255.0;
        yuv.y = (128.0 + 224.0 * yuv.y) / 255.0;
        yuv.z = (128.0 + 224.0 * yuv.z) / 255.0;
    } else {
        yuv.y += 0.5;
        yuv.z += 0.5;
    }
    return saturate(yuv);
}

#endif
