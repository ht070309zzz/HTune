#pragma once
#include <d3d11.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace stage {
struct StageRgb { float r=0,g=0,b=0; };
// Representative colors of one cover, ordered by visual weight. Purely visual:
// never an identity, version or playback signal.
struct CoverPalette {
    std::array<StageRgb,5> colors{};
    std::array<float,5> weights{};
    StageRgb vivid{},average{};
    bool valid=false;
};
// One immutable description of the GPU stage layer for a single frame.
struct StageGpuFrame {
    float seconds=0;                 // cumulative animation clock (already speed scaled)
    float stageWidth=1,height=1;     // render target pixels covered by the stage
    float dpiScale=1;
    int style=0;                     // 0 flow, 1 cinema, 2 typography
    float low=0,mid=0,high=0,beat=0,rms=0;
    float response=1,camera=1,motion=1,shade=0;
    std::array<StageRgb,5> palette{};
    StageRgb base{},accent{};
    bool lightBackground=false;
    float finish=1;                  // vignette/grain strength
    bool particles=false;
    float fieldX=0,fieldY=0,fieldSize=0,fieldOpacity=0; // pixels
    float dustOpacity=0;
    int64_t nowMs=0;                 // monotonic wall clock for cover cross-fades
    int quality=1;                   // 0 high .. 3 low: particles, atmosphere scale, dust
    bool reducedMotion=false;       // Freeze the extra decorative light planes.
};
// Direct3D 11 layer drawn before Direct2D into the same back buffer. Shaders
// are compiled offline by fxc; a device below feature level 10_0 simply keeps
// the Direct2D-only scene. No web runtime, script engine or shader compiler
// is loaded at runtime.
class StageGpu {
public:
    StageGpu();~StageGpu();
    StageGpu(const StageGpu&)=delete;
    StageGpu& operator=(const StageGpu&)=delete;
    // False leaves the layer disabled; Direct2D keeps drawing the full scene.
    bool Create(ID3D11Device* device,bool software,bool desktop);
    bool Ready() const;
    // rgba: an UprightCoverSquare result. The previous cover cross-fades out.
    void SetCover(std::vector<uint8_t> rgba,uint32_t edge,int64_t nowMs);
    void ClearCover(int64_t nowMs);
    // 1 while a cover is current, fading to 0 after ClearCover.
    float CoverPresence(int64_t nowMs) const;
    // Draws the atmosphere, particle field and dust into target at the origin.
    // Throws only for device failures that require the renderer's recovery path.
    void Render(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,const StageGpuFrame& frame);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
// Upright (EXIF 1..8), center-cropped square of at most maxEdge pixels from
// premultiplied top-down BGRA8 input, returned as opaque RGBA8 composed over
// black. Returns empty for invalid geometry; never reads beyond bytes.
std::vector<uint8_t> UprightCoverSquare(const uint8_t* bgra,size_t bytes,uint32_t width,uint32_t height,
    uint32_t stride,uint16_t orientation,uint32_t maxEdge,uint32_t& edge);
// rgba: size*size tightly packed RGBA8 pixels.
CoverPalette AnalyzeCoverPalette(const uint8_t* rgba,uint32_t size);
}
