// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-FileCopyrightText: Folia contributors
// SPDX-FileCopyrightText: 2026 HTune contributors (native adaptation)
// Derived from chthollyphile/folia-major, commit 26c57f9b1b26b402a09e8943171a332215418ce6:
// src/components/visualizer/fume/fumeCameraStep.ts and fumeMath.ts.
// Changes: C++ value types, caller-supplied native geometry, playback-time float,
// bounded substeps, explicit pause/static/seek handling; no React/Canvas runtime.
#include "folia_camera.h"
#include <algorithm>
#include <cmath>

namespace stage::folia {
namespace {
float Mix(float a,float b,float t){return a+(b-a)*t;}
float Out(float t){return 1-std::pow(1-std::clamp(t,0.f,1.f),3.f);}
float InOut(float t){t=std::clamp(t,0.f,1.f);return t<.5f?4*t*t*t:1-std::pow(-2*t+2,3.f)/2;}
}
void CameraMotion::Step(const CameraInput& input){
    const float targetScale=std::clamp(input.scale,.22f,2.24f);
    if(!initialized_||input.reset||input.staticMode){
        camera_={input.target.x,input.target.y,targetScale,0,0,0,input.target.x,input.target.y,targetScale};
        initialized_=true;block_=input.block;started_=input.time;bridge_=false;phase_=1;return;
    }
    if(!input.playing)return;
    const bool retarget=block_!=input.block;
    if(retarget){
        block_=input.block;started_=input.time;duration_=std::clamp(input.duration,.075f,.65f);
        fromX_=camera_.x;fromY_=camera_.y;fromScale_=camera_.scale;
        const float scale=std::max({camera_.scale,targetScale,.001f});
        bridge_=std::hypot(input.target.x-fromX_,input.target.y-fromY_)*scale>=std::min(input.width,input.height)*.42f;
    }
    phase_=std::clamp(static_cast<float>(input.time-started_)/std::max(duration_,.001f),0.f,1.f);
    const float boost=1-Out(phase_),entryBias=std::pow(boost,.58f);
    float tx=Mix(input.target.x,input.entry.x,entryBias),ty=Mix(input.target.y,input.entry.y,entryBias),ts=targetScale;
    const float floatPhase=static_cast<float>(input.time/7.*6.283185307179586);
    tx-=std::sin(floatPhase*.74f+.8f)*18.f*.34f/std::max(ts,.001f);
    ty-=(std::sin(floatPhase)*18.f+std::sin(floatPhase*.5f+1.1f)*18.f*.22f)/std::max(ts,.001f);
    ts=std::clamp(ts*(1+std::sin(floatPhase+.9f)*.011f),.22f,2.24f);
    // Preserve the Fume retarget bridge and spring math; integrate at bounded
    // steps so a delayed native frame cannot destabilize its explicit spring.
    float remaining=std::clamp(input.dt,0.f,.1f);
    while(remaining>0){const float dt=std::min(remaining,1.f/120.f);remaining-=dt;
        if(bridge_&&phase_<1){
            const float t=InOut(phase_),x=Mix(fromX_,tx,t),y=Mix(fromY_,ty,t),s=Mix(fromScale_,ts,t);
            const float catchUp=1-std::exp(-dt*Mix(10.5f,17.5f,1-phase_));
            camera_.focusX=x;camera_.focusY=y;camera_.focusScale=s;
            camera_.x+=(x-camera_.x)*catchUp;camera_.y+=(y-camera_.y)*catchUp;camera_.scale+=(s-camera_.scale)*catchUp;
            const float decay=std::pow(.72f,dt*60),scaleDecay=std::pow(.68f,dt*60);
            camera_.velocityX*=decay;camera_.velocityY*=decay;camera_.velocityScale*=scaleDecay;
        }else{
            const float rate=std::clamp(4.8f/std::max(duration_,.05f),20.f,54.f);
            const float catchUp=1-std::exp(-dt*Mix(11.2f,rate,boost));
            camera_.focusX+=(tx-camera_.focusX)*catchUp;camera_.focusY+=(ty-camera_.focusY)*catchUp;
            camera_.focusScale+=(ts-camera_.focusScale)*(1-std::exp(-dt*Mix(5.4f,12.8f,boost)));
            const float strength=Mix(208.f,std::clamp(15.8f/std::max(duration_*duration_,.0064f),260.f,780.f),boost);
            const float damping=Mix(24.f,std::clamp(std::sqrt(strength)*1.36f,24.f,40.f),boost);
            const float ax=(camera_.focusX-camera_.x)*strength-camera_.velocityX*damping;
            const float ay=(camera_.focusY-camera_.y)*strength-camera_.velocityY*damping;
            camera_.velocityX+=ax*dt;camera_.velocityY+=ay*dt;
            const float distance=std::hypot(tx-camera_.x,ty-camera_.y);
            const float maxVelocity=Mix(1320.f,std::clamp(distance/std::max(duration_*.28f,.028f),2600.f,8800.f),boost);
            camera_.velocityX=std::clamp(camera_.velocityX,-maxVelocity,maxVelocity);
            camera_.velocityY=std::clamp(camera_.velocityY,-maxVelocity,maxVelocity);
            camera_.x+=camera_.velocityX*dt;camera_.y+=camera_.velocityY*dt;
            const float as=(camera_.focusScale-camera_.scale)*Mix(54.f,108.f,boost)-camera_.velocityScale*Mix(13.5f,21.f,boost);
            camera_.velocityScale=std::clamp(camera_.velocityScale+as*dt,-1.6f,1.6f);camera_.scale+=camera_.velocityScale*dt;
        }
        camera_.scale=std::clamp(camera_.scale,.22f,2.24f);
    }
}
}
