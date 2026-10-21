#pragma once
#include <windows.h>
#include "catalog_id.h"
#include <array>
#include <algorithm>
#include <limits>
#include <memory>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stage {
inline int64_t MonotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
enum class PlayState { Unknown, Playing, Paused, Stopped };
enum class Reliability { Unavailable, Estimated, Observed, Precise, NativeRead };
enum class MusicProvider { Netease, QqMusic, Kugou, SodaMusic };
inline const wchar_t* MusicProviderName(MusicProvider provider) {
    return provider==MusicProvider::SodaMusic?L"汽水音乐":provider==MusicProvider::Kugou?L"酷狗音乐":provider==MusicProvider::QqMusic?L"QQ 音乐":L"网易云";
}
inline bool CatalogMatchesProvider(std::wstring_view id,MusicProvider provider) {
    if(!SameCatalogId(id,id))return false;
    if(provider==MusicProvider::SodaMusic)return IsSodaCatalogId(id);
    if(provider==MusicProvider::Kugou)return IsKugouCatalogId(id);
    if(provider==MusicProvider::QqMusic)return IsQqCatalogId(id);
    return !IsKugouCatalogId(id)&&!IsQqCatalogId(id)&&!IsSodaCatalogId(id);
}
struct BackgroundFrame;
struct PlaybackSnapshot {
    MusicProvider provider=MusicProvider::Netease;
    // Local session identity, never a catalog/lyric ID. Changes on media events.
    uint64_t mediaIdentityRevision=0;
    std::shared_ptr<const BackgroundFrame> mediaCover;
    DWORD pid = 0;
    // Catalog namespaces: decimal NetEase, qq-<decimal> QQ, kg-<hash> Kugou, soda-<decimal> Soda.
    // Only a validated current client observation may set songIdVerified.
    std::wstring title, artist, album, songId, source, diagnostic;
    std::wstring clientVersion;
    PlayState state = PlayState::Unknown;
    Reliability reliability = Reliability::Unavailable;
    int64_t positionMs = 0, durationMs = 0, sampledAtMs = 0, observedAtMs = 0;
    bool songIdVerified = false;
    bool minimized = false;
    bool nativeClockAdvanceAllowed=true;
    int64_t stateSampledAtMs=0;
    bool metadataStale=false;
    // Current native identity and optional catalog text are separate facts.
    // A verified ID/time may be usable while title/artist details are pending.
    bool catalogMetadataReady=false;
    // A bounded same-process read gap or conflicting native-ID candidate.
    // Withhold the public ID and clock while the last lyrics remain static.
    bool identityReacquiring=false;
    uint64_t processCreatedAtTicks=0; // Client instance identity, never a playback timestamp.
    std::wstring recordCandidateId; // Matching catalog/history entity, not a current playback ID.
};
enum class TimelineSource { Unavailable, Observation, Clock, SampledClock };
inline TimelineSource CurrentTimelineSource(const PlaybackSnapshot& p,int64_t now) {
    if(p.metadataStale||p.sampledAtMs<=0||p.sampledAtMs>now||p.positionMs<0||p.durationMs<0)return TimelineSource::Unavailable;
    const bool observationFresh=p.observedAtMs>0&&p.observedAtMs<=now&&now-p.observedAtMs<=1500;
    // A validated native time point is still usable without a play/pause
    // command. Follow fresh samples only; never infer motion or extrapolate.
    if(p.state==PlayState::Unknown||(p.reliability==Reliability::NativeRead&&p.state==PlayState::Playing&&!p.nativeClockAdvanceAllowed)){
        return p.reliability==Reliability::NativeRead&&p.songIdVerified&&observationFresh&&now-p.sampledAtMs<=750?
            TimelineSource::SampledClock:TimelineSource::Unavailable;
    }
    if((p.reliability==Reliability::Precise||p.reliability==Reliability::Estimated||p.reliability==Reliability::NativeRead)&&observationFresh)return TimelineSource::Clock;
    if(p.reliability==Reliability::Observed&&(now-p.sampledAtMs<=1500||(p.state==PlayState::Paused&&observationFresh)))return TimelineSource::Observation;
    return TimelineSource::Unavailable;
}
inline int64_t PlaybackPositionAt(const PlaybackSnapshot& p,int64_t now) {
    int64_t position=std::max<int64_t>(0,p.positionMs);
    if(CurrentTimelineSource(p,now)==TimelineSource::Clock&&p.state==PlayState::Playing){
        const int64_t elapsed=now-p.sampledAtMs;
        position=position>std::numeric_limits<int64_t>::max()-elapsed?std::numeric_limits<int64_t>::max():position+elapsed;
    }
    return p.durationMs>0?std::min(position,p.durationMs):position;
}
inline int64_t WithLyricOffset(int64_t position,int offset) {
    if(offset>0&&position>std::numeric_limits<int64_t>::max()-offset)return std::numeric_limits<int64_t>::max();
    if(offset<0&&position<std::numeric_limits<int64_t>::min()-offset)return std::numeric_limits<int64_t>::min();
    return position+offset;
}
struct LyricWord { int64_t startMs = 0, durationMs = 0; std::wstring text; };
inline bool SameSongId(std::wstring_view left,std::wstring_view right) {
    auto normalize=[](std::wstring_view id){
        if(id.empty()||id.size()>19||id.find_first_not_of(L"0123456789")!=std::wstring_view::npos)return std::wstring_view{};
        const auto first=id.find_first_not_of(L'0');return first==std::wstring_view::npos?std::wstring_view{}:id.substr(first);
    };
    left=normalize(left);right=normalize(right);return !left.empty()&&left==right;
}
enum class LyricAssociation { Unknown, VerifiedCurrent, PreviouslyVerified, UserConfirmed, AutomaticCandidate, LocalImport };
struct LyricLine {
    int64_t startMs = 0, durationMs = 0;
    std::wstring text;
    std::vector<LyricWord> words;
    std::wstring translation, romanization;
    bool credit = false; // Provider credit metadata, never a sung syllable.
};
inline size_t LyricLineAt(const std::vector<LyricLine>& lines,int64_t position) {
    // Parser and cache validation keep startMs in nondecreasing order.
    const auto after=std::upper_bound(lines.begin(),lines.end(),position,[](int64_t time,const LyricLine& line){return time<line.startMs;});
    return after==lines.begin()?0:(size_t)(after-lines.begin()-1);
}
enum class LyricLoadState { Idle, Loading, Ready, NoLyrics, Failed, Cancelled, AwaitingChoice };
struct LyricTimeline {
    std::wstring songId, source, diagnostic;
    bool wordTimed = false;
    std::vector<LyricLine> lines;
    LyricLoadState state = LyricLoadState::Idle;
    bool fromCache = false;
    bool retryWordTiming = false; // Advice from this request, never persisted or a cache-source flag.
    bool cacheWriteSuggested = false; // Deferred online result; not a claim that it was saved.
    // A zero-time single "暂无歌词" row may be service boilerplate. Only an
    // online response without a true missing-lyrics flag can validate that row
    // as real text. This is cache provenance, never current-song evidence.
    bool emptyNoticeValidated = false;
};
struct SongCandidate { std::wstring id, title, artist, album; int64_t durationMs = 0; bool clientRecordCandidate=false; };
// An expired feature drives a zero target; presentation keeps its own easing.
// This is an input-age budget, not a measured audio/lyric synchronization error.
inline constexpr int64_t AudioResponseFreshnessMs=250;
struct AudioFeatures {
    DWORD pid=0;int64_t sampledAtMs=0;float rms=0,peak=0;std::array<float,32> spectrum{};
    bool active=false;std::wstring diagnostic;
    bool connecting=false,timingTrusted=false;
    int64_t observedAtMs=0;
    uint64_t processCreatedAtTicks=0;
};
inline const wchar_t* PlayStateName(PlayState v) {
    switch(v) { case PlayState::Playing:return L"播放"; case PlayState::Paused:return L"暂停"; case PlayState::Stopped:return L"停止"; default:return L"未知"; }
}
inline const wchar_t* ReliabilityName(Reliability v) {
    switch(v) { case Reliability::NativeRead:return L"客户端内部时钟（只读，未验误差）"; case Reliability::Precise:return L"媒体时间轴（接口）"; case Reliability::Observed:return L"界面观测（秒级）"; case Reliability::Estimated:return L"媒体锚点插值（未验误差）"; default:return L"无可靠时间轴"; }
}
}
