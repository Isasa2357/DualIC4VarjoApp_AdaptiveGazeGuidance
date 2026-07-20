#include "ProcessingCommon.hlsli"
#include "YuvPrimitives.hlsli"

RWTexture2D<float4> Dst : register(u0);

float3 DecodeKnownCode(float3 code, uint format, uint range, uint matrix)
{
    const float3 sampleValue = D3D12YuvCodeToSample(code, format);
    return D3D12DecodeYuvSample(
        sampleValue.x,
        sampleValue.yz,
        format,
        range,
        matrix);
}

[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= 8u || tid.y != 0u || tid.z != 0u) {
        return;
    }

    float3 rgb = 0.0f;
    switch (tid.x) {
    case 0u:
        rgb = DecodeKnownCode(float3(81.0f, 90.0f, 240.0f),
            DXGI_FORMAT_NV12_VALUE, PROCESSING_RANGE_LIMITED, PROCESSING_MATRIX_BT601);
        break;
    case 1u:
        rgb = DecodeKnownCode(float3(63.0f, 102.0f, 240.0f),
            DXGI_FORMAT_NV12_VALUE, PROCESSING_RANGE_LIMITED, PROCESSING_MATRIX_BT709);
        break;
    case 2u:
        rgb = DecodeKnownCode(float3(74.0f, 97.0f, 240.0f),
            DXGI_FORMAT_NV12_VALUE, PROCESSING_RANGE_LIMITED, PROCESSING_MATRIX_BT2020);
        break;
    case 3u:
        rgb = DecodeKnownCode(float3(16.0f, 128.0f, 128.0f),
            DXGI_FORMAT_NV12_VALUE, PROCESSING_RANGE_LIMITED, PROCESSING_MATRIX_BT709);
        break;
    case 4u:
        rgb = DecodeKnownCode(float3(235.0f, 128.0f, 128.0f),
            DXGI_FORMAT_NV12_VALUE, PROCESSING_RANGE_LIMITED, PROCESSING_MATRIX_BT709);
        break;
    case 5u:
        rgb = DecodeKnownCode(float3(250.0f, 409.0f, 960.0f),
            DXGI_FORMAT_P010_VALUE, PROCESSING_RANGE_LIMITED, PROCESSING_MATRIX_BT709);
        break;
    case 6u:
        rgb = DecodeKnownCode(float3(54.0f, 99.0f, 255.0f),
            DXGI_FORMAT_NV12_VALUE, PROCESSING_RANGE_FULL, PROCESSING_MATRIX_BT709);
        break;
    default:
        rgb = DecodeKnownCode(float3(217.0f, 395.0f, 1023.0f),
            DXGI_FORMAT_P010_VALUE, PROCESSING_RANGE_FULL, PROCESSING_MATRIX_BT709);
        break;
    }

    Dst[uint2(tid.x, 0u)] = float4(rgb, 1.0f);
}
