// SPDX-License-Identifier: AGPL-3.0-only
// Native host for Folia background motion and Apache-2.0 Paper shader ports.
#include "folia_backdrop.h"
#include "rift_stage.h"
#include "paper_uniforms.h"
#include "shaders/g_FoliaTransitionVS.h"
#include "shaders/g_PaperPS0.h"
#include "shaders/g_PaperPS1.h"
#include "shaders/g_PaperPS2.h"
#include "shaders/g_PaperPS3.h"
#include "shaders/g_PaperPS4.h"
#include "shaders/g_PaperPS5.h"
#include "shaders/g_PaperPS6.h"
#include "shaders/g_FoliaMonetPS.h"
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <stdexcept>
#include <vector>
namespace stage {
using Microsoft::WRL::ComPtr;
namespace {void Hr(HRESULT h){if(FAILED(h))throw std::runtime_error("Folia backdrop resource failed");}}
struct FoliaBackdrop::Impl {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11VertexShader> vs;
    std::array<ComPtr<ID3D11PixelShader>,7> shaders;ComPtr<ID3D11PixelShader> monet;
    ComPtr<ID3D11Buffer> buffer;ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11ShaderResourceView> image,noise;
    ComPtr<ID3D11BlendState> blend;ComPtr<ID3D11RasterizerState> raster;
    std::shared_ptr<const BackgroundFrame> pixels;uint64_t sequence=UINT64_MAX;
    std::array<std::array<float,4>,74> values{};
    int64_t lastPosition=0,lastNow=0;float clock=0,speed=.3f,seconds=0;std::array<std::array<float,3>,157> driftTrack{};std::wstring identity;
    void Create(ID3D11Device* d){
        const auto hash=[](double lattice,double seed){const double n=std::sin(lattice*127.1+seed*311.7)*43758.5453;return n-std::floor(n);};
        const auto periodic=[&](double t,int points,double seed){const double cell=std::floor(t),fraction=t-cell,smooth=fraction*fraction*(3-2*fraction);const auto a=hash(std::fmod(cell,points),seed),b=hash(std::fmod(cell+1,points),seed);return a+(b-a)*smooth;};
        const double seeds[]={12.9898,78.233,39.425};for(size_t c=0;c<3;++c){float peak=0;for(size_t i=0;i<driftTrack.size();++i){const double t=76*double(i)/156;driftTrack[i][c]=float((periodic(t,76,seeds[c])*.68+periodic(t*2,152,seeds[c]+17.31)*.32)*2-1);peak=std::max(peak,std::abs(driftTrack[i][c]));}for(auto& row:driftTrack)row[c]/=std::max(.001f,peak);}
        device=d;device->GetImmediateContext(&context);Hr(device->CreateVertexShader(g_FoliaTransitionVS,sizeof(g_FoliaTransitionVS),nullptr,&vs));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=74*16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Hr(device->CreateBuffer(&bd,nullptr,&buffer));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Hr(device->CreateSamplerState(&sd,&sampler));
        D3D11_BLEND_DESC b{};auto& r=b.RenderTarget[0];r.BlendEnable=TRUE;r.SrcBlend=D3D11_BLEND_ONE;r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;Hr(device->CreateBlendState(&b,&blend));
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;Hr(device->CreateRasterizerState(&rs,&raster));
        std::array<uint32_t,256*256> data{};uint32_t state=20261020;for(auto& p:data){state=state*1664525+1013904223;p=0xff000000u|(state&0xffffff);}
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=256;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{data.data(),1024,0};ComPtr<ID3D11Texture2D> tx;Hr(device->CreateTexture2D(&td,&initial,&tx));Hr(device->CreateShaderResourceView(tx.Get(),nullptr,&noise));
    }
    void Image(const RiftFrame& f){if(pixels==f.cover&&(!pixels||pixels->sequence==sequence))return;pixels=f.cover;sequence=pixels?pixels->sequence:0;image.Reset();if(!pixels)return;
        const auto& p=*pixels;if(!p.width||!p.height||p.width>3840||p.height>3840||p.stride<p.width*4||uint64_t(p.stride)*p.height>p.bgra.size())return;
        D3D11_TEXTURE2D_DESC td{};td.Width=p.width;td.Height=p.height;td.ArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        // Paper shaders premultiply after sampling; the shared decoder stores premultiplied BGRA.
        std::vector<uint8_t> straight=p.bgra;for(UINT y=0;y<p.height;++y)for(UINT x=0;x<p.width;++x){auto* q=straight.data()+size_t(y)*p.stride+x*4;for(int c=0;c<3;++c)q[c]=q[3]?static_cast<uint8_t>(std::min(255u,(unsigned(q[c])*255+q[3]/2)/q[3])):0;}
        D3D11_SUBRESOURCE_DATA initial{straight.data(),p.stride,0};ComPtr<ID3D11Texture2D> tx;Hr(device->CreateTexture2D(&td,&initial,&tx));Hr(device->CreateShaderResourceView(tx.Get(),nullptr,&image));
    }
    void Shader(int index){if(shaders[index])return;const void* code[]={g_PaperPS0,g_PaperPS1,g_PaperPS2,g_PaperPS3,g_PaperPS4,g_PaperPS5,g_PaperPS6};const size_t bytes[]={sizeof(g_PaperPS0),sizeof(g_PaperPS1),sizeof(g_PaperPS2),sizeof(g_PaperPS3),sizeof(g_PaperPS4),sizeof(g_PaperPS5),sizeof(g_PaperPS6)};Hr(device->CreatePixelShader(code[index],bytes[index],nullptr,&shaders[index]));}
    void Uniforms(const RiftFrame& f,int index,UINT width,UINT height){values={};const auto color=[](D2D1_COLOR_F c){return std::array<float,4>{c.r,c.g,c.b,c.a};};
        const auto& fields=PaperUniforms[index].fields;auto set=[&](std::string_view name,std::array<float,4> value){for(size_t i=0;i<fields.size();++i)if(fields[i]==name){values[i]=value;return;}};
        auto n=[&](std::string_view name,float value){set(name,{value,0,0,0});};
        set("u_resolution",{float(width),float(height),0,0});n("u_pixelRatio",f.dpi);n("u_imageAspectRatio",pixels?float(pixels->width)/std::max(1u,pixels->height):1.f);
        n("u_originX",.5f);n("u_originY",.5f);n("u_worldWidth",float(width));n("u_worldHeight",float(height));n("u_fit",2);n("u_scale",1);n("u_time",f.reduced||!f.following?0.f:clock);
        set("u_colorBack",color(f.background));set("u_colorFront",color(f.accent));set("u_colorHighlight",color(f.ink));set("u_colorPaper",color(f.background));set("u_colorShadow",color(f.accent));
        n("u_type",4);n("u_shape",2);n("u_pxSize",index==0?2.5f:3.f);n("u_colorSteps",4);n("u_distortion",.8f);n("u_swirl",.1f);n("u_colorsCount",4);n("u_grainMixer",.12f);n("u_grainOverlay",.04f);
        n("u_size",.5f);n("u_shadows",.25f);n("u_highlights",.1f);n("u_edges",.25f);n("u_blur",.08f);n("u_distortionShape",1);n("u_radius",1.25f);n("u_contrast",.4f);n("u_grid",1);n("u_grainSize",.5f);
        n("u_isImage",image?1.f:0.f);n("u_blending",.65f);n("u_roughness",.42f);n("u_roughnessSize",.5f);n("u_fiber",.3f);n("u_fiberSize",.2f);n("u_crumples",.35f);n("u_crumpleCount",5);n("u_crumpleSize",.35f);n("u_foldCount",5);n("u_drops",.2f);n("u_folds",.65f);n("u_foldSizeX",.5f);n("u_foldSizeY",.5f);n("u_wrinkleSize",.5f);n("u_seed",5.8f);
        n("u_spread",.45f);n("u_bias",.5f);n("u_count",20);n("u_dispersion",.65f);n("u_dispersionColor",.5f);n("u_perspective",.4f);n("u_focusCenter",.55f);n("u_focusEdges",.8f);n("u_lensCircle",.1f);n("u_lensBulge",.3f);n("u_noiseFrequency",.5f);n("u_imageX",.5f);n("u_imageY",.5f);
        if(index==3)n("u_type",1);if(index==4){n("u_shape",1);n("u_distortion",.5f);}if(index==0){auto back=f.background;back.a=0;auto front=f.accent;front.a=.55f;set("u_colorBack",color(back));set("u_colorFront",color(front));}
        const float viewAspect=float(width)/std::max(1u,height),imageAspect=pixels?float(pixels->width)/std::max(1u,pixels->height):1.f;
        // Native equivalent of Paper's cover sizing and Nomand distortion overscan.
        float overscan=1;if(index==5)overscan=1/(1-2*.02f*(2*.65f+1.5f*.35f+.2f*.2f+.75f*.42f+.1f*.3f))+.01f;
        if(index==6){const float outRadius=.5f*std::hypot(16.f/9.f,1.f),reach=.7f*std::pow(.45f,1.3f+2.7f*.45f),headroom=std::max(.35f,1-reach*.2f/outRadius),bulge=.3f*1.4f;
            float lo=std::max(1.f,2*outRadius*bulge/1.45f),hi=1.8f;for(int i=0;i<32;++i){const float mid=(lo+hi)*.5f,rn=2*outRadius/mid,push=std::tan(rn*bulge)/std::tan(bulge)/rn;if(push>mid*headroom)lo=mid;else hi=mid;}overscan=std::min(1.8f,hi+.01f);}
        values[63]={std::min(1.f,viewAspect/imageAspect)/overscan,std::min(1.f,imageAspect/viewAspect)/overscan,0,0};
        values[64]=color(f.background);values[65]=color(f.accent);values[66]=color(f.background);values[67]=color(f.ink);values[67][3]=.4f;
    }
    void Draw(const RiftFrame& f,ID3D11RenderTargetView* target,UINT width,UINT height){if(f.motionBackground==0||f.motionBackground==4)return;Image(f);
        const bool reset=identity!=f.identity||!lastNow||f.discontinuity;
        if(reset){clock=f.following?float(f.positionMs)*.0003f:0;seconds=f.following?float(f.positionMs)*.001f:0;speed=.3f;}else if(f.playing&&!f.reduced&&f.following){const float dt=f.deltaSeconds;speed+=(.3f+f.low*.5f-speed)*(1-std::exp(-dt*4));clock+=dt*speed;seconds+=dt;}
        identity=f.identity;lastPosition=f.positionMs;lastNow=f.nowMs;
        context->OMSetRenderTargets(1,&target,nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);const D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        auto* cb=buffer.Get();auto* sm=sampler.Get();context->PSSetConstantBuffers(0,1,&cb);context->PSSetSamplers(0,1,&sm);ID3D11ShaderResourceView* textures[]={image.Get(),noise.Get()};context->PSSetShaderResources(0,2,textures);
        if(f.motionBackground==1){if(!monet)Hr(device->CreatePixelShader(g_FoliaMonetPS,sizeof(g_FoliaMonetPS),nullptr,&monet));values={};
            const float at=std::fmod(f.reduced||!f.following?0.f:seconds,240.f)/240*156;const size_t index=std::min<size_t>(155,static_cast<size_t>(at));std::array<float,3> drift{};for(size_t c=0;c<3;++c)drift[c]=driftTrack[index][c]+(driftTrack[index+1][c]-driftTrack[index][c])*(at-static_cast<float>(index));
            values[0]={float(width),float(height),pixels?float(pixels->width)/std::max(1u,pixels->height):1.f,image?1.f:0.f};values[1]={drift[0]*.017f,drift[1]*.017f,1.044f+.0175f*(drift[2]+1)*.5f,0};values[2]={f.background.r,f.background.g,f.background.b,1};values[3]={f.ink.r,f.ink.g,f.ink.b,1};values[4]={f.accent.r,f.accent.g,f.accent.b,1};context->UpdateSubresource(buffer.Get(),0,nullptr,values.data(),0,0);context->PSSetShader(monet.Get(),nullptr,0);context->Draw(3,0);}
        else {const int filters[]={2,4,5,3,6};const int main=f.motionBackground==3?1:filters[std::clamp(f.motionFilter,0,4)];Shader(main);Uniforms(f,main,width,height);context->UpdateSubresource(buffer.Get(),0,nullptr,values.data(),0,0);context->PSSetShader(shaders[main].Get(),nullptr,0);context->Draw(3,0);
            if(f.motionBackground==3){Shader(0);Uniforms(f,0,width,height);context->UpdateSubresource(buffer.Get(),0,nullptr,values.data(),0,0);context->PSSetShader(shaders[0].Get(),nullptr,0);context->Draw(3,0);}}
        ID3D11ShaderResourceView* none[2]={};context->PSSetShaderResources(0,2,none);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};
FoliaBackdrop::FoliaBackdrop():impl_(std::make_unique<Impl>()){}FoliaBackdrop::~FoliaBackdrop()=default;void FoliaBackdrop::Create(ID3D11Device* d){impl_->Create(d);}void FoliaBackdrop::Draw(const RiftFrame& f,ID3D11RenderTargetView* rt,UINT w,UINT h){impl_->Draw(f,rt,w,h);}
}
