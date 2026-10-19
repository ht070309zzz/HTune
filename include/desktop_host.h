#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memory>
#include <string>

namespace stage {

// Explorer desktop hosting is an undocumented shell topology, so unsupported
// arrangements fail safely to normal-window mode. Never changes another
// wallpaper's windows, Explorer styles/z-order, registry, or system wallpaper.
// Prepare/Attach/Detach/Poll must run on the stage's UI thread. IsAttached is
// a non-mutating query; this object itself is not intended for concurrent use.
class DesktopHost {
public:
    DesktopHost();
    ~DesktopHost();
    DesktopHost(const DesktopHost&) = delete;
    DesktopHost& operator=(const DesktopHost&) = delete;

    // Returns the exact parent's DPI context. Temporarily set this context on
    // the UI thread while creating the independent stage HWND, then restore
    // the thread context. nullptr means no verified safe host was found.
    // Can send bounded 0x052C only to request a legal desktop background layer.
    DPI_AWARENESS_CONTEXT Prepare(std::wstring& diagnostic);
    bool Attach(HWND ownStage, HMONITOR monitor, std::wstring& diagnostic);
    bool Attach(HWND ownStage, std::wstring& diagnostic) {
        return Attach(ownStage, nullptr, diagnostic);
    }
    // Restores captured placement/styles/owner/parent/visibility. Optional
    // diagnostic lets callers distinguish restoration failure from no-op.
    bool Detach(std::wstring* diagnostic = nullptr);
    bool IsAttached() const;
    // Call about once per second and after WM_DISPLAYCHANGE. Attempts host
    // recovery when the stage survives; on failure restores its original
    // window. If Explorer also destroyed the HWND, returns false and asks the
    // caller to recreate that stage; control/tray HWND must remain separate.
    // After stage destruction, call Poll/Detach before creating its replacement
    // so captured state is cleared before Windows can reuse the HWND value.
    bool Poll(std::wstring& diagnostic);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stage
