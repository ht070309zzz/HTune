#pragma once
#include "model.h"
#include <optional>
#include <cstdlib>

namespace stage {
struct QqNativeSong {
    uint32_t id=0;
    std::wstring title,artist,album;
    int64_t durationMs=0,positionMs=0,observedAtMs=0;
    PlayState state=PlayState::Unknown;
};
// Owned and used by one QQ monitor thread. Never injects, writes process memory,
// reads credentials, or treats history/cache/search results as now-playing.
class QqIdentityReader {
public:
    QqIdentityReader();
    ~QqIdentityReader();
    std::optional<QqNativeSong> Read(DWORD pid,uint64_t created);
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
inline bool SameQqNativeSong(const QqNativeSong& a,const QqNativeSong& b) {
    return a.id&&a.id==b.id&&a.title==b.title&&a.artist==b.artist&&a.album==b.album&&
        a.durationMs==b.durationMs;
}
inline bool QqNativeMatchesMedia(const QqNativeSong& native,const PlaybackSnapshot& media,int64_t now) {
    if(!native.id||native.observedAtMs<=0||native.observedAtMs>now||now-native.observedAtMs>500||
       media.provider!=MusicProvider::QqMusic||media.metadataStale||
       native.title!=media.title||native.artist!=media.artist||native.album!=media.album||
       native.title.empty()||native.durationMs<=0||media.durationMs<=0||
       std::abs(native.durationMs-media.durationMs)>1000||native.state!=media.state||
       native.state==PlayState::Unknown||CurrentTimelineSource(media,now)==TimelineSource::Unavailable)return false;
    return std::abs(native.positionMs-PlaybackPositionAt(media,now))<=1500;
}
}
