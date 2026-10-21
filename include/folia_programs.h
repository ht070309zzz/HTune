// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <d2d1_1.h>
#include <array>
#include <memory>
#include <string>
namespace stage {
enum class FoliaProgramKind {Tempera,Lumiere,Sonnet};
struct FoliaProgramInfo {
    size_t index=0,count=0;
    std::string name;
    float cx=.5f,cy=.5f,width=.72f,height=.5f,rotation=0,fontScale=1;
    float enterX=0,enterY=1,travel=0,zoomStart=1,zoomEnd=1;
    float travelX=0,travelY=0;
    int typography=0;
};
class FoliaPrograms {
public:
    FoliaPrograms();~FoliaPrograms();
    void Clear();
    FoliaProgramInfo Info(FoliaProgramKind,size_t variant)const;
    void Draw(ID2D1DeviceContext*,FoliaProgramKind,size_t variant,float elapsed,float duration,float width,float height,bool reduced,float energy,int quality,bool lightField=true);
    std::array<std::array<float,4>,52> LightConstants(size_t variant,float elapsed,float duration,float width,float height,bool reduced,float energy,int quality)const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
