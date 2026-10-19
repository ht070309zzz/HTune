#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stage {

enum class BackgroundKind { None, Image, Video };

// Immutable published pixels: top-down, tightly packed, premultiplied BGRA8.
// Use DXGI_FORMAT_B8G8R8A8_UNORM + D2D1_ALPHA_MODE_PREMULTIPLIED.
// Maximum output is 3840 pixels on either axis and 3840*2160 pixels total.
struct BackgroundFrame {
    uint32_t width = 0, height = 0, stride = 0;
    uint64_t sequence = 0;
    int64_t ptsMs = 0, presentedAtMs = 0;
    std::vector<uint8_t> bgra;
    // EXIF-compatible 1..8 display transform. Pixel dimensions/stride remain
    // encoded; the renderer normalizes orientation without copying full frames.
    uint16_t orientation=1;
    // Physical width/height ratio of a decoded pixel, before orientation.
    // Images default to square pixels; video preserves the current output type.
    uint32_t pixelAspectNumerator=1,pixelAspectDenominator=1;
};

struct BackgroundSnapshot {
    // Changes on commands, not decoded video frames or canonicalized paths.
    uint64_t sourceRevision = 0;
    BackgroundKind kind = BackgroundKind::None;
    std::wstring filePath, fileName, error;
    HRESULT errorCode = S_OK;
    bool loading = false, playing = false, loop = false;
    std::shared_ptr<const BackgroundFrame> frame;
};

// All commands enqueue work and return without decoding or waiting for a
// previous file. GetSnapshot is thread-safe; retain only the current frame to
// keep renderer memory bounded. Video audio is deselected and never played.
// Local user-selected files on fixed/removable volumes only. Every parent
// and the file itself must be a regular, non-reparse path; URLs, remote
// shares, cloud placeholders and external players are never used.
class BackgroundController {
public:
    BackgroundController();
    ~BackgroundController();
    BackgroundController(const BackgroundController&) = delete;
    BackgroundController& operator=(const BackgroundController&) = delete;
    void SetImage(const std::wstring& filePath);
    void SetVideo(const std::wstring& filePath, bool loop = true);
    // Pause/resume continuous video decoding with the presence of a visible
    // window or desktop stage. Static image preparation is unaffected.
    void SetPlaybackDemand(bool visibleStage);
    // Requeue only the failed source last shown to the caller. A changed source,
    // in-flight retry or stopped controller is never resurrected by a stale UI.
    // Existing pixels/error survive until successful decode or a new selection.
    bool Retry(uint64_t expectedSourceRevision);
    void Clear();
    void Stop(); // Nonblocking Clear; destructor stops and joins the worker.
    BackgroundSnapshot GetSnapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline const wchar_t* BackgroundKindName(BackgroundKind kind) {
    switch (kind) {
    case BackgroundKind::Image: return L"图片";
    case BackgroundKind::Video: return L"视频";
    default: return L"主题";
    }
}

} // namespace stage
