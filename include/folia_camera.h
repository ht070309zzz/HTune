// SPDX-License-Identifier: AGPL-3.0-only
// Native adaptation of Folia's Fume camera. See third_party/folia/NOTICE.md.
#pragma once
#include <cstdint>

namespace stage::folia {
struct Point {float x=0,y=0;};
struct Camera {float x=0,y=0,scale=1,velocityX=0,velocityY=0,velocityScale=0,focusX=0,focusY=0,focusScale=1;};
struct CameraInput {
    Point target,entry;
    float scale=1,width=1,height=1,dt=0,duration=.15f;
    double time=0;
    uint64_t block=0;
    bool reset=false,staticMode=false,playing=true;
};
class CameraMotion {
public:
    void Step(const CameraInput&);
    const Camera& View()const{return camera_;}
    bool Flying()const{return bridge_&&phase_<1;}
private:
    Camera camera_;
    uint64_t block_=0;
    double started_=0;
    float duration_=.15f,fromX_=0,fromY_=0,fromScale_=1,phase_=1;
    bool initialized_=false,bridge_=false;
};
}
