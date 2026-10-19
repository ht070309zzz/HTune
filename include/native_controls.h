#pragma once
#include "themes.h"
#include <windows.h>
#include <memory>

namespace stage {
// UI-thread-only skin for this application's buttons, tabs and dropdown lists.
// Existing native controls retain their input, selection and command behavior.
class NativeControls {
public:
    NativeControls();~NativeControls();
    NativeControls(const NativeControls&)=delete;
    NativeControls& operator=(const NativeControls&)=delete;
    bool AttachButton(HWND);
    bool AttachTabs(HWND);
    bool AttachCombo(HWND);
    void SetTheme(const ThemeDefinition&);
    void Advance(int64_t monotonicMs);
    bool DrawItem(const DRAWITEMSTRUCT&,HFONT);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
