#pragma once
#include "model.h"
#include <stop_token>
namespace stage {
std::vector<SongCandidate> SearchQqLyrics(const std::wstring& query,std::wstring& error,std::stop_token stop);
// Network only; the shared LyricsClient owns cache validation and persistence.
LyricTimeline FetchQqLyrics(const std::wstring& catalogId,std::stop_token stop);
LyricTimeline ParseQrc(const std::wstring& text);
}
