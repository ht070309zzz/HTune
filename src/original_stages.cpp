#include "original_stages.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "shaders/g_OriginalFullscreenVS.h"
#include "shaders/g_OriginalBackgroundPS.h"
#include "shaders/g_OriginalWeaveVS.h"
#include "shaders/g_OriginalEchoVS.h"
#include "shaders/g_OriginalThreadPS.h"

namespace stage {
namespace {
float Unit(float v) {return std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;}
float Smooth(float v) {v=Unit(v);return v*v*(3.f-2.f*v);}
float Approach(float a,float b,float dt,float rate) {return a+(b-a)*(1.f-std::exp(-std::clamp(dt,0.f,.1f)*rate));}
void Check(HRESULT hr) {if(FAILED(hr))throw std::runtime_error("Original stage GPU resource failure: "+std::to_string(static_cast<unsigned long>(hr)));}
struct alignas(16) Constants {
    float resolution[4],clock[4],audio[4],pointer[4],base[4],accent[4],ink[4],drawing[4];
    float history[128][4];
};
static_assert(sizeof(Constants)==2176);
void Color(float (&dst)[4],ThemeColor color) {dst[0]=Unit(color.r);dst[1]=Unit(color.g);dst[2]=Unit(color.b);dst[3]=Unit(color.a);}
}

OriginalComposition ComposeOriginal(int style,float width,float height,float drawer) {
    OriginalComposition g;
    width=std::max(1.f,width);height=std::max(1.f,height);
    g.drawerWidth=std::min(width,std::max(340.f,std::min(680.f,width*.46f)))*Unit(drawer);
    g.drawerLeft=width-g.drawerWidth;g.width=std::max(1.f,g.drawerLeft);g.height=height;
    g.compact=g.width<720.f||height<500.f;g.margin=std::clamp(g.width*.026f,18.f,46.f);
    g.railY=height-std::clamp(height*.052f,20.f,52.f);
    g.controlsX=style==EchoStyle?g.margin:g.width-std::max(64.f,g.width*.055f);
    if(style==PosterStyle)g.controlsX=g.width*.944f+std::max(6.f,g.width*.01f);
    if(style==WeaveStyle){
        g.lyricX=g.width*.15f;g.lyricY=height*.36f;g.lyricWidth=g.width*.70f;
        g.lyricHeight=height*.31f;
        g.coverX=g.margin;g.coverY=std::max(36.f,height*.07f);g.coverWidth=g.coverHeight=g.compact?36.f:52.f;
    }else if(style==PosterStyle){
        g.lyricX=g.margin;g.lyricY=height*.21f;g.lyricWidth=g.width*.64f-g.margin;
        g.lyricHeight=height*.72f;
        g.coverX=g.width*.755f;g.coverY=0;g.coverWidth=g.width*.185f;g.coverHeight=height*.86f;
    }else{
        g.lyricX=g.width*.24f;g.lyricY=height*.23f;g.lyricWidth=g.width*.55f;
        g.lyricHeight=height*.25f;
        g.coverWidth=g.coverHeight=g.compact?46.f:72.f;
        g.coverX=g.width-g.margin-g.coverWidth;g.coverY=28.f;
    }
    if(g.compact){
        g.lyricX=style==PosterStyle?g.margin:g.width*.12f;
        g.lyricWidth=std::max(60.f,g.width-g.lyricX-(style==PosterStyle?g.width*.25f:g.width*.12f));
        g.lyricY=std::max(105.f,height*(style==EchoStyle?.29f:.34f));
        g.lyricHeight=std::max(36.f,std::min(height*.38f,g.railY-g.lyricY-20.f));
    }
    return g;
}

void OriginalStageState::History(std::array<std::array<float,4>,128>& out) const {
    out={};if(samples_.size()<2)return;
    const auto begin=samples_.front().position,end=samples_.back().position;
    size_t cursor=0;
    for(size_t i=0;i<out.size();++i){
        const auto at=begin+static_cast<int64_t>((end-begin)*static_cast<double>(i)/(out.size()-1));
        while(cursor+1<samples_.size()&&samples_[cursor+1].position<at)++cursor;
        const auto& a=samples_[cursor];const auto& b=samples_[std::min(cursor+1,samples_.size()-1)];
        const float t=b.position>a.position?Unit(static_cast<float>(at-a.position)/static_cast<float>(b.position-a.position)):0.f;
        // Never bridge seek, capture or identity gaps with invented audio.
        const bool valid=b.position-a.position<=1500;
        out[i]={a.level+(b.level-a.level)*t,valid?1.f:0.f,0,0};
    }
}
void OriginalStageState::Update(const PlaybackSnapshot& p,const AudioFeatures& a,bool currentAudio,
    bool preview,bool mismatch,int64_t now,float dt,bool reduced,bool pointerActive,float px,float py) {
    const bool valid=!preview&&!mismatch&&!p.metadataStale&&!p.identityReacquiring&&p.pid&&p.processCreatedAtTicks&&
        (p.songIdVerified?SameCatalogId(p.songId,p.songId):!p.title.empty());
    const bool same=identity_&&pid_==p.pid&&instance_==p.processCreatedAtTicks&&verified_==p.songIdVerified&&
        (verified_?SameCatalogId(songId_,p.songId):(title_==p.title&&artist_==p.artist));
    if(valid&&!same){
        previous_=displayed_;previousVisible_=samples_.size()>1;previousExtent_=historyExtent_;
        samples_.clear();displayed_={};historyExtent_=0;
        changedAt_=identity_?now:0;pid_=p.pid;instance_=p.processCreatedAtTicks;verified_=p.songIdVerified;
        songId_=p.songId;title_=p.title;artist_=p.artist;identity_=true;lastAudioAt_=0;lastPosition_=0;
        line_=SIZE_MAX;rowChangedAt_=0;
    }
    if(previousVisible_&&(reduced||now-changedAt_>=1300))previousVisible_=false;
    // Unavailable identity holds the sculpture, but cannot append new samples.
    const bool clock=CurrentTimelineSource(p,now)!=TimelineSource::Unavailable;
    const auto audioAt=a.timingTrusted?a.sampledAtMs:a.observedAtMs;
    if(valid&&currentAudio&&clock&&p.state==PlayState::Playing&&audioAt>lastAudioAt_){
        const auto pos=PlaybackPositionAt(p,now);
        if(!samples_.empty()&&(pos<lastPosition_-150||pos-lastPosition_>1800)){
            samples_.clear();displayed_={};historyExtent_=0;
        }
        float spectral=0;for(size_t i=0;i<12;++i)spectral+=Unit(a.spectrum[i]);spectral/=12.f;
        const float level=Unit(std::sqrt(Unit(a.rms))*.56f+spectral*.44f);
        if(samples_.empty()||pos-samples_.back().position>=100){
            samples_.push_back({pos,level});if(samples_.size()>4096)samples_.pop_front();
        }else if(pos>=samples_.back().position)samples_.back().level=std::max(samples_.back().level,level);
        lastAudioAt_=audioAt;lastPosition_=pos;
    }
    if(pointerActive&&std::isfinite(px)&&std::isfinite(py)){
        if(pointerPresence_<.01f){pointerX_=px;pointerY_=py;}
        pointerX_=Approach(pointerX_,px,dt,20.f);pointerY_=Approach(pointerY_,py,dt,20.f);
    }
    pointerPresence_=Approach(pointerPresence_,pointerActive&&!reduced?1.f:0.f,dt,9.f);
    std::array<std::array<float,4>,128> destination{};History(destination);
    const float extent=samples_.size()>1?Unit(static_cast<float>(samples_.back().position-samples_.front().position)/45000.f):0.f;
    historyExtent_=Approach(historyExtent_,extent,dt,9.f);
    for(size_t i=0;i<displayed_.size();++i){
        displayed_[i][0]=Approach(displayed_[i][0],destination[i][0],dt,9.f);
        displayed_[i][1]=Approach(displayed_[i][1],destination[i][1],dt,12.f);
    }
}
void OriginalStageState::SelectLine(size_t line,uint64_t revision,int64_t now,bool following) {
    if(line_!=line||revision_!=revision){rowChangedAt_=following&&line_!=SIZE_MAX?now:0;line_=line;revision_=revision;}
}
float OriginalStageState::TrackEnter(int64_t now,bool reduced) const {return reduced||!changedAt_?1.f:Smooth(static_cast<float>(std::max<int64_t>(0,now-changedAt_))/1250.f);}
float OriginalStageState::RowEnter(int64_t now,bool reduced) const {return reduced||!rowChangedAt_?1.f:Smooth(static_cast<float>(std::max<int64_t>(0,now-rowChangedAt_))/540.f);}
void OriginalStageState::Apply(OriginalVisualFrame& f,int64_t now,bool reduced) const {
    f.pointerX=pointerX_;f.pointerY=pointerY_;f.pointerPresence=pointerPresence_;
    f.trackEnter=TrackEnter(now,reduced);f.rowEnter=RowEnter(now,reduced);
    f.history=displayed_;f.previousHistory=previous_;f.previousHistoryVisible=previousVisible_;
    f.historyExtent=historyExtent_;f.previousExtent=previousExtent_;
    if(!samples_.empty()){f.historyBegin=static_cast<float>(samples_.front().position);f.historyEnd=static_cast<float>(samples_.back().position);}
}

struct OriginalStageGpu::Impl {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreen,weave,echo;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> background,thread;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth;
};
OriginalStageGpu::OriginalStageGpu()=default;
OriginalStageGpu::~OriginalStageGpu()=default;
bool OriginalStageGpu::Create(ID3D11Device* d) {
    impl_.reset();if(!d||d->GetFeatureLevel()<D3D_FEATURE_LEVEL_10_0)return false;
    auto p=std::make_unique<Impl>();
    Check(d->CreateVertexShader(g_OriginalFullscreenVS,sizeof(g_OriginalFullscreenVS),nullptr,&p->fullscreen));
    Check(d->CreateVertexShader(g_OriginalWeaveVS,sizeof(g_OriginalWeaveVS),nullptr,&p->weave));
    Check(d->CreateVertexShader(g_OriginalEchoVS,sizeof(g_OriginalEchoVS),nullptr,&p->echo));
    Check(d->CreatePixelShader(g_OriginalBackgroundPS,sizeof(g_OriginalBackgroundPS),nullptr,&p->background));
    Check(d->CreatePixelShader(g_OriginalThreadPS,sizeof(g_OriginalThreadPS),nullptr,&p->thread));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(Constants);bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    Check(d->CreateBuffer(&bd,nullptr,&p->constants));
    D3D11_BLEND_DESC blend{};auto& b=blend.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_ONE;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    Check(d->CreateBlendState(&blend,&p->blend));
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;Check(d->CreateRasterizerState(&rs,&p->raster));
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=FALSE;ds.StencilEnable=FALSE;Check(d->CreateDepthStencilState(&ds,&p->depth));
    impl_=std::move(p);return true;
}
void OriginalStageGpu::Render(ID3D11DeviceContext* c,ID3D11RenderTargetView* target,const OriginalVisualFrame& f,int quality) {
    if(!impl_||!c||!target)return;
    auto& p=*impl_;Constants data{};
    data.resolution[0]=std::max(1.f,f.width);data.resolution[1]=std::max(1.f,f.height);data.resolution[2]=std::max(.5f,f.dpi);data.resolution[3]=static_cast<float>(f.style);
    data.clock[0]=f.seconds;data.clock[1]=Unit(f.trackEnter);data.clock[2]=Unit(f.rowEnter);data.clock[3]=Unit(f.motion);
    data.audio[0]=Unit(f.low*f.response);data.audio[1]=Unit(f.mid*f.response);data.audio[2]=Unit(f.high*f.response);data.audio[3]=Unit(f.pulse*f.response);
    if(f.style==EchoStyle)data.audio[3]=std::isfinite(f.response)?std::clamp(f.response,0.f,2.f):0.f;
    data.pointer[0]=f.pointerX;data.pointer[1]=f.pointerY;data.pointer[2]=Unit(f.pointerPresence);data.pointer[3]=Unit(f.drawer);
    Color(data.base,f.base);Color(data.accent,f.accent);Color(data.ink,f.ink);
    std::memcpy(data.history,f.history.data(),sizeof(data.history));
    const int strands=quality>=3?440:quality==2?620:880;
    const int contours=quality>=3?36:quality==2?48:64;
    const int segments=f.style==EchoStyle?(quality>=3?192:384):(quality>=3?96:160);
    data.drawing[0]=static_cast<float>(segments);data.drawing[1]=static_cast<float>(f.style==WeaveStyle?strands:contours);
    data.drawing[3]=Unit(f.historyExtent);
    const auto upload=[&]{D3D11_MAPPED_SUBRESOURCE mapped{};Check(c->Map(p.constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));std::memcpy(mapped.pData,&data,sizeof(data));c->Unmap(p.constants.Get(),0);};
    upload();ID3D11Buffer* cb=p.constants.Get();c->VSSetConstantBuffers(0,1,&cb);c->PSSetConstantBuffers(0,1,&cb);
    c->ClearRenderTargetView(target,data.base);
    c->OMSetRenderTargets(1,&target,nullptr);c->OMSetDepthStencilState(p.depth.Get(),0);c->RSSetState(p.raster.Get());
    const D3D11_VIEWPORT vp{0,0,data.resolution[0],data.resolution[1],0,1};c->RSSetViewports(1,&vp);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->GSSetShader(nullptr,nullptr,0);
    c->OMSetBlendState(nullptr,nullptr,0xffffffffu);c->VSSetShader(p.fullscreen.Get(),nullptr,0);c->PSSetShader(p.background.Get(),nullptr,0);c->Draw(3,0);
    c->OMSetBlendState(p.blend.Get(),nullptr,0xffffffffu);c->PSSetShader(p.thread.Get(),nullptr,0);
    if(f.style==WeaveStyle){c->VSSetShader(p.weave.Get(),nullptr,0);c->DrawInstanced(segments*6,strands,0,0);}
    if(f.style==EchoStyle){
        c->VSSetShader(p.echo.Get(),nullptr,0);
        // Wide soft shadow, then the fine green/white laminated contours.
        data.drawing[2]=1;upload();c->DrawInstanced(segments*6,contours,0,0);
        data.drawing[2]=3;upload();c->DrawInstanced(segments*6,contours,0,0);
        data.drawing[2]=0;upload();c->DrawInstanced(segments*6,contours,0,0);
        if(f.previousHistoryVisible){
            std::memcpy(data.history,f.previousHistory.data(),sizeof(data.history));data.drawing[2]=2;data.drawing[3]=Unit(f.previousExtent);upload();c->DrawInstanced(segments*6,contours,0,0);
        }
    }
    c->OMSetRenderTargets(0,nullptr,nullptr);
}
}
