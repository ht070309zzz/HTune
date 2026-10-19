#pragma once
#include "model.h"
#include "themes.h"
#include <d3d11.h>
#include <array>
#include <deque>
#include <memory>

namespace stage {
inline constexpr int WeaveStyle=3,PosterStyle=4,EchoStyle=5;
inline bool IsOriginalStyle(int style) { return style>=WeaveStyle&&style<=EchoStyle; }

// Shared by the native edge controls and the drawing layer, in client DIPs.
struct OriginalComposition {
    float width=1,height=1,margin=24,railY=1;
    float lyricX=0,lyricY=0,lyricWidth=1,lyricHeight=1;
    float coverX=0,coverY=0,coverWidth=0,coverHeight=0;
    float controlsX=0,drawerLeft=1,drawerWidth=0;
    bool compact=false;
};
OriginalComposition ComposeOriginal(int style,float width,float height,float drawer=0);

struct OriginalVisualFrame {
    int style=WeaveStyle;
    float width=1,height=1,dpi=1,seconds=0,delta=0;
    float low=0,mid=0,high=0,pulse=0,response=1,motion=1;
    float pointerX=-1,pointerY=-1,pointerPresence=0,drawer=0;
    float rowEnter=1,trackEnter=1,historyBegin=0,historyEnd=0;
    float historyExtent=0,previousExtent=0;
    ThemeColor base{},accent{},ink{};
    std::array<std::array<float,4>,128> history{};
    std::array<std::array<float,4>,128> previousHistory{};
    bool previousHistoryVisible=false;
};

// Per-renderer presentation state. Contains only bounded scalar feature data;
// no audio samples, file I/O, network calls or playback-control operations.
class OriginalStageState {
public:
    void Update(const PlaybackSnapshot& playback,const AudioFeatures& audio,
        bool currentAudio,bool preview,bool mismatch,int64_t now,float delta,bool reducedMotion,
        bool pointerActive,float pointerX,float pointerY);
    void SelectLine(size_t line,uint64_t revision,int64_t now,bool following);
    void Apply(OriginalVisualFrame& frame,int64_t now,bool reducedMotion) const;
    float TrackEnter(int64_t now,bool reducedMotion) const;
    float RowEnter(int64_t now,bool reducedMotion) const;
private:
    struct Sample { int64_t position=0;float level=0; };
    std::deque<Sample> samples_;
    std::array<std::array<float,4>,128> previous_{};
    std::array<std::array<float,4>,128> displayed_{};
    std::wstring songId_,title_,artist_;
    DWORD pid_=0;uint64_t instance_=0;
    bool identity_=false,verified_=false,previousVisible_=false;
    int64_t changedAt_=0,lastAudioAt_=0,lastPosition_=0,rowChangedAt_=0;
    size_t line_=SIZE_MAX;uint64_t revision_=0;
    float pointerX_=0,pointerY_=0,pointerPresence_=0;
    float historyExtent_=0,previousExtent_=0;
    void History(std::array<std::array<float,4>,128>& out) const;
};

class OriginalStageGpu {
public:
    OriginalStageGpu();~OriginalStageGpu();
    bool Create(ID3D11Device* device);
    void Render(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,const OriginalVisualFrame& frame,int quality);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
