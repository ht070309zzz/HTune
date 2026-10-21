#pragma once
#include "model.h"
#include "background.h"
#include "motion_styles.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <memory>
#include <string>

namespace stage {
inline constexpr int RiftStyle=6;
struct RiftFrame {
    std::shared_ptr<const LyricTimeline> lyrics;
    std::wstring identity;
    uint64_t revision=0;
    int64_t positionMs=0,nowMs=0,sampledAtMs=0;
    // Sample-only timelines do not carry a play/pause claim.
    bool sampled=false,paused=false;
    float deltaSeconds=0;
    bool discontinuity=false;
    size_t previewIndex=0;
    float width=1,height=1,dpi=1,lyricSize=44,browseOffset=0;
    std::wstring lyricFont,lyricFontPath;
    std::array<float,32> spectrum{};
    float low=0,mid=0,high=0,rms=0,pulse=0,response=1,camera=1;
    D2D1_COLOR_F background{.04f,.045f,.05f,1};
    D2D1_COLOR_F ink{.957f,.949f,.933f,1},accent{1,.353f,.122f,1};
    int quality=0;
    LyricMotion kind=LyricMotion::Rift;
    int variant=-1,particlePreset=0,motionFx=0,motionCamera=0,motionLines=3,motionBackground=0,motionFilter=0;
    float pointerX=0,pointerY=0;bool pointerActive=false;
    std::shared_ptr<const BackgroundFrame> cover;
    bool following=false,precise=false,playing=false,reduced=false,focused=false;
    bool phraseEntry=false; // Short entrance stagger, separate from sung-word timing.
    bool showTranslation=false,showRomanization=false;
};
struct RiftGlyphReading {
    size_t text=0,length=0,row=0;
    float left=0,top=0,right=0,bottom=0,fill=0,anchorX=0,anchorY=0;
};
struct RiftReading {
    std::wstring fontFamily; // Resolved layout face, for bounded offline checks.
    size_t missingGlyphs=0;
    size_t particleEmitters=0; // Bounded actual glyph particle submissions.
    double cometFlightSeconds=0;size_t cometTrailSamples=0; // Internal observations, never shown in the stage.
    uint32_t cometRelays=0;
    // Bounded actual rendered-quad observations for the offline checks.
    std::array<RiftGlyphReading,24> glyphs{};size_t glyphCount=0;
    size_t line=0,page=0,pages=1,visibleClusters=0,submittedClusters=0,fullyVisibleClusters=0;
    float glyphLeft=0,glyphTop=0,glyphRight=0,glyphBottom=0;
    float offset=0,maxOffset=0,step=1;
    float cameraX=0,cameraY=0,cameraScale=1;
    int composition=0,layoutOverlaps=0;
    bool wordTimed=false,reset=false,valid=false,embeddedFont=false;
    size_t activeText=SIZE_MAX;
    bool activeFullyVisible=false;
    bool cameraFlying=false,awaitingWords=false;
    bool sampledClock=false,advancing=false;int64_t renderPositionMs=0;float audioLevel=0;
    bool auxiliaryVisible=false;
    bool bubbleVisible=false,fireflyVisible=false;
    D2D1_RECT_F bubbleBounds{};
    D2D1_POINT_2F fireflyPosition{};
    size_t fireflyTrailSegments=0;
    size_t reassemblyFragments=0;
    std::array<float,6> auxiliaryTransform{};
    float auxiliaryWidth=0,auxiliaryHeight=0;
};
// One bounded transparent native layer. No I/O, media controls or audio capture.
// Font bytes are an embedded resource; all presentation time is supplied by caller.
class RiftStage {
public:
    RiftStage();~RiftStage();
    RiftStage(const RiftStage&)=delete;
    RiftStage& operator=(const RiftStage&)=delete;
    void Create(ID3D11Device*,IDWriteFactory*);
    ID2D1Bitmap1* Render(const RiftFrame&,ID2D1DeviceContext* destination);
    RiftReading Reading() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
