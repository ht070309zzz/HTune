#pragma once
#include <filesystem>
#include <string>
#include <optional>
#include <unordered_map>
#include <cstddef>

namespace stage {
inline constexpr std::size_t MaxConfirmedCandidates=512;
inline constexpr std::size_t MaxConfirmedCandidateKeyChars=1024;
struct AppSettings {
    int theme=2, offsetMs=0;
    int motionVariant=-1,motionFx=0,motionCamera=0,motionLines=3,motionBackground=0,motionFilter=0;
    float response=1.f, lyricSize=44.f;
    std::wstring lyricFont,lyricFontPath;
    bool reducedMotion=false, panelVisible=true, nativeClockEnabled=true;
    bool showTranslation=true, showRomanization=false;
    int lyricDisplayMode=0; // 0 flowing column, 1 focused sentence, 2 current/next
    bool fullscreenAutoHide=true;
    float backgroundShade=0,cameraMotion=1;
    // Presentation cost. qualityMode: 0 automatic, 1 high, 2 medium, 3 low.
    // frameRateMode: 0 automatic (about 60 fps at most), 1 display rate, 2 about 30 fps.
    int qualityMode=0,frameRateMode=0;
    // Suspend stages while another application is fullscreen (games, video).
    bool pauseForFullscreenApps=true;
    // 0 = normal window, 1 = desktop stage. Fullscreen is intentionally
    // transient so a reboot never traps the user in a borderless window.
    int presentationMode=0;
    int backgroundKind=0;
    std::wstring backgroundPath;
    bool hasWindowPlacement=false, windowMaximized=false;
    int windowX=0,windowY=0,windowWidth=1280,windowHeight=900;
    // Normal bounds use WINDOWPLACEMENT workspace coordinates; dimensions
    // are physical pixels. Zero DPI means a legacy record without scale data.
    int windowDpi=0;
    // Path to a theme saved internally by this application. Schema 1 remains
    // compatible when absent. LoadTheme is responsible for file/path safety.
    std::wstring customThemePath;
    std::unordered_map<std::wstring,std::wstring> confirmedCandidates;
    // Runtime-only: unsupported, malformed or unreadable original settings
    // must survive automatic saves. Explicit recovery keeps a backup first.
    bool writable=true;
};
std::filesystem::path UserDataDirectory();
AppSettings LoadSettings(std::wstring& diagnostic);
bool SaveSettings(const AppSettings&,std::wstring& diagnostic);
bool RecoverSettings(AppSettings&,std::filesystem::path& backupFile,std::wstring& diagnostic);
std::wstring TrackMatchKey(const std::wstring& title,const std::wstring& artist);
struct TrackLyricPreference {
    std::optional<int> offsetMs;
    std::wstring importPath;
};
struct TrackLyricPreferences {
    std::unordered_map<std::wstring,TrackLyricPreference> entries;
    // A malformed existing file is preserved, never overwritten by defaults.
    bool writable=true;
};
std::wstring TrackLyricKey(const std::wstring& verifiedId,const std::wstring& title,const std::wstring& artist);
TrackLyricPreferences LoadTrackLyricPreferences(std::wstring& diagnostic);
bool SaveTrackLyricPreferences(const TrackLyricPreferences&,std::wstring& diagnostic);
bool RecoverTrackLyricPreferences(TrackLyricPreferences&,std::filesystem::path& backupFile,std::wstring& diagnostic);
}
