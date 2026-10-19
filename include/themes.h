#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace stage {
inline constexpr int BuiltInStyleCount=6;
inline constexpr int CustomThemeChoice=BuiltInStyleCount;
struct ThemeColor { float r = 0, g = 0, b = 0, a = 1; };
constexpr ThemeColor ThemeRgb(uint32_t rgb) {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1};
}
enum class ThemeBackgroundKind { None, Image, Video };
struct ThemeDefinition {
    std::wstring name = L"声色排版";
    int baseStyle = 2; // 0 flow, 1 cinema, 2 paper, 3 weave, 4 poster, 5 echo
    ThemeColor background = ThemeRgb(0xF7F2E8), panel = ThemeRgb(0xECE5D8),
        ink = ThemeRgb(0x352719), muted = ThemeRgb(0x8E7864),
        accent = ThemeRgb(0xBA432B), glow = ThemeRgb(0xE1C7A8);
    float response = 1, lyricSize = 44; // clamped to 0..2 and 26..80, respectively
    bool reducedMotion = false;
    int lyricDisplayMode=0;
    float backgroundShade=0,cameraMotion=1;
    ThemeBackgroundKind backgroundKind = ThemeBackgroundKind::None;
    std::filesystem::path backgroundPath;
};
struct ThemeResult {
    ThemeDefinition definition;
    std::wstring diagnostic; // warnings may accompany ok=true
    bool ok = false;
};
ThemeDefinition BuiltInTheme(int style);
// UTF-8 schema 1: name/baseStyle, colors{six named colors}, response,
// lyricSize/reducedMotion/lyricDisplayMode, background{kind:"none"|"image"|"video",path}.
// Pure data parser; relative asset paths stay relative until LoadTheme resolves
// them against the selected theme's parent. Do not directly open parser paths.
ThemeResult ParseThemeJson(std::string_view utf8);
ThemeResult LoadTheme(const std::filesystem::path& selectedFile);
// Writes only this explicitly selected local destination. Relative asset
// references are exported when the asset is below its destination directory.
// Selected directories must already exist; no implicit AppData or material scan.
ThemeResult SaveTheme(const std::filesystem::path& selectedFile, const ThemeDefinition& definition);
// Same validation and durable temporary-file write as SaveTheme, but the
// destination must not exist. Used for immutable application-private
// snapshots so a failed settings commit cannot overwrite the prior theme.
ThemeResult SaveThemeNewFile(const std::filesystem::path& selectedFile, const ThemeDefinition& definition);
}
