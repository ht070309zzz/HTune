#include "renderer.h"
#include "window_layout.h"
#include "lyric_layout.h"
#include "lyric_glyphs.h"
#include "stage_gpu.h"
#include "stage_compositor.h"
#include "stage_ui_draw.h"
#include "original_stages.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite_2.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stage {
namespace {
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("Native scene resource failure: " + std::to_string((unsigned long)hr));
}
std::wstring Time(int64_t ms) {
    std::wostringstream out; ms = std::max<int64_t>(0, ms) / 1000;
    out << ms / 60 << L":" << std::setw(2) << std::setfill(L'0') << ms % 60; return out.str();
}
float Approach(float value, float goal, float dt, float rate) {
    return value + (goal - value) * (1.f - std::exp(-std::max(0.f, dt) * rate));
}
float UnitSignal(float value) { return std::isfinite(value)?std::clamp(value,0.f,1.f):0.f; }
float SmoothStep(float value) { const auto t=std::clamp(value,0.f,1.f);return t*t*(3.f-2.f*t); }
float EaseOutCubic(float value) { const float u=1.f-std::clamp(value,0.f,1.f);return 1.f-u*u*u; }
float EaseOutBack(float value) {
    const float t=std::clamp(value,0.f,1.f),u=t-1.f;
    return 1.f+2.4f*u*u*u+1.4f*u*u;
}
// Deterministic per-glyph variation. Visual only; never mixed with timing.
float Hash01(uint64_t a,uint64_t b) {
    uint64_t h=a*0x9E3779B97F4A7C15ull^(b+0x632BE59BD9B4E019ull+(a<<6)+(a>>2));
    h^=h>>31;h*=0xBF58476D1CE4E5B9ull;h^=h>>27;h*=0x94D049BB133111EBull;h^=h>>31;
    return static_cast<float>(h&0xFFFFFFull)/16777215.f;
}
struct PlaybackContinuityIdentity {
    DWORD pid=0;
    uint64_t processCreatedAtTicks=0;
    bool verified=false;
    std::wstring songId,title,artist,album;
    bool Matches(const PlaybackSnapshot& playback) const {
        if(pid!=playback.pid||processCreatedAtTicks!=playback.processCreatedAtTicks||verified!=playback.songIdVerified)return false;
        if(verified)return SameCatalogId(songId,playback.songId);
        return songId==playback.songId&&title==playback.title&&artist==playback.artist&&album==playback.album;
    }
    static PlaybackContinuityIdentity Capture(const PlaybackSnapshot& playback) {
        PlaybackContinuityIdentity result;
        result.pid=playback.pid;result.processCreatedAtTicks=playback.processCreatedAtTicks;
        result.verified=playback.songIdVerified;result.songId=playback.songId;
        if(!result.verified){result.title=playback.title;result.artist=playback.artist;result.album=playback.album;}
        return result;
    }
};
struct Palette { D2D1_COLOR_F background, panel, ink, muted, accent, glow; };
Palette Colors(int theme) {
    if(theme==WeaveStyle)return {D2D1::ColorF(0x101112),D2D1::ColorF(0x191819),D2D1::ColorF(0xF6F0E8),D2D1::ColorF(0x858181),D2D1::ColorF(0xF7877E),D2D1::ColorF(0x8F443D)};
    if(theme==PosterStyle)return {D2D1::ColorF(0x073CDD),D2D1::ColorF(0x101112),D2D1::ColorF(0xFAFAF6),D2D1::ColorF(0xCFDDFD),D2D1::ColorF(0xDDFF00),D2D1::ColorF(0x1234AB)};
    if(theme==EchoStyle)return {D2D1::ColorF(0xFCFDFC),D2D1::ColorF(0xEFF3F0),D2D1::ColorF(0x153E30),D2D1::ColorF(0x8DA699),D2D1::ColorF(0x28664D),D2D1::ColorF(0xCBDCD1)};
    if (theme == 0) return {D2D1::ColorF(0x111B24), D2D1::ColorF(0x17232E), D2D1::ColorF(0xF0F4F3), D2D1::ColorF(0x9CAFB8), D2D1::ColorF(0x90D4C7), D2D1::ColorF(0x457C8C)};
    if (theme == 1) return {D2D1::ColorF(0x151411), D2D1::ColorF(0x201E19), D2D1::ColorF(0xF4ECDF), D2D1::ColorF(0xB3A68D), D2D1::ColorF(0xDAB374), D2D1::ColorF(0x755B35)};
    return {D2D1::ColorF(0xF7F2E8), D2D1::ColorF(0xECE5D8), D2D1::ColorF(0x352719), D2D1::ColorF(0x8E7864), D2D1::ColorF(0xBA432B), D2D1::ColorF(0xE1C7A8)};
}
D2D1_COLOR_F Color(const ThemeColor& c) { return {c.r, c.g, c.b, c.a}; }
Palette Colors(const ThemePalette& p) {return {Color(p.background),Color(p.panel),Color(p.ink),Color(p.muted),Color(p.accent),Color(p.glow)};}
Palette Colors(const SceneView& view) {
    if (!view.customTheme) return Colors(view.theme);
    const auto& t = *view.customTheme;
    return {Color(t.background), Color(t.panel), Color(t.ink), Color(t.muted), Color(t.accent), Color(t.glow)};
}
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float weight) {
    return {a.r + (b.r - a.r) * weight, a.g + (b.g - a.g) * weight, a.b + (b.b - a.b) * weight, a.a + (b.a - a.a) * weight};
}
float Luminance(D2D1_COLOR_F c) {
    auto linear = [](float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); };
    return .2126f * linear(c.r) + .7152f * linear(c.g) + .0722f * linear(c.b);
}
bool SameColor(D2D1_COLOR_F a, D2D1_COLOR_F b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c,float alpha) {c.a*=std::clamp(alpha,0.f,1.f);return c;}
StageRgb ToRgb(D2D1_COLOR_F c) {return {c.r,c.g,c.b};}
D2D1_COLOR_F FromRgb(StageRgb c,float alpha=1.f) {return {c.r,c.g,c.b,alpha};}
// Same hue, lifted toward white until it reads as a light on a dark stage.
D2D1_COLOR_F Luminous(D2D1_COLOR_F c,float minimum) {
    for(int i=0;i<8&&Luminance(c)<minimum;++i)c=Mix(c,D2D1::ColorF(1,1,1,c.a),.2f);
    return c;
}
struct MotionState {
    double phase = 0;
    float speed = 1, amplitude = 1, rms = 0, scroll = 0;
    float translationVisibility=1,romanizationVisibility=0;
    float focusVisibility=0,pairVisibility=0,chromeVisibility=1,lyricAnchor=0;
    bool anchorInitialized=false;
    float low=0,mid=0,high=0,peak=0,response=1;
    float backgroundShade=0,cameraMotion=1;
    double particlePhase=0;
    std::array<float, 32> spectrum{};
    int64_t audioUpdatedAtMs=0;
    // Low-band onset envelope for visual pulses only. It is not presented
    // as tempo, beat or synchronization evidence.
    float pulse=0,lowFast=0,lowSlow=0;
    int64_t lastPulseAt=0;
    float consoleVisibility=0;
    bool initialized = false, scrollInitialized = false;
};
enum FontRole : int { FontChrome=0,FontChromeStrong,FontLyricFlow,FontLyricCinema,FontLyricType,FontSecondary,FontRoman,FontDisplay,FontCount };
int LyricFont(int style) {return style==WeaveStyle?FontChrome:style==0||style==PosterStyle?FontLyricFlow:style==1||style==EchoStyle?FontLyricCinema:FontLyricType;}
struct SceneGeometry {
    float stageWidth = 0, height = 0;
    float lyricLeft = 0, lyricWidth = 0, lyricTop = 0, lyricBottom = 0, anchor = 0;
    float metaX = 0, metaY = 0, metaWidth = 0, footerY = 0;
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    bool panel=false,compact=false;
    // Stage composition: particle field, progress rail and now-playing block.
    bool particles=false;
    float fieldX=0,fieldY=0,fieldSize=0,fieldOpacity=0;
    float railLeft=0,railRight=0,railY=0;
    int nowPlaying=0; // 0 bottom card, 1 below the field, 2 typographic header, 3 desktop, 4 compact
};
SceneGeometry Geometry(float width, float height, int style, const SceneView& view) {
    SceneGeometry g;
    g.stageWidth=view.panelVisible&&!view.desktop?std::max(1.f,width-SettingsRailWidth(width)):width;
    g.height = height; g.panel = view.panelVisible && !view.desktop;
    g.compact=CompactStageLayout(g.stageWidth,height);
    const float margin=g.compact?20.f:std::clamp(g.stageWidth*.055f,28.f,88.f);
    g.railLeft=margin;g.railRight=std::max(margin+1.f,g.stageWidth-margin);
    g.railY=height-(g.compact?16.f:view.desktop?54.f:30.f);
    g.footerY=g.railY;
    if(IsOriginalStyle(style)){
        const auto original=ComposeOriginal(style,g.stageWidth,height,view.desktop?0.f:view.originalDrawer);
        g.stageWidth=original.width;g.compact=original.compact;
        g.lyricLeft=original.lyricX;g.lyricWidth=original.lyricWidth;g.lyricTop=original.lyricY;
        g.lyricBottom=original.lyricY+original.lyricHeight;g.anchor=original.lyricY;
        g.alignment=style==PosterStyle?DWRITE_TEXT_ALIGNMENT_LEADING:DWRITE_TEXT_ALIGNMENT_CENTER;
        g.railLeft=original.margin;g.railRight=original.width-original.margin;g.railY=original.railY;g.footerY=g.railY;
        g.nowPlaying=5;return g;
    }
    if (view.desktop && style != 1) {
        // Flow/type retain the desktop lyric margin. Cinema shares the same
        // responsive cover-and-lyric composition as its window/fullscreen stage;
        // being behind Explorer icons must not remove its album artwork.
        g.lyricLeft = g.stageWidth * .53f;
        g.lyricWidth = std::max(150.f, g.stageWidth * .43f);
        g.metaX = g.lyricLeft; g.metaY = std::max(32.f, height*.16f);
        g.metaWidth = g.lyricWidth; g.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        g.nowPlaying=3;g.railLeft=g.lyricLeft;g.railRight=g.lyricLeft+g.lyricWidth;
        g.lyricTop=g.metaY+70.f;g.lyricBottom=g.railY-60.f;
    } else if(g.compact) {
        g.lyricLeft=margin;g.lyricWidth=std::max(160.f,g.stageWidth-2*margin);
        g.metaX=margin;g.metaY=14.f;g.metaWidth=std::max(120.f,g.lyricWidth-130.f); // clear of the settings toggle
        g.alignment=style==0?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING;
        g.nowPlaying=4;g.lyricTop=g.metaY+44.f;g.lyricBottom=g.railY-30.f;
    } else if (style == 1) {
        const bool wide=g.stageWidth>=860.f&&g.stageWidth>=height*1.05f;
        if(wide){
            // Cover particles on the left, the lyric column on the right.
            g.particles=true;g.fieldOpacity=1.f;
            g.fieldSize=std::min(g.stageWidth*.36f,height*.50f);
            g.fieldX=std::max(margin+g.fieldSize*.58f,g.stageWidth*.27f);
            g.fieldY=height*.44f;
            g.lyricLeft=std::max(g.fieldX+g.fieldSize*.66f,g.stageWidth*.50f);
            g.lyricWidth=std::max(200.f,g.stageWidth-margin-g.lyricLeft);
            g.alignment=DWRITE_TEXT_ALIGNMENT_LEADING;
            g.nowPlaying=1;g.metaWidth=std::max(160.f,g.fieldSize*1.25f);
            g.metaX=g.fieldX-g.metaWidth*.5f;g.metaY=g.fieldY+g.fieldSize*.5f+34.f;
            g.lyricTop=108.f;g.lyricBottom=g.railY-60.f;
        } else {
            g.lyricLeft=margin;g.lyricWidth=std::max(160.f,g.stageWidth-2*margin);
            g.alignment=DWRITE_TEXT_ALIGNMENT_CENTER;g.nowPlaying=0;
            g.particles=true;
            g.fieldSize=std::min(g.stageWidth*.44f,height*.30f);
            g.fieldX=g.stageWidth*.5f;g.fieldY=96.f+g.fieldSize*.5f;
            g.lyricTop=g.fieldY+g.fieldSize*.5f+26.f;g.lyricBottom=g.railY-96.f;
            g.fieldOpacity=.95f;
            if(g.lyricBottom-g.lyricTop<200.f){
                // Too short to stack: keep a faint field behind the lyrics.
                g.fieldSize=std::min(g.stageWidth*.6f,height*.62f);g.fieldY=height*.46f;
                g.fieldOpacity=.30f;g.lyricTop=108.f;
            }
        }
    } else if (style == 2) {
        g.lyricLeft=margin*1.35f;g.lyricWidth=std::max(200.f,g.stageWidth-g.lyricLeft-margin*1.5f);
        g.alignment=DWRITE_TEXT_ALIGNMENT_LEADING;g.nowPlaying=2;
        g.metaX=g.lyricLeft;g.metaY=124.f;g.metaWidth=g.lyricWidth; // below the stage action buttons
        g.lyricTop=g.metaY+96.f;g.lyricBottom=g.railY-56.f;
    } else {
        const float side=std::clamp(g.stageWidth*.09f,32.f,140.f);
        g.lyricLeft=side;g.lyricWidth=std::max(160.f,g.stageWidth-2*side);
        g.alignment=DWRITE_TEXT_ALIGNMENT_CENTER;g.nowPlaying=0;
        g.lyricTop=108.f;g.lyricBottom=g.railY-104.f;
    }
    g.lyricBottom=std::max(g.lyricTop+32.f,g.lyricBottom);
    g.anchor = g.lyricTop + (g.lyricBottom - g.lyricTop) * (style == 2 ? .40f : .45f);
    return g;
}
// Text colors for the active lyric. dim/lit carry the theme ink; accent and
// glow may follow the cover (built-in flow theme only), never the timing.
struct StageInk {
    D2D1_COLOR_F dim{},lit{},accentLit{},glow{};
    float glowStrength=0;
    bool lightStage=false;
};
struct KineticItem {
    const glyphs::Run* run=nullptr;
    size_t first=0,last=0;
    D2D1_POINT_2F baseline{};
    D2D1_MATRIX_3X2_F transform=D2D1::Matrix3x2F::Identity();
    float lit=0;          // 0 unsung .. 1 sung (solid color mix)
    bool sweep=false;     // feathered lit/dim edge at sweepX (item-local x)
    float sweepX=0;
    float glow=0;         // 0..1
    float accent=0;       // 0..1 toward StageInk::accentLit
    float opacity=1;
};
struct KineticGuide { D2D1_POINT_2F from{},to{}; D2D1_MATRIX_3X2_F transform=D2D1::Matrix3x2F::Identity(); float opacity=0; };
struct KineticRow {
    std::vector<KineticItem> items;
    std::vector<KineticGuide> guides;
    std::shared_ptr<const glyphs::Layout> source; // keeps runs referenced by items alive
    // Row role weight and the plain-row color it converges to, so a row that
    // stops animating continues exactly as the static context rows do.
    float activity=1;
    D2D1_COLOR_F resting{};
    bool valid=false;
};
struct WordSpan {
    UINT32 start=0,end=0;
    float progress=0,sinceEnd=-1,duration=0,advance=0,offset=0;
    float left=std::numeric_limits<float>::max(),right=-std::numeric_limits<float>::max(),baseline=0;
    bool baselineSet=false,wrapped=false;
};
}
struct SceneRenderer::Impl {
    HWND window = nullptr;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> target; ComPtr<ID2D1Bitmap1> backBuffer;
    ComPtr<ID3D11RenderTargetView> backBufferView;
    ComPtr<ID3D11Texture2D> offscreenTexture; // preview target; no swap chain
    ComPtr<ID3D11Texture2D> backTexture;      // the texture behind backBuffer/backBufferView
    bool offscreen=false;
    std::unique_ptr<StageGpu> gpu;
    std::unique_ptr<OriginalStageGpu> originalGpu;
    OriginalStageState originalState;
    OriginalVisualFrame originalFrame;
    // Optional second pass: the stage is drawn into the compositor's texture
    // when Backstage or a glass panel needs it, otherwise straight to the
    // back buffer exactly as before.
    std::unique_ptr<StageCompositor> compositor;
    ComPtr<ID2D1Bitmap1> stageBitmap,blurBitmap;
    std::unique_ptr<ui::Painter> painter;
    uint64_t compositorGeneration=0;float compositorScale=0;int64_t compositorUsedAt=0;
    // Backstage camera transition (presentation only): spring position and
    // velocity, and the reduced-motion cross-fade.
    float backstageValue=0,backstageVelocity=0,backstageMix=1;
    ComPtr<IDWriteFactory> textFactory; ComPtr<IDWriteInlineObject> ellipsis;
    ComPtr<ID2D1SolidColorBrush> brush, accentBrush;
    ComPtr<ID2D1Bitmap1> bitmap, coverBitmap;
    ComPtr<ID2D1Effect> coverScale, coverBlur, glowBlur;
    uint64_t bitmapSequence = 0, coverSequence = 0;
    uint16_t bitmapOrientation=1;
    double bitmapPixelAspect=1.0;
    bool coverReady = false;
    D2D1_COLOR_F coverColor = D2D1::ColorF(0x457C8C);
    ComPtr<ID2D1Bitmap1> previousBackground;
    uint64_t sourceRevision=UINT64_MAX;
    int64_t transitionStartedAt=0;
    bool sourceCover=false,awaitingFrame=false;
    float previousTint=0,previousCoverWeight=0;
    D2D1_COLOR_F previousCoverColor=D2D1::ColorF(0x457C8C);
    // GPU stage cover: one upright square thumbnail per decoded cover frame.
    const BackgroundFrame* stageCoverFrame=nullptr;uint64_t stageCoverSequence=0;
    CoverPalette coverPalette;
    ComPtr<ID2D1Bitmap1> thumbnail;ComPtr<ID2D1BitmapBrush> thumbnailBrush;uint32_t thumbnailEdge=0;
    bool gpuStageActive=false;
    ComPtr<IDWriteTextFormat> bodyFormat;
    struct FontSpec { std::wstring family; DWRITE_FONT_WEIGHT weight=DWRITE_FONT_WEIGHT_NORMAL; ComPtr<IDWriteFontFallback> fallback; };
    std::array<FontSpec,FontCount> fonts;
    struct TextEntry {
        std::wstring text;
        int role=FontChrome;
        float width = 0, height = 0, size = 0, measuredHeight = 0;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        ComPtr<IDWriteTextLayout> layout;
        bool trimmed=true,balanceCentered=false;
    };
    std::deque<TextEntry> textCache;
    struct GlyphEntry { ComPtr<IDWriteTextLayout> layout; std::shared_ptr<const glyphs::Layout> glyphs; };
    std::deque<GlyphEntry> glyphCache;
    struct PartialGeometry {
        ComPtr<IDWriteTextLayout> layout;
        UINT32 start=0,length=0;
        std::vector<DWRITE_HIT_TEST_METRICS> cells;
        float width=0;
    } partialGeometry;
    struct GlowEntry { D2D1_COLOR_F color{}; ComPtr<ID2D1RadialGradientBrush> brush; };
    std::array<GlowEntry, 6> glows{};
    ComPtr<ID2D1LinearGradientBrush> scrim;
    D2D1_COLOR_F scrimColor{};
    float scrimLead = -1, scrimTrail = -1;
    struct SweepEntry { D2D1_COLOR_F from{},to{}; ComPtr<ID2D1LinearGradientBrush> brush; };
    std::array<SweepEntry,4> sweeps{};size_t sweepNext=0;
    std::array<ComPtr<ID2D1PathGeometry>,4> shapes; // triangle, square, cross, star
    HANDLE frameEvent = nullptr;
    bool software=false,blt=false,compositionPaced=false,occluded=false;
    HRESULT lastPresentResult=S_FALSE,lastVisibilityResult=S_FALSE;
    uint64_t presentAttempts=0,presentedFrames=0;
    UINT width = 0, height = 0; float scale = 1;
    int64_t lastFrame = 0, lastPresentedAt = 0;
    MotionState motion;
    std::shared_ptr<const LyricTimeline> lastTimeline;
    uint64_t lastLyricRevision = 0,lastLyricReadingRevision=0;
    // A reliable source can briefly disappear while GSMTC/UIA/native-read
    // ownership changes. Preserve the last rendered position for at most one
    // second so the lyric column does not jump to its manual preview row and
    // back. The frozen value never advances, never enables word highlighting,
    // and is cleared immediately when the track identity changes.
    PlaybackContinuityIdentity continuityIdentity;
    int64_t continuityPositionMs = 0, continuityCapturedAtMs = 0;
    int lastStyle = -1;
    std::array<float, 9> rowRoles{}; size_t roleFirst = 0; bool rolesInitialized = false;
    std::array<float,9> upcomingRoles{};
    struct LyricViewportState {
        std::array<float,9> offsets{};
        size_t first=0;
        bool initialized=false;
    } lyricViewport;
    // Presentation-only entrance of a newly selected row (typography stage).
    size_t entryIndex=SIZE_MAX;int64_t entrySince=0;
    // Presentation cost: non-blocking GPU frame timing, an adaptive quality
    // tier (0 high .. 3 low) and the present cadence. None of this is timing
    // evidence for lyrics; it only scales visual work.
    struct GpuTiming { ComPtr<ID3D11Query> disjoint,start,end; bool pending=false; };
    std::array<GpuTiming,4> timings{};size_t timingSlot=0;bool timingOpen=false;
    float gpuFrameMs=0;bool gpuFrameSampled=false;
    int qualityTier=1;int64_t qualityOverSince=0,qualityUnderSince=0,qualityChangedAt=0;
    UINT refreshHz=60;int64_t refreshCheckedAt=0;
    int64_t lastActivityAt=0,lastActivityPosition=std::numeric_limits<int64_t>::min();
    // A stage that becomes drawable again (restored, uncovered, resumed after
    // a fullscreen application) fades in from its background color.
    int64_t veilStartedAt=0;

    ~Impl() {
        // Same-HWND flip recreation must complete deferred resource destruction.
        // https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush
        partialGeometry={};textCache.clear();glyphCache.clear(); ellipsis.Reset();
        coverBlur.Reset(); coverScale.Reset(); glowBlur.Reset(); coverBitmap.Reset(); bitmap.Reset();
        thumbnailBrush.Reset();thumbnail.Reset();previousBackground.Reset();
        painter.reset();stageBitmap.Reset();blurBitmap.Reset();
        brush.Reset(); accentBrush.Reset(); for (auto& glow : glows) glow.brush.Reset(); scrim.Reset();
        for(auto& sweep:sweeps)sweep.brush.Reset();for(auto& shape:shapes)shape.Reset();
        if (target) target->SetTarget(nullptr);
        backBufferView.Reset();backTexture.Reset();gpu.reset();compositor.reset();offscreenTexture.Reset();
        backBuffer.Reset(); target.Reset(); d2dDevice.Reset(); factory.Reset(); swap.Reset();
        if (context) { context->ClearState(); context->Flush(); }
        if (frameEvent) CloseHandle(frameEvent);
    }
    void CreateDevice(bool forceSoftware) {
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = forceSoftware ? E_FAIL : D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
        if (FAILED(hr)) {
            software = true;
            Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context));
        }
    }
    void Create(HWND h, bool desktopBlt, bool forceSoftware) {
        window = h; blt = desktopBlt;
        const UINT dpi = GetDpiForWindow(h); scale = (dpi ? dpi : 96) / 96.f;
        CreateDevice(forceSoftware);
        ComPtr<IDXGIDevice> dxgi; Check(device.As(&dxgi)); ComPtr<IDXGIAdapter> adapter; Check(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> f; Check(adapter->GetParent(IID_PPV_ARGS(&f)));
        RECT r{}; GetClientRect(h, &r); width = (UINT)std::max<LONG>(1, r.right); height = (UINT)std::max<LONG>(1, r.bottom);
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
        desc.SwapEffect = blt ? DXGI_SWAP_EFFECT_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Flags = blt ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        Check(f->CreateSwapChainForHwnd(device.Get(), h, &desc, nullptr, nullptr, &swap)); f->MakeWindowAssociation(h, DXGI_MWA_NO_ALT_ENTER);
        if (!blt) {
            ComPtr<IDXGISwapChain2> s; Check(swap.As(&s)); Check(s->SetMaximumFrameLatency(1)); frameEvent = s->GetFrameLatencyWaitableObject();
            if(!frameEvent)Check(E_HANDLE);
        }
        CreateResources();
    }
    // Development previews: the same scene drawn into a texture, without a
    // window, swap chain, presentation or frame pacing.
    void CreateOffscreen(UINT pixelWidth,UINT pixelHeight,float dpiScale,bool forceSoftware) {
        window=nullptr;blt=false;offscreen=true;
        scale=std::isfinite(dpiScale)?std::clamp(dpiScale,.5f,4.f):1.f;
        width=std::clamp(pixelWidth,1u,8192u);height=std::clamp(pixelHeight,1u,8192u);
        CreateDevice(forceSoftware);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&desc,nullptr,&offscreenTexture));
        CreateResources();
    }
    bool CaptureBgra(std::vector<uint8_t>& pixels,UINT& pixelWidth,UINT& pixelHeight) {
        if(!offscreenTexture||!device||!context)return false;
        D3D11_TEXTURE2D_DESC desc{};offscreenTexture->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&desc,nullptr,&staging));
        context->CopyResource(staging.Get(),offscreenTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        pixelWidth=desc.Width;pixelHeight=desc.Height;pixels.resize(size_t(desc.Width)*desc.Height*4);
        for(UINT y=0;y<desc.Height;++y)std::memcpy(&pixels[size_t(y)*desc.Width*4],static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(desc.Width)*4);
        context->Unmap(staging.Get(),0);
        return true;
    }
    void CreateResources() {
        ComPtr<IDXGIDevice> dxgi; Check(device.As(&dxgi));
        // Native Direct2D 1.1 effect graph; no CPU blur, web renderer or media copy.
        // https://learn.microsoft.com/en-us/windows/win32/direct2d/devices-and-device-contexts
        Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        Check(factory->CreateDevice(dxgi.Get(), &d2dDevice));
        Check(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &target));
        Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(textFactory.GetAddressOf())));
        ResolveFonts();
        Format(15, bodyFormat, FontChrome);
        Check(textFactory->CreateEllipsisTrimmingSign(bodyFormat.Get(), &ellipsis));
        // The GPU stage is optional: a device without SM4 keeps the 2D scene.
        gpu=std::make_unique<StageGpu>();
        if(!gpu->Create(device.Get(),software,blt))gpu.reset();
        originalGpu=std::make_unique<OriginalStageGpu>();
        if(!originalGpu->Create(device.Get()))originalGpu.reset();
        // The desktop host never shows Backstage or glass panels.
        if(!blt){
            compositor=std::make_unique<StageCompositor>();
            if(!compositor->Create(device.Get()))compositor.reset();
        }
        MakeTarget();
        Check(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
        Check(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &accentBrush));
        painter=std::make_unique<ui::Painter>();
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if(!software&&SUCCEEDED(target->CreateEffect(CLSID_D2D1GaussianBlur,&glowBlur))){
            if(FAILED(glowBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED)))glowBlur.Reset();
        }
        CreateShapes();
        // Optional: without timestamp queries the tier simply stays put.
        for(auto& timing:timings){
            D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
            if(FAILED(device->CreateQuery(&query,&timing.disjoint))){timing={};continue;}
            query.Query=D3D11_QUERY_TIMESTAMP;
            if(FAILED(device->CreateQuery(&query,&timing.start))||FAILED(device->CreateQuery(&query,&timing.end)))timing={};
        }
    }
    void BeginGpuTiming() {
        timingOpen=false;
        auto& timing=timings[timingSlot];
        if(!timing.disjoint||!timing.start||!timing.end||timing.pending)return; // still in flight
        context->Begin(timing.disjoint.Get());context->End(timing.start.Get());timingOpen=true;
    }
    void EndGpuTiming() {
        if(!timingOpen)return;
        auto& timing=timings[timingSlot];
        context->End(timing.end.Get());context->End(timing.disjoint.Get());
        timing.pending=true;timingOpen=false;timingSlot=(timingSlot+1)%timings.size();
    }
    void CollectGpuTiming() {
        for(auto& timing:timings){
            if(!timing.pending)continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};UINT64 begin=0,end=0;
            if(context->GetData(timing.disjoint.Get(),&disjoint,sizeof(disjoint),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||
                context->GetData(timing.start.Get(),&begin,sizeof(begin),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||
                context->GetData(timing.end.Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
            timing.pending=false;
            if(disjoint.Disjoint||!disjoint.Frequency||end<begin)continue;
            const float ms=static_cast<float>(static_cast<double>(end-begin)*1000.0/static_cast<double>(disjoint.Frequency));
            if(!std::isfinite(ms)||ms<0.f||ms>250.f)continue;
            gpuFrameMs=gpuFrameSampled?gpuFrameMs+(ms-gpuFrameMs)*.12f:ms;gpuFrameSampled=true;
        }
    }
    UINT RefreshRate() {
        const auto now=MonotonicMs();
        if(refreshCheckedAt&&now>=refreshCheckedAt&&now-refreshCheckedAt<2000)return refreshHz;
        refreshCheckedAt=now;UINT hz=60;
        MONITORINFOEXW info{};info.cbSize=sizeof(info);DEVMODEW mode{};mode.dmSize=sizeof(mode);
        const auto monitor=window?MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST):nullptr;
        if(monitor&&GetMonitorInfoW(monitor,&info)&&EnumDisplaySettingsW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode)&&
            mode.dmDisplayFrequency>1&&mode.dmDisplayFrequency<1000)hz=mode.dmDisplayFrequency;
        refreshHz=hz;return hz;
    }
    // Vertical blanks per presented frame: about 60 fps (automatic), the
    // display rate, or about 30 fps; idle or lite stages drop to about 30.
    UINT PresentInterval(const SceneView& view,bool idle) {
        const double hz=RefreshRate();
        const auto every=[&](double fps){return static_cast<UINT>(std::clamp<long>(std::lround(hz/fps),1,4));};
        UINT interval=view.frameRateMode==1?1u:view.frameRateMode==2?every(30):every(60);
        if(idle||view.lite)interval=std::max(interval,every(30));
        return interval;
    }
    // Adaptive tier with hysteresis: step down after 1.5 s over budget, step
    // up after 8 s with ample headroom, and hold 3 s after any change.
    void UpdateQuality(const SceneView& view,int64_t now,float budgetMs) {
        int best=0,worst=3;
        if(software||view.lite)best=worst=3;
        else if(view.qualityMode==1)best=worst=0;
        else if(view.qualityMode==2)best=worst=2;
        else if(view.qualityMode==3)best=worst=3;
        else best=view.onBattery?2:0;
        qualityTier=std::clamp(qualityTier,best,worst);
        if(best==worst||!gpuFrameSampled||!std::isfinite(budgetMs)||budgetMs<=0.f)return;
        if(qualityChangedAt&&now-qualityChangedAt<3000){qualityOverSince=qualityUnderSince=0;return;}
        if(gpuFrameMs>budgetMs*.55f){
            qualityUnderSince=0;
            if(!qualityOverSince)qualityOverSince=now;
            else if(now-qualityOverSince>=1500&&qualityTier<worst){++qualityTier;qualityChangedAt=now;qualityOverSince=0;}
        }else if(gpuFrameMs<budgetMs*.25f){
            qualityOverSince=0;
            if(!qualityUnderSince)qualityUnderSince=now;
            else if(now-qualityUnderSince>=8000&&qualityTier>best){--qualityTier;qualityChangedAt=now;qualityUnderSince=0;}
        }else qualityOverSince=qualityUnderSince=0;
    }
    // Backstage camera transition. An underdamped spring (about 450 ms, a
    // slight overshoot when opening, none past full screen when closing);
    // reduced motion keeps the final framing and cross-fades instead.
    // Returns true while anything moves, which keeps the full cadence.
    bool UpdateBackstage(const SceneView& view,float dt) {
        if(IsOriginalStyle(view.customTheme?view.customTheme->baseStyle:view.theme)){
            const bool changed=backstageValue!=0;backstageValue=backstageVelocity=0;backstageMix=1;return changed;
        }
        const float goal=view.backstage&&compositor?1.f:0.f;
        const float valueBefore=backstageValue,mixBefore=backstageMix;
        if(view.reducedMotion){
            if(backstageValue<=0.f)backstageMix=0.f; // a fade starts from the plain stage
            backstageVelocity=0.f;
            backstageMix=Approach(backstageMix,goal,dt,9.f);
            if(goal<=0.f&&backstageMix<.002f)backstageMix=0.f;
            if(goal>0.f&&backstageMix>.998f)backstageMix=1.f;
            backstageValue=backstageMix>0.f||goal>0.f?1.f:0.f;
        }else{
            backstageMix=backstageValue<=0.f?1.f:Approach(backstageMix,1.f,dt,12.f);
            constexpr float omega=13.f,damping=.78f;
            float remaining=std::clamp(dt,0.f,.1f);
            while(remaining>0.f){
                const float h=std::min(remaining,.004f);remaining-=h;
                const float accel=-omega*omega*(backstageValue-goal)-2.f*damping*omega*backstageVelocity;
                backstageVelocity+=accel*h;backstageValue+=backstageVelocity*h;
            }
            if(backstageValue<0.f){backstageValue=0.f;backstageVelocity=0.f;}
            if(backstageValue>1.08f){backstageValue=1.08f;backstageVelocity=std::min(backstageVelocity,0.f);}
            if(std::abs(backstageValue-goal)<.0005f&&std::abs(backstageVelocity)<.002f){backstageValue=goal;backstageVelocity=0.f;}
            if(!std::isfinite(backstageValue)||!std::isfinite(backstageVelocity)){backstageValue=goal;backstageVelocity=0.f;}
        }
        return backstageValue!=valueBefore||backstageMix!=mixBefore;
    }
    // Sizes the compositor and wraps its textures for Direct2D: the stage
    // texture as a drawing target, the blurred copy as a brush source.
    bool EnsureComposite() {
        if(!compositor||!backBufferView||!backTexture)return false;
        compositor->Prepare(width,height);
        if(!compositor->StageTexture()||!compositor->StageTarget()||!compositor->BlurTexture())return false;
        if(stageBitmap&&blurBitmap&&compositorGeneration==compositor->Generation()&&compositorScale==scale)return true;
        stageBitmap.Reset();blurBitmap.Reset();
        ComPtr<IDXGISurface> surface;Check(compositor->StageTexture()->QueryInterface(IID_PPV_ARGS(&surface)));
        const auto stageProps=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96*scale,96*scale);
        Check(target->CreateBitmapFromDxgiSurface(surface.Get(),&stageProps,&stageBitmap));
        ComPtr<IDXGISurface> blurSurface;Check(compositor->BlurTexture()->QueryInterface(IID_PPV_ARGS(&blurSurface)));
        const auto blurProps=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        Check(target->CreateBitmapFromDxgiSurface(blurSurface.Get(),&blurProps,&blurBitmap));
        compositorGeneration=compositor->Generation();compositorScale=scale;
        return true;
    }
    static bool FamilyExists(IDWriteFontCollection* collection,const wchar_t* name) {
        UINT32 index=0;BOOL exists=FALSE;
        return collection&&name&&SUCCEEDED(collection->FindFamilyName(name,&index,&exists))&&exists;
    }
    // A family qualifies only when its nearest real face is close to the
    // requested weight; a heavy-only family must not stand in for body text.
    static bool FamilyWeight(IDWriteFontCollection* collection,const wchar_t* name,DWRITE_FONT_WEIGHT requested,int lowest,int highest) {
        UINT32 index=0;BOOL exists=FALSE;
        if(!collection||!name||FAILED(collection->FindFamilyName(name,&index,&exists))||!exists)return false;
        ComPtr<IDWriteFontFamily> family;ComPtr<IDWriteFont> font;
        if(FAILED(collection->GetFontFamily(index,&family))||FAILED(family->GetFirstMatchingFont(requested,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&font)))return false;
        const int weight=static_cast<int>(font->GetWeight());
        return weight>=lowest&&weight<=highest&&font->GetSimulations()==DWRITE_FONT_SIMULATIONS_NONE;
    }
    ComPtr<IDWriteFontFallback> CjkFallback(const std::wstring& cjkFamily) {
        ComPtr<IDWriteFactory2> factory2;ComPtr<IDWriteFontFallbackBuilder> builder;
        if(FAILED(textFactory.As(&factory2))||FAILED(factory2->CreateFontFallbackBuilder(&builder)))return {};
        static const DWRITE_UNICODE_RANGE ranges[]={{0x2E80,0x2FDF},{0x3000,0x30FF},{0x3100,0x31FF},{0x3200,0x33FF},
            {0x3400,0x4DBF},{0x4E00,0x9FFF},{0xF900,0xFAFF},{0xFE30,0xFE4F},{0xFF00,0xFFEF},{0x20000,0x2FA1F}};
        const wchar_t* names[]={cjkFamily.c_str()};
        if(FAILED(builder->AddMapping(ranges,static_cast<UINT32>(std::size(ranges)),names,1)))return {};
        ComPtr<IDWriteFontFallback> system;
        if(SUCCEEDED(factory2->GetSystemFontFallback(&system))&&system&&FAILED(builder->AddMappings(system.Get())))return {};
        ComPtr<IDWriteFontFallback> result;
        if(FAILED(builder->CreateFontFallback(&result)))return {};
        return result;
    }
    void ResolveFonts() {
        ComPtr<IDWriteFontCollection> system;
        if(FAILED(textFactory->GetSystemFontCollection(&system,FALSE)))system.Reset();
        const auto first=[&](std::initializer_list<const wchar_t*> names)->const wchar_t*{
            for(const auto* name:names)if(FamilyExists(system.Get(),name))return name;return nullptr;
        };
        const auto weighted=[&](std::initializer_list<const wchar_t*> names,DWRITE_FONT_WEIGHT weight,int lowest,int highest)->const wchar_t*{
            for(const auto* name:names)if(FamilyWeight(system.Get(),name,weight,lowest,highest))return name;return nullptr;
        };
        const auto* yahei=first({L"Microsoft YaHei UI",L"Microsoft YaHei"});
        const std::wstring sans=yahei?yahei:L"Microsoft YaHei UI";
        const auto* latinDisplay=first({L"Segoe UI Variable Display",L"Segoe UI"});
        const std::initializer_list<const wchar_t*> serifs={L"Noto Serif SC",L"Source Han Serif SC",L"Source Han Serif CN",
            L"\x601D\x6E90\x5B8B\x4F53",L"Noto Serif CJK SC",L"Source Han Serif"};
        const auto* serifBody=weighted(serifs,DWRITE_FONT_WEIGHT_SEMI_BOLD,400,700);
        const auto* serifBold=weighted(serifs,DWRITE_FONT_WEIGHT_BOLD,600,900);
        fonts[FontChrome]={sans,DWRITE_FONT_WEIGHT_NORMAL,{}};
        fonts[FontChromeStrong]={sans,DWRITE_FONT_WEIGHT_BOLD,{}};
        fonts[FontSecondary]={sans,DWRITE_FONT_WEIGHT_NORMAL,{}};
        fonts[FontRoman]={first({L"Segoe UI Variable Text",L"Segoe UI"})?first({L"Segoe UI Variable Text",L"Segoe UI"}):L"Segoe UI",DWRITE_FONT_WEIGHT_NORMAL,{}};
        // Modern sans for the flow stage: Latin display face, CJK in YaHei.
        if(latinDisplay)fonts[FontLyricFlow]={latinDisplay,DWRITE_FONT_WEIGHT_BOLD,CjkFallback(sans)};
        else fonts[FontLyricFlow]={sans,DWRITE_FONT_WEIGHT_BOLD,{}};
        // Serif stages use an installed CJK serif with real weights, or Georgia
        // with SimSun at its native weight instead of synthesized bold.
        if(serifBody)fonts[FontLyricCinema]={serifBody,DWRITE_FONT_WEIGHT_SEMI_BOLD,{}};
        else fonts[FontLyricCinema]={L"Georgia",DWRITE_FONT_WEIGHT_NORMAL,CjkFallback(L"SimSun")};
        if(serifBold)fonts[FontLyricType]={serifBold,DWRITE_FONT_WEIGHT_BOLD,{}};
        else if(latinDisplay)fonts[FontLyricType]={latinDisplay,DWRITE_FONT_WEIGHT_BOLD,CjkFallback(sans)};
        else fonts[FontLyricType]={sans,DWRITE_FONT_WEIGHT_BOLD,{}};
        fonts[FontDisplay]=fonts[FontLyricType];
    }
    void Format(float size, ComPtr<IDWriteTextFormat>& out, int role) {
        const auto& font=fonts[std::clamp(role,0,FontCount-1)];
        out.Reset();
        Check(textFactory->CreateTextFormat(font.family.empty()?L"Microsoft YaHei UI":font.family.c_str(), nullptr, font.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &out));
        if(font.fallback){ComPtr<IDWriteTextFormat1> format1;if(SUCCEEDED(out.As(&format1)))Check(format1->SetFontFallback(font.fallback.Get()));}
    }
    void CreateShapes() {
        const auto make=[&](std::initializer_list<D2D1_POINT_2F> points,ComPtr<ID2D1PathGeometry>& out){
            ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
            if(FAILED(factory->CreatePathGeometry(&path))||FAILED(path->Open(&sink)))return;
            auto it=points.begin();sink->BeginFigure(*it,D2D1_FIGURE_BEGIN_HOLLOW);
            for(++it;it!=points.end();++it)sink->AddLine(*it);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if(SUCCEEDED(sink->Close()))out=path;
        };
        make({{0,-1},{.866f,.5f},{-.866f,.5f}},shapes[0]);
        make({{-.7f,-.7f},{.7f,-.7f},{.7f,.7f},{-.7f,.7f}},shapes[1]);
        make({{-.22f,-1},{.22f,-1},{.22f,-.22f},{1,-.22f},{1,.22f},{.22f,.22f},{.22f,1},{-.22f,1},{-.22f,.22f},{-1,.22f},{-1,-.22f},{-.22f,-.22f}},shapes[2]);
        make({{0,-1},{.24f,-.24f},{1,0},{.24f,.24f},{0,1},{-.24f,.24f},{-1,0},{-.24f,-.24f}},shapes[3]);
    }
    // The swap chain's current back buffer, or the preview texture.
    HRESULT BackTexture(ComPtr<ID3D11Texture2D>& texture) {
        texture.Reset();
        if(offscreen){texture=offscreenTexture;return texture?S_OK:E_POINTER;}
        return swap?swap->GetBuffer(0,IID_PPV_ARGS(&texture)):E_POINTER;
    }
    void MakeTarget() {
        ComPtr<ID3D11Texture2D> texture;Check(BackTexture(texture));
        ComPtr<IDXGISurface> surface; Check(texture.As(&surface));
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * scale, 96 * scale);
        Check(target->CreateBitmapFromDxgiSurface(surface.Get(), &props, &backBuffer));
        target->SetTarget(backBuffer.Get()); target->SetDpi(96 * scale, 96 * scale);
        backBufferView.Reset();backTexture=texture;
        if((gpu||originalGpu||compositor)&&FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&backBufferView)))backBufferView.Reset();
    }
    void Resize() {
        if (offscreen || !swap || !target) return;
        RECT r{}; GetClientRect(window, &r); if (r.right < 1 || r.bottom < 1) return;
        // No resource may retain the old DXGI back buffer across ResizeBuffers.
        target->SetTarget(nullptr); backBuffer.Reset(); backBufferView.Reset(); backTexture.Reset();
        if(context)context->ClearState();
        width = (UINT)r.right; height = (UINT)r.bottom;
        const UINT dpi = GetDpiForWindow(window); scale = (dpi ? dpi : 96) / 96.f;
        Check(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, blt ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
        MakeTarget();partialGeometry={};textCache.clear();glyphCache.clear();lyricViewport.initialized=false;
    }
    TextEntry& Layout(const std::wstring& text, float w, float h, float size, int role, DWRITE_TEXT_ALIGNMENT alignment,bool trimmed=true,bool balanceCentered=false) {
        w = std::max(1.f, w); h = std::max(1.f, h);
        for (auto& entry : textCache) if (entry.text == text && entry.width == w && entry.height == h && entry.size == size && entry.role == role && entry.alignment == alignment&&entry.trimmed==trimmed&&entry.balanceCentered==balanceCentered) return entry;
        ComPtr<IDWriteTextFormat> format; Format(size, format, role);
        Check(format->SetTextAlignment(alignment)); Check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
        TextEntry entry{text, role, w, h, size, 0, alignment, {}};
        entry.trimmed=trimmed;entry.balanceCentered=balanceCentered;
        Check(textFactory->CreateTextLayout(text.c_str(), (UINT32)text.size(), format.Get(), w, h, &entry.layout));
        const DWRITE_TRIMMING trim{trimmed?DWRITE_TRIMMING_GRANULARITY_CHARACTER:DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        Check(entry.layout->SetTrimming(&trim, trimmed?ellipsis.Get():nullptr));
        if(balanceCentered&&!trimmed)Check(detail::BalanceCenteredLyricLayout(entry.layout.Get(),text));
        DWRITE_TEXT_METRICS metrics{}; Check(entry.layout->GetMetrics(&metrics));
        if(!std::isfinite(metrics.height)||metrics.height<0)Check(E_FAIL);
        entry.measuredHeight = trimmed?std::clamp(metrics.height,size,h):std::max(size,metrics.height);
        if (textCache.size() >= 96) textCache.pop_front();
        textCache.push_back(std::move(entry)); return textCache.back();
    }
    void Text(const std::wstring& text, float x, float y, float w, float h, float size, D2D1_COLOR_F color,
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING, int role = FontChrome) {
        if (text.empty()||color.a<=0) return;
        auto& entry = Layout(text, w, h, size, role, alignment);
        Check(entry.layout->SetDrawingEffect(nullptr, {0, (UINT32)text.size()}));
        brush->SetColor(color); target->DrawTextLayout(D2D1::Point2F(x, y), entry.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float TextWidth(const std::wstring& text,float size,int role) {
        if(text.empty())return 0.f;
        auto& entry=Layout(text,4096.f,size*2.f,size,role,DWRITE_TEXT_ALIGNMENT_LEADING);
        DWRITE_TEXT_METRICS metrics{};Check(entry.layout->GetMetrics(&metrics));
        return std::isfinite(metrics.widthIncludingTrailingWhitespace)?metrics.widthIncludingTrailingWhitespace:0.f;
    }
    std::shared_ptr<const glyphs::Layout> Glyphs(const ComPtr<IDWriteTextLayout>& layout) {
        for(const auto& entry:glyphCache)if(entry.layout.Get()==layout.Get())return entry.glyphs;
        auto captured=std::make_shared<glyphs::Layout>();
        if(FAILED(glyphs::Capture(layout.Get(),*captured)))*captured=glyphs::Layout{};
        if(glyphCache.size()>=12)glyphCache.pop_front();
        glyphCache.push_back({layout,captured});
        return captured;
    }
    float TransitionProgress(int64_t now) const {
        return SmoothStep(float(std::max<int64_t>(0,now-transitionStartedAt))/850.f);
    }
    float WaitingOpacity(int64_t now) const {
        // Briefly retain the old visual while decoding; it then fades to the
        // theme even when the new source stalls indefinitely.
        return 1.f-SmoothStep(float(std::max<int64_t>(0,now-transitionStartedAt-900))/650.f);
    }
    float CurrentTint(int style) const {
        return sourceCover?(style==2?.87f:style==1?.67f:.50f):(style==2?.20f:style==1?.26f:.16f);
    }
    float CameraZoom() const {
        return 1.f+motion.cameraMotion*((sourceCover?.025f:.012f)+motion.amplitude*
            (.006f*(float)std::sin(motion.phase*.035)+.007f*motion.low*motion.response));
    }
    float DisplayedTint(int style,int64_t now) const {
        if(awaitingFrame)return previousTint;
        return previousBackground?previousTint+(CurrentTint(style)-previousTint)*TransitionProgress(now):CurrentTint(style);
    }
    void MediaLayer(float w,float h,int64_t now,float cameraZoom,D2D1_COLOR_F base) {
        const auto t=TransitionProgress(now);
        if(previousBackground){
            const float opacity=awaitingFrame?WaitingOpacity(now):bitmap?1.f:1.f-t;
            CoverFill(previousBackground.Get(),w,h,1.f,opacity);
        }
        if(bitmap){
            const float opacity=awaitingFrame?0.f:t;
            // Fade the new source's base too: transparent PNG areas eventually
            // reveal the theme, never a stale preceding source.
            if(previousBackground){base.a=opacity;Fill(D2D1::RectF(0,0,w,h),base);}
            CoverFill(sourceCover&&coverReady?coverBitmap.Get():bitmap.Get(),w,h,cameraZoom,opacity,bitmapOrientation,bitmapPixelAspect);
        }
    }
    ComPtr<ID2D1Bitmap1> CaptureMedia(const SceneGeometry& g,const Palette& colors,int64_t now,float cameraZoom) {
        if(!bitmap&&!previousBackground)return {};
        // Freeze the currently composed media, including an interrupted
        // transition. The retained texture never aliases a mutable video frame.
        const float ratio=std::min(1.f,1024.f/std::max(g.stageWidth,g.height));
        const UINT cw=std::max(1u,(UINT)std::lround(g.stageWidth*ratio));
        const UINT ch=std::max(1u,(UINT)std::lround(g.height*ratio));
        ComPtr<ID2D1Bitmap1> frozen;
        const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        Check(target->CreateBitmap(D2D1::SizeU(cw,ch),nullptr,0,&properties,&frozen));
        D2D1_MATRIX_3X2_F transform;target->GetTransform(&transform);
        target->SetTarget(frozen.Get());target->SetDpi(96,96);
        target->SetTransform(D2D1::Matrix3x2F::Scale(float(cw)/g.stageWidth,float(ch)/g.height));
        target->BeginDraw();target->Clear(colors.background);MediaLayer(g.stageWidth,g.height,now,cameraZoom,colors.background);
        const auto result=target->EndDraw();
        target->SetTarget(backBuffer.Get());target->SetDpi(96*scale,96*scale);target->SetTransform(transform);Check(result);
        return frozen;
    }
    void BeginBackgroundSource(const BackgroundSnapshot& background,bool cover,const SceneGeometry& g,
                               const Palette& colors,int style,int64_t now,float cameraZoom) {
        if(sourceRevision==background.sourceRevision&&sourceCover==cover)return;
        const float t=TransitionProgress(now),waiting=WaitingOpacity(now);
        const float oldWeight=previousBackground?previousCoverWeight*(awaitingFrame?waiting:1.f-t):0.f;
        const float newWeight=sourceCover&&coverReady?t:0.f;
        const float total=oldWeight+newWeight;
        const auto tint=DisplayedTint(style,now);
        const auto color=total>0?Mix(previousCoverColor,coverColor,newWeight/total):colors.glow;
        // A renderer that was minimized, hidden in the tray, or occluded may
        // have missed several automatic cover changes.  Its GPU bitmap then
        // belongs to an old song, not to a continuously presented transition.
        // Never flash that stale media when the surface becomes drawable again.
        const auto presentationNow=MonotonicMs();
        const bool continuousPresentation=lastPresentedAt>0&&presentationNow>=lastPresentedAt&&
            presentationNow-lastPresentedAt<=500;
        auto frozen=continuousPresentation?CaptureMedia(g,colors,now,cameraZoom):ComPtr<ID2D1Bitmap1>{};
        if(coverScale)coverScale->SetInput(0,nullptr);
        bitmap.Reset();coverBitmap.Reset();bitmapSequence=coverSequence=0;coverReady=false;
        previousBackground=std::move(frozen);previousTint=tint;previousCoverWeight=std::clamp(total,0.f,1.f);previousCoverColor=color;
        sourceRevision=background.sourceRevision;sourceCover=cover;
        transitionStartedAt=continuousPresentation?now:now-850;
        awaitingFrame=background.loading&&!background.frame;
    }
    void UploadBackground(const BackgroundSnapshot& background, bool cover,const SceneGeometry& g,const Palette& colors,
                          int style,int64_t now,float cameraZoom) {
        BeginBackgroundSource(background,cover,g,colors,style,now,cameraZoom);
        if(awaitingFrame&&(background.frame||!background.loading)){
            // Start from the exact partially faded waiting scene, rather than
            // resurrecting its original opacity when decoding finally finishes.
            const float waiting=WaitingOpacity(now);
            auto frozen=CaptureMedia(g,colors,now,cameraZoom);
            previousBackground=std::move(frozen);previousCoverWeight*=waiting;
            transitionStartedAt=now;awaitingFrame=false;
        }
        if(previousBackground&&((awaitingFrame&&WaitingOpacity(now)<=0)||(!awaitingFrame&&TransitionProgress(now)>=1.f)))previousBackground.Reset();
        if (!background.frame) {
            if (coverScale) coverScale->SetInput(0, nullptr);
            bitmap.Reset(); coverBitmap.Reset(); bitmapSequence = coverSequence = 0; coverReady = false; return;
        }
        const auto& f = *background.frame;
        if (!f.width || !f.height || f.width > 4096 || f.height > 4096 || uint64_t(f.width) * f.height > 3840ull * 2160 ||
            uint64_t(f.stride) < uint64_t(f.width) * 4 || uint64_t(f.stride) * f.height > f.bgra.size()||f.orientation<1||f.orientation>8||
            !f.pixelAspectNumerator||!f.pixelAspectDenominator)
            throw std::runtime_error("Invalid or oversized native background frame");
        bitmapOrientation=f.orientation;
        bitmapPixelAspect=static_cast<double>(f.pixelAspectNumerator)/f.pixelAspectDenominator;
        if (!bitmap || bitmap->GetPixelSize().width != f.width || bitmap->GetPixelSize().height != f.height) {
            bitmap.Reset(); coverReady = false;
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            Check(target->CreateBitmap(D2D1::SizeU(f.width, f.height), f.bgra.data(), f.stride, &props, &bitmap));
            bitmapSequence = f.sequence;
        } else if (f.sequence != bitmapSequence) {
            Check(bitmap->CopyFromMemory(nullptr, f.bgra.data(), f.stride)); bitmapSequence = f.sequence;
        }
        if (!cover) return;
        if (coverReady && coverSequence == f.sequence) return;
        if (!coverScale) {
            Check(target->CreateEffect(CLSID_D2D1Scale, &coverScale));
            Check(target->CreateEffect(CLSID_D2D1GaussianBlur, &coverBlur));
            Check(coverScale->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_LINEAR));
            Check(coverScale->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD));
            Check(coverBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD));
            Check(coverBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED));
            // Deliberately blur album lettering into atmosphere. A 512-edge cache
            // bounds GPU memory, even for an extreme aspect ratio source.
            Check(coverBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 24.f));
            coverBlur->SetInputEffect(0, coverScale.Get());
        }
        const float ratio = 512.f / std::max(f.width, f.height);
        const UINT cw = std::max(1u, (UINT)std::lround(f.width * ratio));
        const UINT ch = std::max(1u, (UINT)std::lround(f.height * ratio));
        coverScale->SetInput(0, bitmap.Get(), TRUE);
        Check(coverScale->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(float(cw) / f.width, float(ch) / f.height)));
        if (!coverBitmap || coverBitmap->GetPixelSize().width != cw || coverBitmap->GetPixelSize().height != ch) {
            coverBitmap.Reset();
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            Check(target->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, &props, &coverBitmap));
        }
        // All effect/cache coordinates are explicit 96-DPI DIPs. Do this pass
        // before BeginDraw for the main scene, then restore its target and DPI.
        target->SetTarget(coverBitmap.Get()); target->SetDpi(96, 96);
        target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0, 0, 0));
        const auto crop = D2D1::RectF(0, 0, (float)cw, (float)ch);
        target->DrawImage(coverBlur.Get(), D2D1::Point2F(0, 0), crop, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
        const HRESULT result = target->EndDraw();
        target->SetTarget(backBuffer.Get()); target->SetDpi(96 * scale, 96 * scale); Check(result);
        double red = 0, green = 0, blue = 0, weights = 0;
        for (UINT y = 0; y < f.height; y += std::max(1u, f.height / 16))
            for (UINT x = 0; x < f.width; x += std::max(1u, f.width / 16)) {
                const auto* pixel = &f.bgra[(size_t)y * f.stride + (size_t)x * 4];
                const float r = pixel[2] / 255.f, greenSample = pixel[1] / 255.f, b = pixel[0] / 255.f;
                const float saturation = std::max({r, greenSample, b}) - std::min({r, greenSample, b});
                const float weight = pixel[3] / 255.f * (.05f + saturation);
                red += r * weight; green += greenSample * weight; blue += b * weight; weights += weight;
            }
        if (weights > .0001) coverColor = D2D1::ColorF((float)(red / weights), (float)(green / weights), (float)(blue / weights));
        coverSequence = f.sequence; coverReady = true;
    }
    // The 2D media state belongs to the image/video path. Leaving it for the
    // GPU stage drops every retained bitmap so a later return can never flash
    // media that belonged to an earlier song or source.
    void ResetMediaState() {
        if(coverScale)coverScale->SetInput(0,nullptr);
        bitmap.Reset();coverBitmap.Reset();previousBackground.Reset();
        bitmapSequence=coverSequence=0;coverReady=false;awaitingFrame=false;
        sourceRevision=UINT64_MAX;sourceCover=false;transitionStartedAt=0;previousCoverWeight=0;
    }
    void UpdateThumbnail(const std::vector<uint8_t>& rgba,uint32_t edge) {
        thumbnailBrush.Reset();thumbnail.Reset();thumbnailEdge=0;
        if(!edge||rgba.size()!=size_t(edge)*edge*4)return;
        // Swizzle to BGRA; opaque, so straight and premultiplied agree.
        std::vector<uint8_t> bgra(rgba.size());
        for(size_t i=0;i+3<rgba.size();i+=4){bgra[i]=rgba[i+2];bgra[i+1]=rgba[i+1];bgra[i+2]=rgba[i];bgra[i+3]=255;}
        const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        Check(target->CreateBitmap(D2D1::SizeU(edge,edge),bgra.data(),edge*4,&props,&thumbnail));
        Check(target->CreateBitmapBrush(thumbnail.Get(),&thumbnailBrush));
        thumbnailBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        thumbnailEdge=edge;
    }
    // GPU stage cover follows the decoded cover frame only; the thumbnail and
    // palette are derived from the same pixels and never from song metadata.
    void UpdateStageCover(const BackgroundSnapshot& background,int64_t now) {
        const auto& frame=background.frame;
        if(frame){
            if(frame.get()==stageCoverFrame&&frame->sequence==stageCoverSequence)return;
            stageCoverFrame=frame.get();stageCoverSequence=frame->sequence;
            uint32_t edge=0;
            auto rgba=UprightCoverSquare(frame->bgra.data(),frame->bgra.size(),frame->width,frame->height,frame->stride,frame->orientation,256,edge);
            if(rgba.empty()){coverPalette={};thumbnailBrush.Reset();thumbnail.Reset();thumbnailEdge=0;if(gpu)gpu->ClearCover(now);return;}
            coverPalette=AnalyzeCoverPalette(rgba.data(),edge);
            UpdateThumbnail(rgba,edge);
            if(gpu)gpu->SetCover(std::move(rgba),edge,now);
            return;
        }
        // A replacement decode of the same selection keeps the visible cover
        // until its pixels arrive. A song change clears the source first
        // (kind None), so an earlier song's cover is never carried forward.
        if(background.loading&&background.kind!=BackgroundKind::None){stageCoverFrame=nullptr;stageCoverSequence=0;return;}
        if(stageCoverFrame||thumbnail||coverPalette.valid){
            stageCoverFrame=nullptr;stageCoverSequence=0;coverPalette={};
            thumbnailBrush.Reset();thumbnail.Reset();thumbnailEdge=0;
            if(gpu)gpu->ClearCover(now);
        }
    }
    void Fill(D2D1_RECT_F rect, D2D1_COLOR_F color) { brush->SetColor(color); target->FillRectangle(rect, brush.Get()); }
    void CoverFill(ID2D1Bitmap* image, float w, float h, float zoom = 1,float opacity=1,uint16_t orientation=1,double pixelAspect=1.0) {
        if(!image||opacity<=0||!std::isfinite(pixelAspect)||pixelAspect<=0||!std::isfinite(zoom)||zoom<=0)return;
        const auto size = image->GetSize();
        if(size.width<=0||size.height<=0||w<=0||h<=0)return;
        double sw = size.width, sh = size.height;
        const bool quarterTurn=orientation>=5;
        // Convert the pre-orientation target shape into source-pixel units.
        // Mapping that crop to the target then preserves the physical PAR.
        const double aspect=(quarterTurn?double{h}/std::max(1.f,w):double{w}/std::max(1.f,h))/pixelAspect;
        if(!std::isfinite(aspect)||aspect<=0)return;
        if (sw / sh > aspect) sw = sh * aspect; else sh = sw / aspect;
        sw /= zoom; sh /= zoom;
        const auto source = D2D1::RectF(static_cast<float>((size.width-sw)*.5),static_cast<float>((size.height-sh)*.5),
            static_cast<float>((size.width+sw)*.5),static_cast<float>((size.height+sh)*.5));
        if(!std::isfinite(source.left)||!std::isfinite(source.top)||!std::isfinite(source.right)||!std::isfinite(source.bottom)||
            source.right<=source.left||source.bottom<=source.top)return;
        const auto center=D2D1::Point2F(w*.5f,h*.5f);
        float rotation=0,flipX=1,flipY=1;
        switch(orientation){
            case 2:flipX=-1;break;case 3:rotation=180;break;case 4:flipY=-1;break;
            case 5:flipX=-1;rotation=270;break;case 6:rotation=90;break;
            case 7:flipX=-1;rotation=90;break;case 8:rotation=270;break;default:break;
        }
        D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(flipX,flipY,center)*D2D1::Matrix3x2F::Rotation(rotation,center)*previous);
        const auto destination=quarterTurn?D2D1::RectF((w-h)*.5f,(h-w)*.5f,(w+h)*.5f,(h+w)*.5f):D2D1::RectF(0,0,w,h);
        target->DrawBitmap(image,destination,std::clamp(opacity,0.f,1.f),D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,&source);
        target->SetTransform(previous);
    }
    ID2D1RadialGradientBrush* GlowBrush(int slot, D2D1_COLOR_F color, float x, float y, float rx, float ry, float opacity) {
        auto& entry = glows[slot];
        if (!entry.brush || !SameColor(entry.color, color)) {
            const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(color.r, color.g, color.b, 1)}, {1, D2D1::ColorF(color.r, color.g, color.b, 0)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            Check(target->CreateGradientStopCollection(stops, 2, &collection)); entry.brush.Reset();
            Check(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(x, y), D2D1::Point2F(0, 0), rx, ry), collection.Get(), &entry.brush));
            entry.color = color;
        }
        entry.brush->SetCenter(D2D1::Point2F(x, y)); entry.brush->SetRadiusX(rx); entry.brush->SetRadiusY(ry);
        entry.brush->SetOpacity(std::clamp(opacity * color.a, 0.f, 1.f));
        return entry.brush.Get();
    }
    void Glow(int slot, D2D1_COLOR_F color, float x, float y, float rx, float ry, float opacity, float w, float h) {
        target->FillRectangle(D2D1::RectF(0, 0, w, h), GlowBrush(slot,color,x,y,rx,ry,opacity));
    }
    // A soft radial light limited to its own bounds (cheap for small lights).
    void Spot(int slot,D2D1_COLOR_F color,float x,float y,float radius,float opacity) {
        if(radius<=0||opacity<=0)return;
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius),GlowBrush(slot,color,x,y,radius,radius,opacity));
    }
    void MainGlow(const Palette& colors,int style,int64_t now,float x,float y,float rx,float ry,float opacity,float w,float h) {
        const float mix=style==2?.10f:.55f,t=TransitionProgress(now);
        const float prior=previousBackground?previousCoverWeight*(awaitingFrame?WaitingOpacity(now):1.f-t):0.f;
        const float current=sourceCover&&coverReady?t:0.f;
        Glow(0,colors.glow,x,y,rx,ry,opacity*(1.f-mix*std::clamp(prior+current,0.f,1.f)),w,h);
        // Gradient colors remain stable for each source. Fade brush opacity
        // instead of allocating a new gradient for every interpolated RGB.
        if(current>0)Glow(2,coverColor,x,y,rx,ry,opacity*mix*current,w,h);
        if(prior>0)Glow(3,previousCoverColor,x,y,rx,ry,opacity*mix*prior,w,h);
    }
    void Readability(const SceneGeometry& g, const Palette& colors, bool hasImage, int style) {
        if (!hasImage) return;
        D2D1_COLOR_F matte = colors.background;
        const float inkLum = Luminance(colors.ink), matteLum = Luminance(matte);
        const float contrast = (std::max(inkLum, matteLum) + .05f) / (std::min(inkLum, matteLum) + .05f);
        if (contrast < 4.5f) matte = inkLum > .179f ? D2D1::ColorF(0x101317) : D2D1::ColorF(0xFAF6EF);
        const float x = std::max(0.f, g.lyricLeft - 100.f);
        const float right = std::min(g.stageWidth, g.lyricLeft + g.lyricWidth + 100.f);
        const float span = std::max(1.f, right - x);
        const float lead = std::clamp((g.lyricLeft - x) / span, .01f, .25f);
        const float trail = std::clamp((g.lyricLeft + g.lyricWidth - x) / span, lead + .05f, .99f);
        if (!scrim || !SameColor(scrimColor, matte) || scrimLead != lead || scrimTrail != trail) {
            // Opaque enough at the lyric column, feathered into the selected media.
            const D2D1_GRADIENT_STOP stops[] = {
                {0, D2D1::ColorF(matte.r, matte.g, matte.b, 0)},
                {lead, D2D1::ColorF(matte.r, matte.g, matte.b, .86f)},
                {trail, D2D1::ColorF(matte.r, matte.g, matte.b, .86f)},
                {1, D2D1::ColorF(matte.r, matte.g, matte.b, 0)}};
            ComPtr<ID2D1GradientStopCollection> collection; Check(target->CreateGradientStopCollection(stops, 4, &collection));
            scrim.Reset(); Check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)), collection.Get(), &scrim));
            scrimColor = matte; scrimLead = lead; scrimTrail = trail;
        }
        scrim->SetStartPoint(D2D1::Point2F(x, 0));
        scrim->SetEndPoint(D2D1::Point2F(right, 0));
        scrim->SetOpacity(style == 2 ? 1.f : .94f);
        // Keep the readability column continuous through header and footer.
        // Cutting it at text bounds leaves visible horizontal bands whenever
        // cover art or video is brighter than the lyric matte. The existing
        // horizontal gradient still feathers into the surrounding media.
        target->FillRectangle(D2D1::RectF(x, 0, g.stageWidth, g.height), scrim.Get());
    }
    // Direct2D-only scene: selected image/video media, or a device without
    // the GPU stage. Retains the original media transitions and readability.
    void Background(const SceneGeometry& g, const Palette& colors, int style, const SceneView& view) {
        target->Clear(colors.background);
        const auto now=MonotonicMs();
        if (bitmap||previousBackground) {
            const float zoom=CameraZoom();
            MediaLayer(g.stageWidth,g.height,now,zoom,colors.background);
            const float baseTint=DisplayedTint(style,now);
            const float alpha=baseTint+(1.f-baseTint)*motion.backgroundShade;
            Fill(D2D1::RectF(0, 0, g.stageWidth, g.height), D2D1::ColorF(colors.background.r, colors.background.g, colors.background.b, alpha));
        }
        const float response = std::min(.30f, (.7f*motion.low+.3f*motion.rms)*motion.response);
        const float t = (float)std::sin(motion.phase * .052);
        if (style == 0) {
            MainGlow(colors,style,now,g.stageWidth * (.69f + .04f * motion.amplitude * t), g.height * .42f, g.stageWidth * .64f, g.height * .64f, .31f + response * .30f, g.stageWidth, g.height);
            Glow(1, colors.accent, g.stageWidth * (.14f + .03f * motion.amplitude * (float)std::cos(motion.phase * .041)), g.height * .82f, g.stageWidth * .40f, g.height * .42f, .07f + response * .10f, g.stageWidth, g.height);
        } else if (style == 1) {
            MainGlow(colors,style,now,g.stageWidth * (.92f + .012f * motion.amplitude * t), g.height * .32f, g.stageWidth * .48f, g.height * .9f, .38f + response * .22f, g.stageWidth, g.height);
            Glow(1, colors.accent, g.stageWidth * .08f, g.height * .97f, g.stageWidth * .65f, g.height * .32f, .055f + response * .08f, g.stageWidth, g.height);
            Fill(D2D1::RectF(0, 0, g.stageWidth, 18), D2D1::ColorF(0, 0, 0, .17f));
            Fill(D2D1::RectF(0, g.height - 22, g.stageWidth, g.height), D2D1::ColorF(0, 0, 0, .17f));
        } else {
            MainGlow(colors,style,now,g.stageWidth * (.85f + .016f * motion.amplitude * t), g.height * .39f, g.stageWidth * .38f, g.stageWidth * .38f, .45f + response * .10f, g.stageWidth, g.height);
            brush->SetColor(D2D1::ColorF(colors.accent.r, colors.accent.g, colors.accent.b, .09f));
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(g.stageWidth * .94f, g.height * .20f), g.stageWidth * .44f, g.stageWidth * .44f), brush.Get(), .8f);
        }
        if (style != 2) {
            const int count = view.desktop ? 12 : style == 1 ? 28 : 18;
            for (int i = 0; i < count; ++i) {
                const double seed = i * 1.61803;
                const float x = g.stageWidth * (.06f + .88f * (.5f + .5f * (float)std::sin(seed * 2.71 + motion.particlePhase * .009)));
                const float y = g.height * (.08f + .80f * (.5f + .5f * (float)std::cos(seed * 1.31 + motion.particlePhase * .016)));
                const float shimmer=(.5f+.5f*(float)std::sin(seed+motion.phase*.51))*motion.high*motion.response;
                const float opacity = (.020f + response * .11f + shimmer*.07f) * (.35f + .65f * motion.amplitude);
                brush->SetColor(D2D1::ColorF(colors.accent.r, colors.accent.g, colors.accent.b, colors.accent.a * opacity));
                const float radius = (i % 5 ? .9f : 1.7f)+motion.peak*motion.response*motion.amplitude*.18f; target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush.Get());
            }
        }
        Readability(g, colors, bitmap.Get()!=nullptr||previousBackground.Get()!=nullptr, style);
    }
    // Sets up and runs the GPU atmosphere/particles before Direct2D draws.
    void StageGpuLayer(ID3D11RenderTargetView* stageTarget,const SceneGeometry& g,const Palette& colors,int style,const SceneView& view,int64_t now) {
        StageGpuFrame frame;
        frame.seconds=static_cast<float>(std::fmod(motion.phase,7200.0));
        frame.stageWidth=g.stageWidth*scale;frame.height=g.height*scale;frame.dpiScale=scale;
        frame.style=style;frame.low=motion.low;frame.mid=motion.mid;frame.high=motion.high;frame.beat=motion.pulse;frame.rms=motion.rms;
        frame.response=motion.response;frame.camera=motion.cameraMotion;frame.motion=motion.amplitude;frame.shade=motion.backgroundShade;
        const bool light=Luminance(colors.background)>.45f;
        frame.lightBackground=light;
        frame.base=ToRgb(colors.background);frame.accent=ToRgb(colors.accent);
        if(coverPalette.valid)frame.palette=coverPalette.colors;
        else frame.palette={ToRgb(Mix(colors.background,colors.glow,.65f)),ToRgb(colors.glow),ToRgb(Mix(colors.accent,colors.background,.45f)),
            ToRgb(Mix(colors.panel,colors.glow,.5f)),ToRgb(Mix(colors.muted,colors.background,.5f))};
        frame.finish=view.desktop?.7f:1.f;
        const float presence=gpu->CoverPresence(now);
        frame.particles=g.particles&&presence>0.f;
        frame.fieldX=g.fieldX*scale;frame.fieldY=g.fieldY*scale;frame.fieldSize=std::max(1.f,g.fieldSize*scale);
        frame.fieldOpacity=g.fieldOpacity;
        frame.dustOpacity=style==1?(light?0.f:.55f):0.f;
        frame.nowMs=now;
        frame.quality=qualityTier;
        gpu->Render(context.Get(),stageTarget,frame);
    }
    // Presentation-only decorations above the GPU atmosphere.
    void StageDecorations(const SceneGeometry& g,const Palette& colors,int style,const SceneView& view) {
        const float t=static_cast<float>(std::fmod(motion.phase,7200.0));
        const float amplitude=motion.amplitude;
        if(style==0){
            // Slowly rising motes that catch the high band.
            const auto mote=Luminous(Mix(colors.accent,D2D1::ColorF(1,1,1),.35f),.5f);
            const int count=view.desktop?10:22;
            for(int i=0;i<count;++i){
                const float h1=Hash01(i,11),h2=Hash01(i,23),h3=Hash01(i,37);
                const float travel=std::fmod(h1+t*(.006f+.010f*h2)*(.4f+.6f*amplitude),1.f);
                const float x=g.stageWidth*(.04f+.92f*h3)+std::sin(t*.3f+i)*14.f*amplitude;
                const float y=g.height*(1.05f-1.1f*travel);
                const float fade=SmoothStep(travel*6.f)*SmoothStep((1.f-travel)*5.f);
                const float twinkle=.55f+.45f*std::sin(t*(1.1f+h2*1.7f)+i*1.3f);
                const float opacity=(.05f+.16f*twinkle*(.4f+.6f*motion.high*motion.response))*fade;
                const float radius=1.f+1.8f*h2+motion.pulse*.6f*motion.response;
                brush->SetColor(WithAlpha(mote,opacity));
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius),brush.Get());
            }
        } else if(style==2){
            // Original geometric score marks: outlines that drift and turn slowly.
            const bool light=Luminance(colors.background)>.45f;
            D2D1_MATRIX_3X2_F saved;target->GetTransform(&saved);
            const int count=view.desktop?6:13;
            for(int i=0;i<count;++i){
                const float h1=Hash01(i,101),h2=Hash01(i,211),h3=Hash01(i,307),h4=Hash01(i,401),h5=Hash01(i,503);
                const float x=g.stageWidth*(.05f+.90f*h1)+std::sin(t*(.05f+.04f*h2)+i)*28.f*amplitude;
                const float y=g.height*(.06f+.88f*h2)+std::cos(t*(.04f+.03f*h3)+i*2.f)*22.f*amplitude;
                const float size=(14.f+44.f*h3)*(1.f+.06f*motion.pulse*motion.response);
                const float angle=h4*360.f+t*(h5-.5f)*16.f*amplitude;
                const float opacity=light?(.08f+.08f*h5):(.10f+.10f*h5);
                const auto color=WithAlpha(i%4==0?colors.accent:Mix(colors.muted,colors.ink,.25f),opacity);
                target->SetTransform(D2D1::Matrix3x2F::Scale(size,size)*D2D1::Matrix3x2F::Rotation(angle)*D2D1::Matrix3x2F::Translation(x,y)*saved);
                brush->SetColor(color);
                const float stroke=1.2f/size;
                const int kind=static_cast<int>(h4*5.f)%5;
                if(kind==4)target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0,0),1.f,1.f),brush.Get(),stroke);
                else if(shapes[kind]){
                    target->DrawGeometry(shapes[kind].Get(),brush.Get(),stroke);
                    if(i%5==2){brush->SetColor(WithAlpha(color,.35f));target->FillGeometry(shapes[kind].Get(),brush.Get());}
                }
            }
            target->SetTransform(saved);
        }
    }
    const PartialGeometry& WordGeometry(IDWriteTextLayout* layout,UINT32 start,UINT32 length) {
        if(partialGeometry.layout.Get()==layout&&partialGeometry.start==start&&partialGeometry.length==length)return partialGeometry;
        partialGeometry={};partialGeometry.layout=layout;partialGeometry.start=start;partialGeometry.length=length;
        UINT32 count=0;const auto result=layout->HitTestTextRange(start,length,0,0,nullptr,0,&count);
        if(FAILED(result)&&result!=E_NOT_SUFFICIENT_BUFFER)Check(result);
        if(!count||count>8192)return partialGeometry;
        auto& cells=partialGeometry.cells;cells.resize(count);
        Check(layout->HitTestTextRange(start,length,0,0,cells.data(),count,&count));cells.resize(count);
        std::erase_if(cells,[](const auto& c){return !std::isfinite(c.left)||!std::isfinite(c.top)||
            !std::isfinite(c.width)||!std::isfinite(c.height)||c.width<=0||c.height<=0;});
        std::stable_sort(cells.begin(),cells.end(),[](const auto& a,const auto& b){return a.textPosition<b.textPosition;});
        for(const auto& cell:cells)partialGeometry.width+=cell.width;
        return partialGeometry;
    }
    struct WordProgress {
        UINT32 complete=0,partialStart=0,partialLength=0,focusStart=0,focusLength=0;
        float fraction=0,focusFraction=0;
    };
    static WordProgress CurrentWord(const LyricLine& line,int64_t position,bool highlight) {
        WordProgress result;
        if(highlight)for(const auto& word:line.words){
            const auto length=static_cast<UINT32>(word.text.size());
            if(position>=word.startMs+word.durationMs){
                if(length){result.focusStart=result.complete;result.focusLength=length;result.focusFraction=1;}
                result.complete+=length;continue;
            }
            if(position>=word.startMs){
                result.partialStart=result.complete;result.partialLength=length;
                result.fraction=word.durationMs?std::clamp(float(position-word.startMs)/float(word.durationMs),0.f,1.f):1.f;
                if(length){result.focusStart=result.complete;result.focusLength=length;result.focusFraction=result.fraction;}
            }
            break;
        }
        return result;
    }
    float LyricViewportOffset(IDWriteTextLayout* layout,UINT32 length,const WordProgress& progress,
        float fullHeight,float viewportHeight,float previous,bool retained,const SceneView& view,bool precise,float dt) {
        const float maximum=std::max(0.f,fullHeight-viewportHeight);
        previous=std::isfinite(previous)?std::clamp(previous,0.f,maximum):0.f;
        if(view.manualLyricBrowse)return std::isfinite(view.previewScrollOffset)?std::clamp(view.previewScrollOffset,0.f,maximum):0.f;
        if(!precise)return previous; // Unknown clocks keep a static reading position.
        float targetOffset=0;
        std::optional<DWRITE_HIT_TEST_METRICS> focus;
        if(progress.focusLength&&progress.focusStart<length){
            const auto& geometry=WordGeometry(layout,progress.focusStart,std::min(progress.focusLength,length-progress.focusStart));
            float remaining=geometry.width*progress.focusFraction;
            for(const auto& cell:geometry.cells){
                if(cell.isText&&!cell.isTrimmed){focus=cell;if(remaining<=cell.width)break;}
                remaining-=cell.width;
            }
            if(!focus)return previous;
            targetOffset=std::clamp(focus->top+focus->height*.5f-viewportHeight*.5f,0.f,maximum);
        }
        float offset=!retained||view.reducedMotion||std::abs(targetOffset-previous)>viewportHeight*.75f?
            targetOffset:Approach(previous,targetOffset,dt,12.f);
        if(focus){
            // Smooth the surrounding text while keeping the actual current
            // visual fragment inside the viewport. No synthetic word timing.
            const float lower=std::max(0.f,focus->top+focus->height-viewportHeight);
            const float upper=std::min(maximum,focus->top);
            if(lower<=upper)offset=std::clamp(offset,lower,upper);
        }
        return std::clamp(offset,0.f,maximum);
    }
    // Whole-layout fallback when glyph capture is unavailable.
    void ActiveLyric(IDWriteTextLayout* layout, UINT32 layoutLength,float x,float y,
        const WordProgress& progress,const Palette& colors,int style) {
        const UINT32 length=layoutLength,complete=progress.complete,partialStart=progress.partialStart,partialLength=progress.partialLength;
        const float fraction=progress.fraction;
        if(length)Check(layout->SetDrawingEffect(nullptr,{0,length}));
        accentBrush->SetColor(colors.accent);
        if (complete) Check(layout->SetDrawingEffect(accentBrush.Get(), {0, std::min(complete, length)}));
        brush->SetColor(colors.ink); target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        if (partialLength && fraction > 0 && partialStart < length) {
            const auto& geometry=WordGeometry(layout,partialStart,std::min(partialLength,length-partialStart));
            if (!geometry.cells.empty()) {
                float remaining=geometry.width*fraction;
                Check(layout->SetDrawingEffect(accentBrush.Get(), {partialStart, std::min(partialLength, length - partialStart)}));
                for (const auto& cell : geometry.cells) {
                    const float amount = std::min(cell.width, remaining); if (amount <= 0) break;
                    remaining-=amount;
                    if(!cell.isText||cell.isTrimmed)continue;
                    const float left=x+cell.left+((cell.bidiLevel&1)?cell.width-amount:0.f),top=y+cell.top;
                    target->PushAxisAlignedClip(D2D1::RectF(left,top,left+amount,top+cell.height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP); target->PopAxisAlignedClip();
                    if (style == 2) { accentBrush->SetOpacity(.80f); target->DrawLine(D2D1::Point2F(left,top+cell.height-1),D2D1::Point2F(left+amount,top+cell.height-1),accentBrush.Get(),1.2f); accentBrush->SetOpacity(1); }
                }
            }
        }
    }
    // Builds animated glyph items for one row. Timing input is the same
    // (offset-applied) lyric position used for row selection; words with
    // no real timing never receive a synthetic sweep.
    KineticRow BuildKinetic(const std::shared_ptr<const glyphs::Layout>& captured,D2D1_POINT_2F origin,const D2D1_MATRIX_3X2_F& rowTransform,
        const LyricLine& line,size_t displayLength,int64_t position,bool timed,bool following,bool lineLit,float activity,float rowOpacity,
        int style,uint64_t seed,float size,bool entryAnimated,float entryElapsedMs,DWRITE_TEXT_ALIGNMENT alignment,bool reducedMotion) const {
        KineticRow row;
        if(!captured||!captured->valid)return row;
        row.source=captured;
        const auto& runs=captured->runs;
        std::vector<WordSpan> words;
        if(timed){
            UINT32 cursor=0;
            for(const auto& word:line.words){
                WordSpan span;span.start=cursor;
                const size_t length=std::min<size_t>(word.text.size(),displayLength>cursor?displayLength-cursor:0);
                span.end=cursor+static_cast<UINT32>(length);cursor=span.end;
                const int64_t endMs=word.startMs+std::max<int64_t>(0,word.durationMs);
                span.duration=static_cast<float>(std::max<int64_t>(0,word.durationMs));
                if(position>=endMs){span.progress=1.f;span.sinceEnd=static_cast<float>(std::min<int64_t>(position-endMs,600000));}
                else if(position>=word.startMs)span.progress=word.durationMs>0?std::clamp(float(position-word.startMs)/float(word.durationMs),0.f,1.f):1.f;
                words.push_back(span);
            }
        }
        const auto wordAt=[&](UINT32 textPosition)->int{
            if(words.empty())return -1;
            const auto it=std::upper_bound(words.begin(),words.end(),textPosition,[](UINT32 value,const WordSpan& span){return value<span.start;});
            if(it==words.begin())return -1;
            const auto index=static_cast<int>(it-words.begin()-1);
            return textPosition<words[index].end?index:-1;
        };
        // Pass 1: real advance of each timed word and its visual extent.
        for(const auto& run:runs){
            if(run.Rtl())continue;
            for(size_t g=0;g<run.indices.size();++g){
                const int w=wordAt(run.textPositions[g]);if(w<0)continue;
                auto& span=words[w];
                const float left=origin.x+run.origin.x+run.pen[g],baseline=origin.y+run.origin.y;
                span.advance+=run.advances[g];
                span.left=std::min(span.left,left);span.right=std::max(span.right,left+run.advances[g]);
                if(!span.baselineSet){span.baseline=baseline;span.baselineSet=true;}
                else if(std::abs(span.baseline-baseline)>.5f)span.wrapped=true;
            }
        }
        const float motionScale=reducedMotion?.25f:1.f;
        const auto wordGlow=[&](const WordSpan& span){
            if(span.progress<=0.f)return 0.f;
            if(span.progress<1.f)return .55f+.45f*span.progress;
            // Sung words keep only a faint glow so the current word leads.
            return .18f+.82f*std::exp(-std::max(0.f,span.sinceEnd)/520.f);
        };
        const auto wordAccent=[&](const WordSpan& span){
            if(span.progress<=0.f)return 0.f;
            if(span.progress<1.f)return 1.f;
            return std::clamp(1.f-std::max(0.f,span.sinceEnd)/600.f,0.f,1.f);
        };
        const auto glyphFill=[&](const WordSpan& span,float offset,float advance){
            if(span.progress>=1.f)return 1.f;
            if(span.progress<=0.f||advance<=0.f)return 0.f;
            return std::clamp((span.progress*span.advance-offset)/advance,0.f,1.f);
        };
        if(style==2){
            // Kinetic typography: each glyph keeps reading order on its line,
            // with deterministic scale/baseline/rotation; the sung word swells.
            struct Placed { const glyphs::Run* run; size_t g; float x,baseline,advance,scale,dy,rotation,opacity,fill,accent,glow; };
            std::vector<Placed> placed;placed.reserve(64);
            std::vector<size_t> lineStarts;
            float lastBaseline=std::numeric_limits<float>::quiet_NaN();
            size_t ordinal=0;
            for(const auto& run:runs){
                const float baseline=origin.y+run.origin.y;
                if(run.Rtl()){
                    KineticItem item;item.run=&run;item.first=0;item.last=run.indices.size();
                    item.baseline={origin.x+run.origin.x,baseline};item.transform=rowTransform;
                    item.lit=lineLit?1.f:0.f;item.opacity=rowOpacity;row.items.push_back(item);continue;
                }
                for(size_t g=0;g<run.indices.size();++g,++ordinal){
                    if(!(std::abs(baseline-lastBaseline)<=.5f)){lineStarts.push_back(placed.size());lastBaseline=baseline;}
                    const UINT32 textPosition=run.textPositions[g];
                    const float advance=run.advances[g];
                    const float h1=Hash01(seed,textPosition*3ull+1),h2=Hash01(seed,textPosition*3ull+2),h3=Hash01(seed,textPosition*3ull+3);
                    float glyphScale=.94f+.20f*h1,dy=(h2-.5f)*.20f*size,rotation=(h3-.5f)*7.f;
                    float fill=lineLit?1.f:0.f,accent=0.f,glow=0.f;
                    const int w=timed?wordAt(textPosition):-1;
                    if(w>=0){
                        auto& span=words[w];
                        fill=glyphFill(span,span.offset,advance);span.offset+=advance;
                        if(span.progress>0.f&&span.progress<1.f)glyphScale*=1.16f+.05f*std::sin(span.progress*3.14159f);
                        else if(span.progress>=1.f)glyphScale*=1.03f;
                        accent=wordAccent(span)*fill;glow=wordGlow(span)*fill*(span.progress<1.f?1.f:.6f);
                    }else if(timed)fill=0.f;
                    const float a=activity*motionScale;
                    glyphScale=1.f+(glyphScale-1.f)*a;dy*=a;rotation*=a;
                    float opacity=rowOpacity;
                    if(entryAnimated){
                        // Staggered by reading order, capped so long lines land within ~1 s.
                        const float local=std::clamp((entryElapsedMs-std::min(static_cast<float>(ordinal)*26.f,520.f))/420.f,0.f,1.f);
                        const float eased=EaseOutBack(local);
                        dy+=(1.f-eased)*(-.55f*size);rotation+=(1.f-local)*(h3-.5f)*36.f;
                        opacity*=std::clamp(local*1.6f,0.f,1.f);glyphScale*=.85f+.15f*eased;
                    }
                    placed.push_back({&run,g,origin.x+run.origin.x+run.pen[g],baseline,advance,glyphScale,dy,rotation,opacity,fill,accent,glow*activity});
                }
            }
            lineStarts.push_back(placed.size());
            for(size_t l=0;l+1<lineStarts.size();++l){
                const size_t begin=lineStarts[l],end=lineStarts[l+1];if(begin>=end)continue;
                float left=std::numeric_limits<float>::max(),right=-std::numeric_limits<float>::max(),scaled=0;
                for(size_t i=begin;i<end;++i){left=std::min(left,placed[i].x);right=std::max(right,placed[i].x+placed[i].advance);scaled+=placed[i].advance*placed[i].scale;}
                const float newLeft=alignment==DWRITE_TEXT_ALIGNMENT_CENTER?(left+right)*.5f-scaled*.5f:left;
                float pen=newLeft;
                const float ascent=placed[begin].run->ascent;
                for(size_t i=begin;i<end;++i){
                    const auto& p=placed[i];
                    KineticItem item;item.run=p.run;item.first=p.g;item.last=p.g+1;
                    item.baseline={p.x,p.baseline};
                    item.transform=D2D1::Matrix3x2F::Translation(-p.x,-p.baseline)*D2D1::Matrix3x2F::Scale(p.scale,p.scale)*
                        D2D1::Matrix3x2F::Rotation(p.rotation,D2D1::Point2F(p.advance*p.scale*.5f,-ascent*.35f*p.scale))*
                        D2D1::Matrix3x2F::Translation(pen,p.baseline+p.dy)*rowTransform;
                    item.lit=p.fill;item.accent=p.accent;item.glow=p.glow;item.opacity=p.opacity;
                    row.items.push_back(item);pen+=p.advance*p.scale;
                }
                const float under=placed[begin].baseline+placed[begin].run->descent*.55f+2.f;
                if(activity>.02f)row.guides.push_back({{newLeft-10.f,under},{pen+10.f,under},rowTransform,.5f*activity*rowOpacity});
            }
            row.valid=!row.items.empty();
            return row;
        }
        // Flow and cinema: words lift as they are sung; a feathered sweep crosses
        // the current word. Glyphs sharing a state are drawn as one slice.
        const float liftAmplitude=size*(style==0?.050f:.034f)*activity*motionScale;
        for(const auto& run:runs){
            const D2D1_POINT_2F base{origin.x+run.origin.x,origin.y+run.origin.y};
            if(run.Rtl()||!timed){
                KineticItem item;item.run=&run;item.first=0;item.last=run.indices.size();item.baseline=base;
                item.transform=rowTransform;item.lit=lineLit?1.f:0.f;item.opacity=rowOpacity;
                // A following line-timed row glows softly; static previews do not.
                item.glow=lineLit&&following&&!timed?.40f*activity:0.f;
                row.items.push_back(item);continue;
            }
            size_t g=0;const size_t count=run.indices.size();
            while(g<count){
                const int w=wordAt(run.textPositions[g]);
                float lift=0,fill=0,glow=0,accent=0;
                D2D1_MATRIX_3X2_F local=D2D1::Matrix3x2F::Identity();
                if(w>=0){
                    auto& span=words[w];
                    fill=glyphFill(span,span.offset,run.advances[g]);
                    lift=liftAmplitude*EaseOutCubic(std::min(1.f,span.progress*1.8f));
                    glow=wordGlow(span)*activity;accent=wordAccent(span);
                    if(span.duration>=900.f&&span.progress>0.f&&span.progress<1.f&&!span.wrapped){
                        const float swell=1.f+.04f*std::sin(span.progress*3.14159f)*activity*motionScale;
                        local=D2D1::Matrix3x2F::Scale(swell,swell,D2D1::Point2F((span.left+span.right)*.5f,span.baseline));
                    }
                }
                // A slice draws its first glyph at the slice origin, so the
                // baseline advances by the pen offset of that glyph.
                KineticItem item;item.run=&run;item.first=g;item.baseline={base.x+run.pen[g],base.y};
                item.transform=local*D2D1::Matrix3x2F::Translation(0,-lift)*rowTransform;
                item.glow=glow;item.accent=accent;item.opacity=rowOpacity;
                if(fill>0.f&&fill<1.f){
                    item.last=g+1;item.sweep=true;item.sweepX=base.x+run.pen[g]+fill*run.advances[g];item.lit=fill;
                    if(w>=0)words[w].offset+=run.advances[g];
                    row.items.push_back(item);++g;continue;
                }
                item.lit=fill;
                size_t next=g;
                while(next<count){
                    const int nextWord=wordAt(run.textPositions[next]);
                    if(nextWord!=w)break;
                    if(w>=0){
                        auto& span=words[w];
                        const float nextFill=glyphFill(span,span.offset,run.advances[next]);
                        if(nextFill!=fill)break;
                        span.offset+=run.advances[next];
                    }
                    ++next;
                }
                item.last=next;row.items.push_back(item);g=next;
            }
        }
        row.valid=!row.items.empty();
        return row;
    }
    ID2D1LinearGradientBrush* SweepBrush(D2D1_COLOR_F from,D2D1_COLOR_F to) {
        for(auto& entry:sweeps)if(entry.brush&&SameColor(entry.from,from)&&SameColor(entry.to,to))return entry.brush.Get();
        auto& entry=sweeps[sweepNext++%sweeps.size()];
        const D2D1_GRADIENT_STOP stops[]={{0,from},{1,to}};
        ComPtr<ID2D1GradientStopCollection> collection;
        Check(target->CreateGradientStopCollection(stops,2,&collection));
        entry.brush.Reset();
        Check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(1,0)),collection.Get(),&entry.brush));
        entry.from=from;entry.to=to;
        return entry.brush.Get();
    }
    void DrawKineticItems(const KineticRow& row,const StageInk& ink,float feather,bool glowPass) {
        D2D1_MATRIX_3X2_F saved;target->GetTransform(&saved);
        for(const auto& item:row.items){
            if(!item.run||item.last<=item.first)continue;
            if(glowPass&&item.glow<=.01f)continue;
            const auto slice=glyphs::Slice(*item.run,item.first,item.last);
            target->SetTransform(item.transform*saved);
            if(glowPass){
                const float opacity=item.glow*item.opacity;
                if(item.sweep){
                    auto* sweep=SweepBrush(ink.glow,WithAlpha(ink.glow,0.f));
                    sweep->SetStartPoint(D2D1::Point2F(item.sweepX-feather*.5f,0));sweep->SetEndPoint(D2D1::Point2F(item.sweepX+feather*.5f,0));
                    sweep->SetOpacity(std::clamp(opacity,0.f,1.f));
                    target->DrawGlyphRun(item.baseline,&slice,sweep,item.run->measuring);
                }else{
                    brush->SetColor(WithAlpha(ink.glow,opacity*item.lit));
                    target->DrawGlyphRun(item.baseline,&slice,brush.Get(),item.run->measuring);
                }
                continue;
            }
            const auto lit=Mix(ink.lit,ink.accentLit,std::clamp(item.accent,0.f,1.f));
            const float blend=std::clamp(row.activity,0.f,1.f);
            if(item.sweep&&blend>=.98f){
                auto* sweep=SweepBrush(lit,ink.dim);
                sweep->SetStartPoint(D2D1::Point2F(item.sweepX-feather*.5f,0));sweep->SetEndPoint(D2D1::Point2F(item.sweepX+feather*.5f,0));
                sweep->SetOpacity(std::clamp(item.opacity,0.f,1.f));
                target->DrawGlyphRun(item.baseline,&slice,sweep,item.run->measuring);
            }else{
                // While a row gains or loses its role, a partial glyph uses its
                // fill fraction as a solid mix instead of a per-frame gradient.
                auto color=Mix(ink.dim,lit,std::clamp(item.lit,0.f,1.f));
                if(blend<1.f)color=Mix(row.resting,color,blend);
                brush->SetColor(WithAlpha(color,item.opacity));
                target->DrawGlyphRun(item.baseline,&slice,brush.Get(),item.run->measuring);
            }
        }
        if(!glowPass)for(const auto& guide:row.guides){
            target->SetTransform(guide.transform*saved);
            brush->SetColor(WithAlpha(ink.accentLit,guide.opacity));
            target->DrawLine(guide.from,guide.to,brush.Get(),1.f);
            target->DrawLine(D2D1::Point2F(guide.to.x,guide.to.y-9.f),D2D1::Point2F(guide.to.x,guide.to.y+4.f),brush.Get(),1.f);
            target->DrawLine(D2D1::Point2F(guide.from.x,guide.from.y-4.f),D2D1::Point2F(guide.from.x,guide.from.y+4.f),brush.Get(),1.f);
        }
        target->SetTransform(saved);
    }
    void Polygon(std::initializer_list<D2D1_POINT_2F> points,D2D1_COLOR_F color) {
        if(points.size()<3)return;
        ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
        Check(factory->CreatePathGeometry(&path));Check(path->Open(&sink));auto it=points.begin();
        sink->BeginFigure(*it,D2D1_FIGURE_BEGIN_FILLED);for(++it;it!=points.end();++it)sink->AddLine(*it);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);Check(sink->Close());brush->SetColor(color);target->FillGeometry(path.Get(),brush.Get());
    }
    void OriginalArtwork(D2D1_RECT_F r,const SceneView& view,const Palette& colors) {
        const float w=r.right-r.left,h=r.bottom-r.top;if(w<=0||h<=0)return;
        D2D1_MATRIX_3X2_F old;target->GetTransform(&old);
        target->SetTransform(D2D1::Matrix3x2F::Translation(r.left,r.top)*old);
        target->PushAxisAlignedClip(D2D1::RectF(0,0,w,h),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if(!view.coverBackground&&bitmap)CoverFill(bitmap.Get(),w,h,1,1,bitmapOrientation,bitmapPixelAspect);
        else if(thumbnail)CoverFill(thumbnail.Get(),w,h);
        else{
            // Original neutral architecture, used only until real cover/media is ready.
            Fill(D2D1::RectF(0,0,w,h),Mix(colors.ink,colors.panel,.15f));
            Polygon({{w*.18f,0},{w*.48f,0},{w*.48f,h},{w*.18f,h}},colors.panel);
            Polygon({{w*.48f,h*.28f},{w,h*.08f},{w,h*.24f},{w*.48f,h*.48f}},colors.background);
            Polygon({{0,h*.57f},{w,h*.83f},{w,h},{0,h*.74f}},Mix(colors.panel,colors.background,.25f));
        }
        target->PopAxisAlignedClip();target->SetTransform(old);
    }
    void OriginalBackdrop(const SceneGeometry& g,const Palette& colors,int style,const SceneView& view,int64_t now,bool gpuDrawn) {
        const auto o=ComposeOriginal(style,g.stageWidth,g.height);
        if(!gpuDrawn){
            Fill(D2D1::RectF(0,0,g.stageWidth,g.height),colors.background);
            if(style==WeaveStyle){
                // The software/device fallback retains the woven composition.
                for(int i=0;i<150;++i){
                    const float u=(i+.5f)/150.f;D2D1_POINT_2F previous{};
                    for(int j=0;j<=60;++j){
                        const float v=j/60.f,open=.335f*std::exp(-std::pow(std::abs((v-.5f)/.190f),6.f));
                        const auto p=D2D1::Point2F((.5f+(u<.5f?-1.f:1.f)*(open+std::abs(u-.5f)*(1-1.9f*open)))*g.stageWidth,v*g.height);
                        if(j){brush->SetColor(WithAlpha(colors.accent,.28f));target->DrawLine(previous,p,brush.Get(),.7f);}previous=p;
                    }
                }
            }
        }
        if(style==PosterStyle){
            const float enter=originalState.RowEnter(now,view.reducedMotion);
            const float displacement=(1.f-enter)*g.stageWidth*.13f;
            const float seam=g.stageWidth*.53f+displacement;
            const auto white=D2D1::ColorF(0xF6F7F2),black=D2D1::ColorF(0x101112);
            Polygon({{seam,0},{g.stageWidth,0},{g.stageWidth,g.height},{g.stageWidth*.70f,g.height},{g.stageWidth*.64f,g.height*.55f}},white);
            brush->SetColor(colors.accent);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.stageWidth*.77f,g.height*.68f),g.height*(.23f+.006f*motion.pulse),g.height*(.23f+.006f*motion.pulse)),brush.Get());
            OriginalArtwork(D2D1::RectF(o.coverX,o.coverY,o.coverX+o.coverWidth,o.coverY+o.coverHeight),view,colors);
            Fill(D2D1::RectF(g.stageWidth*.944f,0,g.stageWidth,g.height),black);
            Polygon({{g.stageWidth*.52f,g.height},{g.stageWidth*.944f,g.height*.78f},{g.stageWidth*.944f,g.height}},black);
            brush->SetColor(colors.accent);target->DrawLine(D2D1::Point2F(seam,0),D2D1::Point2F(g.stageWidth*.72f,g.height*.52f),brush.Get(),1.f);
            if(!g.compact){
                const float hover=originalFrame.pointerPresence*std::clamp(1.f-std::abs(originalFrame.pointerX-.53f)*12.f,0.f,1.f);
                const float flap=24.f+14.f*hover;
                Polygon({{seam-2.f,0},{seam-2.f-flap,0},{seam-2.f,flap}},colors.accent);
            }
        }
        if(style==WeaveStyle&&!view.coverBackground&&bitmap){
            // Custom media gently inhabits the cloth; it never replaces the threads.
            CoverFill(bitmap.Get(),g.stageWidth,g.height,1,.10f,bitmapOrientation,bitmapPixelAspect);
        }
    }
    void OriginalChrome(const SceneGeometry& g,const Palette& colors,int style,const SceneView& view,
        const std::wstring& title,const std::wstring& artist,int64_t now) {
        const auto o=ComposeOriginal(style,g.stageWidth,g.height);const float alpha=motion.chromeVisibility;
        if(style==WeaveStyle){
            OriginalArtwork(D2D1::RectF(o.coverX,o.coverY,o.coverX+o.coverWidth,o.coverY+o.coverHeight),view,colors);
            Text(title,o.coverX+o.coverWidth+14.f,o.coverY,std::max(60.f,g.stageWidth*.48f),28.f,g.compact?14.f:20.f,WithAlpha(colors.ink,alpha),DWRITE_TEXT_ALIGNMENT_LEADING,FontChromeStrong);
            Text(artist,o.coverX+o.coverWidth+14.f,o.coverY+30.f,g.stageWidth*.45f,20.f,12.f,WithAlpha(colors.muted,alpha));
        }else if(style==PosterStyle){
            Text(L"HTune",o.margin,24.f,g.stageWidth*.42f,42.f,g.compact?20.f:30.f,WithAlpha(colors.ink,alpha),DWRITE_TEXT_ALIGNMENT_LEADING,FontChromeStrong);
            if(!g.compact){
                Text(title,g.stageWidth*.57f,g.height*.13f,g.stageWidth*.165f,54.f,24.f,D2D1::ColorF(0x151515),DWRITE_TEXT_ALIGNMENT_CENTER,FontChromeStrong);
                Text(artist,g.stageWidth*.57f,g.height*.13f+60.f,g.stageWidth*.165f,36.f,12.f,D2D1::ColorF(0x444444),DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }else{
            Text(L"M U S I C S T A G E",o.margin+12.f,26.f,g.stageWidth*.51f,38.f,g.compact?14.f:22.f,WithAlpha(colors.ink,alpha),DWRITE_TEXT_ALIGNMENT_LEADING,FontLyricCinema);
            OriginalArtwork(D2D1::RectF(o.coverX,o.coverY,o.coverX+o.coverWidth,o.coverY+o.coverHeight),view,colors);
            Text(title,std::max(o.margin,o.coverX-240.f),o.coverY+12.f,std::min(220.f,o.coverX-o.margin),30.f,18.f,WithAlpha(colors.ink,alpha),DWRITE_TEXT_ALIGNMENT_TRAILING,FontLyricCinema);
        }
        if(!view.coverBackground&&view.backgroundShade>0)Fill(D2D1::RectF(0,0,g.stageWidth,g.height),WithAlpha(colors.background,view.backgroundShade*.35f));
        (void)now;
    }
    void OriginalLyrics(const SceneInput& input,const SceneView& view,const SceneGeometry& g,const Palette& colors,int style,
        bool synced,bool precise,int64_t position,float dt,int64_t now,std::optional<PresentedLyricPosition>& selectedLine,LyricViewportState& nextViewport) {
        if(!input.lyrics||input.lyrics->lines.empty())return;
        const auto& lines=input.lyrics->lines;
        const size_t index=synced?LyricLineAt(lines,position):std::min(view.previewIndex,lines.size()-1);
        const auto& line=lines[index];const auto revision=input.lyricReadingRevision?input.lyricReadingRevision:input.lyricRevision;
        originalState.SelectLine(index,revision,now,synced);
        const bool retained=lastStyle==style&&lastLyricReadingRevision==revision&&lyricViewport.initialized&&lyricViewport.first==index;
        lastStyle=style;lastTimeline=input.lyrics;lastLyricRevision=input.lyricRevision;lastLyricReadingRevision=revision;
        std::wstring display=line.text.empty()?L"· · ·":line.text;
        auto progress=CurrentWord(line,position,synced&&precise);
        bool posterPair=false;
        // Short CJK poster lines receive a deliberate two-line composition.
        // Offset all timing ranges across the inserted visual line break.
        if(style==PosterStyle&&display.size()>=6&&display.size()<=16&&
           std::all_of(display.begin(),display.end(),[](wchar_t c){return c>=0x3400&&c<=0x9fff;})){
            posterPair=true;
            const auto split=static_cast<UINT32>(display.size()/2);display.insert(split,1,L'\n');
            const auto remap=[split](UINT32& start,UINT32& length){if(start>=split)++start;else if(start+length>split)++length;};
            if(progress.complete>=split)++progress.complete;
            remap(progress.partialStart,progress.partialLength);remap(progress.focusStart,progress.focusLength);
        }
        const float factor=std::clamp(view.lyricSize/44.f,.59f,1.82f);
        // A wrapped paragraph must remain inside the blue plane at its top;
        // short two-line headlines may use the wider blue area further down.
        const float textWidth=style==PosterStyle&&!posterPair?
            std::max(40.f,std::min(g.lyricWidth,g.stageWidth*.52f-g.lyricLeft)):g.lyricWidth;
        const float ideal=std::clamp(g.stageWidth*(style==PosterStyle?.145f:style==WeaveStyle?.063f:.039f)*factor,26.f,style==PosterStyle?300.f:140.f);
        float size=ideal;
        const int font=LyricFont(style);
        const bool showNext=view.lyricDisplayMode==2||(style!=PosterStyle&&view.lyricDisplayMode!=1);
        const float nextSpace=showNext?std::clamp(ideal*.61f,26.f,80.f):0.f;
        const float auxiliarySpace=(view.showTranslation&&!line.translation.empty()?32.f:0.f)+(view.showRomanization&&!line.romanization.empty()?26.f:0.f);
        const float available=std::max(30.f,g.lyricBottom-g.lyricTop-nextSpace-auxiliarySpace);
        ComPtr<IDWriteTextLayout> layout;float measured=0;
        for(int fit=0;fit<25;++fit){
            auto& entry=Layout(display,textWidth,available,size,font,g.alignment,false,style!=PosterStyle);
            layout=entry.layout;measured=entry.measuredHeight;
            if(measured<=available||size<=26.f)break;
            size=std::max(26.f,size*.90f);
        }
        const float previous=retained?lyricViewport.offsets[0]:view.previewScrollOffset;
        const float offset=LyricViewportOffset(layout.Get(),static_cast<UINT32>(display.size()),progress,measured,available,previous,retained,view,precise,dt);
        nextViewport.initialized=true;nextViewport.first=index;nextViewport.offsets[0]=offset;
        const float maximum=std::max(0.f,measured-available);
        selectedLine=PresentedLyricPosition{index,offset,maximum,std::max(16.f,size*.72f),std::max(24.f,available*.75f)};
        const float enter=originalState.RowEnter(now,view.reducedMotion);
        const float shift=style==PosterStyle?(1.f-enter)*24.f:0.f;
        auto textColors=colors;textColors.ink.a*=.50f+.50f*enter;textColors.accent.a*=.50f+.50f*enter;
        target->PushAxisAlignedClip(D2D1::RectF(g.lyricLeft-2.f,g.lyricTop,g.lyricLeft+textWidth+2.f,g.lyricTop+available),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ActiveLyric(layout.Get(),static_cast<UINT32>(display.size()),g.lyricLeft,g.lyricTop-offset+shift,progress,textColors,style);
        target->PopAxisAlignedClip();
        float y=g.lyricTop+std::min(measured,available)+10.f;
        if(view.showTranslation&&!line.translation.empty()){Text(line.translation,g.lyricLeft,y,g.lyricWidth,30.f,std::clamp(size*.27f,12.f,24.f),colors.muted,g.alignment,FontSecondary);y+=32.f;}
        if(view.showRomanization&&!line.romanization.empty()){Text(line.romanization,g.lyricLeft,y,g.lyricWidth,24.f,std::clamp(size*.22f,11.f,19.f),colors.muted,g.alignment,FontRoman);y+=26.f;}
        if(showNext&&index+1<lines.size()){
            const float nextSize=std::clamp(size*(style==PosterStyle?.23f:.52f),15.f,52.f);
            if(y+nextSize<g.railY-22.f)Text(lines[index+1].text,g.lyricLeft,y,g.lyricWidth,std::min(nextSize*2.f,g.railY-y-22.f),nextSize,WithAlpha(colors.muted,.85f),g.alignment,font);
        }
        if(style==PosterStyle&&index>0&&g.lyricTop>100.f){
            Text(lines[index-1].text,g.lyricLeft,g.lyricTop-66.f,g.lyricWidth,40.f,std::clamp(size*.24f,16.f,34.f),WithAlpha(colors.ink,.82f),DWRITE_TEXT_ALIGNMENT_LEADING,FontChrome);
        }
        if(style==EchoStyle&&originalFrame.historyEnd>originalFrame.historyBegin){
            const float span=originalFrame.historyEnd-originalFrame.historyBegin;
            const size_t start=index>3?index-3:0;
            for(size_t j=start;j<index;++j){
                const float time=static_cast<float>(WithLyricOffset(lines[j].startMs,-view.offsetMs));
                const float t=(time-originalFrame.historyBegin)/span;if(t<.02f||t>.92f)continue;
                const float extent=originalFrame.historyExtent;
                const float x=(1-extent+t*extent)*g.stageWidth*.705f,y0=g.height*(.46f+.025f*std::sin(t*10.f));
                brush->SetColor(WithAlpha(colors.muted,.45f));target->DrawLine(D2D1::Point2F(x,y0),D2D1::Point2F(x,y0+30.f),brush.Get(),1.f);
                Text(lines[j].text,x+10.f,y0-4.f,g.stageWidth*.16f,24.f,13.f,colors.muted,DWRITE_TEXT_ALIGNMENT_LEADING,FontLyricCinema);
            }
        }
    }
    void Lyrics(const SceneInput& input,const SceneView& view,const SceneGeometry& g,const Palette& colors,const StageInk& ink,int style,bool synced,bool precise,int64_t lyricPosition,float dt,int64_t now,std::optional<PresentedLyricPosition>& selectedLine,LyricViewportState& nextViewport) {
        static const std::vector<LyricLine> empty;
        const auto& lines = input.lyrics ? input.lyrics->lines : empty;
        if(IsOriginalStyle(style)&&!lines.empty()){
            OriginalLyrics(input,view,g,colors,style,synced,precise,lyricPosition,dt,now,selectedLine,nextViewport);return;
        }
        const int lyricFont=LyricFont(style);
        if (lines.empty()) {
            partialGeometry={};lastTimeline.reset();lastLyricRevision=0;lastLyricReadingRevision=0;
            motion.scrollInitialized=false;rolesInitialized=false;motion.anchorInitialized=false;lyricViewport.initialized=false;
            entryIndex=SIZE_MAX;entrySince=0;
            const auto state=input.lyrics?input.lyrics->state:LyricLoadState::Idle;
            const auto heading=state==LyricLoadState::Failed?L"歌词暂时未能取得":
                state==LyricLoadState::NoLyrics?L"暂未找到可用歌词":
                state==LyricLoadState::AwaitingChoice?L"选择一个歌词版本":
                state==LyricLoadState::Loading?L"正在加载歌词":L"让音乐开始";
            const float top=g.compact?g.lyricTop:g.anchor-24.f;
            Text(heading,g.lyricLeft,top,g.lyricWidth,g.compact?36.f:80.f,g.compact?20.f:34.f,WithAlpha(colors.ink,.72f),g.alignment,lyricFont);
            Text(input.lyricStatus,g.lyricLeft,top+(g.compact?36.f:58.f),g.lyricWidth,g.compact?std::max(18.f,g.lyricBottom-top-36.f):80.f,g.compact?11.f:14.f,colors.muted,g.alignment);return;
        }
        size_t selected = std::min(view.previewIndex, lines.size() - 1);
        if (synced) selected=LyricLineAt(lines,lyricPosition);
        const bool timelineChanged = input.lyricRevision ? input.lyricRevision != lastLyricRevision : input.lyrics != lastTimeline;
        const auto readingRevision=input.lyricReadingRevision?input.lyricReadingRevision:input.lyricRevision;
        const bool readingChanged=readingRevision?readingRevision!=lastLyricReadingRevision:input.lyrics!=lastTimeline;
        if (readingChanged || style != lastStyle) {
            lastStyle = style; motion.scrollInitialized = false; rolesInitialized = false;motion.anchorInitialized=false;partialGeometry={};lyricViewport.initialized=false;
            entryIndex=SIZE_MAX;entrySince=0;
        }else if(timelineChanged)partialGeometry={};
        // Request cancellation/enrichment is not new lyric content. Keep its
        // scroll and row roles; the current clock/static selection can change
        // without changing presentation ownership.
        // Supplemental text reshapes its own layout and clamps retained offsets
        // below, without restarting an otherwise unchanged original reading grid.
        lastTimeline=input.lyrics;lastLyricRevision = input.lyricRevision;lastLyricReadingRevision=readingRevision;
        // Typography entrance: presentation clock only, started when a row
        // becomes current under a usable timeline. Static rows land at once.
        if(!synced){entryIndex=SIZE_MAX;entrySince=0;}
        else if(selected!=entryIndex){entryIndex=selected;entrySince=now;}
        if (!motion.scrollInitialized || std::abs((float)selected - motion.scroll) > 3.5f) { motion.scroll = (float)selected; motion.scrollInitialized = true; }
        const float rate=10.f;
        motion.scroll = view.reducedMotion?(float)selected:Approach(motion.scroll,(float)selected,dt,rate);
        const size_t begin = selected > 4 ? selected - 4 : 0, end = std::min(lines.size(), selected + 5), count = end - begin;
        std::array<float, 9> nextRoles{};
        std::array<float,9> nextUpcoming{};
        for (size_t n = 0; n < count; ++n) {
            const size_t index = begin + n;
            const float oldRole = rolesInitialized && index >= roleFirst && index < roleFirst + rowRoles.size() ? rowRoles[index - roleFirst] : (index == selected ? 1.f : 0.f);
            const float roleTarget=index==selected?1.f:0.f;
            nextRoles[n]=view.reducedMotion?roleTarget:Approach(oldRole,roleTarget,dt,10.f);
            const float oldUpcoming=rolesInitialized&&index>=roleFirst&&index<roleFirst+upcomingRoles.size()?upcomingRoles[index-roleFirst]:(index==selected+1?1.f:0.f);
            const float upcomingTarget=index==selected+1?1.f:0.f;
            nextUpcoming[n]=view.reducedMotion?upcomingTarget:Approach(oldUpcoming,upcomingTarget,dt,10.f);
        }
        rowRoles = nextRoles;upcomingRoles=nextUpcoming;roleFirst = begin; rolesInitialized = true;
        const float size=std::clamp(view.lyricSize*(view.fullscreen?1.10f:1.f),26.f,std::max(26.f,std::min(g.compact?32.f:88.f,g.lyricWidth*.15f)));
        const float translationSize=std::clamp(size*.38f,13.f,28.f);
        const float romanizationSize=std::clamp(size*.31f,12.f,24.f);
        const float secondaryGap=std::max(5.f,size*.13f);
        const float availableHeight=std::max(1.f,g.lyricBottom-g.lyricTop);
        const float maxHeight=std::min(availableHeight,std::max(size*1.5f,availableHeight*(g.compact?.95f:.72f)));
        std::array<ComPtr<IDWriteTextLayout>, 9> layouts;
        std::array<ComPtr<IDWriteTextLayout>, 9> translations,romanizations;
        std::array<float, 9> heights{}, scales{}, relative{},visibility{};
        std::array<float, 9> mainHeights{},translationHeights{},romanizationHeights{};
        std::array<float,9> fullHeights{},viewportHeights{},nextViewportOffsets{};
        std::array<UINT32, 9> layoutLengths{};
        const auto wordProgress=CurrentWord(lines[selected],lyricPosition,synced&&precise&&!lines[selected].text.empty());
        for (size_t n = 0; n < count; ++n) {
            // A timed empty LRC line represents an instrumental gap. Keep its
            // timing and show a restrained marker instead of rendering an
            // apparently broken blank row or applying an effect to range 0.
            const auto display = lines[begin + n].text.empty() ? std::wstring(L"· · ·") : lines[begin + n].text;
            const auto& line=lines[begin+n];
            if(!line.translation.empty()){
                auto& secondary=Layout(line.translation,g.lyricWidth,translationSize*3.2f,translationSize,
                    FontSecondary,g.alignment,false);
                translations[n]=secondary.layout;translationHeights[n]=secondary.measuredHeight;
            }
            if(!line.romanization.empty()){
                auto& secondary=Layout(line.romanization,g.lyricWidth,romanizationSize*3.2f,romanizationSize,
                    FontRoman,g.alignment,false);
                romanizations[n]=secondary.layout;romanizationHeights[n]=secondary.measuredHeight;
            }
            // Shape the entire text; the group viewport, not a trimming sign
            // or an intrinsic layout clip, limits what can appear on screen.
            const float rowBudget=view.lyricDisplayMode==2?std::min(availableHeight,std::max(size*1.5f,availableHeight*.43f)):maxHeight;
            auto& entry = Layout(display,g.lyricWidth,rowBudget,size,lyricFont,g.alignment,false,true);
            layouts[n]=entry.layout;mainHeights[n]=entry.measuredHeight;
            scales[n]=.68f+.32f*rowRoles[n]+.12f*motion.pairVisibility*(1.f-rowRoles[n]);
            visibility[n]=(1.f-motion.focusVisibility+motion.focusVisibility*rowRoles[n])*
                (1.f-motion.pairVisibility+motion.pairVisibility*std::max(rowRoles[n],upcomingRoles[n]));
            const float translationSpace=translations[n]?(translationHeights[n]+secondaryGap)*motion.translationVisibility:0;
            const float romanizationSpace=romanizations[n]?(romanizationHeights[n]+secondaryGap)*motion.romanizationVisibility:0;
            fullHeights[n]=mainHeights[n]+translationSpace+romanizationSpace;
            if(!std::isfinite(fullHeights[n]))Check(E_FAIL);
            viewportHeights[n]=std::min(fullHeights[n],rowBudget);
            heights[n]=viewportHeights[n]*scales[n];
            layoutLengths[n] = static_cast<UINT32>(display.size());
            const auto index=begin+n;
            const bool retained=lyricViewport.initialized&&index>=lyricViewport.first&&index<lyricViewport.first+lyricViewport.offsets.size();
            float previous=retained?lyricViewport.offsets[index-lyricViewport.first]:0.f;
            if(!retained&&index==selected&&selected==view.previewIndex&&!precise)previous=view.previewScrollOffset;
            const float maximum=std::max(0.f,fullHeights[n]-viewportHeights[n]);
            nextViewportOffsets[n]=index==selected?LyricViewportOffset(layouts[n].Get(),layoutLengths[n],wordProgress,
                fullHeights[n],viewportHeights[n],previous,retained,view,synced&&precise,dt):
                std::isfinite(previous)?std::clamp(previous,0.f,maximum):0.f;
        }
        const size_t pivot = selected - begin;
        const float scrollStep=std::max(16.f,size*1.35f);
        selectedLine=PresentedLyricPosition{selected,nextViewportOffsets[pivot],
            std::max(0.f,fullHeights[pivot]-viewportHeights[pivot]),scrollStep,std::max(scrollStep,viewportHeights[pivot]*.85f)};
        nextViewport.offsets=nextViewportOffsets;nextViewport.first=begin;nextViewport.initialized=true;
        const float gap = style == 2 ? std::max(18.f, size * .45f) : std::max(20.f, size * .50f);
        relative[pivot] = 0;
        for (size_t n = pivot; n > 0; --n) relative[n - 1] = relative[n] - (heights[n] + heights[n - 1]) * .5f - gap;
        for (size_t n = pivot + 1; n < count; ++n) relative[n] = relative[n - 1] + (heights[n - 1] + heights[n]) * .5f + gap;
        const float localScroll = std::clamp(motion.scroll - (float)begin, 0.f, (float)(count - 1));
        const size_t lower = (size_t)localScroll, upper = std::min(count - 1, lower + 1);
        const float offset = -(relative[lower] + (relative[upper] - relative[lower]) * (localScroll - (float)lower));
        const float centerAnchor=(g.lyricTop+g.lyricBottom)*.5f;
        const float pairedAnchor=pivot+1<count?centerAnchor-(heights[pivot+1]+gap)*.5f:centerAnchor;
        const float desiredAnchor=g.anchor+(centerAnchor-g.anchor)*motion.focusVisibility+(pairedAnchor-g.anchor)*motion.pairVisibility;
        // A group can fit inside the lyric area yet overhang it around a style
        // anchor. Bound the resting target using its scaled viewport, not the
        // full text extent; retain the existing row/anchor transition motion.
        const float selectedHalfHeight=std::min(availableHeight,heights[pivot])*.5f;
        const float anchorTarget=std::clamp(desiredAnchor,g.lyricTop+selectedHalfHeight,g.lyricBottom-selectedHalfHeight);
        if(!motion.anchorInitialized){motion.lyricAnchor=anchorTarget;motion.anchorInitialized=true;}
        else motion.lyricAnchor=view.reducedMotion?anchorTarget:Approach(motion.lyricAnchor,anchorTarget,dt,rate);
        // Row placement shared by the glow recording and the visible pass.
        struct RowPlacement { bool visible=false; float center=0,viewportTop=0,unscaledHeight=0,y=0,textX=0,opacity=0; D2D1_MATRIX_3X2_F transform=D2D1::Matrix3x2F::Identity(); };
        std::array<RowPlacement,9> placement{};
        const float anchorX = g.alignment == DWRITE_TEXT_ALIGNMENT_CENTER ? g.lyricLeft + g.lyricWidth * .5f : g.lyricLeft;
        for (size_t n = 0; n < count; ++n) {
            auto& row=placement[n];
            row.center = motion.lyricAnchor + relative[n] + offset;
            if (row.center + heights[n] * .5f < g.lyricTop || row.center - heights[n] * .5f > g.lyricBottom) continue;
            row.visible=true;
            row.unscaledHeight = heights[n] / scales[n];row.viewportTop=row.center-row.unscaledHeight*.5f;
            row.y=row.viewportTop-nextViewportOffsets[n];
            // Context rows finish fading before their own edge reaches the
            // lyric clip, so no half-cut line shows under a header. The row
            // holding the current role keeps the original, gentler fade.
            const float fade=std::max(1.f,size*.9f);
            const float loose=SmoothStep((row.center+heights[n]*.5f-g.lyricTop)/fade)*
                SmoothStep((g.lyricBottom-row.center+heights[n]*.5f)/fade);
            const float strict=SmoothStep((row.center-heights[n]*.5f-g.lyricTop)/fade)*
                SmoothStep((g.lyricBottom-row.center-heights[n]*.5f)/fade);
            const float edge=strict+(loose-strict)*std::clamp(rowRoles[n],0.f,1.f);
            row.opacity=visibility[n]*edge;
            row.textX=g.lyricLeft+(g.alignment==DWRITE_TEXT_ALIGNMENT_CENTER?(g.lyricWidth-layouts[n]->GetMaxWidth())*.5f:0.f);
            row.transform=D2D1::Matrix3x2F::Scale(scales[n], scales[n], D2D1::Point2F(anchorX, row.center));
        }
        // Rows still carrying the current role animate glyph by glyph.
        std::array<KineticRow,9> kinetic{};
        const bool entryAnimated=style==2&&synced&&!view.reducedMotion&&entrySince>0;
        for(size_t n=0;n<count;++n){
            const auto index=begin+n;
            if(!placement[n].visible||!layoutLengths[n]||(index!=selected&&rowRoles[n]<=.01f))continue;
            const auto& line=lines[index];
            const bool rowTimed=synced&&precise&&!line.text.empty()&&!line.words.empty();
            const float role=std::clamp(rowRoles[n],0.f,1.f);
            kinetic[n]=BuildKinetic(Glyphs(layouts[n]),D2D1::Point2F(placement[n].textX,placement[n].y),placement[n].transform,
                line,layoutLengths[n],lyricPosition,rowTimed,synced,true,role,placement[n].opacity,style,
                (static_cast<uint64_t>(readingRevision)<<20)^index,size,entryAnimated&&index==selected,
                static_cast<float>(std::clamp<int64_t>(now-entrySince,0,600000)),g.alignment,view.reducedMotion);
            // Same color the plain context row would use at this role.
            const float resting=index<selected?.44f:.62f;
            kinetic[n].activity=role;
            kinetic[n].resting=WithAlpha(Mix(colors.muted,colors.ink,role),resting+(1.f-resting)*role);
        }
        const float feather=std::max(6.f,size*.35f);
        // Glow is recorded before any clip is pushed: the context target may
        // only switch to a command list with a balanced clip/layer stack.
        ComPtr<ID2D1CommandList> glowList;
        if(glowBlur&&ink.glowStrength>.01f){
            bool any=false;
            for(size_t n=0;n<count&&!any;++n)for(const auto& item:kinetic[n].items)if(item.glow>.01f){any=true;break;}
            if(any){
                Check(target->CreateCommandList(&glowList));
                ComPtr<ID2D1Image> previousTarget;target->GetTarget(&previousTarget);
                target->SetTarget(glowList.Get());
                for(size_t n=0;n<count;++n)if(kinetic[n].valid)DrawKineticItems(kinetic[n],ink,feather,true);
                target->SetTarget(previousTarget.Get());
                Check(glowList->Close());
            }
        }
        target->PushAxisAlignedClip(D2D1::RectF(g.lyricLeft - size*.5f, g.lyricTop, g.lyricLeft + g.lyricWidth + size*.5f, g.lyricBottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if(glowList){
            glowBlur->SetInput(0,glowList.Get());
            Check(glowBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,std::clamp(size*.14f,4.f,16.f)*(qualityTier>=2?.7f:1.f)));
            D2D1_MATRIX_3X2_F saved;target->GetTransform(&saved);target->SetTransform(D2D1::Matrix3x2F::Identity());
            ComPtr<ID2D1Image> glowImage;glowBlur->GetOutput(&glowImage);
            // Additive light on dark stages, soft ink bleed on light paper.
            if(!ink.lightStage){
                target->DrawImage(glowImage.Get(),D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_PLUS);
                if(ink.glowStrength>1.05f)target->DrawImage(glowImage.Get(),D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_PLUS);
            }else target->DrawImage(glowImage.Get(),D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_SOURCE_OVER);
            target->SetTransform(saved);
            // The effect keeps this list as its input until the next frame
            // replaces it, after EndDraw has consumed the pending draw.
        }
        for (size_t n = 0; n < count; ++n) {
            const auto& row=placement[n];
            if(!row.visible)continue;
            auto rowColors=colors;rowColors.ink=Mix(colors.muted,colors.ink,rowRoles[n]);
            rowColors.ink.a*=row.opacity;rowColors.muted.a*=row.opacity;rowColors.accent.a*=row.opacity;
            D2D1_MATRIX_3X2_F old{}; target->GetTransform(&old);
            target->SetTransform(row.transform * old);
            // Animated glyphs may rise/turn beyond the row box: give them room
            // unless the row scrolls internally and must stay tightly clipped.
            const bool roomy=kinetic[n].valid&&fullHeights[n]<=viewportHeights[n]+.5f;
            const float padX=kinetic[n].valid?size*.5f:8.f,padTop=roomy?size*.75f:0.f,padBottom=roomy?size*.35f:0.f;
            target->PushAxisAlignedClip(D2D1::RectF(g.lyricLeft-padX,row.viewportTop-padTop,g.lyricLeft+g.lyricWidth+padX,row.viewportTop+row.unscaledHeight+padBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if(kinetic[n].valid){
                target->SetTransform(old);
                DrawKineticItems(kinetic[n],ink,feather,false);
                target->SetTransform(row.transform*old);
            }
            else if (begin + n == selected) ActiveLyric(layouts[n].Get(), layoutLengths[n],row.textX,row.y,wordProgress,rowColors,style);
            else {
                if (layoutLengths[n]) Check(layouts[n]->SetDrawingEffect(nullptr, {0, layoutLengths[n]}));
                auto faded=Mix(colors.muted,colors.ink,rowRoles[n]);const float resting=begin+n<selected?.44f:.62f;
                faded.a*=(resting+(1.f-resting)*rowRoles[n])*row.opacity;
                brush->SetColor(faded); target->DrawTextLayout(D2D1::Point2F(row.textX, row.y), layouts[n].Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            }
            float secondaryY=row.y+mainHeights[n];
            const bool active=begin+n==selected;
            auto secondary=[&](IDWriteTextLayout* layout,float height,float channelVisibility,float opacity,D2D1_COLOR_F tint){
                if(!layout||channelVisibility<=0)return;
                secondaryY+=secondaryGap*channelVisibility;
                const float visibleHeight=height*channelVisibility;
                auto color=tint;color.a*=opacity*channelVisibility;
                brush->SetColor(color);
                target->PushAxisAlignedClip(D2D1::RectF(g.lyricLeft,secondaryY,g.lyricLeft+g.lyricWidth,secondaryY+visibleHeight),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                target->DrawTextLayout(D2D1::Point2F(g.lyricLeft,secondaryY),layout,brush.Get(),D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                target->PopAxisAlignedClip();secondaryY+=visibleHeight;
            };
            const auto translationTint=Mix(rowColors.muted,WithAlpha(ink.accentLit,rowColors.muted.a),active&&style!=2?.35f*rowRoles[n]:0.f);
            secondary(translations[n].Get(),translationHeights[n],motion.translationVisibility,active?.86f:.42f,translationTint);
            secondary(romanizations[n].Get(),romanizationHeights[n],motion.romanizationVisibility,active?.68f:.30f,rowColors.muted);
            target->PopAxisAlignedClip();
            target->SetTransform(old);
        }
        target->PopAxisAlignedClip();
    }
    void Thumbnail(float x,float y,float edge,float opacity,const Palette& colors,float radiusRatio=.16f) {
        const auto rect=D2D1::RoundedRect(D2D1::RectF(x,y,x+edge,y+edge),edge*radiusRatio,edge*radiusRatio);
        if(thumbnailBrush&&thumbnailEdge){
            const float ratio=edge/float(thumbnailEdge);
            thumbnailBrush->SetTransform(D2D1::Matrix3x2F::Scale(ratio,ratio)*D2D1::Matrix3x2F::Translation(x,y));
            thumbnailBrush->SetOpacity(std::clamp(opacity,0.f,1.f));
            target->FillRoundedRectangle(rect,thumbnailBrush.Get());
        } else {
            brush->SetColor(WithAlpha(Mix(colors.panel,colors.glow,.4f),opacity*.9f));
            target->FillRoundedRectangle(rect,brush.Get());
            brush->SetColor(WithAlpha(colors.accent,opacity*.85f));
            const float cx=x+edge*.46f,cy=y+edge*.62f,r=edge*.11f;
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),r*1.15f,r),brush.Get());
            target->DrawLine(D2D1::Point2F(cx+r,cy),D2D1::Point2F(cx+r,y+edge*.26f),brush.Get(),std::max(1.2f,edge*.045f));
            target->DrawLine(D2D1::Point2F(cx+r,y+edge*.26f),D2D1::Point2F(cx+r+edge*.16f,y+edge*.33f),brush.Get(),std::max(1.2f,edge*.045f));
        }
        brush->SetColor(WithAlpha(colors.ink,.10f*opacity));
        target->DrawRoundedRectangle(rect,brush.Get(),1.f);
    }
    // presence: 1 normally, 0 while the stage UI console carries the title.
    void NowPlaying(const SceneGeometry& g,const Palette& colors,int style,const std::wstring& title,const std::wstring& artist,float chrome,float presence=1.f) {
        presence=std::clamp(presence,0.f,1.f);
        if(presence<=.01f)return;
        const auto ink=WithAlpha(colors.ink,(.40f+.55f*chrome)*presence);
        const auto muted=WithAlpha(colors.muted,(.30f+.65f*chrome)*presence);
        switch(g.nowPlaying){
            case 1:{
                Text(title,g.metaX,g.metaY,g.metaWidth,34.f,22.f,ink,DWRITE_TEXT_ALIGNMENT_CENTER,FontDisplay);
                Text(artist,g.metaX,g.metaY+34.f,g.metaWidth,22.f,14.f,muted,DWRITE_TEXT_ALIGNMENT_CENTER);
                break;
            }
            case 2:{
                brush->SetColor(WithAlpha(colors.accent,.85f*chrome+.1f));
                target->FillRectangle(D2D1::RectF(g.metaX,g.metaY-18.f,g.metaX+42.f,g.metaY-16.f),brush.Get());
                Text(title,g.metaX,g.metaY,g.metaWidth,48.f,34.f,ink,DWRITE_TEXT_ALIGNMENT_LEADING,FontDisplay);
                Text(artist,g.metaX,g.metaY+50.f,g.metaWidth,24.f,14.f,muted);
                break;
            }
            case 3:{
                Text(title,g.metaX,g.metaY,g.metaWidth,34.f,22.f,ink,DWRITE_TEXT_ALIGNMENT_LEADING,FontChromeStrong);
                Text(artist,g.metaX,g.metaY+34.f,g.metaWidth,22.f,14.f,muted);
                break;
            }
            case 4:{
                Text(title,g.metaX,g.metaY,g.metaWidth,22.f,15.f,ink,DWRITE_TEXT_ALIGNMENT_LEADING,FontChromeStrong);
                Text(artist,g.metaX,g.metaY+21.f,g.metaWidth,18.f,11.5f,muted);
                break;
            }
            default:{
                const float edge=48.f,x=g.railLeft,y=g.railY-22.f-edge;
                Thumbnail(x,y,edge,(.35f+.65f*chrome)*presence,colors);
                const float textX=x+edge+14.f,textWidth=std::max(80.f,std::min(460.f,(g.railRight-textX)*.55f));
                Text(title,textX,y+2.f,textWidth,24.f,16.f,ink,DWRITE_TEXT_ALIGNMENT_LEADING,FontChromeStrong);
                Text(artist,textX,y+26.f,textWidth,20.f,13.f,muted);
                break;
            }
        }
        (void)style;
    }
    void Progress(const SceneGeometry& g,const Palette& colors,const StageInk& ink,float progress,bool frozen,float chrome,const std::wstring& time) {
        if(chrome<=.01f)return;
        const float left=g.railLeft,right=g.railRight,y=g.railY;
        brush->SetColor(WithAlpha(colors.muted,.20f*chrome));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,y-1.f,right,y+1.f),1.f,1.f),brush.Get());
        const float head=left+(right-left)*std::clamp(progress,0.f,1.f);
        const auto fill=ink.lightStage?colors.accent:Luminous(colors.accent,.35f);
        brush->SetColor(WithAlpha(fill,(frozen?.45f:.85f)*chrome));
        if(head>left)target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,y-1.f,head,y+1.f),1.f,1.f),brush.Get());
        if(!ink.lightStage)Spot(4,fill,head,y,10.f+4.f*motion.pulse*motion.response,(frozen?.25f:.45f)*chrome);
        brush->SetColor(WithAlpha(fill,(frozen?.55f:.95f)*chrome));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(head,y),3.f,3.f),brush.Get());
        if(!time.empty()&&!g.compact)Text(time,right-180.f,y-24.f,180.f,18.f,11.5f,WithAlpha(colors.muted,.9f*chrome),DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    void StatusPill(const SceneGeometry& g,const Palette& colors,const std::wstring& status,bool warning,float chrome) {
        if(status.empty())return;
        const float opacity=.55f+.45f*chrome;
        if(g.compact){
            Text(status,g.railLeft,g.railY-26.f,g.railRight-g.railLeft,16.f,10.5f,WithAlpha(warning?colors.accent:colors.muted,opacity));
            return;
        }
        const float size=11.5f;
        const float textWidth=std::min(TextWidth(status,size,FontChrome),std::max(60.f,g.railRight-g.railLeft-40.f));
        const float pillWidth=textWidth+34.f,pillHeight=24.f;
        // Bottom card on the left: the pill sits right, above the time label.
        const float x=g.nowPlaying==3?g.railLeft:g.nowPlaying==0?std::max(g.railLeft,g.railRight-pillWidth):
            std::max(g.railLeft,g.stageWidth*.5f-pillWidth*.5f);
        // Above the stage UI console (76 DIPs high, 24 above the bottom).
        const float resting=g.railY-(g.nowPlaying==0?58.f:52.f),raised=std::min(resting,g.height-136.f);
        const float y=resting+(raised-resting)*motion.consoleVisibility;
        brush->SetColor(WithAlpha(Mix(colors.panel,colors.background,.3f),.62f*opacity));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+pillWidth,y+pillHeight),pillHeight*.5f,pillHeight*.5f),brush.Get());
        brush->SetColor(WithAlpha(warning?colors.accent:colors.muted,.95f*opacity));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+13.f,y+pillHeight*.5f),3.f,3.f),brush.Get());
        Text(status,x+23.f,y+(pillHeight-size*1.45f)*.5f,textWidth+6.f,size*1.6f,size,WithAlpha(warning?colors.accent:colors.ink,.82f*opacity));
    }
    // Stage UI above the stage. Glass samples the blurred stage only when
    // this frame went through the compositor.
    void PaintUi(const ui::DrawList& list,bool composed) {
        if(!painter)return;
        ui::PaintResources resources;resources.target=target.Get();
        resources.blur=composed?blurBitmap.Get():nullptr;
        resources.clientWidth=width/scale;resources.clientHeight=height/scale;
        resources.cover=thumbnail.Get();
        painter->Paint(list,resources);
    }
    // Room lighting for the Backstage card, derived from the stage's own
    // light so the room carries the stage color (paper reads as a lit page).
    StageCompositeFrame BackstageFrame(const BackstagePose& pose,const StageInk& ink) const {
        StageCompositeFrame frame;frame.pose=pose;frame.mix=backstageMix;
        const bool light=ink.lightStage;
        frame.backdrop=light?.26f:.50f;frame.reflection=light?.20f:.30f;frame.shadow=light?.95f:.85f;
        const auto glow=D2D1::ColorF(ink.glow.r,ink.glow.g,ink.glow.b);
        const auto spot=Luminous(Mix(glow,D2D1::ColorF(1,1,1),.45f),.55f);
        frame.light={spot.r,spot.g,spot.b};frame.lightIntensity=light?.55f:1.f;
        const auto rim=Mix(Luminous(glow,.45f),D2D1::ColorF(1,1,1),.55f);
        frame.rim={rim.r,rim.g,rim.b};frame.rimStrength=light?.30f:.50f;
        return frame;
    }
    bool RefreshVisibility() {
        if(!occluded)return true;
        if(!swap||!IsWindow(window)||IsIconic(window))return false;
        // DXGI_PRESENT_TEST queries graphics availability; it draws no frame.
        const auto status=swap->Present(0,DXGI_PRESENT_TEST);lastVisibilityResult=status;Check(status);
        occluded=status==DXGI_STATUS_OCCLUDED;
        if(!occluded)lastFrame=0;
        return !occluded;
    }
    bool Draw(const SceneInput& input,bool pacePresentation,std::optional<PresentedLyricPosition>* presentedLyricLine) {
        compositionPaced = false;
        if(!target||!backBuffer||occluded)return false;
        if(!offscreen&&(!IsWindow(window)||IsIconic(window)))return false;
        const auto current=MonotonicMs();
        const auto now=input.frameAtMs>0&&input.frameAtMs<=current&&current-input.frameAtMs<=250?input.frameAtMs:current;
        auto view=input.view;
        CollectGpuTiming();
        if(!lastPresentedAt||current<lastPresentedAt||current-lastPresentedAt>1200)veilStartedAt=current;
        if(!lastActivityAt)lastActivityAt=current;
        const auto stageWidth=view.panelVisible&&!view.desktop?std::max(1.f,width/scale-SettingsRailWidth(width/scale)):width/scale;
        if(CompactStageLayout(stageWidth,height/scale)){
            view.lyricDisplayMode=1;
            if(height/scale<380){view.showTranslation=false;view.showRomanization=false;}
        }
        const float dt = lastFrame ? std::clamp((now - lastFrame) / 1000.f, 0.f, .1f) : .016f; lastFrame = now;
        const bool backstageMoving=UpdateBackstage(view,dt);
        if (!motion.initialized) {
            motion.phase=now/1000.0;motion.speed=view.reducedMotion?.12f:1.f;
            motion.amplitude=view.reducedMotion?.18f:1.f;motion.initialized=true;
            motion.translationVisibility=view.showTranslation?1.f:0.f;
            motion.romanizationVisibility=view.showRomanization?1.f:0.f;
            motion.focusVisibility=view.lyricDisplayMode==1?1.f:0.f;
            motion.pairVisibility=view.lyricDisplayMode==2?1.f:0.f;
            motion.chromeVisibility=view.quietFullscreen?0.f:1.f;
            motion.response=std::isfinite(view.response)?std::clamp(view.response,0.f,2.f):1.f;
            motion.backgroundShade=UnitSignal(view.backgroundShade);
            motion.cameraMotion=std::isfinite(view.cameraMotion)?std::clamp(view.cameraMotion,0.f,2.f):1.f;
            motion.particlePhase=motion.phase;
        }
        motion.speed=Approach(motion.speed,view.reducedMotion?.12f:1.f,dt,1.7f);
        motion.amplitude=Approach(motion.amplitude,view.reducedMotion?.18f:1.f,dt,1.7f);
        motion.translationVisibility=Approach(motion.translationVisibility,view.showTranslation?1.f:0.f,dt,10.f);
        motion.romanizationVisibility=Approach(motion.romanizationVisibility,view.showRomanization?1.f:0.f,dt,10.f);
        const float focusTarget=view.lyricDisplayMode==1?1.f:0.f,pairTarget=view.lyricDisplayMode==2?1.f:0.f;
        motion.focusVisibility=view.reducedMotion?focusTarget:Approach(motion.focusVisibility,focusTarget,dt,8.f);
        motion.pairVisibility=view.reducedMotion?pairTarget:Approach(motion.pairVisibility,pairTarget,dt,8.f);
        motion.chromeVisibility=Approach(motion.chromeVisibility,view.fullscreen&&view.quietFullscreen&&!view.desktop?0.f:1.f,dt,8.f);
        motion.consoleVisibility=Approach(motion.consoleVisibility,view.consoleShown&&!view.desktop?1.f:0.f,dt,view.reducedMotion?14.f:9.f);
        motion.phase += dt * motion.speed; // Cumulative phase never multiplies wall time by a toggled flag.
        const auto& p=input.playback;const auto& a=input.audio;
        const auto audioAt=a.timingTrusted?a.sampledAtMs:a.observedAtMs;
        // Shared frame time keeps the hosts coherent but cannot renew an old
        // feature's lifetime. Expiry uses this draw's actual monotonic instant.
        const bool currentAudio=a.active&&!a.connecting&&p.pid&&a.pid==p.pid&&p.processCreatedAtTicks&&a.processCreatedAtTicks==p.processCreatedAtTicks&&audioAt>0&&now>=audioAt&&current>=audioAt&&current-audioAt<AudioResponseFreshnessMs;
        const bool audioClockValid=motion.audioUpdatedAtMs>0&&motion.audioUpdatedAtMs<=current;
        const float audioElapsed=audioClockValid?(current-motion.audioUpdatedAtMs)/1000.f:.016f;
        const float audioDt=std::min(audioElapsed,.1f),audioGap=std::max(0.f,audioElapsed-audioDt);
        motion.audioUpdatedAtMs=current;
        if(!audioClockValid){
            motion.rms=motion.peak=motion.low=motion.mid=motion.high=0;
            motion.spectrum.fill(0);motion.lowFast=motion.lowSlow=motion.pulse=0;
        }else if(audioGap>0){
            // Expire unseen history before applying the latest input for one
            // bounded step. Frame/occlusion resets must not revive old energy
            // or pretend the current packet covered the entire draw gap.
            motion.rms=Approach(motion.rms,0.f,audioGap,4.f);
            motion.peak=Approach(motion.peak,0.f,audioGap,3.5f);
            for(auto& band:motion.spectrum)band=Approach(band,0.f,audioGap,4.f);
            motion.low=Approach(motion.low,0.f,audioGap,7.f);
            motion.mid=Approach(motion.mid,0.f,audioGap,7.f);
            motion.high=Approach(motion.high,0.f,audioGap,7.f);
            motion.lowFast=Approach(motion.lowFast,0.f,audioGap,7.f);
            motion.lowSlow=Approach(motion.lowSlow,0.f,audioGap,7.f);
            motion.pulse=Approach(motion.pulse,0.f,audioGap,6.f);
        }
        const float signal = currentAudio?UnitSignal(a.rms):0;
        motion.rms = Approach(motion.rms, signal, audioDt, signal > motion.rms ? 14.f : 4.f);
        const float peak=currentAudio?UnitSignal(a.peak):0;
        motion.peak=Approach(motion.peak,peak,audioDt,peak>motion.peak?18.f:3.5f);
        motion.response=Approach(motion.response,std::isfinite(view.response)?std::clamp(view.response,0.f,2.f):1.f,dt,7.f);
        motion.backgroundShade=Approach(motion.backgroundShade,UnitSignal(view.backgroundShade),dt,7.f);
        motion.cameraMotion=Approach(motion.cameraMotion,std::isfinite(view.cameraMotion)?std::clamp(view.cameraMotion,0.f,2.f):1.f,dt,7.f);
        float low=0,mid=0,high=0,lowInstant=0;
        for(size_t i=0;i<motion.spectrum.size();++i){
            const float value=currentAudio?UnitSignal(a.spectrum[i]):0;
            if(i<6)lowInstant+=value;
            motion.spectrum[i]=Approach(motion.spectrum[i],value,audioDt,value>motion.spectrum[i]?12.f:4.f);
            if(i<9)low+=motion.spectrum[i];else if(i<21)mid+=motion.spectrum[i];else high+=motion.spectrum[i];
        }
        // The capture publishes 32 logarithmic spectral bands. These groups
        // are envelopes, not a claim to detect beats or individual instruments.
        motion.low=Approach(motion.low,low/9.f,audioDt,7.f);
        motion.mid=Approach(motion.mid,mid/12.f,audioDt,7.f);
        motion.high=Approach(motion.high,high/11.f,audioDt,7.f);
        // A visual pulse on sharp low-band rises of the current process audio.
        // Bands are log-compressed (0..1), so a kick is a modest relative rise.
        lowInstant/=6.f;
        motion.lowFast=Approach(motion.lowFast,lowInstant,audioDt,30.f);
        motion.lowSlow=Approach(motion.lowSlow,lowInstant,audioDt,1.6f);
        if(currentAudio&&!view.reducedMotion&&motion.lowFast>motion.lowSlow*1.18f+.03f&&current-motion.lastPulseAt>240){
            motion.pulse=1.f;motion.lastPulseAt=current;
        }
        motion.pulse=Approach(motion.pulse,0.f,dt,5.5f);
        motion.particlePhase+=dt*motion.speed*(.7f+motion.mid*motion.response*1.3f);
        const auto colors=input.palette?Colors(*input.palette):Colors(view);
        const int style = std::clamp(view.customTheme ? view.customTheme->baseStyle : view.theme, 0, BuiltInStyleCount-1);
        const bool original=IsOriginalStyle(style);
        const auto g = Geometry(width / scale, height / scale, style, view);
        const float cameraZoom=CameraZoom();
        // GPU stage for the cover/theme atmosphere; selected image/video media
        // keeps its original Direct2D transitions and readability matte.
        const bool gpuStage=!original&&gpu&&backBufferView&&view.coverBackground;
        const bool originalGpuStage=original&&originalGpu&&backBufferView;
        // The compositor pass runs only while Backstage is visible or a glass
        // panel samples the blurred stage; otherwise nothing changes.
        const bool hasUi=input.ui&&!input.ui->Empty()&&!view.desktop;
        const bool composite=(backstageValue>0.f||view.glass||(hasUi&&input.ui->UsesGlass()))&&EnsureComposite();
        if(composite)compositorUsedAt=current;
        else if(compositor&&compositor->StageTexture()&&current-compositorUsedAt>4000){
            // Unused for a while (Backstage closed, no glass): give the memory
            // back, e.g. while a fullscreen game keeps the stage suspended.
            if(painter)painter->ReleaseBitmaps();
            stageBitmap.Reset();blurBitmap.Reset();compositor->Release();
        }
        ID3D11RenderTargetView* stageView=composite?compositor->StageTarget():backBufferView.Get();
        BeginGpuTiming();
        // In cover mode the decoded background frame is the album cover; any
        // other selection is user media and owns no thumbnail or cover palette.
        if(view.coverBackground)UpdateStageCover(input.background,now);
        else if(stageCoverFrame||thumbnail){
            stageCoverFrame=nullptr;stageCoverSequence=0;coverPalette={};
            thumbnailBrush.Reset();thumbnail.Reset();thumbnailEdge=0;
            if(gpu)gpu->ClearCover(now);
        }
        if(original){
            originalState.Update(p,a,currentAudio,input.manualLyricPreview,input.lyricVersionMismatch,now,dt,view.reducedMotion,
                view.pointerActive&&!view.desktop,view.pointerX/std::max(1.f,g.stageWidth),view.pointerY/std::max(1.f,g.height));
            originalFrame={};originalFrame.style=style;originalFrame.width=g.stageWidth*scale;originalFrame.height=g.height*scale;originalFrame.dpi=scale;
            originalFrame.seconds=static_cast<float>(std::fmod(motion.phase,7200.0));originalFrame.delta=dt;
            originalFrame.low=motion.low;originalFrame.mid=motion.mid;originalFrame.high=motion.high;originalFrame.pulse=motion.pulse;
            originalFrame.response=motion.response;originalFrame.motion=view.reducedMotion?0.f:motion.cameraMotion*.5f;
            originalFrame.drawer=view.originalDrawer;
            originalFrame.base={colors.background.r,colors.background.g,colors.background.b,1};
            originalFrame.accent={colors.accent.r,colors.accent.g,colors.accent.b,1};originalFrame.ink={colors.ink.r,colors.ink.g,colors.ink.b,1};
            originalState.Apply(originalFrame,now,view.reducedMotion);
            if(!view.coverBackground)UploadBackground(input.background,false,g,colors,style,now,cameraZoom);
            if(originalGpuStage)originalGpu->Render(context.Get(),stageView,originalFrame,qualityTier);
        }else if(gpuStage){
            if(!gpuStageActive)ResetMediaState();
            StageGpuLayer(stageView,g,colors,style,view,now);
        }else UploadBackground(input.background,view.coverBackground,g,colors,style,now,cameraZoom);
        gpuStageActive=gpuStage||originalGpuStage;
        // Text light. Built-in flow follows the cover's most vivid color.
        StageInk ink;
        ink.lightStage=Luminance(colors.background)>.45f;
        D2D1_COLOR_F highlight=colors.accent;
        if(style==0&&!view.customTheme&&gpuStage&&coverPalette.valid)highlight=Mix(colors.accent,Luminous(FromRgb(coverPalette.vivid),.42f),.65f);
        if(style==0){ink.dim=WithAlpha(colors.ink,.40f);ink.lit=colors.ink;ink.accentLit=colors.ink;ink.glow=Luminous(highlight,.50f);}
        else if(style==1){ink.dim=WithAlpha(colors.ink,.38f);ink.lit=Mix(colors.ink,colors.accent,.62f);ink.accentLit=Luminous(colors.accent,.42f);ink.glow=Luminous(colors.accent,.45f);}
        else{ink.dim=WithAlpha(Mix(colors.muted,colors.ink,.35f),.70f);ink.lit=colors.ink;ink.accentLit=colors.accent;ink.glow=WithAlpha(colors.accent,ink.lightStage?.45f:1.f);}
        if(ink.lightStage&&style!=2){ink.dim=WithAlpha(colors.ink,.45f);ink.glow=WithAlpha(colors.accent,.40f);}
        ink.glowStrength=software||!glowBlur||qualityTier>=3?0.f:std::clamp(.55f+.45f*motion.response,0.f,1.4f)*(qualityTier==2?.6f:1.f);
        if(ink.glowStrength>0.f&&!ink.lightStage)ink.glow=WithAlpha(ink.glow,std::min(1.f,.80f*ink.glowStrength));
        target->SetTarget(composite?stageBitmap.Get():backBuffer.Get());target->SetDpi(96*scale,96*scale);
        target->BeginDraw(); target->SetTransform(D2D1::Matrix3x2F::Identity());
        if(original)OriginalBackdrop(g,colors,style,view,now,originalGpuStage);
        else if(gpuStage)StageDecorations(g,colors,style,view);
        else Background(g, colors, style, view);
        if (g.panel) Fill(D2D1::RectF(g.stageWidth, 0, width / scale, g.height), colors.panel);
        auto chromeInk=colors.ink,chromeMuted=colors.muted;
        chromeInk.a*=motion.chromeVisibility;chromeMuted.a*=motion.chromeVisibility;
        if(!original&&!view.desktop&&!g.compact&&style!=2){
            Text(L"HTune",g.railLeft,24,220,20,12.5f,WithAlpha(chromeInk,.62f),DWRITE_TEXT_ALIGNMENT_LEADING,FontChromeStrong);
            Text(style == 0 ? L"轻光流动" : L"电影剧场",g.railLeft,43,220,18,11,WithAlpha(chromeMuted,.75f));
        }
        const bool verifiedIdOnly=!p.metadataStale&&p.songIdVerified&&p.title.empty()&&SameCatalogId(p.songId,p.songId);
        const auto title=input.manualLyricPreview?(input.lyricPreviewTitle.empty()?L"手动查找歌词":input.lyricPreviewTitle):
            p.identityReacquiring?L"正在重新确认歌曲":p.metadataStale?L"正在识别歌曲":
            verifiedIdOnly?L"歌曲 ID "+p.songId:p.title.empty()?L"打开网易云、QQ 音乐或酷狗音乐，播放一首歌":p.title;
        const auto artist=input.manualLyricPreview?(input.lyricPreviewArtist.empty()?L"手动歌词预览":input.lyricPreviewArtist):
            p.metadataStale?(p.title.empty()?L"等待歌曲信息":L"此前歌曲："+p.title):
            verifiedIdOnly?L"歌曲资料加载中":p.artist.empty()?L"等待歌曲信息":p.artist;
        // Without the GPU stage the cinema field shows the still cover instead.
        if(g.particles&&!gpuStage&&thumbnailBrush){
            const float edge=g.fieldSize*.82f;
            Thumbnail(g.fieldX-edge*.5f,g.fieldY-edge*.5f,edge,g.fieldOpacity,colors,.035f);
        }
        // The console carries the same title while it is up.
        if(original)OriginalChrome(g,colors,style,view,title,artist,now);
        else NowPlaying(g,colors,style,title,artist,motion.chromeVisibility,g.nowPlaying==0?1.f-motion.consoleVisibility:1.f);
        // Shared frame time aligns host animation/projection; it cannot renew
        // an old sample's eligibility at this draw's actual entry time.
        const auto timelineSource=input.manualLyricPreview?TimelineSource::Unavailable:CurrentTimelineSource(p,current);
        const bool media=timelineSource==TimelineSource::Clock||timelineSource==TimelineSource::SampledClock;
        const bool sourceSynced=timelineSource!=TimelineSource::Unavailable;
        const bool hasIdentity=!input.manualLyricPreview&&!p.metadataStale&&p.pid&&p.processCreatedAtTicks&&
            (p.songIdVerified?SameCatalogId(p.songId,p.songId):!p.title.empty());
        if (hasIdentity?!continuityIdentity.Matches(p):continuityIdentity.pid!=0) {
            continuityIdentity=hasIdentity?PlaybackContinuityIdentity::Capture(p):PlaybackContinuityIdentity{};
            continuityPositionMs = 0;
            continuityCapturedAtMs = 0;
        }
        // A snapshot may arrive after the shared frame instant. Project no
        // earlier than its anchors, and never beyond the actual draw instant.
        // An unavailable source must not regain interpolation via an old frame.
        const auto playbackAt=sourceSynced?std::min(current,std::max({now,p.sampledAtMs,p.observedAtMs})):current;
        int64_t pos=input.manualLyricPreview?0:PlaybackPositionAt(p,playbackAt);
        bool frozen = false;
        bool synced = sourceSynced;
        if (sourceSynced && hasIdentity) {
            continuityPositionMs = pos;
            continuityCapturedAtMs = current;
        } else if (hasIdentity && continuityCapturedAtMs > 0 && current >= continuityCapturedAtMs &&
            current - continuityCapturedAtMs <= 1000) {
            pos = continuityPositionMs;
            synced = true;
            frozen = true;
        }
        // Presentation idleness (cadence only, never a play-state claim):
        // no audible current-process audio and no moving timeline for 5 s.
        const bool recentInput=view.interactionAtMs>0&&current>=view.interactionAtMs&&current-view.interactionAtMs<3000;
        if((currentAudio&&signal>.004f)||recentInput||backstageMoving||view.manualLyricBrowse||(synced&&!frozen&&pos!=lastActivityPosition))lastActivityAt=current;
        if(synced)lastActivityPosition=pos;
        const bool presentationIdle=current>=lastActivityAt&&current-lastActivityAt>5000;
        // The stage names only states that change what the lyrics mean:
        // previews, browsing, static or conflicting versions. Normal following
        // details (sampled clock, cache, short holds) stay on the status page
        // unless diagnostics are enabled.
        std::wstring stageStatus;bool statusWarning=false;
        if(input.manualLyricPreview)stageStatus=L"手动歌词预览 · 不使用当前进度";
        else if(p.identityReacquiring)stageStatus=L"歌曲身份短暂中断 · 此前歌词保持静态";
        else if(p.metadataStale)stageStatus=L"歌曲信息更新中 · 此前歌词为静态预览";
        else if(input.lyricVersionMismatch){stageStatus=p.songIdVerified?L"歌词版本不同 · 当前为静态预览":L"歌词版本存在歧义 · 当前为静态预览";statusWarning=true;}
        else if(view.manualLyricBrowse)stageStatus=sourceSynced?L"浏览歌词 · 稍后自动返回":L"浏览歌词 · 暂无可靠时间轴";
        else if(!synced)stageStatus=L"静态歌词预览 · 滚轮浏览";
        else if(view.diagnostics){
            if(frozen)stageStatus=L"时间源切换中 · 保持最近位置";
            else if(timelineSource==TimelineSource::SampledClock)stageStatus=p.state==PlayState::Unknown?L"按新时间样本跟随 · 播放状态未提供":L"按新时间样本跟随";
            else if(input.lyrics&&input.lyrics->fromCache)stageStatus=input.lyricRefreshPending?L"本机歌词缓存 · 正在更新在线版本":L"本机歌词缓存";
            else stageStatus=p.reliability==Reliability::NativeRead?L"内部时钟 · 只读":media?L"媒体时间轴":L"秒级观测";
        }
        if(view.diagnostics&&synced&&!input.manualLyricPreview&&!p.metadataStale&&!input.lyricVersionMismatch&&
            !view.manualLyricBrowse&&!frozen&&input.lyrics&&input.lyrics->retryWordTiming){
            if(!stageStatus.empty())stageStatus+=L" · ";
            stageStatus+=input.lyricRefreshPending?L"逐句已保留 · 正在重试逐字":L"逐句已就绪 · 逐字稍后重试";
        }
        const wchar_t* association=nullptr;
        if(!input.manualLyricPreview&&!p.metadataStale&&!input.lyricVersionMismatch)switch(input.lyricAssociation){
            case LyricAssociation::VerifiedCurrent:if(view.diagnostics)association=L"歌词版本已核验";break;
            case LyricAssociation::PreviouslyVerified:if(view.diagnostics)association=L"此前确认版本 · 待重新核验";break;
            case LyricAssociation::UserConfirmed:if(view.diagnostics)association=L"你选择的歌词版本";break;
            case LyricAssociation::AutomaticCandidate:if(view.diagnostics){association=L"自动匹配候选 · 版本未核验";statusWarning=true;}break;
            case LyricAssociation::LocalImport:if(view.diagnostics)association=L"本地歌词";break;
            default:break;
        }
        if(association){auto linked=std::wstring(association);if(!stageStatus.empty())linked+=L" · "+stageStatus;stageStatus=std::move(linked);}
        const bool followLyrics=synced&&!input.manualLyricPreview&&!p.metadataStale&&!view.manualLyricBrowse&&!input.lyricVersionMismatch;
        std::optional<PresentedLyricPosition> selectedLine;
        LyricViewportState nextViewport;
        Lyrics(input,view,g,colors,ink,style,followLyrics,media&&!frozen&&followLyrics,WithLyricOffset(pos,view.offsetMs),dt,now,selectedLine,nextViewport);
        StatusPill(g,colors,stageStatus,statusWarning,motion.chromeVisibility);
        // Progress exists only for a usable timeline, exactly as before.
        if(style==EchoStyle&&synced){
            const float x=g.stageWidth*.705f,y=g.height*.65f;
            brush->SetColor(WithAlpha(colors.muted,.55f));target->DrawLine(D2D1::Point2F(x,y-58.f),D2D1::Point2F(x,y-20.f),brush.Get(),1.f);
            brush->SetColor(D2D1::ColorF(0xF47C16));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),6.5f,6.5f),brush.Get());
            Text(Time(pos),x-48.f,y-84.f,96.f,26.f,17.f,colors.ink,DWRITE_TEXT_ALIGNMENT_CENTER,FontLyricCinema);
        }else if(synced&&p.durationMs>0)Progress(g,colors,ink,std::clamp(float(pos)/float(p.durationMs),0.f,1.f),frozen,
            motion.chromeVisibility*(1.f-.85f*motion.consoleVisibility),Time(pos)+L" / "+Time(p.durationMs));
        else if(synced&&!g.compact)Text(Time(pos),g.railRight-180.f,g.railY-24.f,180.f,18.f,11.5f,WithAlpha(chromeMuted,.9f),DWRITE_TEXT_ALIGNMENT_TRAILING);
        auto footerMessage=input.background.error;
        const bool coverFailed=view.coverBackground&&!input.coverError.empty();
        if(coverFailed){if(!footerMessage.empty())footerMessage+=L"\n";footerMessage+=L"封面 · "+input.coverError;}
        if(view.diagnostics){if(!footerMessage.empty())footerMessage+=L"\n";footerMessage+=p.source+L" · "+p.diagnostic;}
        if(!footerMessage.empty()&&!g.compact)Text(footerMessage,g.railLeft,g.railY+5.f,g.railRight-g.railLeft,std::max(12.f,g.height-g.railY-6.f),10.5f,
            input.background.error.empty()&&!coverFailed?colors.muted:colors.accent,DWRITE_TEXT_ALIGNMENT_LEADING);
        const float veil=1.f-SmoothStep(static_cast<float>(std::max<int64_t>(0,current-veilStartedAt))/450.f);
        if(hasUi&&!composite)PaintUi(*input.ui,false);
        if(!composite&&veil>0.f)Fill(D2D1::RectF(0,0,width/scale,height/scale),WithAlpha(colors.background,veil));
        {
            const HRESULT drawn=target->EndDraw();
            if(composite)target->SetTarget(backBuffer.Get());
            Check(drawn); // Device/target loss follows the app's complete recovery path.
        }
        if(composite){
            // Second pass: blurred copy, then the plain stage (a copy) or the
            // Backstage room, then whatever overlays the composed frame.
            compositor->Blur(context.Get());
            const auto pose=ComputeBackstagePose(float(width),float(height),scale,backstageValue);
            if(pose.identity)context->CopyResource(backTexture.Get(),compositor->StageTexture());
            else compositor->Present(context.Get(),backBufferView.Get(),BackstageFrame(pose,ink));
            target->SetDpi(96*scale,96*scale);
            target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());
            if(hasUi)PaintUi(*input.ui,true);
            if(veil>0.f)Fill(D2D1::RectF(0,0,width/scale,height/scale),WithAlpha(colors.background,veil));
            Check(target->EndDraw());
        }
        EndGpuTiming();
        if(offscreen){
            // Preview frames are complete once submitted; nothing is presented.
            context->Flush();
            UpdateQuality(view,current,1000.f/60.f);
            lastPresentedAt=current;lyricViewport=nextViewport;
            if(presentedLyricLine)*presentedLyricLine=selectedLine;
            return true;
        }
        const UINT interval=PresentInterval(view,presentationIdle);
        ++presentAttempts;
        const auto status=swap->Present(blt?0:interval,0);lastPresentResult=status;Check(status);
        UpdateQuality(view,current,static_cast<float>(interval)*1000.f/static_cast<float>(std::max(1u,RefreshRate())));
        occluded=status==DXGI_STATUS_OCCLUDED;
        if(occluded)return false;
        if(status==S_OK)++presentedFrames;
        lastPresentedAt=current;
        // Keep only offsets belonging to a presented frame. A failed or
        // occluded draw must not become the next static reading position.
        lyricViewport=nextViewport;
        // The desktop host presents immediately; one composition wait per
        // display refresh in the cadence keeps it at the same frame rate.
        if(blt&&pacePresentation){compositionPaced=true;for(UINT wait=0;wait<interval;++wait)compositionPaced=SUCCEEDED(DwmFlush())&&compositionPaced;}
        if(presentedLyricLine)*presentedLyricLine=selectedLine;
        return true;
    }
};
SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}
SceneRenderer::~SceneRenderer() = default;
void SceneRenderer::Create(HWND h, bool desktopBlt, bool forceSoftware) {
    ++frameGeneration_;
    const MotionState previous = impl_ ? impl_->motion : MotionState{};
    const std::array<float,3> backstage=impl_?std::array<float,3>{impl_->backstageValue,impl_->backstageVelocity,impl_->backstageMix}:std::array<float,3>{0.f,0.f,1.f};
    impl_.reset(); // Flush deferred same-HWND swap destruction before its replacement.
    impl_ = std::make_unique<Impl>(); impl_->motion = previous; impl_->Create(h, desktopBlt, forceSoftware);
    if(impl_->compositor){impl_->backstageValue=backstage[0];impl_->backstageVelocity=backstage[1];impl_->backstageMix=backstage[2];}
}
void SceneRenderer::CreateOffscreen(UINT width,UINT height,float dpiScale,bool forceSoftware) {
    ++frameGeneration_;
    impl_.reset();
    impl_=std::make_unique<Impl>();impl_->CreateOffscreen(width,height,dpiScale,forceSoftware);
}
bool SceneRenderer::CaptureBgra(std::vector<uint8_t>& pixels,UINT& width,UINT& height) {
    return impl_&&impl_->CaptureBgra(pixels,width,height);
}
float SceneRenderer::GpuFrameMs() const {return impl_&&impl_->gpuFrameSampled?impl_->gpuFrameMs:0.f;}
int SceneRenderer::QualityTier() const {return impl_?impl_->qualityTier:0;}
void SceneRenderer::Resize() {if(impl_)impl_->Resize();}
bool SceneRenderer::Draw(const SceneInput& input,bool pacePresentation,std::optional<PresentedLyricPosition>* presentedLyricLine) {
    if(presentedLyricLine)presentedLyricLine->reset();
    return impl_&&impl_->Draw(input,pacePresentation,presentedLyricLine);
}
HANDLE SceneRenderer::FrameEvent() const {return impl_?impl_->frameEvent:nullptr;}
uint64_t SceneRenderer::FrameGeneration() const {return frameGeneration_;}
bool SceneRenderer::Software() const {return impl_&&impl_->software;}
HRESULT SceneRenderer::DeviceRemovedReason() const {return impl_&&impl_->device?impl_->device->GetDeviceRemovedReason():E_FAIL;}
bool SceneRenderer::CompositionPaced() const {return impl_&&impl_->compositionPaced;}
bool SceneRenderer::Occluded() const {return impl_&&impl_->occluded;}
std::string SceneRenderer::PresentationDiagnostic() const {
    if(!impl_)return "renderer=none";
    std::ostringstream out;
    out << "path=" << (impl_->blt?"desktop-blt":"window-flip")
        << " size=" << impl_->width << 'x' << impl_->height
        << " attempts=" << impl_->presentAttempts << " submitted=" << impl_->presentedFrames
        << " present=0x" << std::hex << static_cast<uint32_t>(impl_->lastPresentResult)
        << " visibility=0x" << static_cast<uint32_t>(impl_->lastVisibilityResult)
        << std::dec << " occluded=" << impl_->occluded << " software=" << impl_->software;
    return out.str();
}
bool SceneRenderer::RefreshVisibility() {return impl_&&impl_->RefreshVisibility();}
}
