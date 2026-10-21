// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <d2d1_1.h>
#include <memory>
#include <cstddef>
namespace stage {
struct RiftFrame;
struct DioramaProjection {float x=0,y=0,scale=0;bool visible=false;};
class FoliaDiorama {
public:
    FoliaDiorama();~FoliaDiorama();
    void Clear();
    void Step(const RiftFrame&,size_t line,float wordProgress,bool reset);
    void Draw(ID2D1DeviceContext*,const RiftFrame&);
    DioramaProjection Text(size_t line,float x,float y)const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
