#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stop_token>
#include <string>

namespace stage {
struct SongMetadata {
    std::wstring id,title,artist,album;
    int64_t durationMs=0;
};
inline bool SongMetadataDurationMatches(int64_t metadata,int64_t native) {
    constexpr int64_t maximum=24*3600*1000LL;
    return metadata>0&&metadata<=maximum&&native>0&&native<=maximum&&
        std::abs(metadata-native)<=std::max<int64_t>(2000,native/50);
}
// Static catalog data only. Neither call establishes a current playback ID,
// position, state or clock. Load on the details worker and save on its dedicated
// cache worker; both operations run outside playback locks and the UI thread.
// Saving keeps a bounded global cache. It only evicts fully revalidated records
// for other song IDs and refuses to write when the directory cannot be audited.
std::optional<SongMetadata> LoadSongMetadataCache(const std::wstring& id,int64_t nativeDuration,
    std::stop_token stop,std::wstring& diagnostic) noexcept;
bool SaveSongMetadataCache(const SongMetadata& value,std::wstring& diagnostic) noexcept;
}
