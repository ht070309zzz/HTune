#include "theme_transition.h"
#include <algorithm>
#include <cmath>

namespace stage {
namespace {
bool Same(ThemeColor a,ThemeColor b){return a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a;}
ThemeColor Mix(ThemeColor a,ThemeColor b,float t){return {a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t};}
float Luminance(ThemeColor c){
    const auto linear=[](float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);};
    return .2126f*linear(c.r)+.7152f*linear(c.g)+.0722f*linear(c.b);
}
float Contrast(ThemeColor a,ThemeColor b){const auto x=Luminance(a),y=Luminance(b);return (std::max(x,y)+.05f)/(std::min(x,y)+.05f);}
float MinimumContrast(ThemeColor ink,const ThemePalette& p){return std::min(Contrast(ink,p.background),Contrast(ink,p.panel));}
ThemeColor TransitionInk(ThemeColor mixed,ThemeColor previous,ThemeColor next,const ThemePalette& p){
    if(MinimumContrast(mixed,p)>=4.5f)return mixed;
    auto best=mixed;float score=MinimumContrast(best,p);
    for(auto candidate:{previous,next,ThemeRgb(0x171717),ThemeRgb(0xFAFAFA)}){
        candidate.a=mixed.a;const auto current=MinimumContrast(candidate,p);
        if(current>score){score=current;best=candidate;}
    }
    return best;
}
ThemeColor SecondaryInk(ThemeColor mixed,ThemeColor ink,const ThemePalette& p){
    if(MinimumContrast(mixed,p)>=3.f||MinimumContrast(ink,p)<3.f)return mixed;
    float low=0,high=1;
    for(int i=0;i<8;++i){const auto mid=(low+high)*.5f;if(MinimumContrast(Mix(mixed,ink,mid),p)>=3.f)high=mid;else low=mid;}
    auto result=Mix(mixed,ink,high);result.a=mixed.a;return result;
}
}
ThemePalette PaletteOf(const ThemeDefinition& t){return {t.background,t.panel,t.ink,t.muted,t.accent,t.glow};}
void ApplyPalette(ThemeDefinition& t,const ThemePalette& p){t.background=p.background;t.panel=p.panel;t.ink=p.ink;t.muted=p.muted;t.accent=p.accent;t.glow=p.glow;}
bool SamePalette(const ThemePalette& a,const ThemePalette& b){return Same(a.background,b.background)&&Same(a.panel,b.panel)&&Same(a.ink,b.ink)&&Same(a.muted,b.muted)&&Same(a.accent,b.accent)&&Same(a.glow,b.glow);}
ThemePalette ThemeTransition::Sample(int64_t now) const {
    if(SamePalette(from_,target_))return target_;
    const auto elapsed=now>=startedAt_?now-startedAt_:0;
    if(elapsed>=durationMs_)return target_;
    if(!elapsed)return from_;
    float t=static_cast<float>(elapsed)/static_cast<float>(durationMs_);t=t*t*(3.f-2.f*t);
    ThemePalette p{Mix(from_.background,target_.background,t),Mix(from_.panel,target_.panel,t),Mix(from_.ink,target_.ink,t),
        Mix(from_.muted,target_.muted,t),Mix(from_.accent,target_.accent,t),Mix(from_.glow,target_.glow,t)};
    // Mixed light/dark themes can otherwise make ink and paper converge to
    // the same gray. These temporary choices never replace saved colors.
    p.ink=TransitionInk(p.ink,from_.ink,target_.ink,p);
    p.muted=SecondaryInk(p.muted,p.ink,p);p.accent=SecondaryInk(p.accent,p.ink,p);
    return p;
}
ThemePalette ThemeTransition::Update(const ThemePalette& target,int64_t now,bool reducedMotion){
    if(!initialized_){from_=target_=target;initialized_=true;startedAt_=now;return target_;}
    const auto visible=Sample(now);
    if(!SamePalette(target,target_)){
        from_=visible;target_=target;startedAt_=now;durationMs_=reducedMotion?150:650;
    }else if(reducedMotion&&durationMs_>150&&now>=startedAt_&&now-startedAt_<durationMs_){
        from_=visible;startedAt_=now;durationMs_=150;
    }
    return Sample(now);
}
}
