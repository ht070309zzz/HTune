#pragma once
#include "stage_ui.h"
#include <d2d1_1.h>
#include <memory>

namespace stage::ui {
// Everything the painter may sample this frame. All optional except target.
struct PaintResources {
    ID2D1DeviceContext* target=nullptr;
    // Blurred copy of the composed stage covering the whole client area
    // (stage_compositor). Without it, glass uses a denser plain tint.
    ID2D1Bitmap1* blur=nullptr;
    float clientWidth=0,clientHeight=0; // DIPs
    // Square cover thumbnail of the current song, or null.
    ID2D1Bitmap1* cover=nullptr;
};
// Paints a DrawList with Direct2D. Owned by the renderer and recreated with
// its device; caches only brushes.
class Painter {
public:
    Painter();~Painter();
    Painter(const Painter&)=delete;
    Painter& operator=(const Painter&)=delete;
    // Leaves the target's transform and clip stack as it found them.
    void Paint(const DrawList& list,const PaintResources& resources);
    // Drops cached bitmap brushes so their source textures can be freed.
    void ReleaseBitmaps();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
