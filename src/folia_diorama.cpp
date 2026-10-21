// SPDX-License-Identifier: AGPL-3.0-only
// Folia cameraPath.ts path/placement/SmoothDamp port and original formation data.
// Copyright Folia contributors; native adaptation by HTune contributors.
#include "folia_diorama.h"
#include "rift_stage.h"
#include "resource.h"
#include "motion_resource.h"
#include "third_party/json.hpp"
#include <d2d1_1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <array>
#include <stdexcept>
namespace stage {
namespace {
using Json=nlohmann::json;using Microsoft::WRL::ComPtr;
constexpr float Pi=3.141592653589793f;
struct V {float x=0,y=0,z=0;V operator+(V b)const{return {x+b.x,y+b.y,z+b.z};}V operator-(V b)const{return {x-b.x,y-b.y,z-b.z};}V operator*(float b)const{return {x*b,y*b,z*b};}};
float Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}V Cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V Norm(V a){return a*(1/std::max(.00001f,std::sqrt(Dot(a,a))));}
float Unit(float x){return std::clamp(x,0.f,1.f);}float Smooth(float x){x=Unit(x);return x*x*(3-2*x);}
float Random(float seed){const double a=std::sin(double(seed)*12.9898)*43758.5453;return float(a-std::floor(a));}
float Damp(float current,float target,float& velocity,float dt){const float omega=2/.42f,x=omega*dt,e=1/(1+x+.48f*x*x+.235f*x*x*x),change=current-target,temp=(velocity+omega*change)*dt;velocity=(velocity-omega*temp)*e;return target+(change+temp)*e;}
V From(const Json& p){return {p.at("x"),p.at("y"),p.at("z")};}

}
struct FoliaDiorama::Impl {
    struct Sample {float right=0,up=0,back=0;};
    struct Shape {V position;float scale=1,stretch=1;int kind=0;};
    struct RecipeData {std::vector<Sample> curve;std::vector<Shape> shapes;};
    MotionRecipes catalog{IDR_FOLIA_DIORAMA,"frames"};
    std::vector<std::unique_ptr<RecipeData>> recipes;
    const RecipeData& Recipe(size_t index){
        if(recipes.empty())recipes.resize(catalog.Size());index%=recipes.size();
        if(!recipes[index]){auto data=std::make_unique<RecipeData>();const auto json=catalog.Load(index);
            for(const auto& v:json.at("curve"))data->curve.push_back({v.at("right"),v.at("up"),v.at("back")});
            if(data->curve.size()!=121)throw std::runtime_error("Invalid Diorama curve size");
            for(const auto& v:json.at("shapes")){const auto kind=v.at("kind").get<std::string>();data->shapes.push_back({From(v.at("position")),v.at("scale"),v.at("stretchY"),kind=="sphere"?0:kind=="torus"?1:2});}
            recipes[index]=std::move(data);
        }return *recipes[index];
    }
    void Clear(){recipes.clear();frames.clear();identity.clear();lastPosition=-1;brush.Reset();factory.Reset();}
    struct Frame {V p,forward,right,up;float offsetR=0,offsetU=0,scale=1,roll=0,yaw=0,lookR=0;int shot=0,seed=0;};
    std::vector<Frame> frames;V eye,aim,velocity,aimVelocity;float width=1,height=1;int64_t lastPosition=-1;size_t active=0;uint64_t revision=0;std::wstring identity;
    ComPtr<ID2D1SolidColorBrush> brush;ComPtr<ID2D1Factory> factory;
    V Local(const Frame& f,float r,float u,float d)const{return f.p+f.right*r+f.up*u+f.forward*d;}
    int Shot(const RiftFrame& f,size_t i)const{
        if(f.variant>=0)return f.variant%13;
        const auto& line=f.lyrics->lines[i];std::array<float,13> w={1,1,1,2.1f,1.9f,1.9f,.85f,.9f,1.8f,.9f,1.4f,1.7f,1.6f};
        const size_t count=line.words.size();if(count>=8){w[3]+=1.6f;w[1]+=1;w[2]-=.4f;w[0]-=.3f;}else if(count<=3){w[0]+=1.4f;w[2]+=.9f;w[5]+=.5f;w[3]-=.8f;}
        int64_t longest=0,sum=0;for(const auto& word:line.words){longest=std::max(longest,word.durationMs);sum+=word.durationMs;}
        if(longest>=1300){w[6]+=2.4f;w[7]+=1;w[11]+=1.2f;w[8]+=.6f;w[2]+=.8f;w[10]+=.6f;w[5]+=.3f;w[3]-=.5f;}
        if(count>=4&&sum>0&&sum/static_cast<int64_t>(count)<=340){w[3]+=1.4f;w[9]+=1.5f;w[0]+=.8f;w[6]-=1.6f;w[5]-=.8f;w[4]-=.4f;}
        uint32_t hash=0;for(auto c:line.text)hash=hash*31+c;float total=0;for(auto& v:w){v=std::max(.05f,v);total+=v;}float pick=Random(float(hash%100000))*total;
        for(int n=0;n<13;++n){pick-=w[n];if(pick<=0)return n;}return 5;
    }
    void Build(const RiftFrame& f){frames.clear();if(!f.lyrics)return;float seed=1;for(auto c:f.identity)seed=std::fmod(seed*31+c,100000.f);V p{};
        for(size_t i=0;i<f.lyrics->lines.size();++i){const float index=static_cast<float>(i),yaw=.5f*(std::sin(index*.23f+seed*.017f)*.62f+std::sin(index*.11f+seed*.041f+1.3f)*.38f),pitch=.32f*std::sin(index*.17f+seed*.029f+.7f);
            Frame frame;frame.p=p;frame.forward={std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};frame.right=Norm(Cross(frame.forward,{0,1,0}));frame.up=Norm(Cross(frame.right,frame.forward));
            frame.offsetR=(std::sin(index*.47f+seed*.019f)*.7f+(Random(seed+index*11+2)-.5f)*.6f)*1.8f;
            frame.offsetU=(std::sin(index*.29f+seed*.027f+.9f)*.7f+(Random(seed+index*11+4)-.5f)*.6f)*1.2f;
            frame.scale=.82f+Random(seed+index*11+5)*.46f;frame.roll=(Random(seed+index*11+6)-.5f)*.4f;frame.yaw=(Random(seed+index*11+7)-.5f)*.32f;frame.lookR=(Random(seed+index*11+8)-.5f)*2.2f;
            frame.seed=static_cast<int>(Random(seed+index*97)*16)%16;frame.shot=Shot(f,i);if(f.variant<0&&i&&frame.shot==frames.back().shot)frame.shot=(frame.shot+1+frame.seed%11)%13;
            frames.push_back(frame);p=p+frame.forward*8;}
    }
    DioramaProjection Project(V p)const{const auto z=Norm(eye-aim),x=Norm(Cross({0,1,0},z)),y=Cross(z,x);const auto d=p-eye;const float depth=-Dot(d,z);if(depth<.15f||depth>40)return {};
        const float scale=height/(2*std::tan(.68f*.5f)*depth);return {width*.5f+Dot(d,x)*scale,height*.47f-Dot(d,y)*scale,scale,true};}
    V TextWorld(size_t line,float x,float y)const{const auto& f=frames[std::min(line,frames.size()-1)];const float c=std::cos(f.roll),s=std::sin(f.roll),r=(x*c-y*s)*f.scale,u=(x*s+y*c)*f.scale;
        return Local(f,f.offsetR+r*std::cos(f.yaw),f.offsetU+u,r*std::sin(f.yaw));}
    void Step(const RiftFrame& f,size_t line,float word,bool reset){if(reset||identity!=f.identity||revision!=f.revision||frames.size()!=(f.lyrics?f.lyrics->lines.size():0)){Build(f);identity=f.identity;revision=f.revision;reset=true;}
        width=f.width;height=f.height;active=std::min(line,frames.empty()?size_t(0):frames.size()-1);if(frames.empty())return;const auto& frame=frames[active];const auto& lyric=f.lyrics->lines[active];
        const float progress=f.reduced||!f.following?.5f:Unit(float(f.positionMs-lyric.startMs)/float(std::max<int64_t>(1,lyric.durationMs)));
        const auto& curve=Recipe(static_cast<size_t>(frame.seed*13+frame.shot)).curve;const float at=progress*120;const size_t n=std::min<size_t>(119,static_cast<size_t>(at));
        auto value=[&](float Sample::*key){return curve[n].*key+(curve[n+1].*key-curve[n].*key)*(at-float(n));};
        const float motion=f.reduced||!f.following?0:std::clamp(f.camera,0.f,2.f);const float right=value(&Sample::right)*motion,up=.5f+(value(&Sample::up)-.5f)*motion,back=5.2f+(value(&Sample::back)-5.2f)*motion;
        const float truck=f.reduced?0:std::tanh((word-.5f)*2)*.45f;
        const auto targetAim=Local(frame,frame.offsetR+truck+frame.lookR*.4f,frame.offsetU,0),targetEye=Local(frame,frame.offsetR+truck+right,frame.offsetU+up,-back);
        const float dt=f.deltaSeconds;lastPosition=f.positionMs;
        if(reset||f.reduced||!f.following){eye=targetEye;aim=targetAim;velocity=aimVelocity={};}
        else if(f.playing){eye={Damp(eye.x,targetEye.x,velocity.x,dt),Damp(eye.y,targetEye.y,velocity.y,dt),Damp(eye.z,targetEye.z,velocity.z,dt)};
            aim={Damp(aim.x,targetAim.x,aimVelocity.x,dt),Damp(aim.y,targetAim.y,aimVelocity.y,dt),Damp(aim.z,targetAim.z,aimVelocity.z,dt)};}
    }
    void Draw(ID2D1DeviceContext* dc,const RiftFrame& f){if(frames.empty())return;if(!brush){dc->GetFactory(&factory);if(FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1,1,1),&brush)))return;}
        const float illumination=1+(f.reduced?0:.45f*std::sqrt(std::clamp((f.low+f.rms)*f.response,0.f,1.f)));
        const size_t begin=active>6?active-6:0,end=std::min(frames.size(),active+8);
        for(size_t i=end;i-->begin;){const auto& fr=frames[i];const auto& recipe=Recipe(static_cast<size_t>(fr.seed*13+fr.shot));
            for(const auto& shape:recipe.shapes){const auto local=shape.position;const auto world=Local(fr,fr.offsetR+local.x,fr.offsetU+local.y,-local.z);const auto projected=Project(world);if(!projected.visible)continue;
                const float distance=std::sqrt(Dot(world-eye,world-eye)),life=Smooth((27-distance)/7)*Smooth((distance-1.2f)/1.8f);if(life<=0)continue;
                const float radius=shape.scale*projected.scale,stretch=shape.stretch;auto color=f.accent;color.a=life*(.12f+.16f*(f.reduced?0:f.low));brush->SetColor(color);
                const auto kind=shape.kind;if(kind==0){dc->FillEllipse(D2D1::Ellipse({projected.x,projected.y},radius,radius),brush.Get());color.a=life*.45f*illumination;brush->SetColor(color);dc->DrawEllipse(D2D1::Ellipse({projected.x-radius*.12f,projected.y-radius*.12f},radius*.92f,radius*.92f),brush.Get(),1.2f);}
                else if(kind==1){dc->DrawEllipse(D2D1::Ellipse({projected.x,projected.y},radius,radius*.72f),brush.Get(),std::max(1.f,radius*.12f));}
                else {const float r=shape.scale,h=r*stretch;std::array<D2D1_POINT_2F,8> corners{};bool okay=true;
                    for(int v=0;v<8;++v){const auto q=Project(world+fr.right*(v&1?r:-r)+fr.up*(v&2?h:-h)+fr.forward*(v&4?r:-r));corners[v]={q.x,q.y};okay&=q.visible;}
                    if(!okay)continue;constexpr int edges[][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};color.a=life*.35f*illumination;brush->SetColor(color);for(const auto& e:edges)dc->DrawLine(corners[e[0]],corners[e[1]],brush.Get(),std::max(.6f,f.height*.001f));}
            }
        }
    }
};
FoliaDiorama::FoliaDiorama():impl_(std::make_unique<Impl>()){}FoliaDiorama::~FoliaDiorama()=default;
void FoliaDiorama::Clear(){impl_->Clear();}
void FoliaDiorama::Step(const RiftFrame& f,size_t line,float word,bool reset){impl_->Step(f,line,word,reset);}void FoliaDiorama::Draw(ID2D1DeviceContext* dc,const RiftFrame& f){impl_->Draw(dc,f);}
DioramaProjection FoliaDiorama::Text(size_t line,float x,float y)const{return impl_->frames.empty()?DioramaProjection{}:impl_->Project(impl_->TextWorld(line,x,y));}
}
