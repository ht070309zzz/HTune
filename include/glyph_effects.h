#pragma once
#include <d3d11.h>
#include <d2d1.h>
#include <array>
#include <memory>
#include <cstdint>

namespace stage {
// Per-letter anchors from the actual shaped and projected glyph. No independent
// text layout or playback clock: a seek simply replaces this bounded frame list.
struct GlyphEmitter {
    std::array<D2D1_POINT_2F,4> corners{}; // TL, TR, BR, BL, in stage DIPs
    D2D1_COLOR_F color{};
    float age=0,active=0,seed=0,padding=0;
    float progress=0,reserved[3]{}; // Actual singing progress, independent of light/entrance envelope.
};
inline constexpr size_t MaxGlyphEmitters=64;
struct GlyphEffectFrame {
    std::array<GlyphEmitter,MaxGlyphEmitters> emitters{};
    size_t count=0;
    float width=1,height=1,seconds=0;
    int style=0,quality=0;
    bool reduced=false;
    uint64_t phrase=0; // Visible source phrase/page, for continuous handoff within the same song.
    uint64_t stream=0; // Lyric source/generation; never share flight history across songs.
    bool reset=false;
    int64_t presentationMs=0; // Display clock; never used to advance lyrics or fill.
    bool advancing=false,sampled=false,paused=false,allowGap=false;
    D2D1_COLOR_F accent{1,1,1,1};
    D2D1_RECT_F clip{0,0,1,1};
    bool Add(const GlyphEmitter&);
};
// family: silk, crystal/circuit, gilt/ink, orbital, prism, woven ribbon.
struct GlyphEffectStyle {float family,speed,spread,weight;};
GlyphEffectStyle GlyphEffectRecipe(int style);
float GlyphSingingLight(float progress);
// One instanced pass, offline HLSL, no atlas, particle texture or full-screen
// scratch allocation. Owner ends D2D before drawing and resumes it afterwards.
class GlyphEffects {
public:
    GlyphEffects();~GlyphEffects();
    void Create(ID3D11Device*);
    void Draw(ID3D11DeviceContext*,ID3D11RenderTargetView*,UINT pixelWidth,UINT pixelHeight,const GlyphEffectFrame&);
    void Reset();
    size_t TrailSamples() const;
    double FlightSeconds() const;
    uint32_t RelayCount() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
