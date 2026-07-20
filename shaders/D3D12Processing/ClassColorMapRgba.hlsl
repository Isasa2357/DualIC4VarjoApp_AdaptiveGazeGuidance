#include "ThresholdCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    float v = SelectChannel(LoadSrc(p), Channel);
    uint count = max(ClassCount, 1u);
    uint classId = (uint)round(saturate(v) * ClassScale);
    classId = min(classId, count - 1u);

    float4 color = Palette16(classId);
    color.a *= saturate(Opacity);
    Store(p, color);
}
