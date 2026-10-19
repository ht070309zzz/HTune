#pragma once
#include "model.h"
#include "background.h"
#include "themes.h"
#include "theme_transition.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace stage {
namespace ui { class DrawList; }
struct PresentedLyricPosition {
    size_t lineIndex=0;
    float scrollOffset=0,maxScrollOffset=0,scrollStep=1,pageScrollStep=1;
};
struct SceneView {
    int theme=2,offsetMs=0;
    float response=1.f,lyricSize=44.f;
    size_t previewIndex=0;
    bool panelVisible=true,fullscreen=false,desktop=false,reducedMotion=false,diagnostics=false;
    std::shared_ptr<const ThemeDefinition> customTheme;
    bool coverBackground=false; // Explicit source intent; filenames never determine presentation.
    bool showTranslation=true,showRomanization=false;
    int lyricDisplayMode=0;
    bool quietFullscreen=false;
    bool manualLyricBrowse=false;
    float backgroundShade=0,cameraMotion=1;
    float previewScrollOffset=0;
    // Presentation cost (AppSettings::qualityMode/frameRateMode). lite keeps a
    // stage that shares the session with a fullscreen application on another
    // monitor at the lowest tier and about 30 fps.
    int qualityMode=0,frameRateMode=0;
    bool onBattery=false,lite=false;
    // Last user input (monotonic ms); recent input keeps full cadence.
    int64_t interactionAtMs=0;
    // Backstage: the camera pulls back and the live stage becomes a tilted
    // card beside the settings pages. Presentation only, spring animated.
    bool backstage=false;
    // A glass panel covers part of the stage this frame and samples its
    // blurred copy; the stage is then drawn through the compositor.
    bool glass=false;
    // The stage UI console is up: the stage's own bottom card and progress
    // step back, and its status note rises above the console.
    bool consoleShown=false;
    // New stages use a flat edge drawer and pointer-local material deformation.
    float originalDrawer=0,pointerX=-1,pointerY=-1;
    bool pointerActive=false;
};
struct SceneInput {
    PlaybackSnapshot playback;
    AudioFeatures audio;
    std::shared_ptr<const LyricTimeline> lyrics;
    BackgroundSnapshot background;
    std::wstring lyricStatus;
    std::wstring coverError;
    uint64_t lyricRevision=0;
    SceneView view;
    LyricAssociation lyricAssociation=LyricAssociation::Unknown;
    bool lyricVersionMismatch=false;
    bool lyricRefreshPending=false;
    int64_t frameAtMs=0; // One immutable playback/motion instant for both hosts.
    bool manualLyricPreview=false;
    std::wstring lyricPreviewTitle,lyricPreviewArtist;
    std::optional<ThemePalette> palette;
    // Changes for lyric ID, original text/timing grid or load-state changes.
    // Zero preserves the legacy full-revision/pointer behavior for direct users.
    uint64_t lyricReadingRevision=0;
    // Stage UI drawn above the stage (main window only); glass panels in it
    // route the frame through the compositor for their blurred backdrop.
    std::shared_ptr<const ui::DrawList> ui;
};
class SceneRenderer {
public:
    SceneRenderer();~SceneRenderer();
    SceneRenderer(const SceneRenderer&)=delete;
    SceneRenderer& operator=(const SceneRenderer&)=delete;
    void Create(HWND,bool desktopBltCompatibility=false,bool forceSoftware=false);void Resize();
    // Development previews only: the same scene drawn into a texture of the
    // given physical size, without a window, swap chain or presentation.
    void CreateOffscreen(UINT width,UINT height,float dpiScale,bool forceSoftware=false);
    bool CaptureBgra(std::vector<uint8_t>& pixels,UINT& width,UINT& height);
    float GpuFrameMs() const; // smoothed GPU time of recent frames; 0 when unavailable
    int QualityTier() const;  // 0 high .. 3 low
    // A successful presentation reports the row actually selected by the
    // renderer, including its bounded clock hold and group scroll geometry.
    // No row means empty lyrics; occluded/failed frames publish no position.
    bool Draw(const SceneInput&,bool pacePresentation=true,std::optional<PresentedLyricPosition>* presentedLyricLine=nullptr);
    HANDLE FrameEvent() const;bool Software() const;
    uint64_t FrameGeneration() const;
    HRESULT DeviceRemovedReason() const;
    bool CompositionPaced() const;
    bool Occluded() const;
    // UI-thread snapshot of submission evidence, not proof of screen visibility.
    std::string PresentationDiagnostic() const;
    bool RefreshVisibility(); // Only queries a swap chain already reporting occlusion.
private:
    uint64_t frameGeneration_=0;
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
