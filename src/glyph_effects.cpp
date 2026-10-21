#include "glyph_effects.h"
#include "lyric_fairies.h"
#include "shaders/g_GlyphEffectVS.h"
#include "shaders/g_GlyphEffectPS.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace stage {
namespace {
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Glyph particle GPU HRESULT "+std::to_string(static_cast<unsigned long>(hr)));}
struct Constants {
    float view[4],recipe[4],clip[4],budget[4];
    GlyphEmitter emitters[MaxGlyphEmitters];
    float fairies[FairyCount][FairySamples][4]; // head: x/y/tilt/size; history: x/y/age/visibility
    float fairyColor[FairyCount][4];
    float fairyMotion[FairyCount][4];
};
static_assert(sizeof(GlyphEmitter)==80&&sizeof(Constants)==9936);
}
bool GlyphEffectFrame::Add(const GlyphEmitter& e){
    if(count==emitters.size()||e.color.a<=.005f||e.age<0||!std::isfinite(e.age)||!std::isfinite(e.active)||!std::isfinite(e.seed)||!std::isfinite(e.padding)||!std::isfinite(e.progress))return false;
    for(float c:{e.color.r,e.color.g,e.color.b,e.color.a})if(!std::isfinite(c))return false;
    float left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;
    for(auto p:e.corners){if(!std::isfinite(p.x)||!std::isfinite(p.y))return false;
        left=std::min(left,p.x);top=std::min(top,p.y);right=std::max(right,p.x);bottom=std::max(bottom,p.y);}
    if(right-left<2||bottom-top<2||right<=clip.left||left>=clip.right||bottom<=clip.top||top>=clip.bottom)return false;
    emitters[count]=e;emitters[count].active=std::clamp(e.active,0.f,1.f);
    emitters[count].color.a=std::clamp(e.color.a,0.f,1.f);++count;return true;
}
GlyphEffectStyle GlyphEffectRecipe(int style){
    // Theme-specific dust cadence, spread, material and palette weight.
    switch(style){
    case 0:return {0,.64f,1.f,.85f}; case 1:return {4,.50f,.88f,.75f};
    case 2:return {2,.48f,.82f,.8f}; case 3:return {5,.72f,1.f,.95f};
    case 4:return {1,1.10f,.78f,1.f};case 5:return {2,.40f,1.08f,.75f};
    case 6:return {1,1.25f,1.12f,1.12f};case 7:return {1,.82f,.9f,.9f};
    case 8:return {2,.65f,1.03f,.85f};case 9:return {0,.68f,1.16f,1.12f};
    case 10:return {0,.94f,.96f,.88f};case 11:return {4,.62f,1.05f,.9f};
    case 12:return {5,.48f,1.16f,.8f};case 13:return {0,.82f,1.08f,.85f};
    case 14:return {4,.86f,.85f,.9f};case 15:return {3,.62f,1.04f,1.f};
    case 16:return {2,.36f,.88f,.7f};case 17:return {3,.42f,.84f,.85f};
    case 18:return {5,.60f,1.04f,.85f};case 19:return {4,.98f,1.12f,1.f};
    case 20:return {0,.25f,.76f,.6f};case 21:return {4,.72f,1.02f,.95f};
    case 22:return {3,.80f,.88f,.95f};case 24:return {1,.58f,1.12f,.85f};
    case 25:return {3,.55f,1.08f,.8f};case 26:return {0,.53f,1.2f,1.f};
    case 28:return {5,.92f,1.03f,1.f};case 29:return {5,.74f,1.18f,1.f};
    default:return {0,.64f,1.f,.85f};
    }
}
float GlyphSingingLight(float progress){
    const float p=std::isfinite(progress)?std::clamp(progress,0.f,1.f):0.f;
    // A continuous envelope across the end of a word, instead of switching
    // abruptly from full light to the sung-word residue on a single frame.
    return .24f*p+.76f*std::max(0.f,std::sin(p*3.14159265358979323846f));
}
struct GlyphEffects::Impl {
    LyricFairies fairies;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;ComPtr<ID3D11DepthStencilState> depth;
    void Create(ID3D11Device* device){
        // Match the other optional SM4 effects: old devices retain readable D2D.
        if(device->GetFeatureLevel()<D3D_FEATURE_LEVEL_10_0)return;
        Check(device->CreateVertexShader(g_GlyphEffectVS,sizeof(g_GlyphEffectVS),nullptr,&vs));
        Check(device->CreatePixelShader(g_GlyphEffectPS,sizeof(g_GlyphEffectPS),nullptr,&ps));
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(Constants);buffer.Usage=D3D11_USAGE_DYNAMIC;
        buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&buffer,nullptr,&constants));
        D3D11_BLEND_DESC b{};auto& rt=b.RenderTarget[0];rt.BlendEnable=TRUE;
        rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&b,&blend));
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
        Check(device->CreateRasterizerState(&r,&raster));
        D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=FALSE;Check(device->CreateDepthStencilState(&d,&depth));
    }
    void Draw(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,UINT pw,UINT ph,const GlyphEffectFrame& f){
        if(!target||!pw||!ph||f.reduced||!vs||f.width<=0||f.height<=0){fairies.Reset();return;}
        fairies.Step(f);
        Constants data{};data.view[0]=f.width;data.view[1]=f.height;data.view[2]=static_cast<float>(fairies.Time());
        const auto style=GlyphEffectRecipe(f.style);data.recipe[0]=style.family;data.recipe[1]=style.speed;data.recipe[2]=style.spread;data.recipe[3]=style.weight;
        data.clip[0]=f.clip.left;data.clip[1]=f.clip.top;data.clip[2]=f.clip.right;data.clip[3]=f.clip.bottom;
        const UINT particles=f.quality>=2?3:6,arcs=0,segments=1;
        data.budget[0]=static_cast<float>(particles);data.budget[1]=static_cast<float>(std::min(f.count,MaxGlyphEmitters));
        data.budget[2]=f.quality>=2?2.f:4.f;
        std::copy_n(f.emitters.begin(),std::min(f.count,MaxGlyphEmitters),data.emitters);
        for(size_t i=0;i<fairies.count;++i){const auto& a=fairies.agents[i];
            auto* head=data.fairies[i][0];head[0]=a.position.x;head[1]=a.position.y;head[2]=a.tilt;head[3]=a.size;

            data.fairyColor[i][0]=fairies.color.r;data.fairyColor[i][1]=fairies.color.g;data.fairyColor[i][2]=fairies.color.b;
            data.fairyColor[i][3]=fairies.color.a*fairies.opacity;

            for(size_t j=1;j<FairySamples;++j){auto* sample=data.fairies[i][j];sample[2]=2.f;
                if(j>a.count)continue;const auto& h=a.history[a.count-j];const float age=static_cast<float>(fairies.Time()-h.time);
                sample[0]=h.x;sample[1]=h.y;sample[2]=age;
                // Shader masks just covered pixels, preserving the rest of the wake.
                sample[3]=h.speed;
            }
        }
        D3D11_MAPPED_SUBRESOURCE map{};Check(context->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map));
        std::memcpy(map.pData,&data,sizeof(data));context->Unmap(constants.Get(),0);
        ID3D11ShaderResourceView* empty[2]{};context->PSSetShaderResources(0,2,empty);context->VSSetShaderResources(0,2,empty);
        context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(depth.Get(),0);
        const D3D11_VIEWPORT viewport{0,0,float(pw),float(ph),0,1};context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);
        context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        auto* cb=constants.Get();context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
        context->DrawInstanced(6*segments,static_cast<UINT>(std::min(f.count,MaxGlyphEmitters))*(particles+arcs),0,0);
        if(fairies.count){data.budget[3]=1;
            Check(context->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map));std::memcpy(map.pData,&data,sizeof(data));context->Unmap(constants.Get(),0);
            context->DrawInstanced(6*(1+(f.quality>=2?2:4)*static_cast<UINT>(FairySamples-1)/(f.quality>=2?2:1)),static_cast<UINT>(fairies.count),0,0);}
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};
GlyphEffects::GlyphEffects():impl_(std::make_unique<Impl>()){}GlyphEffects::~GlyphEffects()=default;
void GlyphEffects::Create(ID3D11Device* d){impl_->Create(d);}
void GlyphEffects::Draw(ID3D11DeviceContext* c,ID3D11RenderTargetView* t,UINT w,UINT h,const GlyphEffectFrame& f){impl_->Draw(c,t,w,h,f);}
void GlyphEffects::Reset(){impl_->fairies.Reset();}
size_t GlyphEffects::TrailSamples() const {size_t count=0;for(size_t i=0;i<impl_->fairies.count;++i)count+=impl_->fairies.agents[i].count;return count;}
double GlyphEffects::FlightSeconds() const {return impl_->fairies.Time();}
uint32_t GlyphEffects::RelayCount() const {return impl_->fairies.relays;}
}
