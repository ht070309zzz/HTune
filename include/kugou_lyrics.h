#pragma once
#include "model.h"
#include <stop_token>
namespace stage {
LyricTimeline ParseKrc(const std::wstring& text);
// Bounded desktop catalog response, including grouped recording versions.
// Throws no parse errors to callers; incomplete groups fail the whole result.
std::vector<SongCandidate> ParseKugouSearchResponse(std::string_view json,std::wstring& error);
std::vector<SongCandidate> SearchKugouLyrics(const std::wstring& query,std::wstring& error,std::stop_token stop={});
LyricTimeline FetchKugouLyrics(const std::wstring& key,std::stop_token stop={});
}
