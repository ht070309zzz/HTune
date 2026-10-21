#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace stage {
struct GlyphMark {float x,y,w,h,alpha;bool particle=false;};
struct GlyphOrnament {std::array<GlyphMark,24> marks{};size_t count=0;};
// Small, letter-local hardware-like corner details. Geometry belongs to the
// glyph, and active syllables emit a few nearby motes; no audio-driven scaling.
inline GlyphOrnament MakeGlyphOrnament(uint32_t seed,float seconds,float active,bool reduced,int quality){
    GlyphOrnament out;auto add=[&](float x,float y,float w,float h,float a,bool dot=false){out.marks[out.count++]={x,y,w,h,a,dot};};
    const float opacity=.34f+.50f*active;const int variant=seed%4;
    for(int k=0;k<4;++k){const float x=k%2?.96f:.04f,y=k/2?.96f:.04f;
        const float sx=k%2?-1.f:1.f,sy=k/2?-1.f:1.f;
        const float span=.12f+.04f*((seed+k)%3);
        if(variant!=1||k==0||k==3){add(x+sx*span*.5f,y,span,.011f,opacity);add(x,y+sy*span*.5f,.011f,span,opacity);}
        if((seed+k)%3==0)add(x+sx*.025f,y+sy*.08f,.007f,.035f,opacity*.55f);
    }
    if(variant==2){add(.5f,.04f,.16f,.006f,opacity*.6f);add(.5f,.96f,.08f,.006f,opacity*.6f);}
    if(!reduced&&active>.01f){const int count=quality>=2?2:4;
        for(int k=0;k<count;++k){const float phase=seconds*(.8f+.11f*k)+float(seed%97)*.017f+k*.271f;
            const float t=phase-std::floor(phase),life=std::sin(t*3.14159265f);
            const float side=k%2?1.f:-1.f,x=.5f+side*(.47f+.09f*t),y=.78f-t*.62f+std::sin(phase*3.f)*.025f;
            const float size=.013f+.009f*life;add(x,y,size,size,life*active*.8f,true);}
    }
    return out;
}
}
