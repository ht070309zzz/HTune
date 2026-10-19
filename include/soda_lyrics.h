#pragma once
#include "model.h"
#include <span>
#include <stop_token>
namespace stage {
// Queue data supplies catalog candidates only, never current playback identity.
std::vector<SongCandidate> ParseSodaQueue(std::span<const uint8_t> bytes,std::wstring& error);
std::vector<SongCandidate> ReadSodaCandidates(std::wstring& error,std::stop_token stop={});
std::vector<SongCandidate> SearchSodaLyrics(const std::wstring& query,std::wstring& error,std::stop_token stop={});
LyricTimeline ParseSodaLyricsResponse(std::string_view bytes,const std::wstring& id);
LyricTimeline FetchSodaLyrics(const std::wstring& id,std::stop_token stop={});
}
