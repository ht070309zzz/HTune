// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-FileCopyrightText: 2026 chthollyphile and Folia contributors
// SPDX-FileCopyrightText: 2026 HTune contributors (native adaptation)
// Original composition/rig recipes: Folia 26c57f9b1b26b402a09e8943171a332215418ce6.
// Tempera item scheduling adapted from temperaBlocks.ts; drawing uses native D2D
// and the complete offline-exported original recipe data. No JS runtime is used.
#include "folia_programs.h"
#include "resource.h"
#include "motion_resource.h"
#include "third_party/json.hpp"
#include <d2d1_1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace stage {
namespace {
using Json=nlohmann::json;using Microsoft::WRL::ComPtr;
constexpr float Pi=3.14159265358979323846f;
float Unit(float v){return std::clamp(v,0.f,1.f);}
float InOut(float v){v=Unit(v);return v<.5f?4*v*v*v:1-std::pow(-2*v+2,3.f)/2;}
float Enter(float v){return 1-std::pow(1-Unit(v),3.f);}
void Check(HRESULT h){if(FAILED(h))throw std::runtime_error("Native Folia program drawing failed");}
float N(const Json& v,const char* key,float fallback=0){const auto it=v.find(key);return it!=v.end()&&it->is_number()?it->get<float>():fallback;}
D2D1_COLOR_F Color(uint32_t c,float a=1){return D2D1::ColorF(c,a);}
float Seed(size_t n){auto v=static_cast<uint32_t>(n)*747796405u+2891336453u;v=((v>>((v>>28)+4))^v)*277803737u;return static_cast<float>((v>>22)^v)/4294967295.f;}
}
struct FoliaPrograms::Impl {
    MotionRecipes tempera{IDR_MOTION_TEMPERA,"frames"},lumiere{IDR_MOTION_LUMIERE,"profiles"},sonnet{IDR_FOLIA_SONNET,"frames"};
    Json activeRecipe;FoliaProgramKind recipeKind=FoliaProgramKind::Tempera;size_t recipeIndex=SIZE_MAX;
    const MotionRecipes& Catalog(FoliaProgramKind kind)const{return kind==FoliaProgramKind::Tempera?tempera:kind==FoliaProgramKind::Lumiere?lumiere:sonnet;}
    const Json& Recipe(FoliaProgramKind kind,size_t index){index%=Catalog(kind).Size();
        if(index!=recipeIndex||kind!=recipeKind){
            if(kind!=recipeKind){nodes.clear();cached=SIZE_MAX;sonnetNodes.clear();sonnetCached=SIZE_MAX;}
            activeRecipe=Catalog(kind).Load(index);recipeKind=kind;recipeIndex=index;
        }return activeRecipe;}
    void Clear(){activeRecipe=Json{};recipeIndex=SIZE_MAX;nodes.clear();cached=SIZE_MAX;sonnetNodes.clear();sonnetCached=SIZE_MAX;}
    struct Op{ComPtr<ID2D1PathGeometry> shape;D2D1_COLOR_F color{};float stroke=0,delay=0,span=.8f,length=0;std::vector<D2D1_POINT_2F> points;};
    struct Node{std::vector<Op> ops;float x=0,y=0,px=0,py=0,rotation=0,dx=0,dy=0,delay=0,span=.45f,alpha=1,phase=0;int group=-1;bool drift=false,grow=false;};
    ComPtr<ID2D1Factory> factory;ComPtr<ID2D1SolidColorBrush> brush;ComPtr<ID2D1RadialGradientBrush> glow;
    std::vector<Node> nodes;size_t cached=SIZE_MAX;
    struct SonnetNode {std::vector<Op> ops;D2D1_MATRIX_3X2_F transform=D2D1::Matrix3x2F::Identity();float alpha=1;};
    std::vector<SonnetNode> sonnetNodes;size_t sonnetCached=SIZE_MAX;
    void Create(ID2D1DeviceContext* dc){if(brush)return;dc->GetFactory(&factory);Check(dc->CreateSolidColorBrush(Color(0xffffff),&brush));
        const D2D1_GRADIENT_STOP stops[]={{0,Color(0xffffff,.48f)},{.22f,Color(0xffffff,.13f)},{1,Color(0xffffff,0)}};
        ComPtr<ID2D1GradientStopCollection> collection;Check(dc->CreateGradientStopCollection(stops,3,&collection));
        Check(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0,0},{0,0},1,1),collection.Get(),&glow));}
    void Path(ID2D1GeometrySink* sink,const Json& commands,bool filled){
        bool open=false;auto finish=[&](D2D1_FIGURE_END end){if(open){sink->EndFigure(end);open=false;}};
        auto begin=[&](float x,float y){finish(D2D1_FIGURE_END_OPEN);sink->BeginFigure({x,y},filled?D2D1_FIGURE_BEGIN_FILLED:D2D1_FIGURE_BEGIN_HOLLOW);open=true;};
        for(const auto& command:commands){const auto name=command.at(0).get<std::string>();
            if(name=="poly"){const auto& p=command.at(1);if(p.size()<4)continue;begin(p[0].get<float>(),p[1].get<float>());
                for(size_t i=2;i+1<p.size();i+=2)sink->AddLine({p[i].get<float>(),p[i+1].get<float>()});finish(command.size()<3||command[2].get<bool>()?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);}
            else if(name=="rect"){const float x=command[1],y=command[2],w=command[3],h=command[4];begin(x,y);sink->AddLine({x+w,y});sink->AddLine({x+w,y+h});sink->AddLine({x,y+h});finish(D2D1_FIGURE_END_CLOSED);}
            else if(name=="circle"){const float x=command[1],y=command[2],r=command[3];if(r<=0)continue;begin(x+r,y);
                sink->AddArc(D2D1::ArcSegment({x-r,y},{r,r},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
                sink->AddArc(D2D1::ArcSegment({x+r,y},{r,r},0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));finish(D2D1_FIGURE_END_CLOSED);}
            else if(name=="move")begin(command[1].get<float>(),command[2].get<float>());
            else if(name=="line"){const D2D1_POINT_2F p{command[1].get<float>(),command[2].get<float>()};if(!open)begin(p.x,p.y);else sink->AddLine(p);}
            else if(name=="quadratic"){if(!open)begin(0,0);sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({command[1],command[2]},{command[3],command[4]}));}
            else if(name=="bezier"){if(!open)begin(0,0);sink->AddBezier(D2D1::BezierSegment({command[1],command[2]},{command[3],command[4]},{command[5],command[6]}));}
            else if(name=="arc"){const float x=command[1],y=command[2],r=command[3],a=command[4],end=command[5];float diff=end-a;const bool anti=command.size()>6&&command[6].get<bool>();
                if(anti&&diff>0)diff-=2*Pi;else if(!anti&&diff<0)diff+=2*Pi;const int steps=std::max(1,static_cast<int>(std::ceil(std::abs(diff)*16)));
                const D2D1_POINT_2F first{x+r*std::cos(a),y+r*std::sin(a)};if(!open)begin(first.x,first.y);else sink->AddLine(first);
                for(int i=1;i<=steps;++i){const float angle=a+diff*static_cast<float>(i)/steps;sink->AddLine({x+r*std::cos(angle),y+r*std::sin(angle)});}}
            else throw std::runtime_error("Unsupported exported vector command");
        }finish(D2D1_FIGURE_END_OPEN);
    }
    void Sonnet(ID2D1DeviceContext* dc,size_t index,float elapsed,float duration,float w,float h,bool reduced){
        index%=100;if(sonnetCached!=index){sonnetNodes.clear();sonnetCached=index;const auto& recipe=Recipe(FoliaProgramKind::Sonnet,index);
            for(const auto& n:recipe.at("nodes")){SonnetNode node;
                for(auto it=n.at("transforms").rbegin();it!=n.at("transforms").rend();++it){const auto& t=*it;node.alpha*=N(t,"alpha",1);
                    node.transform=node.transform*D2D1::Matrix3x2F::Scale(N(t,"scaleX",1),N(t,"scaleY",1))*D2D1::Matrix3x2F::Rotation(N(t,"rotation")*180/Pi)*D2D1::Matrix3x2F::Translation(N(t,"x"),N(t,"y"));}
                for(const auto& operation:n.at("ops")){Op op;const bool fill=operation.at("kind")=="fill";Check(factory->CreatePathGeometry(&op.shape));ComPtr<ID2D1GeometrySink> sink;Check(op.shape->Open(&sink));
                    Path(sink.Get(),operation.at("path"),fill);Check(sink->Close());const auto& style=operation.at("style");op.color=Color(style.value("color",uint32_t(0xffffff)),N(style,"alpha",1));op.stroke=fill?0:N(style,"width",1);op.delay=N(operation,"delay");op.span=N(operation,"span",.8f);
                    if(op.stroke>0){Check(op.shape->ComputeLength(nullptr,&op.length));const int pieces=std::max(1,std::min(240,static_cast<int>(std::ceil(op.length/8))));
                        for(int j=0;j<=pieces;++j){D2D1_POINT_2F point{};Check(op.shape->ComputePointAtLength(op.length*static_cast<float>(j)/pieces,nullptr,&point,nullptr));op.points.push_back(point);}}
                    node.ops.push_back(std::move(op));}
                sonnetNodes.push_back(std::move(node));}}
        const float progress=reduced?1.f:Unit(elapsed/std::max(1.f,duration*.95f));D2D1_MATRIX_3X2_F old;dc->GetTransform(&old);
        const float dolly=reduced?1.f:1+.04f*InOut(progress);const auto parent=D2D1::Matrix3x2F::Scale(w/1600*dolly,h/900*dolly)*D2D1::Matrix3x2F::Translation(w*.5f,h*.47f);
        for(const auto& n:sonnetNodes){dc->SetTransform(n.transform*parent*old);
            for(const auto& op:n.ops){const float local=Unit((progress-op.delay)/std::max(.001f,op.span));if(local<=0)continue;auto color=op.color;color.a*=n.alpha*Enter(std::min(1.f,local*2))*(op.stroke>0?.62f:.16f);brush->SetColor(color);
                if(op.stroke>0&&!op.points.empty()){const float extent=Enter(local)*static_cast<float>(op.points.size()-1);
                    for(size_t j=1;j<op.points.size()&&static_cast<float>(j-1)<extent;++j){const auto& a=op.points[j-1];const auto& b=op.points[j];const float part=Unit(extent-static_cast<float>(j-1));
                        if(std::hypot(b.x-a.x,b.y-a.y)<std::max(40.f,op.length/static_cast<float>(op.points.size()-1)*3))dc->DrawLine(a,{a.x+(b.x-a.x)*part,a.y+(b.y-a.y)*part},brush.Get(),op.stroke);}}
                else dc->FillGeometry(op.shape.Get(),brush.Get());}}
        dc->SetTransform(old);
    }
    void PrepareTempera(size_t index){if(cached==index)return;nodes.clear();cached=index;const auto& frame=Recipe(FoliaProgramKind::Tempera,index);
        for(const auto& v:frame.at("nodes")){Node node;node.x=N(v,"x");node.y=N(v,"y");node.px=N(v.at("pivot"),"x");node.py=N(v.at("pivot"),"y");node.rotation=N(v,"rotation");node.group=v.value("group",-1);
            const auto& o=v.at("options");node.dx=N(o,"enterDX");node.dy=N(o,"enterDY");node.delay=N(o,"delay");node.span=N(o,"span",.45f);node.alpha=N(o,"alpha",1);node.drift=o.value("drift",false);node.grow=o.value("grow",false);node.phase=Seed(nodes.size()+173)*2*Pi;
            for(const auto& operation:v.at("ops")){Op op;const bool fill=operation.at("kind")=="fill";Check(factory->CreatePathGeometry(&op.shape));ComPtr<ID2D1GeometrySink> sink;Check(op.shape->Open(&sink));sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                Path(sink.Get(),operation.at("path"),fill);if(operation.contains("holes"))for(const auto& hole:operation.at("holes"))Path(sink.Get(),hole,true);Check(sink->Close());
                const auto& style=operation.at("style");op.color=Color(style.value("color",uint32_t(0xffffff)),N(style,"alpha",1));op.stroke=fill?0:N(style,"width",1);node.ops.push_back(std::move(op));}
            nodes.push_back(std::move(node));}
    }
    void Tempera(ID2D1DeviceContext* dc,size_t index,float elapsed,float duration,float w,float h,bool reduced){
        PrepareTempera(index);const auto& recipe=Recipe(FoliaProgramKind::Tempera,index);const float pace=std::max(.2f,duration),progress=Unit(elapsed/pace),carry=1600.f*.09f,creep=InOut(progress)*carry*.35f;
        const float flowX=std::cos(.4f),flowY=std::sin(.4f);D2D1_MATRIX_3X2_F old;dc->GetTransform(&old);
        brush->SetColor(Color(0xf4f0e6));dc->FillRectangle({0,0,w,h},brush.Get());
        const auto& camera=recipe.at("profile").at("camera");
        const float zoom=reduced?1.f:N(camera,"zoomStart",1)+(N(camera,"zoomEnd",1)-N(camera,"zoomStart",1))*InOut(progress);
        const float travel=reduced?0:N(camera,"travel")*InOut(progress);
        const auto parent=D2D1::Matrix3x2F::Translation(-w*.5f,-h*.5f)*D2D1::Matrix3x2F::Scale(zoom,zoom)*D2D1::Matrix3x2F::Translation(w*(.5f-std::cos(.4f)*travel),h*(.5f-std::sin(.4f)*travel));
        for(const auto& n:nodes){float delay=std::clamp(pace*n.delay,0.f,1.4f),span=std::clamp(pace*n.span,.7f,2.6f);const float compress=std::min(1.f,std::max(.5f,pace)/(delay+span));delay*=compress;span*=compress;
            const float enter=reduced?1.f:Enter((elapsed-delay)/std::max(.001f,span));if(enter<.001f)continue;
            const float behind=reduced?0:(1-enter)*carry-creep;const float x=n.x+n.dx*(1-enter)-flowX*behind,y=n.y+n.dy*(1-enter)-flowY*behind;
            const float breath=reduced||!n.drift?1.f:1+std::sin(elapsed*.5f+n.phase)*.02f;
            auto transform=D2D1::Matrix3x2F::Translation(-n.px,-n.py)*D2D1::Matrix3x2F::Scale(n.grow?std::max(.0001f,enter)*breath:breath,breath)*
                D2D1::Matrix3x2F::Rotation((n.rotation+(n.drift&&!reduced?std::sin(elapsed*.33f+n.phase)*.012f:0))*180/Pi)*D2D1::Matrix3x2F::Translation(x,y);
            if(n.group>=0){const auto& g=recipe.at("groups").at(static_cast<size_t>(n.group));transform=transform*D2D1::Matrix3x2F::Rotation(N(g,"rotation")*180/Pi)*D2D1::Matrix3x2F::Translation(N(g,"x"),N(g,"y"));}
            dc->SetTransform(transform*D2D1::Matrix3x2F::Scale(w/1600.f,h/900.f)*parent*old);
            for(const auto& op:n.ops){auto color=op.color;color.a*=n.alpha*enter;brush->SetColor(color);if(op.stroke>0)dc->DrawGeometry(op.shape.Get(),brush.Get(),op.stroke);else dc->FillGeometry(op.shape.Get(),brush.Get());}
        }dc->SetTransform(old);
    }
    void Glow(ID2D1DeviceContext* dc,float x,float y,float radius,float alpha){if(radius<=0||alpha<=0)return;glow->SetCenter({x,y});glow->SetRadiusX(radius);glow->SetRadiusY(radius);glow->SetOpacity(Unit(alpha));dc->FillEllipse(D2D1::Ellipse({x,y},radius,radius),glow.Get());}
    void Lumiere(ID2D1DeviceContext* dc,size_t index,float elapsed,float duration,float w,float h,bool reduced,float energy,int quality,bool lightField){
        const auto& p=Recipe(FoliaProgramKind::Lumiere,index);const auto& rig=p.at("light");const float t=reduced?2.f:std::max(0.f,elapsed),opening=reduced?1.f:Enter(t/1.2f),aspect=16.f/9.f;
        D2D1_MATRIX_3X2_F old;dc->GetTransform(&old);const auto& travel=p.at("camera");const float progress=Unit(t/std::max(.2f,duration));
        const float zoom=1+(reduced?0:N(travel,"push")*InOut(progress));
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-w*.5f,-h*.5f)*D2D1::Matrix3x2F::Scale(zoom,zoom)*D2D1::Matrix3x2F::Translation(w*(.5f+N(travel,"driftX")*progress),h*(.5f+N(travel,"driftY")*progress))*old);
        if(lightField)for(const auto& beam:rig.at("beams")){
            float angle=N(beam,"angle");if(!reduced&&beam.contains("sway")){const auto& sway=beam["sway"];angle+=std::sin(t/std::max(.1f,N(sway,"period",10))*2*Pi+N(sway,"phase"))*N(sway,"amplitude");}
            const float bx=N(beam,"x",.5f)*w,by=N(beam,"y")*h,length=N(beam,"length",.8f)*h;
            float intensity=N(beam,"intensity",1)*opening*(1+Unit(energy)*.15f);if(!reduced&&beam.contains("pulse")){const auto& pulse=beam["pulse"];intensity*=1+std::sin(t/std::max(.1f,N(pulse,"period",5))*2*Pi+N(pulse,"phase"))*N(pulse,"amplitude");}
            const float spread=std::tan(std::min(1.4f,std::abs(N(beam,"spread",.1f))));
            const float width=N(beam,"width",.02f)*h;const int strands=quality>=2?5:11;
            for(int i=-strands;i<=strands;++i){const float at=static_cast<float>(i)/strands,edge=1-std::abs(at);
                const float dx=std::cos(angle)-std::sin(angle)*at*spread,dy=std::sin(angle)+std::cos(angle)*at*spread;
                D2D1_COLOR_F color=Color(0xffe7bc,Unit(intensity*(.018f+.045f*edge)));if(beam.contains("tint")){color.r=beam["tint"][0];color.g=beam["tint"][1];color.b=beam["tint"][2];}
                brush->SetColor(color);dc->DrawLine({bx-std::sin(angle)*at*width,by+std::cos(angle)*at*width},{bx+dx*length,by+dy*length},brush.Get(),std::max(1.f,width*(.3f+.5f*edge)));}
            Glow(dc,bx,by,h*.11f,intensity*.35f);
        }
        const auto& art=p.at("lineArt");const float gain=N(p,"artGain",1);
        for(const auto& line:art.at("paths")){const float amount=reduced?1.f:Enter((t- N(line,"delay")*2.4f)/std::max(.1f,N(line,"span",.5f)*2.4f));if(amount<=0)continue;
            const auto& points=line.at("points");if(points.size()<2)continue;const float extent=amount*static_cast<float>(points.size()-1);brush->SetColor(Color(0xf5dfb2,Unit(N(line,"alpha",.5f)*gain*opening*.65f)));
            for(size_t j=1;j<points.size()&&static_cast<float>(j-1)<extent;++j){const auto& a=points[j-1];const auto& b=points[j];const float u=Unit(extent-static_cast<float>(j-1));
                const float ax=a[0].get<float>()/aspect*w,ay=a[1].get<float>()*h,bx=b[0].get<float>()/aspect*w,by=b[1].get<float>()*h;
                dc->DrawLine({ax,ay},{ax+(bx-ax)*u,ay+(by-ay)*u},brush.Get(),std::max(.5f,N(line,"width",.001f)*h));}}
        for(const auto& node:art.at("nodes")){const float alpha=Unit((t-N(node,"delay")*2.4f)*2)*(.65f+.35f*std::sin(t+N(node,"twinklePhase")));Glow(dc,node["at"][0].get<float>()/aspect*w,node["at"][1].get<float>()*h,N(node,"size",.008f)*h,alpha*opening);}
        const auto& motes=p.at("motes");const size_t count=static_cast<size_t>(std::clamp(N(motes,"count",100)/(quality>=2?3.f:1.f),0.f,600.f));
        for(size_t i=0;i<count;++i){const float a=Seed(i+3),b=Seed(i+91),c=Seed(i+172);const float x=std::fmod(a+t*N(motes,"driftX")+std::sin(t/std::max(1.f,N(motes,"swirlPeriod",9))+c*10)*N(motes,"swirl"),1.f);
            const float y=std::fmod(b+t*N(motes,"driftY"),1.f);const float size=N(motes,"sizeMin",.002f)+(N(motes,"sizeMax",.008f)-N(motes,"sizeMin",.002f))*c;
            Glow(dc,(x<0?x+1:x)*w,(y<0?y+1:y)*h,size*h,(.15f+.20f*std::sin(t+a*30))*N(motes,"gain",1)*opening);}
        dc->SetTransform(old);
    }
};
FoliaPrograms::FoliaPrograms():impl_(std::make_unique<Impl>()){}
FoliaPrograms::~FoliaPrograms()=default;
void FoliaPrograms::Clear(){impl_->Clear();}
FoliaProgramInfo FoliaPrograms::Info(FoliaProgramKind kind,size_t variant)const{
    FoliaProgramInfo r;r.count=impl_->Catalog(kind).Size();r.index=variant%r.count;const auto& p=impl_->Recipe(kind,r.index);r.name=p.value(kind==FoliaProgramKind::Lumiere?"label":"kind",std::string{});if(kind==FoliaProgramKind::Sonnet){r.name="Sonnet MG "+std::to_string(r.index+1);return r;}
    const auto& profile=kind==FoliaProgramKind::Tempera?p.at("profile"):p;const auto& region=profile.at("region");r.cx=N(region,"cx",.5f);r.cy=N(region,"cy",.5f);r.width=N(region,"w",.72f);r.height=N(region,"h",.5f);r.rotation=N(region,"rotation");r.fontScale=N(region,"fontScale",1);
    const auto& c=profile.at("camera");r.travel=N(c,"travel");r.zoomStart=N(c,"zoomStart",1);r.zoomEnd=N(c,"zoomEnd",1+N(c,"push"));r.travelX=N(c,"driftX");r.travelY=N(c,"driftY");
    if(profile.contains("enter")){r.enterX=N(profile["enter"],"x");r.enterY=N(profile["enter"],"y",1);}
    const auto type=p.value("typography",std::string{});r.typography=type=="vertical"?1:type=="crossed"?2:0;return r;
}
void FoliaPrograms::Draw(ID2D1DeviceContext* dc,FoliaProgramKind kind,size_t variant,float elapsed,float duration,float width,float height,bool reduced,float energy,int quality,bool lightField){
    impl_->Create(dc);
    D2D1_MATRIX_3X2_F saved;dc->GetTransform(&saved);
    // Preserve the music response on the separate backdrop, not the glyphs.
    const float scale=1+(reduced?0:.065f*std::sqrt(Unit(energy)));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,{width*.5f,height*.5f})*saved);
    if(kind==FoliaProgramKind::Sonnet)impl_->Sonnet(dc,variant,elapsed,duration,width,height,reduced);
    else if(kind==FoliaProgramKind::Tempera)impl_->Tempera(dc,variant,elapsed,duration,width,height,reduced);
    else impl_->Lumiere(dc,variant,elapsed,duration,width,height,reduced,energy,quality,lightField);
    dc->SetTransform(saved);
}
std::array<std::array<float,4>,52> FoliaPrograms::LightConstants(size_t variant,float elapsed,float duration,float width,float height,bool reduced,float energy,int quality)const{
    // Same packing/beam resolution as Folia lightFieldShader.ts and rig.ts.
    std::array<std::array<float,4>,52> u{};const auto& p=impl_->Recipe(FoliaProgramKind::Lumiere,variant);const auto& rig=p.at("light");
    const float aspect=width/std::max(1.f,height),t=reduced?2.f:std::max(0.f,elapsed),intensity=reduced?1.f:Enter(t/1.2f);
    const auto oscillate=[&](const Json& o){return N(o,"amplitude")*std::sin((t/std::max(.001f,N(o,"period",1))+N(o,"phase"))*2*Pi);};
    const auto& beams=rig.at("beams");const size_t count=std::min<size_t>(6,beams.size());
    for(size_t i=0;i<count;++i){const auto& b=beams[i];const float angle=N(b,"angle")+(b.contains("sway")?oscillate(b["sway"]):0);
        const float pulse=1+(b.contains("pulse")?oscillate(b["pulse"]):0),local=N(b,"reveal"),reveal=local>0?.06f+.94f*(Unit(t/local)*Unit(t/local)*(3-2*Unit(t/local))):1;
        u[i]={N(b,"x")*aspect,N(b,"y"),std::cos(angle),std::sin(angle)};
        u[6+i]={N(b,"width")/2,std::tan(N(b,"spread")),N(b,"softness"),N(b,"length")*reveal};
        u[12+i]={N(b,"intensity")*pulse*intensity*(1+.15f*std::pow(std::max(0.f,energy),1.5f)),N(b,"streaks"),N(b,"streakFreq"),t*N(b,"streakSpeed")};
        const auto tint=b.value("tint",Json::array({1,1,1}));u[18+i]={tint[0].get<float>(),tint[1].get<float>()*.88f,tint[2].get<float>()*.62f,N(b,"core",.5f)};
        if(b.contains("gobo")){const auto& g=b["gobo"];const auto pattern=g.value("pattern",std::string{});const float id=pattern=="blinds"?1.f:pattern=="cross"?2.f:pattern=="lattice"?3.f:pattern=="leaves"?4.f:0.f;
            u[24+i]={id,N(g,"frequency"),N(g,"duty",1),N(g,"drift")*t};}
        u[30+i]={N(b,"spectrum"),N(b,"reach"),0,0};
    }
    const auto& fog=rig.at("fog");u[36]={width,height,t,static_cast<float>(count)};
    u[37]={N(fog,"density"),N(fog,"tyndallBase"),N(fog,"scale"),N(fog,"warp")};
    u[38]={N(fog,"driftX"),N(fog,"driftY"),N(fog,"ambient"),quality>=2?2.f:3.f};u[39]={1,.88f,.62f,0};
    if(rig.contains("glare")&&!rig["glare"].is_null()){const auto& g=rig["glare"];u[40]={N(g,"x")*aspect,N(g,"y"),N(g,"radius"),N(g,"intensity")*intensity};u[41]={N(g,"streak"),0,0,0};}
    u[42]={1,.88f,.62f,0};u[43]={.008f,.012f,.018f,.97f};
    if(rig.contains("caustic")){const auto& c=rig["caustic"];u[44]={N(c,"scale"),N(c,"speed"),N(c,"inBeam"),1};
        if(c.contains("floor")){const auto& floor=c["floor"];u[45]={N(floor,"cx")*aspect,N(floor,"cy"),N(floor,"rx")*aspect,N(floor,"ry")};u[46]={N(floor,"strength")*intensity,0,0,0};}}
    if(rig.contains("wave")){const auto& wave=rig["wave"];const auto name=wave.value("mode",std::string{});const float id=name=="rings"?1.f:name=="slits"?2.f:name=="sources"?3.f:name=="airy"?4.f:0.f;
        u[47]={id,N(wave,"frequency"),N(wave,"speed"),N(wave,"strength")*intensity};u[48]={N(wave,"cx")*aspect,N(wave,"cy"),N(wave,"radius"),N(wave,"separation")};u[50]={N(wave,"shield",.45f),0,0,0};}
    const auto& region=p.at("region");u[49]={N(region,"cx")*aspect,N(region,"cy"),N(region,"w")*aspect/2,N(region,"h")/2};
    const auto& camera=p.at("camera");const float progress=reduced?0:Unit(t/std::max(.2f,duration));u[51]={1+N(camera,"push")*InOut(progress),width*N(camera,"driftX")*progress,height*N(camera,"driftY")*progress,0};
    return u;
}
}
