#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <memory>

namespace stage {
// One frame of the Backstage camera: the stage plane seen by a camera that
// has pulled back and turned it toward the settings pages. Pure geometry,
// shared by the compositor and by hit testing; never a playback signal.
struct BackstagePose {
    bool identity=true;
    // Card uv (0..1 across the stage) -> homogeneous target pixels, and back.
    std::array<double,9> forward{1,0,0,0,1,0,0,0,1},inverse{1,0,0,0,1,0,0,0,1};
    // Target pixels, clockwise from the top-left: x0,y0 .. x3,y3.
    std::array<float,8> corners{};
    float cornerRadius=0; // stage pixels
    float presence=0;     // 0 plain stage .. 1 settled Backstage
    // Maps a target pixel to stage pixels; false when it is not on the card.
    bool ToStage(float x,float y,float stageWidth,float stageHeight,float& stageX,float& stageY) const;
};
// width/height: target pixels. t: transition, 0 plain .. 1 settled (a spring
// may overshoot slightly). The settled framing leaves the right half free.
BackstagePose ComputeBackstagePose(float width,float height,float dpiScale,float t);

struct StageCompositeFrame {
    BackstagePose pose;
    float mix=1;          // 0 shows the plain stage (reduced-motion fade) .. 1 the composed room
    float backdrop=.30f;  // brightness of the stage's ambient light in the room
    float reflection=.30f,shadow=.85f;
    std::array<float,3> light{1,1,1};float lightIntensity=0;
    std::array<float,3> rim{1,1,1};float rimStrength=0;
};

// Optional second stage pass. The stage is drawn into StageTexture() instead
// of the back buffer; Blur() makes the Gaussian copy for glass panels and a
// far softer ambient copy that lights the Backstage room; Present() writes
// the composed frame.
// A device below feature level 10_0 leaves it disabled (the stage then draws
// straight into the back buffer as before).
class StageCompositor {
public:
    StageCompositor();~StageCompositor();
    StageCompositor(const StageCompositor&)=delete;
    StageCompositor& operator=(const StageCompositor&)=delete;
    bool Create(ID3D11Device* device);
    // Sizes the stage and blur textures. Generation() changes whenever they
    // are recreated, so dependent Direct2D bitmaps know to follow.
    void Prepare(UINT width,UINT height);
    // Drops the textures (Direct2D bitmaps wrapping them must go first);
    // the next Prepare recreates them.
    void Release();
    uint64_t Generation() const;
    ID3D11Texture2D* StageTexture() const;
    ID3D11RenderTargetView* StageTarget() const;
    ID3D11Texture2D* BlurTexture() const; // B8G8R8A8, 1/8 resolution, about 40 px Gaussian at full size
    void Blur(ID3D11DeviceContext* context);
    // target: the back buffer, the same size as the stage texture.
    void Present(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,const StageCompositeFrame& frame);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
