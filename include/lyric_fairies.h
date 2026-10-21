#pragma once
#include "glyph_effects.h"
#include <algorithm>
#include <cmath>

namespace stage {
// Historical names retained for the native comet layer.
inline constexpr size_t FairyCount=3,FairySamples=97;
struct FairyPoint {float x=0,y=0;double time=0;float speed=0;};
enum class CometIntent {Visit,Exchange,Chase,Relay};
struct LyricFairy {
    D2D1_POINT_2F position{},physical{},previous{},velocity{},from{},offset{};
    std::array<FairyPoint,FairySamples> history{};
    size_t count=0;
    uint32_t anchor=0;
    float tilt=0,size=1,bend=0;
};
class LyricFairies {
    struct Anchor {uint32_t key=0;D2D1_POINT_2F center{};float em=80;};
    std::array<Anchor,7> anchors_{};size_t anchorCount_=0;
    bool ready_=false;
    float media_=0,width_=0,height_=0,em_=80;int64_t sampleInterval_=200;
    int style_=-1,quality_=-1;
    int64_t wall_=0,lastAdvance_=0,lastFocus_=0;
    uint64_t stream_=0,phrase_=0,steps_=0;
    uint32_t random_=1,focusKey_=0;
    double time_=0,remainder_=0,phaseAt_=0,phaseDuration_=1;
    D2D1_POINT_2F target_{};
    float Random(){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return float(random_&0xffffff)/16777216.f;}
    static D2D1_POINT_2F Centre(const GlyphEmitter& e){D2D1_POINT_2F p{};for(auto q:e.corners){p.x+=q.x*.25f;p.y+=q.y*.25f;}return p;}
    static float Extent(const GlyphEmitter& e){return std::clamp(std::hypot(e.corners[3].x-e.corners[0].x,e.corners[3].y-e.corners[0].y),18.f,180.f);}
    static uint32_t Key(const GlyphEffectFrame& f,const GlyphEmitter& e){return uint32_t(std::clamp(e.seed,0.f,65535.f))*64+uint32_t(&e-f.emitters.data())+1;}
    void Record(LyricFairy& a){
        if(a.count==FairySamples){for(size_t j=1;j<a.count;++j)a.history[j-1]=a.history[j];--a.count;}
        a.history[a.count++]={a.physical.x,a.physical.y,time_,std::hypot(a.velocity.x,a.velocity.y)};
    }
    void Anchors(const GlyphEffectFrame& f,const GlyphEmitter& focus){
        focusKey_=Key(f,focus);anchors_[0]={focusKey_,target_,em_};anchorCount_=1;
        std::array<Anchor,MaxGlyphEmitters> nearbyAnchors{};size_t n=0;
        for(size_t i=0;i<std::min(f.count,MaxGlyphEmitters);++i){const auto& e=f.emitters[i];
            if(&e==&focus||e.padding!=0||e.color.a<.04f)continue;
            const auto p=Centre(e);const float d=std::hypot(p.x-target_.x,p.y-target_.y);
            if(d>em_*3.5f)continue;
            nearbyAnchors[n++]={Key(f,e),p,Extent(e)};
        }
        std::stable_sort(nearbyAnchors.begin(),nearbyAnchors.begin()+n,[&](const Anchor& a,const Anchor& b){
            return std::hypot(a.center.x-target_.x,a.center.y-target_.y)<std::hypot(b.center.x-target_.x,b.center.y-target_.y);});
        for(size_t i=0;i<n&&anchorCount_<anchors_.size();++i)anchors_[anchorCount_++]=nearbyAnchors[i];
    }
    uint32_t Neighbor(uint32_t avoid){
        if(anchorCount_<2)return focusKey_;
        const size_t start=1+std::min(anchorCount_-2,size_t(Random()*float(anchorCount_-1)));
        for(size_t j=0;j<anchorCount_;++j){const auto k=anchors_[(start+j)%anchorCount_].key;if(k!=avoid)return k;}
        return focusKey_;
    }
    const Anchor& Find(uint32_t key)const{for(size_t i=0;i<anchorCount_;++i)if(anchors_[i].key==key)return anchors_[i];return anchors_[0];}
    D2D1_POINT_2F Destination(size_t i)const{
        const auto& a=agents[i];const auto& anchor=Find(a.anchor);
        return {anchor.center.x+anchor.em*a.offset.x,anchor.center.y+anchor.em*a.offset.y};
    }
    void Start(CometIntent next){
        intent=next;phaseAt_=time_;
        phaseDuration_=next==CometIntent::Visit?.48+Random()*.36:.64+Random()*.40;
        if(next==CometIntent::Exchange){
            if(agents[0].anchor==agents[1].anchor){agents[0].anchor=focusKey_;agents[1].anchor=Neighbor(focusKey_);}
            std::swap(agents[0].anchor,agents[1].anchor);if(count>2)agents[2].anchor=Neighbor(focusKey_);leader=agents[0].anchor==focusKey_?0:1;++exchanges;
        }else{
            if(next==CometIntent::Relay){leader=(leader+1)%count;++relays;phaseDuration_=.64+Random()*.24;}
            if(next==CometIntent::Chase)++chases;
            if(next==CometIntent::Visit)++visits;
            agents[leader].anchor=focusKey_;
            for(size_t i=0;i<count;++i)if(i!=leader)agents[i].anchor=Neighbor(focusKey_);
        }
        for(size_t i=0;i<count;++i){auto& a=agents[i];a.from=a.physical;
            const float side=(i+exchanges)%2?1.f:-1.f;
            a.offset={(Random()-.5f)*.66f,side*(.65f+Random()*.24f)};
            // Opposite curved approaches permit an actual crossing/exchange.
            // Exchanged directions are reversed: equal curvature signs send the
            // pair around opposite sides. Opposite signs made them collide.
            const float curveSide=next==CometIntent::Exchange?(exchanges%2?1.f:-1.f):side;
            a.bend=curveSide*(.75f+Random()*.30f);
        }
    }
    D2D1_POINT_2F Goal(size_t i)const{
        const auto& a=agents[i];const float elapsed=static_cast<float>(time_-phaseAt_);
        if((intent==CometIntent::Chase||intent==CometIntent::Relay)&&i!=leader){
            if(intent==CometIntent::Relay&&elapsed<.12f)return a.from;
            const auto& front=agents[leader];
            if(front.count){const size_t rank=i<leader?i:i-1;const size_t delay=std::min(front.count-1,size_t(rank?16:10));
                const auto& past=front.history[front.count-1-delay];
                const float offset=(rank?-.22f:.22f)*em_;
                return {past.x-std::sin(front.tilt)*offset,past.y+std::cos(front.tilt)*offset};}
        }
        const auto destination=Destination(i);
        const float dx=destination.x-a.from.x,dy=destination.y-a.from.y,distance=std::hypot(dx,dy);
        const float u=std::clamp(elapsed/float(phaseDuration_*.74),0.f,1.f),p=u*u*(3-2*u);
        const float arc=std::sin(u*3.14159265f)*std::min(distance*.38f,em_*.9f)*a.bend;
        const float nx=distance>1?-dy/distance:0,ny=distance>1?dx/distance:1;
        return {a.from.x+dx*p+nx*arc,a.from.y+dy*p+ny*arc};
    }
    void Separate(){
        for(int pass=0;pass<2;++pass)for(size_t i=0;i<count;++i)for(size_t j=i+1;j<count;++j){auto& a=agents[i];auto& b=agents[j];
            float dx=a.physical.x-b.physical.x,dy=a.physical.y-b.physical.y,d=std::hypot(dx,dy);
            const float gap=em_*.28f;if(d>=gap)continue;
            if(d<.001f){dx=1;dy=0;d=1;}
            const float nx=dx/d,ny=dy/d,push=(gap-d)*.5f;
            a.physical.x+=nx*push;a.physical.y+=ny*push;b.physical.x-=nx*push;b.physical.y-=ny*push;
            const float approach=(a.velocity.x-b.velocity.x)*nx+(a.velocity.y-b.velocity.y)*ny;
            if(approach<0){a.velocity.x-=nx*approach*.5f;a.velocity.y-=ny*approach*.5f;b.velocity.x+=nx*approach*.5f;b.velocity.y+=ny*approach*.5f;}
        }
    }
    void Clamp(const GlyphEffectFrame& f,LyricFairy& a){
        a.physical.x=std::clamp(a.physical.x,f.clip.left+2.f,std::max(f.clip.left+2.f,f.clip.right-2.f));
        a.physical.y=std::clamp(a.physical.y,f.clip.top+2.f,std::max(f.clip.top+2.f,f.clip.bottom-2.f));
    }
    void Present(const GlyphEffectFrame& f){
        // Substeps can alternate 1/3 times with millisecond frame stamps.
        // Predict only the fractional display interval, never lyric time.
        const float fraction=static_cast<float>(remainder_*120.0);
        for(size_t i=0;i<count;++i){auto& a=agents[i];
            a.position={a.physical.x+(a.physical.x-a.previous.x)*fraction,a.physical.y+(a.physical.y-a.previous.y)*fraction};
            a.position.x=std::clamp(a.position.x,f.clip.left+2.f,std::max(f.clip.left+2.f,f.clip.right-2.f));
            a.position.y=std::clamp(a.position.y,f.clip.top+2.f,std::max(f.clip.top+2.f,f.clip.bottom-2.f));
        }
    }
public:
    std::array<LyricFairy,FairyCount> agents{};
    D2D1_COLOR_F color{};
    size_t count=0,leader=0;
    CometIntent intent=CometIntent::Visit;
    uint32_t exchanges=0,chases=0,relays=0,visits=0;
    float opacity=1;
    double Time()const{return time_+remainder_;}
    void Reset(){ready_=false;count=leader=0;time_=remainder_=phaseAt_=0;steps_=0;opacity=1;anchorCount_=0;
        exchanges=chases=relays=visits=0;for(auto& a:agents)a={};}
    void Step(const GlyphEffectFrame& f){
        if(f.reduced||!std::isfinite(f.seconds)||!std::isfinite(f.width)||!std::isfinite(f.height)||f.width<=0||f.height<=0){Reset();return;}
        const GlyphEmitter *focus=nullptr,*completed=nullptr,*first=nullptr,*dedicated=nullptr;
        for(size_t i=0;i<std::min(f.count,MaxGlyphEmitters);++i){const auto& e=f.emitters[i];
            if(e.padding==2){dedicated=&e;continue;}if(e.padding!=0)continue;
            if(!first)first=&e;if(e.progress>0&&e.progress<1&&!focus)focus=&e;if(e.progress>=1)completed=&e;}
        if(!focus)focus=completed?completed:first;if(!focus)focus=dedicated;
        const double elapsed=ready_?double(f.presentationMs-wall_)*.001:0;const float mediaDelta=f.seconds-media_;
        const bool discontinuity=f.reset||f.stream!=stream_||f.style!=style_||f.quality!=quality_||
            f.width!=width_||f.height!=height_||elapsed<0||elapsed>.5||
            (ready_&&(mediaDelta<-.08f||mediaDelta>std::max(.75,std::max(0.,elapsed)+.5)));
        if(!focus&&(!ready_||discontinuity||!f.allowGap||f.presentationMs-lastFocus_>240)){Reset();return;}
        const bool initialize=!ready_||discontinuity;
        if(initialize){Reset();style_=f.style;quality_=f.quality;stream_=f.stream;phrase_=f.phrase;width_=f.width;height_=f.height;
            random_=uint32_t(style_+1)*7919+271;lastAdvance_=f.presentationMs-1000;lastFocus_=f.presentationMs;sampleInterval_=200;}
        if(mediaDelta>.0005f&&!initialize){sampleInterval_=std::clamp<int64_t>(f.presentationMs-lastAdvance_,16,750);lastAdvance_=f.presentationMs;}
        const bool moving=!f.paused&&(f.advancing||(f.sampled&&f.presentationMs-lastAdvance_<=std::clamp<int64_t>(sampleInterval_*2+30,180,750)));
        wall_=f.presentationMs;media_=f.seconds;if(!initialize&&!moving)return;
        if(focus){target_=Centre(*focus);em_=Extent(*focus);
            if(focus->padding==0){const float p=std::clamp(focus->progress,0.f,1.f)-.5f;
                target_.x+=(focus->corners[1].x-focus->corners[0].x)*p*.45f;
                target_.y+=(focus->corners[1].y-focus->corners[0].y)*p*.45f;}
            Anchors(f,*focus);lastFocus_=f.presentationMs;color=f.accent;
        }
        opacity=1-std::clamp(float(f.presentationMs-lastFocus_-80)/160.f,0.f,1.f);
        if(initialize){
            // Interaction requires a pair at every quality tier. Low quality
            // saves tail geometry/dust instead of removing the partner.
            count=f.quality<2&&dedicated?3:2;Start(CometIntent::Visit);
            for(size_t i=0;i<count;++i){auto& a=agents[i];a.physical=Destination(i);Clamp(f,a);a.from=a.physical;
                a.size=std::clamp(em_*.1125f,6.f,15.f)*(i==1?.9f:1.f);}
            Separate();for(size_t i=0;i<count;++i){Clamp(f,agents[i]);agents[i].previous=agents[i].physical;Record(agents[i]);}Start(CometIntent::Visit);ready_=true;Present(f);return;
        }
        if(focus&&f.phrase!=phrase_){phrase_=f.phrase;Start(CometIntent::Relay);}
        else if(intent!=CometIntent::Exchange)agents[leader].anchor=focusKey_;
        constexpr double h=1.0/120.0;remainder_+=std::clamp(elapsed,0.,.1);
        while(remainder_+1e-9>=h){remainder_-=h;time_+=h;++steps_;for(size_t i=0;i<count;++i)agents[i].previous=agents[i].physical;
            if(time_-phaseAt_>=phaseDuration_){
                const float pick=Random();auto next=pick<.38f?CometIntent::Visit:pick<.70f?CometIntent::Exchange:CometIntent::Chase;
                if(next==intent)next=next==CometIntent::Visit?CometIntent::Chase:CometIntent::Visit;
                Start(next);
            }
            // Evaluate both goals before moving either agent, so pursuit uses
            // actual recorded history and is independent of array update order.
            std::array<D2D1_POINT_2F,FairyCount> goals{};for(size_t i=0;i<count;++i)goals[i]=Goal(i);
            for(size_t i=0;i<count;++i){auto& a=agents[i];const auto goal=goals[i];
                const float dx=goal.x-a.physical.x,dy=goal.y-a.physical.y,distance=std::hypot(dx,dy);
                const float stiffness=125.f+float(i)*10.f,damping=19.f+float(i);
                a.velocity.x+=(dx*stiffness-a.velocity.x*damping)*float(h);a.velocity.y+=(dy*stiffness-a.velocity.y*damping)*float(h);
                const float speed=std::hypot(a.velocity.x,a.velocity.y),limit=std::min(em_*(intent==CometIntent::Relay?10.f:7.f),std::min(std::hypot(width_,height_)*2.f,std::max(em_*5.f,distance*5.f)));
                if(speed>limit){a.velocity.x*=limit/speed;a.velocity.y*=limit/speed;}
                a.physical.x+=a.velocity.x*float(h);a.physical.y+=a.velocity.y*float(h);
                if(speed>1)a.tilt=std::atan2(a.velocity.y,a.velocity.x);
                a.size+=(std::clamp(em_*.1125f,6.f,15.f)*(i==1?.9f:1.f)-a.size)*float(h)*8.f;
            }
            Separate();for(size_t i=0;i<count;++i)Clamp(f,agents[i]);
            if(steps_%2==0)for(size_t i=0;i<count;++i)Record(agents[i]);
        }
        Present(f);
    }
};
}
