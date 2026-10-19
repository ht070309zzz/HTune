#pragma once
#include "model.h"
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
namespace stage {
enum class LyricCacheMode { Immediate,Deferred };
enum class LyricCacheSaveResult { Stored,PreservedWords,Failed };
// Blocking disk operation for a dedicated writer, never a UI/playback lock.
LyricCacheSaveResult StoreLyricCache(const LyricTimeline& timeline) noexcept;
class LyricsClient {
public:
    using MemoryCache=std::function<std::shared_ptr<const LyricTimeline>(const std::wstring&)>;
    explicit LyricsClient(std::stop_token stop = {},LyricCacheMode mode=LyricCacheMode::Immediate,MemoryCache memory={})
        : stop_(stop),cacheMode_(mode),memoryCache_(std::move(memory)) {}
    std::vector<SongCandidate> Search(const std::wstring& query, std::wstring& error, MusicProvider provider=MusicProvider::Netease);
    // Searches title+artist and title (NetEase: 100 rows; QQ: 20; Kugou: 30 plus
    // bounded recording groups, web/artist fallback when needed),
    // merges by namespaced catalog ID (Kugou keeps complete alias rows), and puts exact title+artist matches first. Returns
    // the complete merged set: use it for strict uniqueness even if the UI
    // shows fewer rows. A failed query returns no candidates for auto-preview.
    std::vector<SongCandidate> Search(const PlaybackSnapshot& playback, std::wstring& error);
    // Online word timing wins. An online line-only response may retain local
    // words only when the entire original text and every line start agree.
    LyricTimeline Fetch(const std::wstring& songId);
    // Read and validate an existing timeline without waiting for the network.
    // A caller may publish it for this exact ID before refreshing with Fetch.
    // Retains at most one word timeline for that refresh, including while the
    // caller is still waiting to publish it after an identity barrier.
    std::optional<LyricTimeline> Cached(const std::wstring& songId);
    LyricTimeline Import(const std::filesystem::path& file);
private:
    std::stop_token stop_;
    LyricCacheMode cacheMode_;
    MemoryCache memoryCache_;
    // One validated cache read for this client/request; never playback proof.
    std::shared_ptr<const LyricTimeline> preloadedCache_;
    std::optional<LyricTimeline> LoadCached(const std::wstring& normalized,bool networkFailed);
    void PrepareOnlineResult(LyricTimeline& timeline);
    void CacheOnlineResult(LyricTimeline& timeline);
};
void AttachLyricSupplement(LyricTimeline& base,const LyricTimeline& supplement,bool romanization);
LyricTimeline ParseLrc(const std::wstring& text);
LyricTimeline ParseYrc(const std::wstring& text);
// Returns an index only when normalized title AND artist match exactly and
// identify one distinct provider-specific catalog ID. Repeated rows for the same ID
// do not create ambiguity. Missing artist, invalid text or different IDs fail.
// This is a search match for automatic preview, not a verified now-playing ID;
// it must never be used to set PlaybackSnapshot::songIdVerified.
// QQ/Kugou/Soda playback additionally requires a matching duration (within 3 s) and,
// when both snapshots supply one, the same normalized album title.
std::optional<size_t> MatchExactUniqueCandidate(const PlaybackSnapshot& playback,
    const std::vector<SongCandidate>& candidates);
}
