// SPDX-License-Identifier: AGPL-3.0-only
// Native camera/texture/instancing host for the GPL-3.0 Mineradio shaders.
#include "mineradio_stage.h"
#include "rift_stage.h"
#include "background.h"
#include "resource.h"
#include "shaders/g_MineradioVS.h"
#include "shaders/g_MineradioPS.h"
#include "shaders/g_NativeTerrainVS.h"
#include "shaders/g_NativeTerrainPS.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace stage {
using Microsoft::WRL::ComPtr;
namespace {void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Mineradio native D3D resource failed");}}
struct MineradioStage::Impl {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11VertexShader> terrainVS;ComPtr<ID3D11PixelShader> terrainPS;ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11DepthStencilView> depthView;UINT depthWidth=0,depthHeight=0;
    std::array<std::array<float,4>,10> rings{};size_t nextRing=0;float lastPulse=0,lastRing=-10;
    ComPtr<ID3D11ShaderResourceView> cover,previous,edge;
    ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11BlendState> blend;ComPtr<ID3D11RasterizerState> raster;
    std::shared_ptr<const BackgroundFrame> pixels;
    int64_t lastPosition=-1;float time=0,theta=0,phi=0,radius=8,punch=0;
    void Create(ID3D11Device* d){device=d;device->GetImmediateContext(&context);
        Hr(device->CreateVertexShader(g_MineradioVS,sizeof(g_MineradioVS),nullptr,&vs));Hr(device->CreatePixelShader(g_MineradioPS,sizeof(g_MineradioPS),nullptr,&ps));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=20*16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Hr(device->CreateBuffer(&bd,nullptr,&constants));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Hr(device->CreateSamplerState(&sd,&sampler));
        D3D11_BLEND_DESC b{};auto& r=b.RenderTarget[0];r.BlendEnable=TRUE;r.SrcBlend=D3D11_BLEND_ONE;r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;Hr(device->CreateBlendState(&b,&blend));
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;Hr(device->CreateRasterizerState(&rs,&raster));
    }
    void Texture(const RiftFrame& f){if(pixels==f.cover)return;pixels=f.cover;previous=cover;cover.Reset();edge.Reset();if(!pixels)return;
        const auto& p=*pixels;if(!p.width||!p.height||p.width>3840||p.height>3840||p.stride<p.width*4||uint64_t(p.stride)*p.height>p.bgra.size())return;
        D3D11_TEXTURE2D_DESC td{};td.Width=p.width;td.Height=p.height;td.ArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{p.bgra.data(),p.stride,0};ComPtr<ID3D11Texture2D> tx;Hr(device->CreateTexture2D(&td,&data,&tx));Hr(device->CreateShaderResourceView(tx.Get(),nullptr,&cover));
        // Image luminance/edge data only. No AI depth is inferred or claimed.
        std::vector<uint8_t> edges(size_t(p.width)*p.height*4);auto lum=[&](UINT x,UINT y){const auto* q=p.bgra.data()+size_t(y)*p.stride+x*4;return (.299f*q[2]+.587f*q[1]+.114f*q[0])/255;};
        for(UINT y=0;y<p.height;++y)for(UINT x=0;x<p.width;++x){const float l=lum(x,y),e=std::min(1.f,std::abs(l-lum(std::min(x+1,p.width-1),y))*4+std::abs(l-lum(x,std::min(y+1,p.height-1)))*4);
            auto* q=edges.data()+(size_t(y)*p.width+x)*4;q[0]=255;q[1]=static_cast<uint8_t>(e*255);q[2]=128;q[3]=static_cast<uint8_t>(l*255);}
        data={edges.data(),p.width*4,0};tx.Reset();Hr(device->CreateTexture2D(&td,&data,&tx));Hr(device->CreateShaderResourceView(tx.Get(),nullptr,&edge));if(!previous)previous=cover;
    }
    void Draw(const RiftFrame& f,ID3D11RenderTargetView* target,UINT width,UINT height,bool reset){Texture(f);
        const int preset=NormalizeMineradioPreset(f.particlePreset);const float dt=reset?0:f.deltaSeconds;lastPosition=f.positionMs;
        if(preset!=7&&preset!=8){depthView.Reset();depthWidth=depthHeight=0;}
        const bool terrain=preset==7||preset==8;
        if(reset){time=f.following?float(f.positionMs)*.001f:0;theta=phi=punch=0;radius=preset==5?25.f:8.f;rings={};nextRing=0;lastRing=-10;lastPulse=0;}
        if(f.playing&&!f.reduced)time+=dt;
        const float moving=f.reduced||!f.following?0:std::clamp(f.camera,0.f,2.f);
        if(f.playing&&!f.reduced){punch=std::max(punch*std::pow(.12f,dt),f.pulse);
            const float px=f.pointerActive?(f.pointerX/f.width-.5f):0,py=f.pointerActive?(f.pointerY/f.height-.5f):0;
            const float targetTheta=(f.motionCamera==1?px*1.2f:std::sin(time*.055f)*.20f)*moving,targetPhi=(f.motionCamera==1?-py*.8f:std::sin(time*.043f)*.10f)*moving;
            // Source updateCamera uses .10 angular and .07 radial easing at 60 Hz.
            theta+=(targetTheta-theta)*(1-std::pow(.90f,dt*60));phi+=(targetPhi-phi)*(1-std::pow(.90f,dt*60));
            const float base=preset==5?25.f:f.motionCamera==2?6.2f:8.f;const float loudness=std::sqrt(std::clamp(f.rms*f.response,0.f,1.f));const float wanted=base-(punch*.54f+loudness*.85f)*moving;radius+=(wanted-radius)*(1-std::pow(.93f,dt*60));}
        if(terrain&&!f.reduced&&f.playing&&f.pulse>lastPulse+.2f&&time-lastRing>.24f){const float n=static_cast<float>(nextRing);rings[nextRing%10]={std::sin(n*1.71f)*2,std::cos(n*2.31f)*2,time,f.pulse};++nextRing;lastRing=time;}lastPulse=f.pulse;
        const float grid=terrain?(f.quality>=2?96.f:144.f):(f.quality>=3?96.f:f.quality>=2?144.f:208.f),cameraPhi=phi+(terrain?.28f:0);
        std::array<std::array<float,4>,20> cb={{{float(width),float(height),time,f.dpi},{f.low,f.mid,f.high,f.reduced?0:punch},{float(preset),f.response,f.quality>=2?.75f:1.f,1},
            {.25f,0,cover?1.f:0.f,1},{radius*std::cos(cameraPhi)*std::sin(theta),radius*std::sin(cameraPhi),radius*std::cos(cameraPhi)*std::cos(theta),.68f-punch*.03f*moving},{0,0,terrain?-3.f:0.f,0},
            {(f.pointerX/f.width-.5f)*5.12f,(.5f-f.pointerY/f.height)*5.12f,f.pointerActive?1.f:0.f,grid},{f.accent.r,f.accent.g,f.accent.b,0}}};
        for(size_t band=0;band<8;++band){float value=0;for(size_t i=0;i<4;++i)value+=f.spectrum[band*4+i];cb[8+band/4][band%4]=std::clamp(value*.25f,0.f,1.f);}for(size_t i=0;i<rings.size();++i)cb[10+i]=rings[i];
        context->UpdateSubresource(constants.Get(),0,nullptr,cb.data(),0,0);auto* c=constants.Get();context->VSSetConstantBuffers(0,1,&c);context->PSSetConstantBuffers(0,1,&c);
        if(terrain){if(!terrainVS){Hr(device->CreateVertexShader(g_NativeTerrainVS,sizeof(g_NativeTerrainVS),nullptr,&terrainVS));Hr(device->CreatePixelShader(g_NativeTerrainPS,sizeof(g_NativeTerrainPS),nullptr,&terrainPS));
                D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;Hr(device->CreateDepthStencilState(&ds,&depthState));}
            if(width!=depthWidth||height!=depthHeight){depthView.Reset();D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=d.MipLevels=1;d.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
                ComPtr<ID3D11Texture2D> tx;Hr(device->CreateTexture2D(&d,nullptr,&tx));Hr(device->CreateDepthStencilView(tx.Get(),nullptr,&depthView));depthWidth=width;depthHeight=height;}
            context->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH,1,0);}
        context->OMSetRenderTargets(1,&target,terrain?depthView.Get():nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(terrain?depthState.Get():nullptr,0);
        const D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(terrain?terrainVS.Get():vs.Get(),nullptr,0);context->PSSetShader(terrain?terrainPS.Get():ps.Get(),nullptr,0);
        context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        ID3D11ShaderResourceView* textures[]={cover.Get(),previous.Get(),edge.Get(),nullptr};auto* s=sampler.Get();context->VSSetShaderResources(0,4,textures);context->VSSetSamplers(0,1,&s);
        context->DrawInstanced(terrain?36:6,static_cast<UINT>(grid*grid)+(terrain?64:0),0,0);
        ID3D11ShaderResourceView* none[4]={};context->VSSetShaderResources(0,4,none);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};
MineradioStage::MineradioStage():impl_(std::make_unique<Impl>()){}MineradioStage::~MineradioStage()=default;
void MineradioStage::Create(ID3D11Device* d){impl_->Create(d);}void MineradioStage::Draw(const RiftFrame& f,ID3D11RenderTargetView* t,UINT w,UINT h,bool reset){impl_->Draw(f,t,w,h,reset);}
}
