#pragma once
#include "themes.h"
#include <cstdint>

namespace stage {
struct ThemePalette {
    ThemeColor background,panel,ink,muted,accent,glow;
};
ThemePalette PaletteOf(const ThemeDefinition&);
void ApplyPalette(ThemeDefinition&,const ThemePalette&);
bool SamePalette(const ThemePalette&,const ThemePalette&);

// UI-thread-owned. The sampled palette is shared by every rendering host and
// native control; it never changes the user's saved theme definition.
class ThemeTransition {
public:
    ThemePalette Update(const ThemePalette& target,int64_t now,bool reducedMotion);
private:
    ThemePalette Sample(int64_t now) const;
    ThemePalette from_{},target_{};
    int64_t startedAt_=0,durationMs_=650;
    bool initialized_=false;
};
}
