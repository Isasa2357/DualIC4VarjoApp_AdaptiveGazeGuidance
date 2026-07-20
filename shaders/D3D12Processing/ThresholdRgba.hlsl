#include "ThresholdCommon.hlsli"

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (!InDispatchRect(p)) {
        return;
    }

    float v = SelectChannel(LoadSrc(p), Channel);
    bool isPassed = v >= Threshold;
    if (Invert != 0u) {
        isPassed = !isPassed;
    }

    Store(p, isPassed ? ForegroundColor : BackgroundColor);
}
