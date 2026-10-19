#pragma once

#include "model.h"

namespace stage {

// Diagnostic history only; never a source of playback identity or position.
struct PlaybackLogPoint {
    DWORD pid=0;
    uint64_t processCreatedAtTicks=0;
    std::array<wchar_t,40> songId{};
    Reliability reliability=Reliability::Unavailable;
    int64_t positionMs=0,durationMs=0,sampledAtMs=0,observedAtMs=0,capturedAtMs=0;
    bool comparable=false;
};

inline PlaybackLogPoint PlaybackLogPointAt(const PlaybackSnapshot& playback,int64_t now) noexcept {
    PlaybackLogPoint point;
    point.pid=playback.pid;point.processCreatedAtTicks=playback.processCreatedAtTicks;
    point.positionMs=playback.positionMs;point.durationMs=playback.durationMs;
    point.sampledAtMs=playback.sampledAtMs;point.observedAtMs=playback.observedAtMs;
    point.capturedAtMs=now;point.reliability=playback.reliability;
    if(!playback.pid||!playback.processCreatedAtTicks||!playback.songIdVerified||
       playback.identityReacquiring||!SameCatalogId(playback.songId,playback.songId)||
       playback.durationMs<=0||playback.positionMs>playback.durationMs||
       CurrentTimelineSource(playback,now)==TimelineSource::Unavailable)return point;
    if(IsKugouCatalogId(playback.songId)){
        for(size_t i=0;i<playback.songId.size();++i)point.songId[i]=CatalogLower(playback.songId[i]);
        point.comparable=true;return point;
    }
    const auto digits=CatalogDigits(playback.songId);
    const size_t prefix=IsSodaCatalogId(playback.songId)?5:IsQqCatalogId(playback.songId)?3:0;
    for(size_t i=0;i<prefix;++i)point.songId[i]=playback.songId[i];
    const auto first=playback.songId.size()-digits.size();
    for(size_t i=first;i<playback.songId.size();++i)point.songId[prefix+i-first]=playback.songId[i];
    point.comparable=true;
    return point;
}

inline bool PlaybackLogPositionDiscontinuous(const PlaybackLogPoint& previous,const PlaybackLogPoint& current) noexcept {
    if(!previous.comparable||!current.comparable||previous.pid!=current.pid||
       previous.processCreatedAtTicks!=current.processCreatedAtTicks||previous.songId!=current.songId||
       previous.reliability!=current.reliability||previous.durationMs!=current.durationMs||
       current.capturedAtMs<=previous.capturedAtMs||current.sampledAtMs<=previous.sampledAtMs)return false;
    // Old observations after a long UI stall cannot establish a transition.
    // Compare raw sample times, not extrapolated display positions or UTC.
    const auto capturedGap=current.capturedAtMs-previous.capturedAtMs;
    const auto sampledGap=current.sampledAtMs-previous.sampledAtMs;
    if(capturedGap>5000||sampledGap>5000)return false;
    const auto movement=current.positionMs-previous.positionMs;
    // Paused and unknown states may remain still or move naturally. Neither
    // zero movement nor a one-second UI timer slip is a seek diagnosis.
    return movement < -2000 || movement > sampledGap+2000;
}

} // namespace stage
