#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stage {
// Preserve small gestures exactly. Compress only the excessive part, with a
// continuous first derivative, so the original direction and settle survive.
inline float BalancedLyricTravel(float value,float comfortable) {
    const float magnitude=std::abs(value),limit=std::max(.001f,comfortable);
    if(magnitude<=limit)return value;
    return std::copysign(limit+limit*.6f*std::tanh((magnitude-limit)/(limit*.6f)),value);
}
inline float BalancedLyricEntry(float progress) {
    const float t=std::clamp(progress,0.f,1.f),u=t-1;
    const float cubic=1+u*u*u,back=1+2.70158f*u*u*u+1.70158f*u*u;
    return cubic*.7f+back*.3f;
}
inline int64_t LyricHandoffDuration(int64_t phraseDuration) {
    return std::clamp<int64_t>(phraseDuration/4,90,240);
}
}
