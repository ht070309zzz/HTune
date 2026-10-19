#pragma once
#include <string>
#include <string_view>
namespace stage {
enum class ManualLyricKind { Invalid, Search, SongId };
struct ManualLyricRequest {
    ManualLyricKind kind=ManualLyricKind::Invalid;
    std::wstring query,songId,diagnostic;
};
// Parses locally. Links are never opened, followed or sent to a server.
ManualLyricRequest ParseManualLyricRequest(std::wstring_view text);
}
