#pragma once

namespace DualIC4Varjo {

// The rectification prepass has one output UAV. Apply the two Gaussian axes
// within a tile so the horizontal intermediate stays in group-shared memory.
inline constexpr char kBlurWaveComputeHlsl[] = R"hlsl(
static const int TILE = 8;
static const int BASE_RADIUS = 9;
static const int MAX_RADIUS = 39; // ceil(3 * 13 px)
static const int MAX_EXTENT = TILE + 2 * MAX_RADIUS;
groupshared float3 sourceRows[TILE][MAX_EXTENT];
groupshared float3 baseRows[TILE + 2 * BASE_RADIUS][TILE];
groupshared float3 waveRows[MAX_EXTENT][TILE];

// Normalized half-kernel for sigma = 3 px, truncated at three sigma.
static const float baseWeights[10] = {
    0.133175996, 0.125979094, 0.106639002, 0.080775325, 0.054750289,
    0.033207729, 0.018023411, 0.008753462, 0.003804239, 0.001479452
};

float WaveWeight(int index)
{
    return waveWeights[index / 4][index % 4];
}

float RingDistance(float r)
{
    return min(abs(r - rings.x), min(abs(r - rings.y), abs(r - rings.z)));
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID)
{
    int2 tileMin = int2(group.xy) * TILE;
    int2 p = tileMin + int2(lane.xy);
    int2 last = int2(dstWidth, dstHeight) - 1;
    if (blend.x <= 0.0f) {
        if (all(p <= last)) xrOutput[p] = SampleRemappedOutputPixel(float2(p));
        return;
    }

    // These decisions are identical for all lanes, including partial edge tiles.
    float2 tileMax = float2(min(tileMin + TILE - 1, last));
    float2 center = roiPixels.xy;
    float2 farthest = max(abs(float2(tileMin) - center), abs(tileMax - center));
    if (length(farthest / roiPixels.zw) <= 1.0f) {
        if (all(p <= last)) xrOutput[p] = SampleRemappedOutputPixel(float2(p));
        return;
    }

    float nearest = length(clamp(center, float2(tileMin), tileMax) - center);
    float farRadius = length(farthest);
    float band = 0.05f * dstWidth;
    float3 radius = rings.xyz;
    float2 gazeDelta = (frameParams.xy * float2(last) - center) / roiPixels.zw;
    bool waveTile = rings.w > 0.0f && dot(gazeDelta, gazeDelta) > 1.0f &&
        any(max(nearest - radius, radius - farRadius) <= band);
    int halo = waveTile ? int(rings.w) : BASE_RADIUS;
    int extent = TILE + 2 * halo;
    int x = int(lane.x);
    int y = int(lane.y);

    // Stage eight rectified rows at a time, clamping in the rectified image
    // before applying the inverse homography (the Viewer's border convention).
    [loop] for (int block = 0; block < extent; block += TILE) {
        int row = block + y;
        [loop] for (int col = x; col < extent; col += TILE) {
            int2 q = clamp(tileMin + int2(col - halo, row - halo), int2(0,0), last);
            sourceRows[y][col] = SampleRemappedOutputPixel(float2(q)).rgb;
        }
        GroupMemoryBarrierWithGroupSync();
        int baseRow = row - (halo - BASE_RADIUS);
        if (baseRow >= 0 && baseRow < TILE + 2 * BASE_RADIUS) {
            float3 sum = sourceRows[y][x + halo] * baseWeights[0];
            [unroll] for (int i = 1; i <= BASE_RADIUS; ++i)
                sum += (sourceRows[y][x + halo - i] + sourceRows[y][x + halo + i]) * baseWeights[i];
            baseRows[baseRow][x] = sum;
        }
        if (waveTile && row < extent) {
            float3 sum = sourceRows[y][x + halo] * WaveWeight(0);
            [loop] for (int i = 1; i <= halo; ++i)
                sum += (sourceRows[y][x + halo - i] + sourceRows[y][x + halo + i]) * WaveWeight(i);
            waveRows[row][x] = sum;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (any(p > last)) return;
    float4 original = SampleRemappedOutputPixel(float2(p));
    float2 delta = float2(p) - center;
    if (length(delta / roiPixels.zw) <= 1.0f) {
        xrOutput[p] = original;
        return;
    }
    float3 filtered = baseRows[y + BASE_RADIUS][x] * baseWeights[0];
    [unroll] for (int i = 1; i <= BASE_RADIUS; ++i)
        filtered += (baseRows[y + BASE_RADIUS - i][x] + baseRows[y + BASE_RADIUS + i][x]) * baseWeights[i];

    if (waveTile) {
        float coverage = 1.0f - smoothstep(band * 0.5f, band, RingDistance(length(delta)));
        if (coverage > 0.0f) {
            float3 strong = waveRows[y + halo][x] * WaveWeight(0);
            [loop] for (int i = 1; i <= halo; ++i)
                strong += (waveRows[y + halo - i][x] + waveRows[y + halo + i][x]) * WaveWeight(i);
            filtered = lerp(filtered, strong, coverage);
        }
    }
    xrOutput[p] = float4(lerp(original.rgb, filtered, blend.x), original.a);
}
)hlsl";

} // namespace DualIC4Varjo
