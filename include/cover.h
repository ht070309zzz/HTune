#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <stop_token>

namespace stage {
enum class CoverIdOrigin { SelectedCandidate, VerifiedPlaybackId };
struct CoverResult {
    std::filesystem::path file;
    std::wstring songId, source, diagnostic, metadataUrl, imageUrl;
    CoverIdOrigin idOrigin = CoverIdOrigin::SelectedCandidate;
    uint32_t width = 0, height = 0;
    bool fromCache = false;
    // The requested refresh still needs a bounded retry.  When file is not
    // empty it is a validated cached image that may remain displayed; when
    // file is empty the caller keeps its already displayed image unchanged.
    bool refreshPending = false;
};
struct CoverFetchOptions {
    bool forceRevalidate = false;
    // The cache quota must never evict the file currently owned by the scene.
    // Fetch also protects the validated fallback and newly returned file.
    std::filesystem::path protectedFile;
};
class CoverClient {
public:
    explicit CoverClient(std::stop_token stop = {}) : stop_(stop) {}
    // Blocking operation: call from the application's dedicated cover worker,
    // then publish an immutable result guarded by the song generation. Fetch
    // verifies the catalog entity only; it never verifies the now-playing ID.
    // Existing unknown/corrupt records are preserved. Up to eight separately
    // validated recovery locations per song may be used; quota cleanup can
    // remove only complete, re-opened records carrying this application's
    // owner/schema/hash proof, while the current and returned files stay protected.
    CoverResult Fetch(const std::wstring& songId,
        CoverIdOrigin origin = CoverIdOrigin::SelectedCandidate,
        CoverFetchOptions options = {});
private:
    std::stop_token stop_;
};
}
