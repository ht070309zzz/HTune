#pragma once
#include <algorithm>
#include <cmath>
#include <d2d1.h>
namespace stage {
// Cinema's luminous ink, with the theme's hue retained. Background palettes
// and particle colours remain independent of this lyric-only adjustment.
inline D2D1_COLOR_F CinemaLyricLight(D2D1_COLOR_F c,float minimum){
    const auto linear=[](float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);};
    for(int i=0;i<8&&linear(c.r)*.2126f+linear(c.g)*.7152f+linear(c.b)*.0722f<minimum;++i){
        c.r+=(1-c.r)*.2f;c.g+=(1-c.g)*.2f;c.b+=(1-c.b)*.2f;}
    return c;
}
// Shared by the DirectWrite and atlas paths: a close soft edge and a broad
// diffusion layer. Radius is in DIPs and gain is applied after the blur.
inline float LyricGlowRadius(float em){return std::clamp(em*.105f,3.f,20.f);}
// Keep diffuse light subordinate to the ink. Both layers previously amplified
// the same mask above unity; dense strokes saturated into white discs. Attenuate
// after blur so the radius/falloff stays soft without brightening other effects.
inline float LyricGlowGain(bool wide,bool light){return light?(wide?.21f:.40f):(wide?.63f:.74f);}
inline D2D1_COLOR_F LyricGlowTint(D2D1_COLOR_F c,bool light){
    if(!light){const float luminance=c.r*.2126f+c.g*.7152f+c.b*.0722f;
        const float white=std::clamp((.68f-luminance)/std::max(.01f,1-luminance),0.f,.64f);
        c.r+=(1-c.r)*white;c.g+=(1-c.g)*white;c.b+=(1-c.b)*white;}
    return c;
}
}
