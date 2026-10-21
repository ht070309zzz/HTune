#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace stage {
// Immutable snapshot of everything the stage UI shows, built by the app on
// the UI thread. The interface reads it and answers with UiActions; it never
// reaches into app state, playback, lyrics or persistence directly.
struct CandidateView {
    std::wstring id,title,artist,album;
    int64_t durationMs=0;
    bool clientRecord=false; // recent-play record: needs explicit confirmation
};
struct SettingsView {
    // Now playing (presentation text exactly as the stage shows it).
    std::wstring title,artist;
    bool hasCover=false;
    float audioLevel=0; // 0..1 current-process audio envelope, presentation only
    // Presentation.
    bool fullscreen=false,desktop=false,compactLayout=false;
    int backgroundKind=0; // 0 cover, 1 image, 2 video
    std::wstring coverStatus,backgroundNotice;
    bool backgroundRetry=false;
    float response=1.f,lyricSize=44.f,backgroundShade=0.f,cameraMotion=1.f;
    std::wstring lyricFont,lyricFontPath,lyricFontNotice;
    bool reducedMotion=false;
    int qualityMode=0,frameRateMode=0;
    bool pauseForFullscreenApps=true;
    // Theme.
    int themeStyle=1;           // built-in style, or the custom theme's base style
    int motionVariant=-1,motionFx=0,motionCamera=0,motionLines=3,motionBackground=0,motionFilter=0;
    bool customTheme=false,themeRestorePending=false;
    std::wstring customThemeName,themeNotice;
    // Lyrics.
    bool canSearch=false,canFollow=false,independentPreview=false,canImport=false,canFetch=false,hasTrack=false;
    std::vector<CandidateView> candidates;
    uint64_t candidateGeneration=UINT64_MAX;
    int selectedCandidate=-1;
    std::wstring candidateDetails;
    int offsetMs=0;
    bool showTranslation=true,showRomanization=false;
    int lyricDisplayMode=0;
    bool lyricMemoryActionable=false;
    std::wstring lyricMemoryButton,lyricMemoryNote;
    // Stage pills.
    bool resumeLyrics=false,resumeEnabled=false,matchLyrics=false,lyricMismatch=false;
    // Status.
    std::wstring diagnostics,notice;
    bool showDiagnostics=false,nativeClock=false,fullscreenAutoHide=true;
    bool settingsActionable=false;
    std::wstring settingsButton,settingsNote,usageHelp;
};

enum class UiCommand : uint16_t {
    None,
    OpenBackstage,CloseBackstage,
    ToggleFullscreen,EnterDesktop,WindowMode,
    UseCover,ChooseImage,ChooseVideo,RetryBackground,
    SetResponse,SetLyricSize,SetBackgroundShade,SetCameraMotion,ResetBackdrop,SetReducedMotion,
    SetLyricFont,ChooseSystemLyricFont,ImportLyricFont,
    SetQuality,SetFrameRate,SetPauseForFullscreen,
    SetMotionVariant,SetMotionFx,SetMotionCamera,SetMotionLines,SetMotionBackground,SetMotionFilter,
    SelectStyle,ImportTheme,ExportTheme,ChooseAccent,RetryTheme,
    FindLyrics,FollowCurrent,SelectCandidate,FetchCandidate,ImportLyrics,RestoreOnline,
    SetOffset,ResetOffset,SetTranslation,SetRomanization,SetLyricMode,RepairLyricMemory,
    ResumeLyrics,MatchLyrics,
    SetDiagnostics,SetNativeClock,SetFullscreenAutoHide,RepairSettings,
    MinimizeWindow,MaximizeWindow,CloseWindow,
};
struct UiAction {
    UiCommand command=UiCommand::None;
    int value=0;
    float number=0.f;
    std::wstring text;
    uint64_t generation=0; // candidate list generation the selection belongs to
};
}
