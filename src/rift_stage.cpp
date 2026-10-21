#include "rift_stage.h"
#include "mineradio_stage.h"
#include "folia_diorama.h"
#include "folia_backdrop.h"
#include "shaders/g_FoliaTransitionVS.h"
#include "shaders/g_FoliaTransitionPS.h"
#include "shaders/g_FoliaLayerPS.h"
#include "folia_camera.h"
#include "folia_programs.h"
#include "lyric_glyphs.h"
#include "lyric_performance.h"
#include "lyric_motion_balance.h"
#include "glyph_ornament.h"
#include "glyph_effects.h"
#include "lyric_glow.h"
#include "resource.h"
#include "shaders/g_RiftVS.h"
#include "shaders/g_RiftPS.h"
#include "shaders/g_RiftGlowPS.h"
#include "shaders/g_ThemeDetailPS.h"
#include "lyric_typography.h"
#include "optional_lyric_font.h"
#include "shaders/g_LumiereVS.h"
#include "shaders/g_LumierePS.h"
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite_3.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stage {
namespace {
using Microsoft::WRL::ComPtr;
constexpr float Pi=3.14159265358979323846f,Em=192.f,Atlas=2048.f;
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Rift native layer HRESULT "+std::to_string(static_cast<unsigned long>(hr)));}
float Unit(float v){return std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;}
float Smooth(float v){v=Unit(v);return v*v*(3-2*v);}
float Ease(float v){v=Unit(v);return 1-std::pow(1-v,3.f);}
float Spring(float v){v=Unit(v);return 1-std::exp(-6*v)*std::cos(v*8.f);}
int64_t AddTime(int64_t start,int64_t duration){
    duration=std::max<int64_t>(0,duration);
    return start>std::numeric_limits<int64_t>::max()-duration?std::numeric_limits<int64_t>::max():start+duration;
}
uint32_t Hash(std::wstring_view s){uint32_t h=2166136261u;for(wchar_t c:s){h^=c;h*=16777619u;}return h;}
float Seed(uint32_t n){n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;return (n&0xffffff)/16777216.f;}
bool BackdropContrast(const RiftFrame& f){return f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Mineradio&&f.motionBackground>=1&&f.motionBackground<=3;}
struct Vertex{float x,y,u,v,r,g,b,a,kind,edge,p0,p1;float fill=-1,sweepStart=0,sweepAdvance=1;};
struct GlyphPart {size_t run=0,first=0,last=0;};
struct Cell {
    size_t run=0,first=0,last=0;
    UINT32 text=0,length=0;
    float advance=0,ascent=0,descent=0;
    float wordOffset=0,wordAdvance=0;
    int64_t start=0,duration=0;
    size_t page=0;
    bool blank=false;
    std::vector<GlyphPart> parts;
};
struct Page {size_t first=0,last=0;int64_t start=0,end=0;};
struct LineCache {
    size_t index=SIZE_MAX;
    size_t missingGlyphs=0;
    glyphs::Layout layout;
    ComPtr<IDWriteTextLayout> textLayout;
    std::vector<Cell> cells;
    std::vector<Page> pages;
    uint32_t hash=0;
    int family=0;
    bool timed=false,requestedPrecise=false;
};
struct Sprite {
    Cell cell;
    const glyphs::Run* run=nullptr;
    float u0=0,v0=0,u1=0,v1=0,quadW=0,quadH=0;
    float anchorX=0,anchorY=0,baseScale=1,tilt=0;
    size_t ordinal=0,row=0;
    float sweepStart=0,sweepAdvance=1;
};
}

struct RiftStage::Impl {
    GlyphEffects glyphEffects;GlyphEffectFrame glyphEffectFrame;bool requestedReduced=false;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteFactory5> write5;ComPtr<IDWriteInMemoryFontFileLoader> fontLoader;
    ComPtr<IDWriteFontCollection1> fonts;std::wstring family=L"Microsoft YaHei UI";
    ComPtr<IDWriteFontCollection> systemFonts;
    std::array<LyricTypeface,3> typefaces;
    OptionalLyricFont optionalFont;
    ComPtr<ID2D1Factory1> factory;ComPtr<ID2D1Device> d2d;ComPtr<ID2D1DeviceContext> atlasDraw;
    ComPtr<ID2D1SolidColorBrush> white;
    ComPtr<ID3D11Texture2D> atlasTexture,outputTexture;
    ComPtr<ID3D11Texture2D> glowTexture;
    ComPtr<ID3D11RenderTargetView> glowTarget;
    ComPtr<ID2D1Bitmap1> glowBitmap;
    ComPtr<ID2D1Effect> glowDpi,glowNear,glowFar,glowGain;
    ComPtr<ID3D11PixelShader> glowPS;
    ComPtr<ID3D11PixelShader> detailPS;ComPtr<ID3D11VertexShader> detailVS;ComPtr<ID3D11Buffer> detailConstants;
    UINT glowWidth=0,glowHeight=0;float glowDipWidth=0,glowDipHeight=0;
    ComPtr<ID3D11Texture2D> programTexture;
    ComPtr<ID3D11ShaderResourceView> programView;
    ComPtr<ID3D11RenderTargetView> programOutput;
    ComPtr<ID3D11VertexShader> lightVS;ComPtr<ID3D11PixelShader> lightPS;ComPtr<ID3D11Buffer> lightConstants;
    ComPtr<ID3D11ShaderResourceView> atlasView;
    ComPtr<ID3D11RenderTargetView> outputView;
    ComPtr<ID2D1Bitmap1> atlasBitmap,outputBitmap;
    ComPtr<ID2D1Bitmap1> programBitmap;
    ComPtr<ID2D1Bitmap1> decorBitmap;
    ComPtr<ID2D1Bitmap1> portrait;std::shared_ptr<const BackgroundFrame> portraitSource;uint64_t portraitSequence=UINT64_MAX;
    float clockPhase=0,clockBass=0,secondAngle=0,secondVelocity=0,secondElapsed=0,secondStep=0;
    struct ContextLine {size_t index=0;ComPtr<IDWriteTextLayout> layout;float width=0,height=0;};
    std::vector<ContextLine> contextLines;int contextCount=-1;
    ComPtr<ID3D11Texture2D> departingTexture,transitionTexture;
    ComPtr<ID3D11ShaderResourceView> departingView,sceneView;
    ComPtr<ID3D11RenderTargetView> transitionOutput;
    ComPtr<ID2D1Bitmap1> transitionBitmap;ID2D1Bitmap1* presentBitmap=nullptr;
    ComPtr<ID3D11VertexShader> transitionVS;ComPtr<ID3D11PixelShader> transitionPS;ComPtr<ID3D11Buffer> transitionConstants;
    ComPtr<ID3D11VertexShader> layerVS;ComPtr<ID3D11PixelShader> layerPS;
    bool captureTransition=false,hasOutput=false;int64_t transitionAt=-10000,transitionDuration=240;int transitionKind=0;
    FoliaDiorama diorama;std::unique_ptr<FoliaBackdrop> backdrop;
    FoliaPrograms programs;std::unique_ptr<MineradioStage> mineradio;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> vertexBuffer,constants;
    ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11BlendState> blend;ComPtr<ID3D11RasterizerState> raster;
    UINT pixelWidth=0,pixelHeight=0;float pixelDpi=0;size_t vertexCapacity=0;
    std::vector<Vertex> vertices;
    LineCache current,previous,upcoming;
    std::vector<Sprite> sprites,oldSprites,waitingSprites;
    struct GlyphPose {size_t ordinal=0;std::array<Vertex,4> corners{};float fill=0;};
    std::vector<GlyphPose> drawnGlyphs,departureGlyphs,arrivalGlyphs;
    size_t previousPage=0;int64_t assemblyAt=-10000;
    bool Reassembles(const RiftFrame& f)const{return f.kind==LyricMotion::Rift||f.kind==LyricMotion::Tempera||f.kind==LyricMotion::Diorama||(f.kind==LyricMotion::Mineradio&&f.particlePreset==7);}
    std::vector<folia::Point> worldOrigins;std::vector<int> families;
    folia::CameraMotion camera;
    bool lastFollowing=false;
    LyricMotion lastKind=LyricMotion::Rift;
    int lastVariant=-1;
    int lastParticlePreset=-1;
    std::wstring identity;uint64_t revision=0;
    size_t lastLine=SIZE_MAX,lastPage=SIZE_MAX;int lastFamily=-1;
    int64_t lastPosition=0,lastNow=0,cutAt=0;
    float heldPulse=0,lastLayoutWidth=0,lastLayoutHeight=0,lastLyricSize=0;
    bool lastTranslation=false,lastRomanization=false;
    RiftReading reading;int lastLayoutOverlaps=0;
    D2D1_POINT_2F fireflyPosition{};bool fireflyReady=false;
    float fireflyWidth=0,fireflyHeight=0;
    struct AuxiliaryPose {
        bool valid=false;float c=1,s=0,scale=1,left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;
    } auxiliaryPose;
    struct AuxiliaryLayout {
        std::wstring text;float width=0,size=0,height=0,inkWidth=0;ComPtr<IDWriteTextLayout> layout;
    } auxiliaryLayouts[2];
    bool resetFrame=true;
    std::array<float,32> frozenSpectrum{};float frozenLow=0,frozenMid=0,frozenHigh=0,frozenRms=0,frozenPulse=0;
    bool audioPoseValid=false;std::wstring audioIdentity;uint64_t audioRevision=0;LyricMotion audioKind=LyricMotion::Rift;int audioPreset=-1;int64_t audioPosition=0;

    // Presentation follows measured positions; it never advances the player
    // snapshot or reveals future lyrics. Unknown transport is not a pause.
    std::wstring clockIdentity;uint64_t clockRevision=0;
    LyricMotion clockKind=LyricMotion::Rift;int clockVariant=-1,clockPreset=-1;
    int64_t rawPosition=0,rawNow=0,rawSample=0,lastAdvance=0,advanceWindow=180;
    double shownPosition=0;bool clockFollowing=false;
    RiftFrame Presentation(const RiftFrame& input){
        auto f=input;
        if(f.kind==LyricMotion::Still){f.reduced=true;f.low=f.mid=f.high=f.rms=f.pulse=0;f.spectrum.fill(0);}
        const bool fresh=f.following&&(!f.sampled||(f.sampledAtMs>0&&f.nowMs>=f.sampledAtMs&&f.nowMs-f.sampledAtMs<=750));
        const bool changed=!rawNow||clockIdentity!=f.identity||clockRevision!=f.revision||clockKind!=f.kind||clockVariant!=f.variant||clockPreset!=f.particlePreset||fresh!=clockFollowing;
        const int64_t elapsed=rawNow?std::max<int64_t>(0,f.nowMs-rawNow):0;
        const int64_t delta=f.positionMs-rawPosition;
        const bool newSample=!f.sampled||f.sampledAtMs!=rawSample;
        const int64_t sampleElapsed=f.sampled?std::max<int64_t>(0,f.sampledAtMs-rawSample):elapsed;
        const bool jump=!changed&&fresh&&newSample&&(delta < -80||
            std::llabs(delta-((f.playing||f.sampled)?sampleElapsed:0))>700);
        f.discontinuity=changed||jump||elapsed>1200;
        if(f.discontinuity){shownPosition=static_cast<double>(f.positionMs);lastAdvance=0;}
        else if(fresh&&newSample&&delta>0){lastAdvance=f.nowMs;advanceWindow=std::clamp<int64_t>(sampleElapsed*2+30,180,750);}
        const bool advancing=fresh&&!f.paused&&(f.playing||(f.sampled&&lastAdvance>0&&f.nowMs-lastAdvance<=advanceWindow));
        f.deltaSeconds=advancing&&!f.discontinuity?std::clamp(float(elapsed)*.001f,0.f,.1f):0.f;
        if(f.sampled&&!f.discontinuity&&!f.paused&&fresh){
            // Causal interpolation: always within the already observed range,
            // at most 140 ms behind it. Never extrapolate unknown play state.
            shownPosition=std::max(shownPosition,double(f.positionMs)-140.);
            shownPosition+=(double(f.positionMs)-shownPosition)*(1-std::exp(-double(elapsed)/40.));
            shownPosition=std::min(shownPosition,double(f.positionMs));
        }else shownPosition=static_cast<double>(f.positionMs);
        clockIdentity=f.identity;clockRevision=f.revision;clockKind=f.kind;clockVariant=f.variant;clockPreset=f.particlePreset;
        rawPosition=f.positionMs;rawNow=f.nowMs;rawSample=f.sampledAtMs;clockFollowing=fresh;
        f.positionMs=static_cast<int64_t>(std::llround(shownPosition));f.playing=advancing;
        return f;
    }

    RiftFrame StableAudio(const RiftFrame& input){
        auto frame=input;const bool same=audioPoseValid&&audioIdentity==input.identity&&audioRevision==input.revision&&audioKind==input.kind&&audioPreset==input.particlePreset;
        if(input.following&&!input.playing){
            if(same&&std::llabs(input.positionMs-audioPosition)<=700){frame.spectrum=frozenSpectrum;frame.low=frozenLow;frame.mid=frozenMid;frame.high=frozenHigh;frame.rms=frozenRms;frame.pulse=frozenPulse;}
            else {frame.spectrum.fill(0);frame.low=frame.mid=frame.high=frame.rms=frame.pulse=0;}
        }
        frozenSpectrum=frame.spectrum;frozenLow=frame.low;frozenMid=frame.mid;frozenHigh=frame.high;frozenRms=frame.rms;frozenPulse=frame.pulse;
        audioPoseValid=true;audioIdentity=input.identity;audioRevision=input.revision;audioKind=input.kind;audioPreset=input.particlePreset;audioPosition=input.positionMs;return frame;
    }

    ~Impl(){fonts.Reset();if(write5&&fontLoader)write5->UnregisterFontFileLoader(fontLoader.Get());}
    void Font(){
        const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(IDR_RIFT_FONT),RT_RCDATA);
        if(!resource||FAILED(write.As(&write5)))return;
        const auto data=LockResource(LoadResource(nullptr,resource));const auto bytes=SizeofResource(nullptr,resource);
        if(!data||!bytes)return;
        Check(write5->CreateInMemoryFontFileLoader(&fontLoader));Check(write5->RegisterFontFileLoader(fontLoader.Get()));
        ComPtr<IDWriteFontFile> file;Check(fontLoader->CreateInMemoryFontFileReference(write.Get(),data,bytes,nullptr,&file));
        ComPtr<IDWriteFontSetBuilder1> builder;Check(write5->CreateFontSetBuilder(&builder));
        Check(builder->AddFontFile(file.Get()));ComPtr<IDWriteFontSet> set;Check(builder->CreateFontSet(&set));
        Check(write5->CreateFontCollectionFromFontSet(set.Get(),&fonts));
        ComPtr<IDWriteFontFamily> fontFamily;Check(fonts->GetFontFamily(0,&fontFamily));
        ComPtr<IDWriteLocalizedStrings> names;Check(fontFamily->GetFamilyNames(&names));
        UINT32 length=0;Check(names->GetStringLength(0,&length));std::vector<wchar_t> name(length+1);
        Check(names->GetString(0,name.data(),length+1));family=name.data();
    }
    void Create(ID3D11Device* d,IDWriteFactory* w){
        glyphEffects.Create(d);
        device=d;write=w;device->GetImmediateContext(&context);Font();Check(write->GetSystemFontCollection(&systemFonts));
        for(int i=0;i<3;++i)typefaces[i]=ResolveLyricTypeface(systemFonts.Get(),static_cast<LyricTypefaceKind>(i));
        ComPtr<IDXGIDevice> dxgi;Check(device.As(&dxgi));Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));
        Check(factory->CreateDevice(dxgi.Get(),&d2d));Check(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&atlasDraw));
        atlasDraw->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        Check(atlasDraw->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&white));
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=static_cast<UINT>(Atlas);td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&td,nullptr,&atlasTexture));Check(device->CreateShaderResourceView(atlasTexture.Get(),nullptr,&atlasView));
        ComPtr<IDXGISurface> surface;Check(atlasTexture.As(&surface));
        auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(td.Format,D2D1_ALPHA_MODE_PREMULTIPLIED));
        Check(atlasDraw->CreateBitmapFromDxgiSurface(surface.Get(),&props,&atlasBitmap));
        Check(device->CreateVertexShader(g_RiftVS,sizeof(g_RiftVS),nullptr,&vs));Check(device->CreatePixelShader(g_RiftPS,sizeof(g_RiftPS),nullptr,&ps));
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0}};
        Check(device->CreateInputLayout(elements,5,g_RiftVS,sizeof(g_RiftVS),&inputLayout));
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=48;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Check(device->CreateBuffer(&cb,nullptr,&constants));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Check(device->CreateSamplerState(&sd,&sampler));
        D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_ONE;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;Check(device->CreateBlendState(&bd,&blend));
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;Check(device->CreateRasterizerState(&rd,&raster));
    }
    float Desired(const RiftFrame& f)const{
        const float base=f.kind==LyricMotion::Rift?std::min(f.width*.105f,f.height*.205f):
            f.height*(f.kind==LyricMotion::Cappella||f.kind==LyricMotion::Monet?.06f:f.kind==LyricMotion::Sonnet?.14f:f.kind==LyricMotion::Cadenza?.13f:f.kind==LyricMotion::Tilt?.125f:f.kind==LyricMotion::Classic?.115f:.10f);
        return std::clamp(base*(f.lyricSize/44.f),26.f,240.f);
    }
    LyricTypefaceKind TypefaceKind(const RiftFrame& f)const {
        return ThemeTypeface(f.kind==LyricMotion::Mineradio?21+f.particlePreset:6+static_cast<int>(f.kind));
    }
    void LyricFormat(const RiftFrame& f,float size,ComPtr<IDWriteTextFormat>& format) {
        if(optionalFont.Active()){
            Check(write->CreateTextFormat(optionalFont.Family().c_str(),optionalFont.Collection(),optionalFont.Weight(),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-CN",&format));return;
        }
        const auto kind=TypefaceKind(f);const auto& face=typefaces[static_cast<int>(kind)];
        const bool embedded=kind==LyricTypefaceKind::Display&&fonts;
        Check(write->CreateTextFormat(embedded?family.c_str():face.family.c_str(),embedded?fonts.Get():systemFonts.Get(),
            embedded?DWRITE_FONT_WEIGHT_NORMAL:face.weight,embedded?DWRITE_FONT_STYLE_OBLIQUE:DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-CN",&format));
    }
    LineCache Shape(const RiftFrame& f,size_t index){
        LineCache out;out.index=index;const auto& line=f.lyrics->lines[index];out.hash=Hash(line.text);
        out.family=index<families.size()?families[index]:0;
        out.timed=f.precise&&!line.words.empty();
        out.requestedPrecise=f.precise;
        struct Span{UINT32 begin,end;int64_t start,duration;};std::vector<Span> spans;
        if(out.timed){size_t cursor=0;for(const auto& word:line.words){
            if(word.text.empty())continue;const auto at=line.text.find(word.text,cursor);
            if(at==std::wstring::npos){out.timed=false;spans.clear();break;}
            spans.push_back({static_cast<UINT32>(at),static_cast<UINT32>(at+word.text.size()),word.startMs,word.durationMs});cursor=at+word.text.size();}}
        const auto timingSpan=[&](UINT32 position,UINT32 length){
            for(size_t i=0;i<spans.size();++i)if(position<spans[i].end&&position+std::max(1u,length)>spans[i].begin)return i;
            return SIZE_MAX;
        };
        ComPtr<IDWriteTextFormat> format;
        LyricFormat(f,Em,format);
        Check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        Check(write->CreateTextLayout(line.text.c_str(),static_cast<UINT32>(line.text.size()),format.Get(),65536,512,&out.textLayout));
        // Let one system font shape the complete accented cluster. Smiley Sans
        // has Latin bases but not all combining marks; fallback of the mark
        // alone can otherwise produce a spacing accent beside the base.
        const auto combining=[&](size_t i){WORD type=0;GetStringTypeW(CT_CTYPE3,line.text.data()+i,1,&type);
            return (type&(C3_NONSPACING|C3_DIACRITIC|C3_VOWELMARK))!=0;};
        for(size_t i=1;i<line.text.size();++i)if(combining(i)){
            size_t begin=i-1,end=i+1;while(begin>0&&combining(begin))--begin;
            while(end<line.text.size()&&combining(end))++end;
            const DWRITE_TEXT_RANGE range{static_cast<UINT32>(begin),static_cast<UINT32>(end-begin)};
            Check(out.textLayout->SetFontCollection(systemFonts.Get(),range));Check(out.textLayout->SetFontFamilyName(L"Segoe UI",range));i=end-1;
        }
        Check(glyphs::Capture(out.textLayout.Get(),out.layout));
        if(!out.layout.valid)return out;
        for(const auto& run:out.layout.runs)for(size_t g=0;g<run.indices.size();++g)
            if(run.indices[g]==0&&run.textPositions[g]<line.text.size()&&!iswspace(line.text[run.textPositions[g]]))++out.missingGlyphs;
        for(size_t r=0;r<out.layout.runs.size();++r){const auto& run=out.layout.runs[r];
            for(size_t j=0;j<run.indices.size();){
                size_t end=j+1;const UINT32 pos=run.textPositions[j];
                while(end<run.indices.size()&&run.textPositions[end]==pos)++end;
                UINT32 length=run.textLengths[j];
                // Latin letters stay in a word; CJK and combining clusters keep native boundaries.
                if(pos<line.text.size()&&((line.text[pos]>=L'A'&&line.text[pos]<=L'Z')||(line.text[pos]>=L'a'&&line.text[pos]<=L'z'))){
                    float grouped=0;for(size_t k=j;k<end;++k)grouped+=run.advances[k];
                    while(end<run.indices.size()){
                        const auto next=run.textPositions[end];if(next>=line.text.size())break;const auto c=line.text[next];
                        if(!((c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z')||c==L'\''))break;
                        // Keep real timing boundaries even inside a Latin token.
                        // Native ligature/combining clusters above remain indivisible.
                        if(out.timed&&timingSpan(pos,length)!=timingSpan(next,run.textLengths[end]))break;
                        size_t clusterEnd=end+1;while(clusterEnd<run.indices.size()&&run.textPositions[clusterEnd]==next)++clusterEnd;
                        float clusterWidth=0;for(size_t k=end;k<clusterEnd;++k)clusterWidth+=run.advances[k];
                        if(grouped+clusterWidth>Em*4.f)break;
                        length=std::max(length,next+run.textLengths[end]-pos);grouped+=clusterWidth;end=clusterEnd;
                    }
                }
                if(run.Rtl()){end=run.indices.size();length=static_cast<UINT32>(line.text.size());}
                float advance=0;for(size_t k=j;k<end;++k)advance+=run.advances[k];
                Cell cell;cell.run=r;cell.first=j;cell.last=end;cell.text=pos;cell.length=length;
                cell.advance=std::max(advance,1.f);cell.ascent=run.ascent;cell.descent=run.descent;
                cell.start=line.startMs;cell.duration=std::max<int64_t>(1,line.durationMs);
                cell.blank=pos>=line.text.size()||std::all_of(line.text.begin()+pos,line.text.begin()+std::min<size_t>(line.text.size(),pos+length),[](wchar_t c){return iswspace(c)!=0;});
                if(out.timed){
                    bool found=false;
                    for(const auto& word:spans){
                        if(pos<word.end&&pos+std::max(1u,length)>word.begin){cell.start=word.start;cell.duration=std::max<int64_t>(1,word.duration);found=true;break;}}
                    if(!found)cell.start=line.startMs;
                }
                cell.parts.push_back({r,j,end});out.cells.push_back(std::move(cell));j=end;
            }
        }
        // Fallback glyph runs arrive separately; source order survives shaping.
        std::stable_sort(out.cells.begin(),out.cells.end(),[](const Cell& a,const Cell& b){return a.text<b.text;});
        // A font fallback can put a base and its combining mark in different
        // glyph runs. Retain native cluster ownership across those run edges.
        std::vector<UINT32> clusterBegin(line.text.size());
        for(UINT32 i=0;i<clusterBegin.size();++i)clusterBegin[i]=i;
        UINT32 clusterCount=0;out.textLayout->GetClusterMetrics(nullptr,0,&clusterCount);
        if(clusterCount){std::vector<DWRITE_CLUSTER_METRICS> clusters(clusterCount);
            Check(out.textLayout->GetClusterMetrics(clusters.data(),clusterCount,&clusterCount));
            UINT32 at=0;for(const auto& cluster:clusters){const auto end=std::min<size_t>(line.text.size(),size_t(at)+cluster.length);
                for(size_t j=at;j<end;++j)clusterBegin[j]=at;at=static_cast<UINT32>(end);}}
        for(size_t i=1;i<line.text.size();++i){WORD type=0;GetStringTypeW(CT_CTYPE3,line.text.data()+i,1,&type);
            const auto c=line.text[i],before=line.text[i-1];
            if((type&(C3_NONSPACING|C3_DIACRITIC|C3_VOWELMARK))||c==0x200d||before==0x200d||
                (c>=0xfe00&&c<=0xfe0f)||(c>=0xdc00&&c<=0xdfff&&before>=0xd800&&before<=0xdbff))clusterBegin[i]=clusterBegin[i-1];}
        std::vector<Cell> joined;
        for(auto& cell:out.cells){const UINT32 owner=cell.text<clusterBegin.size()?clusterBegin[cell.text]:cell.text;
            const auto base=std::find_if(joined.begin(),joined.end(),[&](const Cell& v){return owner>=v.text&&owner<size_t(v.text)+v.length;});
            if(base==joined.end()){joined.push_back(std::move(cell));continue;}
            const auto& a=out.layout.runs[base->run];const auto& b=out.layout.runs[cell.run];
            const float delta=b.origin.x+(b.Rtl()?-1.f:1.f)*b.pen[cell.first]-a.origin.x-(a.Rtl()?-1.f:1.f)*a.pen[base->first];
            base->advance=std::max(base->advance,delta+cell.advance);base->ascent=std::max(base->ascent,cell.ascent);base->descent=std::max(base->descent,cell.descent);
            base->length=static_cast<UINT32>(std::max<size_t>(base->length,size_t(cell.text)+cell.length-base->text));
            base->blank=base->blank&&cell.blank;base->parts.insert(base->parts.end(),cell.parts.begin(),cell.parts.end());
        }
        out.cells=std::move(joined);
        // One provider word interval, swept across measured cluster advances.
        // These are colour fractions, never invented character timestamps.
        for(size_t first=0;first<out.cells.size();){size_t end=first+1;
            while(end<out.cells.size()&&out.cells[end].start==out.cells[first].start&&out.cells[end].duration==out.cells[first].duration)++end;
            float width=0;for(size_t i=first;i<end;++i){out.cells[i].wordOffset=width;width+=out.cells[i].advance;}
            for(size_t i=first;i<end;++i)out.cells[i].wordAdvance=width;
            first=end;
        }
        const float desired=Desired(f);
        const float budget=std::max(Em*2.f,f.width*.78f/(desired/Em)*1.58f);
        size_t begin=0;float span=0;
        for(size_t i=0;i<out.cells.size();++i){const float width=out.cells[i].advance+12;
            if(i>begin&&(span+width>budget||i-begin>=22)){
                out.pages.push_back({begin,i,out.cells[begin].start,out.cells[i].start});begin=i;span=0;}
            out.cells[i].page=out.pages.size();span+=width;
        }
        if(begin<out.cells.size())out.pages.push_back({begin,out.cells.size(),out.cells[begin].start,AddTime(line.startMs,std::max<int64_t>(1,line.durationMs))});
        // Paging within a multi-character timed span is presentation scheduling,
        // never new per-character timing in the lyric model.
        if(out.timed)for(size_t i=1;i<out.pages.size();++i){auto& page=out.pages[i];const size_t at=page.first;
            size_t first=at,last=at;while(first>0&&out.cells[first-1].start==out.cells[at].start)--first;
            while(last<out.cells.size()&&out.cells[last].start==out.cells[at].start)++last;
            if(first<at){float before=0,total=0;for(size_t j=first;j<last;++j){total+=out.cells[j].advance;if(j<at)before+=out.cells[j].advance;}
                page.start=AddTime(page.start,static_cast<int64_t>(out.cells[at].duration*double(before)/std::max(1.f,total)));}}
        for(size_t i=0;i+1<out.pages.size();++i)out.pages[i].end=out.pages[i+1].start;
        if(!out.timed&&!out.pages.empty())for(size_t i=0;i<out.pages.size();++i){auto& page=out.pages[i];
            const auto lineSpan=std::max<int64_t>(1,line.durationMs);const auto count=static_cast<int64_t>(out.pages.size());
            page.start=AddTime(line.startMs,lineSpan/count*static_cast<int64_t>(i));
            page.end=AddTime(line.startMs,i+1==out.pages.size()?lineSpan:lineSpan/count*static_cast<int64_t>(i+1));}
        return out;
    }
    size_t PageAt(const RiftFrame& f,const LineCache& line)const{
        if(line.pages.empty())return 0;
        if(!f.following)return std::min(line.pages.size()-1,static_cast<size_t>(std::max(0.f,f.browseOffset)/std::max(1.f,f.height*.60f)));
        size_t page=0;for(size_t i=1;i<line.pages.size();++i)if(f.positionMs>=line.pages[i].start)page=i;return page;
    }
    void RasterPage(const RiftFrame& f,const LineCache& line,size_t page,std::vector<Sprite>& result,float& px,float& py,float& rowHeight){
        result.clear();if(line.pages.empty())return;const auto& range=line.pages[page];
        const float desired=Desired(f);
        const float nominal=desired/Em;
        float total=0;for(size_t i=range.first;i<range.last;++i)total+=line.cells[i].advance+12;
        const float available=f.kind==LyricMotion::Monet?.40f:f.kind==LyricMotion::Cappella?.54f:.78f;
        float rowLimit=std::max(Em*2.f,f.width*available/nominal);
        const bool twoRows=total>rowLimit*.93f||(f.kind==LyricMotion::Rift&&line.family==3&&range.last-range.first>4);
        size_t split=range.last;float rowAccum=0;
        if(twoRows){for(size_t i=range.first;i<range.last;++i){rowAccum+=line.cells[i].advance+12;if(rowAccum>=total*.5f){split=i+1;break;}}}
        int layoutRows=twoRows?2:1;
        if(f.kind==LyricMotion::Tilt){const float score=std::log(static_cast<float>(range.last-range.first)+4)/std::log(24.f)*(.7f+.6f*Seed(line.hash+1))*.75f;layoutRows=score<.45f?1:score<1.05f?2:score<1.7f?3:4;}
        const auto rowAt=[&](size_t i){return f.kind==LyricMotion::Tilt?std::min(layoutRows-1,static_cast<int>((i-range.first)*layoutRows/std::max<size_t>(1,range.last-range.first))):i>=split?1:0;};
        std::array<float,4> widths{};
        for(size_t i=range.first;i<range.last;++i)widths[rowAt(i)]+=line.cells[i].advance+12;
        const float fit=std::min(1.f,rowLimit/std::max(1.f,*std::max_element(widths.begin(),widths.end())));
        std::array<float,4> pen{};
        size_t hero=range.first;int64_t longest=-1;bool vertical=true;
        for(size_t i=range.first;i<range.last;++i){if(!line.cells[i].blank&&line.cells[i].duration>longest){longest=line.cells[i].duration;hero=i;}
            vertical=vertical&&line.cells[i].advance<Em*1.55f;}
        float arcPen=0,crossPen=0,crossWidth=0;
        for(size_t i=range.first;i<range.first+(range.last-range.first)/2;++i)crossWidth+=line.cells[i].advance+12;
        for(size_t i=range.first;i<range.last;++i){const auto& cell=line.cells[i];const auto& run=line.layout.runs[cell.run];
            Sprite s;s.cell=cell;s.run=&run;s.ordinal=i-range.first;
            const float pad=18.f;float left=0,top=-cell.ascent;
            float right=cell.advance,bottom=cell.descent;
            const float originX=run.origin.x+(run.Rtl()?-1.f:1.f)*run.pen[cell.first],originY=run.origin.y;
            bool hasBounds=false;
            if(!cell.blank){
                for(const auto& part:cell.parts){const auto& shaped=line.layout.runs[part.run];const auto slice=glyphs::Slice(shaped,part.first,part.last);
                const float x=shaped.origin.x+(shaped.Rtl()?-1.f:1.f)*shaped.pen[part.first]-originX,y=shaped.origin.y-originY;
                ComPtr<IDWriteGlyphRunAnalysis> analysis;
                if(SUCCEEDED(write->CreateGlyphRunAnalysis(&slice,1.f,nullptr,DWRITE_RENDERING_MODE_NATURAL,DWRITE_MEASURING_MODE_NATURAL,x,y,&analysis))){
                    RECT bounds{};if(SUCCEEDED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1,&bounds))&&bounds.right>bounds.left&&bounds.bottom>bounds.top){
                        left=hasBounds?std::min(left,float(bounds.left)):float(bounds.left);top=hasBounds?std::min(top,float(bounds.top)):float(bounds.top);
                        right=hasBounds?std::max(right,float(bounds.right)):float(bounds.right);bottom=hasBounds?std::max(bottom,float(bounds.bottom)):float(bounds.bottom);hasBounds=true;}
                }}
            }
            s.quadW=std::ceil(right-left+2*pad);s.quadH=std::ceil(bottom-top+2*pad);
            s.sweepStart=(pad-left)/s.quadW;s.sweepAdvance=cell.advance/s.quadW;
            const float bake=std::min(1.f,(Atlas-12)/std::max(s.quadW,s.quadH));
            const float texW=std::ceil(s.quadW*bake),texH=std::ceil(s.quadH*bake);
            if(px+texW+4>Atlas){px=4;py+=rowHeight+4;rowHeight=0;}
            if(py+texH+4>Atlas)throw std::runtime_error("Rift bounded glyph atlas exhausted");
            s.u0=px/Atlas;s.v0=py/Atlas;s.u1=(px+texW)/Atlas;s.v1=(py+texH)/Atlas;
            atlasDraw->SetTransform(D2D1::Matrix3x2F::Scale(bake,bake)*D2D1::Matrix3x2F::Translation(px,py));
            if(!cell.blank)for(const auto& part:cell.parts){const auto& shaped=line.layout.runs[part.run];const auto slice=glyphs::Slice(shaped,part.first,part.last);
                const float x=shaped.origin.x+(shaped.Rtl()?-1.f:1.f)*shaped.pen[part.first]-originX,y=shaped.origin.y-originY;
                atlasDraw->DrawGlyphRun({pad-left+x,pad-top+y},&slice,white.Get(),shaped.measuring);}
            atlasDraw->SetTransform(D2D1::Matrix3x2F::Identity());
            px+=texW+4;rowHeight=std::max(rowHeight,texH);
            const int row=rowAt(i);float x=(pen[row]+cell.advance*.5f-widths[row]*.5f)*nominal*fit;
            float y=twoRows?(row==0?-desired*.64f:desired*.64f):0;
            pen[row]+=cell.advance+12;
            s.row=static_cast<size_t>(row);s.baseScale=nominal*fit;
            switch(line.family){
            case 0:y+=x*.25f;s.tilt=-13;break;
            case 1:{
                // Emphasise the long note in its original slot, not at the end.
                s.baseScale*=i==hero?1.35f:.78f;s.tilt=i==hero?-7.f:5.f;
                break;}
            case 2:
                if(vertical&&range.last-range.first<=12){
                    const size_t n=i-range.first,count=range.last-range.first,rows=count<=5?2:3,columns=(count+rows-1)/rows;
                    s.row=n/columns;x=(static_cast<float>(n%columns)-static_cast<float>(columns-1)*.5f)*desired*1.26f;
                    y=(static_cast<float>(s.row)-static_cast<float>(rows-1)*.5f)*desired*1.13f;
                    s.baseScale*=.92f;s.tilt=n%rows==1?8.f:-5.f;
                }else{y+=x*-.22f;s.tilt=10;}
                break;
            default:
                x=(pen[row]-(cell.advance+12)+cell.advance*.5f)*nominal*fit-f.width*.32f;
                y=(row==0?-1.f:1.f)*desired*.67f;s.baseScale*=row?1.18f:1.03f;s.tilt=row?-9.f:3.f;break;
            }
            if(f.kind!=LyricMotion::Rift){
                const size_t n=i-range.first,count=range.last-range.first;
                const float unit=std::max(1.f,static_cast<float>(count)),fraction=(static_cast<float>(n)+.5f)/unit;
                // The immutable glyph atlas is shared; each upstream family has
                // its own measured layout and per-frame transform below.
                s.row=static_cast<size_t>(row);s.baseScale=nominal*fit;s.tilt=0;
                const float measuredX=(pen[row]-(cell.advance+12)+cell.advance*.5f-widths[row]*.5f)*nominal*fit;
                x=measuredX;y=twoRows?(row==0?-desired*.65f:desired*.65f):0;
                switch(f.kind){
                case LyricMotion::Sonnet:
                    switch(f.variant>=0?f.variant%7:line.family){
                    case 0:s.row=n/3;x=-f.width*.27f+static_cast<float>(n%3)*desired*1.5f;y=(static_cast<float>(n/3)-static_cast<float>((count-1)/3)*.5f)*desired*1.2f;break;
                    case 1:s.baseScale*=n==count/2?1.7f:1.f;break;
                    case 2:x=measuredX;y+=(Seed(line.hash+static_cast<uint32_t>(n)*47)-.5f)*desired*.36f;s.tilt=(Seed(line.hash+static_cast<uint32_t>(n)*71)-.5f)*24;break;
                    case 3:y=x*-.18f;s.tilt=-10;break;
                    case 4:y+=desired*.3f;break;
                    case 5:x=measuredX-f.width*.12f;s.baseScale*=1.2f;s.tilt=row?0.f:-6.f;break;
                    default:s.baseScale*=.78f;break;
                    }break;
                case LyricMotion::Tempera:case LyricMotion::Lumiere:{
                    const auto kind=f.kind==LyricMotion::Tempera?FoliaProgramKind::Tempera:FoliaProgramKind::Lumiere;
                    const auto info=programs.Info(kind,Variant(f,line));
                    if(info.typography==1){const size_t rows=std::max<size_t>(1,std::min<size_t>(5,count));
                        const size_t columns=(count+rows-1)/rows;s.row=n/columns;
                        x=(static_cast<float>(n%columns)-static_cast<float>(columns-1)*.5f)*desired*1.25f;
                        y=(static_cast<float>(s.row)-static_cast<float>(rows-1)*.5f)*desired*1.18f;}
                    else if(info.typography==2){
                        if(n<count/2){s.row=0;x=(crossPen+cell.advance*.5f-crossWidth)*nominal-desired*.85f;crossPen+=cell.advance+12;y=0;}
                        else{s.row=n-count/2+1;x=desired*.65f;y=static_cast<float>(s.row)*desired*1.30f;}
                    }
                    const float regionFit=std::min(1.f,info.width*f.width/std::max(1.f,std::max(widths[0],widths[1])*nominal));
                    x=x*regionFit+(info.cx-.5f)*f.width;y=y*regionFit+(info.cy-.47f)*f.height;
                    s.baseScale=nominal*regionFit*info.fontScale;s.tilt=info.rotation*180/Pi;break;}
                case LyricMotion::Classic:y+=(Seed(line.hash+static_cast<uint32_t>(n)*17)-.5f)*desired*.13f;s.tilt=(Seed(line.hash+static_cast<uint32_t>(n)*19)-.5f)*10;break;
                case LyricMotion::Cadenza:
                    x=(fraction-.5f)*f.width*.65f;y=(Seed(line.hash+static_cast<uint32_t>(n)*113)-.5f)*f.height*.34f;
                    s.tilt=(Seed(line.hash+static_cast<uint32_t>(n)*79)-.5f)*16;s.baseScale*=.85f+.42f*Seed(line.hash+static_cast<uint32_t>(n)*71);break;
                case LyricMotion::Partita:{const size_t cols=count<=5?1:count<=10?2:count<=16?3:count<=24?4:5,rows=(count+cols-1)/cols;
                    s.row=n/cols;x=(static_cast<float>(n%cols)-static_cast<float>(cols-1)*.5f)*desired*1.7f;
                    y=(static_cast<float>(s.row)-static_cast<float>(rows-1)*.5f)*desired*1.35f;s.tilt=n%rows==1?7.f:-3.f;break;}
                case LyricMotion::Fume:x=measuredX;y=twoRows?(row==0?-desired*.7f:desired*.7f):0;break;
                case LyricMotion::Tilt:y=(static_cast<float>(row)-static_cast<float>(layoutRows-1)*.5f)*desired*1.45f;
                    if(row==static_cast<int>(line.hash%layoutRows)){y+=measuredX*.14f;s.tilt=-8;s.baseScale*=1.1f;}else s.baseScale*=.94f;break;
                case LyricMotion::Claddagh:{const float radius=std::min(f.width*.30f,f.height*.33f),span=total*nominal/std::max(1.f,radius),arcFit=std::min(1.f,2.4f/std::max(.001f,span));
                    s.row=0;const float angle=(arcPen+cell.advance*.5f-total*.5f)*nominal/std::max(1.f,radius)*arcFit;s.baseScale*=arcFit;
                    x=std::sin(angle)*f.width*.30f;y=std::cos(angle)*f.height*.23f;s.tilt=std::remainder(-angle*180/Pi,180.f);break;}
                case LyricMotion::Monet:x=measuredX+f.width*.24f;y=twoRows?(row==0?-desired*.65f:desired*.65f):0;s.baseScale*=.85f;break;
                case LyricMotion::Pendolo:x=measuredX+f.width*.18f;y=twoRows?(row==0?-desired*.65f:desired*.65f):0;break;
                case LyricMotion::Cappella:x=measuredX+(line.hash&1?1.f:-1.f)*f.width*.15f;y=twoRows?(row==0?-desired*.6f:desired*.6f):0;break;
                case LyricMotion::Diorama:x=measuredX;y=twoRows?(row==0?-desired*.65f:desired*.65f):0;break;
                case LyricMotion::Still:break;
                default:break;
                }
            }
            arcPen+=cell.advance+12;
            s.anchorX=f.width*.5f+x;s.anchorY=f.height*.47f+y;
            result.push_back(s);
        }
        const auto& source=f.lyrics->lines[line.index];
        const float auxiliaryReserve=(f.showTranslation&&!source.translation.empty()?.12f:0.f)+(f.showRomanization&&!source.romanization.empty()?.09f:0.f);
        FitLayout(f,result,auxiliaryReserve);
    }
    void FitLayout(const RiftFrame& f,std::vector<Sprite>& list,float auxiliaryReserve){
        if(list.empty())return;
        const auto half=[](const Sprite& s){const float a=s.tilt*Pi/180,c=std::abs(std::cos(a)),sn=std::abs(std::sin(a));
            return folia::Point{(s.quadW*c+s.quadH*sn)*s.baseScale*.5f,(s.quadW*sn+s.quadH*c)*s.baseScale*.5f};};
        // Preserve each family's measured layout and source-order slots.
        // Symmetric collision pushing could move a character past its neighbour.
        for(size_t row=0;row<list.size();++row){Sprite* preceding=nullptr;float before=0,after=0;size_t count=0;
            for(auto& item:list)if(item.row==row){before+=item.anchorX;++count;
                if(preceding)item.anchorX=std::max(item.anchorX,preceding->anchorX+(half(*preceding).x+half(item).x)*1.38f+2);
                preceding=&item;after+=item.anchorX;}
            if(count)for(auto& item:list)if(item.row==row)item.anchorX-=(after-before)/count;
        }
        float previousBottom=-INFINITY;
        for(size_t row=0;row<list.size();++row){float top=INFINITY,bottom=-INFINITY;
            for(const auto& item:list)if(item.row==row){const auto h=half(item);top=std::min(top,item.anchorY-h.y*1.32f);bottom=std::max(bottom,item.anchorY+h.y*1.32f);}
            if(!std::isfinite(top))continue;const float shift=std::max(0.f,previousBottom+2-top);
            for(auto& item:list)if(item.row==row)item.anchorY+=shift;
            previousBottom=bottom+shift;
        }
        float left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;
        for(const auto& item:list)if(!item.cell.blank){const auto h=half(item);left=std::min(left,item.anchorX-h.x);right=std::max(right,item.anchorX+h.x);top=std::min(top,item.anchorY-h.y);bottom=std::max(bottom,item.anchorY+h.y);}
        if(!std::isfinite(left))return;
        float cx=(left+right)*.5f,cy=(top+bottom)*.5f;
        float targetX=std::clamp(cx,f.width*.30f,f.width*.70f),targetY=std::clamp(cy,f.height*.35f,f.height*.57f);
        float safeWidth=f.width*.78f,safeHeight=f.height*.52f;
        if(f.kind==LyricMotion::Rift||f.kind==LyricMotion::Fume){targetX=f.width*.5f;targetY=f.height*.47f;safeWidth=f.width*.64f;safeHeight=f.height*.44f;}
        if(f.kind==LyricMotion::Claddagh){targetX=f.width*.5f;targetY=f.height*.47f;const float diameter=std::min(f.width*.66f,f.height*.54f);safeWidth=safeHeight=diameter;}
        safeHeight=std::min(safeHeight,f.height*(.60f-auxiliaryReserve));
        const float fit=std::min({1.f,safeWidth/std::max(1.f,right-left),safeHeight/std::max(1.f,bottom-top)});
        const float halfWidth=(right-left)*fit*.5f,halfHeight=(bottom-top)*fit*.5f;
        targetX=std::clamp(targetX,f.width*.07f+halfWidth,f.width*.93f-halfWidth);
        targetY=std::clamp(targetY,f.height*.17f+halfHeight,f.height*(.77f-auxiliaryReserve)-halfHeight);
        for(auto& item:list){item.anchorX=targetX+(item.anchorX-cx)*fit;item.anchorY=targetY+(item.anchorY-cy)*fit;item.baseScale*=fit;}
    }
    int LayoutOverlaps(const std::vector<Sprite>& list)const{
        int overlaps=0;const auto half=[](const Sprite& s){const float a=s.tilt*Pi/180,c=std::abs(std::cos(a)),sn=std::abs(std::sin(a));return folia::Point{(s.quadW*c+s.quadH*sn)*s.baseScale*.5f,(s.quadW*sn+s.quadH*c)*s.baseScale*.5f};};
        for(size_t i=0;i<list.size();++i)for(size_t j=i+1;j<list.size();++j){const auto& a=list[i];const auto& b=list[j];if(a.cell.blank||b.cell.blank)continue;const auto aa=half(a),bb=half(b);
            if(aa.x+bb.x-std::abs(a.anchorX-b.anchorX)>1&&aa.y+bb.y-std::abs(a.anchorY-b.anchorY)>1)++overlaps;}
        return overlaps;
    }
    size_t Variant(const RiftFrame& f,const LineCache& line)const{if(f.variant>=0)return static_cast<size_t>(f.variant);const auto start=f.lyrics->lines[line.index].startMs;return static_cast<size_t>(line.hash^Hash(f.identity)^static_cast<uint32_t>(start)*0x9e3779b9u);}
    folia::Point Origin(const RiftFrame& f,size_t line,size_t page)const{
        const auto base=line<worldOrigins.size()?worldOrigins[line]:folia::Point{};
        return {base.x+static_cast<float>(page)*f.width*.72f,base.y+static_cast<float>(page)*f.height*.78f};
    }
    void World(const RiftFrame& f){
        worldOrigins.assign(f.lyrics->lines.size(),{});families.resize(worldOrigins.size());const int count=f.kind==LyricMotion::Sonnet?7:4;
        for(size_t i=0;i<families.size();++i){const auto& line=f.lyrics->lines[i];const auto hash=Hash(line.text)+static_cast<uint32_t>(line.words.size()*13)+static_cast<uint32_t>(line.durationMs/173);
            int choice=static_cast<int>(hash%count);if(i&&choice==families[i-1])choice=(choice+1+static_cast<int>((hash>>8)%(count-1)))%count;families[i]=choice;}
        for(size_t i=1;i<worldOrigins.size();++i){const auto hash=Hash(f.lyrics->lines[i].text);
            const float dx=(hash&1?1.f:-1.f)*f.width*(.86f+.20f*Seed(hash));
            const float dy=f.height*((hash%5==0?-.72f:.72f)+.16f*Seed(hash+41));
            worldOrigins[i]={worldOrigins[i-1].x+dx,worldOrigins[i-1].y+dy};}
    }
    void MoveCamera(const RiftFrame& f,size_t page,bool reset){
        const auto origin=Origin(f,current.index,page);
        folia::Point focus=origin,entry=origin;
        const auto point=[&](const Sprite& s){return folia::Point{origin.x+(s.anchorX-f.width*.5f)*.68f,origin.y+(s.anchorY-f.height*.47f)*.68f};};
        if(!sprites.empty()){
            const auto first=std::find_if(sprites.begin(),sprites.end(),[](const Sprite& s){return !s.cell.blank;});
            if(first!=sprites.end()){entry=point(*first);focus=entry;}
            if(current.timed){
                const Sprite* active=nullptr;const Sprite* next=nullptr;
                for(const auto& s:sprites)if(!s.cell.blank){if(s.cell.start<=f.positionMs)active=&s;else if(!next)next=&s;}
                if(active){focus=point(*active);if(next){const float t=Smooth(static_cast<float>(f.positionMs-active->cell.start)/static_cast<float>(std::max<int64_t>(1,active->cell.duration)));
                    const auto destination=point(*next);focus.x+=(destination.x-focus.x)*t;focus.y+=(destination.y-focus.y)*t;}}
            }else{const auto& range=current.pages[page];const float t=Smooth(static_cast<float>(f.positionMs-range.start)/std::max(1.f,static_cast<float>(range.end-range.start)));
                const auto last=std::find_if(sprites.rbegin(),sprites.rend(),[](const Sprite& s){return !s.cell.blank;});
                if(last!=sprites.rend()){const auto destination=point(*last);focus.x+=(destination.x-focus.x)*t;focus.y+=(destination.y-focus.y)*t;}}
        }
        const bool flowing=f.following&&!f.reduced&&f.camera>.01f;
        folia::CameraInput input;input.target=flowing?focus:origin;input.entry=entry;
        input.scale=flowing?(current.family==1?.88f:1.06f):1.f;
        input.width=f.width;input.height=f.height;input.time=static_cast<double>(f.positionMs)/1000.;
        input.dt=f.deltaSeconds;
        input.block=(static_cast<uint64_t>(current.index)<<32)|static_cast<uint64_t>(page);
        input.duration=.34f;input.reset=reset;input.staticMode=!flowing;input.playing=f.playing;
        camera.Step(input);
        const auto& pose=camera.View();reading.cameraX=pose.x;reading.cameraY=pose.y;reading.cameraScale=pose.scale;reading.cameraFlying=camera.Flying();
    }
    void Prepare(const RiftFrame& f){
        const bool fontChanged=optionalFont.Select(write.Get(),f.lyricFont,f.lyricFontPath);
        reading={};reading.step=std::max(1.f,f.height*.60f);reading.sampledClock=f.sampled;reading.advancing=f.playing;reading.renderPositionMs=f.positionMs;reading.audioLevel=f.rms;
        if(!f.lyrics||f.lyrics->lines.empty()){sprites.clear();oldSprites.clear();waitingSprites.clear();worldOrigins.clear();families.clear();contextLines.clear();contextCount=0;current={};previous={};upcoming={};identity.clear();lastLine=lastPage=SIZE_MAX;transitionAt=-10000;captureTransition=false;resetFrame=true;return;}
        const auto index=f.following?LyricLineAt(f.lyrics->lines,f.positionMs):std::min(f.previewIndex,f.lyrics->lines.size()-1);
        const bool identityChanged=identity!=f.identity||revision!=f.revision;
        const bool seek=f.discontinuity;
        const bool translated=f.showTranslation&&!f.lyrics->lines[index].translation.empty(),romanized=f.showRomanization&&!f.lyrics->lines[index].romanization.empty();
        const bool sizeChanged=lastLayoutWidth!=f.width||lastLayoutHeight!=f.height||lastLyricSize!=f.lyricSize||lastTranslation!=translated||lastRomanization!=romanized;
        resetFrame=identityChanged||seek;
        const bool kindChanged=fontChanged||f.kind!=lastKind||f.variant!=lastVariant||(f.kind==LyricMotion::Mineradio&&f.particlePreset!=lastParticlePreset);
        resetFrame=resetFrame||kindChanged;
        if(kindChanged){
            if(f.kind!=LyricMotion::Tempera&&f.kind!=LyricMotion::Lumiere&&f.kind!=LyricMotion::Sonnet)programs.Clear();
            if(f.kind!=LyricMotion::Diorama)diorama.Clear();
            if(f.kind!=LyricMotion::Mineradio)mineradio.reset();
            if(f.kind==LyricMotion::Rift||f.kind==LyricMotion::Mineradio)backdrop.reset();
            if(f.kind!=LyricMotion::Monet){portrait.Reset();portraitSource.reset();portraitSequence=UINT64_MAX;}
            if(f.kind!=LyricMotion::Lumiere){lightVS.Reset();lightPS.Reset();lightConstants.Reset();}
        }

        captureTransition=hasOutput&&!identityChanged&&!seek&&!sizeChanged&&!f.reduced&&f.following&&f.playing&&
            (kindChanged||(f.kind==LyricMotion::Sonnet&&current.index!=index));
        if(identityChanged||seek||sizeChanged||!f.following||f.reduced)transitionAt=-10000;
        if(identityChanged||seek||kindChanged){current={};previous={};upcoming={};lastLine=lastPage=SIZE_MAX;lastFamily=-1;oldSprites.clear();waitingSprites.clear();heldPulse=0;cutAt=0;departureGlyphs.clear();drawnGlyphs.clear();assemblyAt=-10000;}
        if(identityChanged||sizeChanged||kindChanged||worldOrigins.size()!=f.lyrics->lines.size())World(f);
        identity=f.identity;revision=f.revision;
        const bool newLine=current.index!=index;
        const bool timingChanged=current.requestedPrecise!=f.precise;
        if(newLine){
            if(!resetFrame&&f.following&&lastLine!=SIZE_MAX&&index==lastLine+1){previous=std::move(current);previousPage=lastPage;cutAt=f.positionMs;departureGlyphs=drawnGlyphs;assemblyAt=f.positionMs;}
            else {previous={};oldSprites.clear();cutAt=0;departureGlyphs.clear();assemblyAt=-10000;}
            current=Shape(f,index);lastFamily=current.family;
            upcoming=index+1<f.lyrics->lines.size()?Shape(f,index+1):LineCache{};
        }else if(sizeChanged||timingChanged){current=Shape(f,index);previous={};oldSprites.clear();upcoming=index+1<f.lyrics->lines.size()?Shape(f,index+1):LineCache{};}
        const auto page=PageAt(f,current);
        if(!newLine&&page!=lastPage&&lastPage!=SIZE_MAX&&!resetFrame&&!sizeChanged&&!timingChanged&&f.following){
            previous=current;previousPage=lastPage;cutAt=assemblyAt=f.positionMs;departureGlyphs=drawnGlyphs;
        }
        if(sizeChanged||timingChanged||!f.following||f.reduced){departureGlyphs.clear();assemblyAt=-10000;}
        const bool rebuild=newLine||page!=lastPage||sizeChanged||timingChanged||resetFrame||sprites.empty();
        if(f.kind==LyricMotion::Pendolo){
            const float dt=f.deltaSeconds;
            if(resetFrame){clockPhase=static_cast<float>(f.positionMs)*.0028f;clockBass=f.low;secondAngle=std::floor(static_cast<float>(f.positionMs)*.001f)*2*Pi/15;secondStep=std::floor(static_cast<float>(f.positionMs)*.001f);secondVelocity=secondElapsed=0;}
            if(f.playing&&!f.reduced){const int count=std::max(1,static_cast<int>(std::ceil(dt*120)));const float step=dt/count;
                for(int i=0;i<count;++i){clockBass+=(f.low-clockBass)*(1-std::exp(-7.7f*step));clockPhase+=step*(2.8f+clockBass*3.5f);secondElapsed+=step;
                    if(secondElapsed>=1){secondStep+=std::floor(secondElapsed);secondElapsed-=std::floor(secondElapsed);}secondVelocity+=(secondStep*2*Pi/15-secondAngle)*92*step;secondVelocity*=std::exp(-13*step);secondAngle+=secondVelocity*step;}}
        }
        const int wantedContext=f.focused?1:f.kind==LyricMotion::Mineradio?std::clamp(f.motionLines,1,3):f.kind==LyricMotion::Pendolo?3:f.kind==LyricMotion::Monet?3:f.kind==LyricMotion::Cappella?3:0;
        if(rebuild||wantedContext!=contextCount){contextLines.clear();contextCount=wantedContext;
            if(wantedContext){const int slot=wantedContext==2?0:wantedContext/2;for(int offset=-slot;offset<wantedContext-slot;++offset){const auto line=static_cast<int64_t>(index)+offset;if(offset==0||line<0||line>=static_cast<int64_t>(f.lyrics->lines.size()))continue;
                    ContextLine contextLine;contextLine.index=static_cast<size_t>(line);ComPtr<IDWriteTextFormat> format;
                    LyricFormat(f,std::clamp(Desired(f)*.28f,14.f,30.f),format);
                    Check(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));const auto& text=f.lyrics->lines[contextLine.index].text;
                    Check(write->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format.Get(),f.width*.64f,f.height*.08f,&contextLine.layout));
                    DWRITE_TEXT_METRICS metrics{};Check(contextLine.layout->GetMetrics(&metrics));contextLine.width=std::max(1.f,metrics.widthIncludingTrailingWhitespace);contextLine.height=metrics.height;
                    Check(contextLine.layout->SetMaxWidth(contextLine.width));contextLines.push_back(std::move(contextLine));}}
        }
        if(rebuild){
            ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(0,1,&none);
            atlasDraw->SetTarget(atlasBitmap.Get());atlasDraw->BeginDraw();white->SetColor(D2D1::ColorF(1,1,1,1));atlasDraw->Clear({0,0,0,0});
            float px=4,py=4,row=0;
            RasterPage(f,current,page,sprites,px,py,row);lastLayoutOverlaps=LayoutOverlaps(sprites);
            if(previous.index!=SIZE_MAX&&!previous.pages.empty())RasterPage(f,previous,std::min(previousPage,previous.pages.size()-1),oldSprites,px,py,row);
            else oldSprites.clear();
            if(upcoming.index!=SIZE_MAX&&!upcoming.pages.empty())RasterPage(f,upcoming,0,waitingSprites,px,py,row);
            else waitingSprites.clear();
            Check(atlasDraw->EndDraw());atlasDraw->SetTarget(nullptr);
        }
        if(!current.pages.empty()){
            if(f.kind==LyricMotion::Rift||f.kind==LyricMotion::Fume)MoveCamera(f,page,resetFrame||kindChanged||sizeChanged||timingChanged||lastFollowing!=f.following);
            else{folia::CameraInput fixed;fixed.target=Origin(f,current.index,page);fixed.entry=fixed.target;fixed.staticMode=true;camera.Step(fixed);}
        }
        if(f.kind==LyricMotion::Diorama){float sung=0;for(const auto& cell:current.cells){if(cell.start<=f.positionMs)sung+=Unit(static_cast<float>(f.positionMs-cell.start)/float(std::max<int64_t>(1,cell.duration)));}
            diorama.Step(f,index,current.cells.empty()?.5f:sung/static_cast<float>(current.cells.size()),resetFrame||sizeChanged);}
        lastKind=f.kind;lastVariant=f.variant;lastParticlePreset=f.particlePreset;
        lastFollowing=f.following;
        lastLine=index;lastPage=page;lastPosition=f.positionMs;lastNow=f.nowMs;
        lastLayoutWidth=f.width;lastLayoutHeight=f.height;lastLyricSize=f.lyricSize;lastTranslation=translated;lastRomanization=romanized;
        if(!f.following)heldPulse=0;
        else if(f.playing)heldPulse=Unit(f.pulse)*std::clamp(f.response,0.f,2.f);
        reading.line=index;reading.page=page;reading.pages=std::max<size_t>(1,current.pages.size());reading.wordTimed=current.timed;
        reading.offset=static_cast<float>(page)*reading.step;reading.maxOffset=static_cast<float>(reading.pages-1)*reading.step;
        reading.layoutOverlaps=lastLayoutOverlaps;
        reading.composition=current.family;reading.reset=resetFrame;
        reading.awaitingWords=f.following&&current.timed&&!sprites.empty()&&std::none_of(sprites.begin(),sprites.end(),[&](const Sprite& item){return !item.cell.blank&&item.cell.start<f.positionMs;});
        reading.valid=current.layout.valid;reading.embeddedFont=TypefaceKind(f)==LyricTypefaceKind::Display&&fonts;
        reading.fontFamily=reading.embeddedFont?family:typefaces[static_cast<int>(TypefaceKind(f))].family;
        reading.missingGlyphs=current.missingGlyphs;
        if(optionalFont.Active()){reading.fontFamily=optionalFont.Family();reading.embeddedFont=false;}
    }
    void Quad(float cx,float cy,float w,float h,float angle,float yaw,float depth,D2D1_COLOR_F color,
        float u0=0,float v0=0,float u1=1,float v1=1,float kind=2,float edge=2,float reveal=1){
        if(color.a<=.001f||vertices.size()>36000)return;
        const float a=angle*Pi/180,yy=yaw*Pi/180,c=std::cos(a),sn=std::sin(a);
        std::array<Vertex,4> v{};
        for(int i=0;i<4;++i){const float x=(i==0||i==3?-1.f:1.f)*w*.5f,y=(i<2?-1.f:1.f)*h*.5f;
            const float z=depth+x*std::sin(yy);const float perspective=1100.f/std::max(450.f,1100.f-z);
            const float xx=x*std::cos(yy),rx=(xx*c-y*sn)*perspective,ry=(xx*sn+y*c)*perspective;
            v[i]={cx+rx,cy+ry,i==0||i==3?u0:u1,i<2?v0:v1,color.r,color.g,color.b,Unit(color.a),kind,edge,u0+(u1-u0)*Ease(reveal),(u1-u0)*.035f};}
        for(int k:{0,1,2,0,2,3})vertices.push_back(v[k]);
    }
    void Karaoke(size_t first,const Sprite& s,float fill){
        for(size_t i=first;i<std::min(first+6,vertices.size());++i){vertices[i].fill=fill;vertices[i].sweepStart=s.sweepStart;vertices[i].sweepAdvance=s.sweepAdvance;}
    }
    bool MeasureGlyph(const RiftFrame& f,size_t first,const Sprite& sprite,float fill){
        ++reading.submittedClusters;if(vertices.size()<first+6)return false;
        float left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;
        for(size_t i=first;i<first+6;++i){const auto& v=vertices[i];left=std::min(left,v.x);right=std::max(right,v.x);top=std::min(top,v.y);bottom=std::max(bottom,v.y);}
        if(reading.glyphCount<reading.glyphs.size())reading.glyphs[reading.glyphCount++]={sprite.cell.text,sprite.cell.length,sprite.row,left,top,right,bottom,fill,sprite.anchorX,sprite.anchorY};
        if(right<=0||left>=f.width||bottom<=0||top>=f.height)return false;
        ++reading.visibleClusters;
        if(left>=0&&right<=f.width&&top>=f.height*.13f&&bottom<=f.height*.81f)++reading.fullyVisibleClusters;
        reading.glyphLeft=std::min(reading.glyphLeft,left);reading.glyphRight=std::max(reading.glyphRight,right);reading.glyphTop=std::min(reading.glyphTop,top);reading.glyphBottom=std::max(reading.glyphBottom,bottom);
        return left>=0&&right<=f.width&&top>=f.height*.13f&&bottom<=f.height*.81f;
    }
    void Ornament(const RiftFrame& f,size_t first,uint32_t seed,float fill,float age,float sungProgress,float singingFill,bool primary){
        if(!primary||vertices.size()<first+6)return;
        const std::array<Vertex,4> q={vertices[first],vertices[first+1],vertices[first+2],vertices[first+5]};
        const bool pop=UsesSyllableEntrance(f.kind,f.particlePreset);
        const float activity=pop?std::max(std::exp(-std::max(0.f,age)*2.6f),GlyphSingingLight(sungProgress)):GlyphSingingLight(fill);
        if(pop)for(size_t i=first;i<first+6;++i)vertices[i].edge=-(.20f+.80f*activity);
        const auto ornament=MakeGlyphOrnament(seed,float(f.positionMs)*.001f,activity,true,f.quality);
        if(!glyphEffectFrame.reduced){GlyphEmitter e;
            for(size_t i=0;i<4;++i)e.corners[i]={q[i].x,q[i].y};
            e.color=(!pop&&fill<=0)?f.ink:f.accent;e.color.a=q[0].a*(!pop&&fill<=0?.25f:1.f);e.age=age;e.progress=singingFill;e.active=activity;e.seed=float(seed%65521);
            glyphEffectFrame.Add(e);}
        const auto point=[&](float x,float y){return D2D1::Point2F((q[0].x*(1-x)+q[1].x*x)*(1-y)+(q[3].x*(1-x)+q[2].x*x)*y,(q[0].y*(1-x)+q[1].y*x)*(1-y)+(q[3].y*(1-x)+q[2].y*x)*y);};
        for(size_t m=0;m<ornament.count&&vertices.size()+6<36000;++m){const auto& mark=ornament.marks[m];Vertex v[4]{};
            for(int i=0;i<4;++i){const auto p=point(mark.x+(i==0||i==3?-.5f:.5f)*mark.w,mark.y+(i<2?-.5f:.5f)*mark.h);
                v[i]={p.x,p.y,0,0,f.accent.r,f.accent.g,f.accent.b,q[0].a*mark.alpha,mark.particle?11.f:10.f,0,0,0};}
            for(int i:{0,1,2,0,2,3})vertices.push_back(v[i]);}
    }
    void IncludeAuxiliaryQuad(size_t first,const Sprite& sprite){
        if(vertices.size()<first+6)return;
        const auto& a=vertices[first];const auto& b=vertices[first+1];
        if(!auxiliaryPose.valid){const float dx=b.x-a.x,dy=b.y-a.y,length=std::hypot(dx,dy);if(length<.01f)return;
            auxiliaryPose.valid=true;auxiliaryPose.c=dx/length;auxiliaryPose.s=dy/length;
            auxiliaryPose.scale=length/std::max(.01f,sprite.quadW*sprite.baseScale);}
        for(size_t i=first;i<first+6;++i){const auto& p=vertices[i];
            const float x=p.x*auxiliaryPose.c+p.y*auxiliaryPose.s,y=-p.x*auxiliaryPose.s+p.y*auxiliaryPose.c;
            auxiliaryPose.left=std::min(auxiliaryPose.left,x);auxiliaryPose.right=std::max(auxiliaryPose.right,x);auxiliaryPose.bottom=std::max(auxiliaryPose.bottom,y);}
        for(size_t i=first;i<first+6;++i)auxiliaryPose.top=std::min(auxiliaryPose.top,-vertices[i].x*auxiliaryPose.s+vertices[i].y*auxiliaryPose.c);
    }
    void DrawSprites(const RiftFrame& f,const std::vector<Sprite>& list,const LineCache& line,int role,bool anchorOnly=false){
        if(line.pages.empty())return;
        const bool old=role<0,waiting=role>0;
        const bool syllable=UsesSyllableEntrance(f.kind,f.particlePreset);
        const size_t pageIndex=waiting?0:old?std::min(previousPage,line.pages.size()-1):std::min(lastPage,line.pages.size()-1);
        const auto& page=line.pages[pageIndex];
        auto world=Origin(f,line.index,pageIndex);const auto& view=camera.View();
        if(f.kind==LyricMotion::Rift||f.kind==LyricMotion::Fume){
            // Retain the world camera/background path. Only excessive screen
            // travel of the lyric plane is compressed, not its small drift.
            world.x=view.x+BalancedLyricTravel((world.x-view.x)*view.scale,std::min(f.width*.04f,Desired(f)*.55f))/view.scale;
            world.y=view.y+BalancedLyricTravel((world.y-view.y)*view.scale,std::min(f.height*.035f,Desired(f)*.35f))/view.scale;
        }
        if(f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Fume){world=Origin(f,current.index,lastPage);
            const float scroll=(f.kind==LyricMotion::Monet||f.kind==LyricMotion::Cappella)?(1-Ease(static_cast<float>(f.positionMs-cutAt)/380.f)):0.f;
            world.y+=(static_cast<float>(role)+scroll)*f.height*.19f;}
        const float pulse=f.reduced?0.f:heldPulse;
        for(const auto& s:list){if(s.cell.blank)continue;
            const auto seed=line.hash+static_cast<uint32_t>(s.ordinal*173);const float rnd=Seed(seed);
            int64_t start=line.timed?s.cell.start:page.start;
            if(f.phraseEntry&&line.timed&&!syllable){const auto origin=std::max(page.start,line.cells[page.first].start);
                start=LyricEntryStart(origin,page.end-origin,page.last-page.first,s.ordinal,start);}
            if(!anchorOnly&&!waiting&&f.kind!=LyricMotion::Still&&f.following&&line.timed&&f.positionMs<start&&!f.reduced)continue;
            const float age=!anchorOnly&&f.following&&!waiting&&f.kind!=LyricMotion::Still?static_cast<float>(f.positionMs-start)/1000.f:2.f;
            const float duration=static_cast<float>(std::max<int64_t>(1,s.cell.duration))/1000.f;
            const float enter=f.reduced||!f.following||waiting||f.kind==LyricMotion::Still?1.f:Unit(age/(.14f+.12f*rnd));
            const float arrival=Spring(enter),impact=std::exp(-std::max(0.f,age)*7.f);
            const float sungAge=static_cast<float>(f.positionMs-s.cell.start)/1000.f;
            const float note=!anchorOnly&&line.timed?std::sin(Pi*Unit(sungAge/std::max(.01f,duration))):0.f;
            const float longNote=Unit((duration-.55f)/.75f)*note;
            const float leave=0;
            const float oldFade=1-Smooth(float(f.positionMs-cutAt)/float(LyricHandoffDuration(current.pages.empty()?960:current.pages[std::min(lastPage,current.pages.size()-1)].end-current.pages[std::min(lastPage,current.pages.size()-1)].start)));
            float alpha=(f.reduced?1.f:Smooth(enter*2.5f))*(old?.24f*oldFade:waiting?.055f:1.f);
            if(!anchorOnly&&role==0&&Reassembles(f)&&!departureGlyphs.empty()&&!f.reduced&&f.following){
                const float phase=Unit(float(f.positionMs-assemblyAt)/float(LyricHandoffDuration(page.end-page.start)));
                alpha*=.25f+.75f*Smooth(phase);
            }
            if(alpha<.001f)continue;
            const float motion=f.reduced||!f.following||f.kind==LyricMotion::Still?0.f:std::clamp(f.camera,0.f,2.f);
            const float inertia=(1-std::exp(-std::max(0.f,age)*1.2f));
            const float drift=motion*(std::sin(rnd*Pi*2+age*.6f)-std::sin(rnd*Pi*2))*9.f*inertia;
            float x=s.anchorX+(1-arrival)*(rnd<.5f?-1.f:1.f)*f.width*.055f*motion+drift;
            float y=s.anchorY+(1-arrival)*(s.ordinal%2?-1.f:1.f)*f.height*.065f*motion;
            x=f.width*.5f+(world.x+x-f.width*.5f-view.x)*view.scale;
            y=f.height*.47f+(world.y+y-f.height*.47f-view.y)*view.scale;
            float size=s.baseScale*(f.reduced?1.f:(.36f+.64f*arrival)*(1+.18f*note+.24f*longNote));
            const float stretch=1+(f.kind==LyricMotion::Rift?impact*.85f*motion:0);
            float angle=s.tilt*motion+(1-arrival)*(rnd-.5f)*32.f*motion;
            float yaw=(1-arrival)*(rnd-.5f)*100.f*motion;
            float depth=(1-arrival)*-420.f*motion+longNote*140.f*motion+leave*240.f;
            if(f.kind!=LyricMotion::Rift){
                const float settle=Ease(age/.42f),t=static_cast<float>(f.positionMs)*.001f;
                x=f.width*.5f+(world.x+s.anchorX-f.width*.5f-view.x)*view.scale;
                y=f.height*.47f+(world.y+s.anchorY-f.height*.47f-view.y)*view.scale;
                size=s.baseScale*(1+.045f*note*motion);angle=s.tilt*motion;yaw=0;depth=0;
                switch(f.kind){
                case LyricMotion::Classic:y-=(1-settle)*f.height*.04f*motion;size*=1+.035f*note*motion;break;
                case LyricMotion::Cadenza:y+=(1-settle)*f.height*.12f*motion;angle+=(1-settle)*(rnd-.5f)*36*motion;break;
                case LyricMotion::Partita:y+=(1-arrival)*f.height*.11f*motion;angle+=(1-settle)*(s.ordinal%2?12.f:-12.f)*motion;break;
                case LyricMotion::Fume:{
                    // A real timed syllable gets one kick and a damped rebound.
                    // Real timestamps or the explicitly estimated presentation view drive this one-shot entry.
                    const float kick=std::sin(std::min(std::max(age,0.f)*12.f,Pi))*std::exp(-std::max(age,0.f)*3.3f);
                    const float rebound=std::sin(std::max(age,0.f)*21.f)*std::exp(-std::max(age,0.f)*8.f);
                    y-=Desired(f)*(.26f*kick+.045f*rebound)*motion;
                    size=s.baseScale*(.74f+.26f*arrival)*(1+(.15f*kick+.10f*longNote)*motion);
                    angle=(rnd<.5f?-1.f:1.f)*7.f*kick*motion;yaw=0;depth=0;break;}
                case LyricMotion::Tilt:x+=(1-settle)*(rnd<.5f?-1.f:1.f)*f.width*.12f*motion;y+=note*8*motion;size*=1+.06f*note*motion;break;
                case LyricMotion::Claddagh:{
                    const float turn=f.kind==LyricMotion::Claddagh?t*.035f:std::sin(t*.6f)*.20f;
                    const float a=turn*motion,c=std::cos(a),sn=std::sin(a),px=x-f.width*.5f,py=y-f.height*.47f;
                    x=f.width*.5f+px*c-py*sn;y=f.height*.47f+px*sn+py*c;angle=std::remainder(angle+a*180/Pi,180.f);break;}
                case LyricMotion::Pendolo:{const float a=f.reduced||!f.following?0.f:(1-Ease(static_cast<float>(f.positionMs-cutAt)/550.f))*.23f;
                    const float cx=f.width*.17f,cy=f.height*.47f,px=s.anchorX-cx,py=s.anchorY-cy;x=cx+px*std::cos(a)-py*std::sin(a);y=cy+px*std::sin(a)+py*std::cos(a);angle=a*180/Pi;break;}
                case LyricMotion::Monet:angle=0;size=s.baseScale;y+=(1-settle)*12*motion;break;
                case LyricMotion::Cappella:angle=0;size=s.baseScale*(1+(1-settle)*.10f*motion);break;
                case LyricMotion::Diorama:yaw=(-32.f+(1-settle)*58.f)*motion;depth=(rnd-.5f)*140*motion;size*=1+.08f*note*motion;break;
                case LyricMotion::Still:angle=0;size=s.baseScale;yaw=0;depth=0;break;
                case LyricMotion::Tempera:{const auto info=programs.Info(FoliaProgramKind::Tempera,Variant(f,line));
                    x+=(1-settle)*info.enterX*Desired(f)*motion;y+=(1-settle)*info.enterY*Desired(f)*motion;angle=info.rotation*180/Pi;break;}
                case LyricMotion::Lumiere:y-=longNote*Desired(f)*.10f*motion;angle=s.tilt*motion;break;
                case LyricMotion::Sonnet:if((f.variant>=0?f.variant%7:line.family)==1)size*=1+(1-settle)*.35f*motion;else if((f.variant>=0?f.variant%7:line.family)==3)x-=(1-settle)*f.width*.18f*motion;break;
                default:break;
                }
            }
            if(motion>0&&!old&&!waiting&&f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Fume&&f.kind!=LyricMotion::Still&&f.kind!=LyricMotion::Diorama&&f.kind!=LyricMotion::Mineradio){
                const float phraseAge=std::max(0.f,float(f.positionMs-page.start)*.001f);
                const float rush=1-Ease(phraseAge/.46f);
                const float turn=(line.hash&1)?1.f:-1.f;
                if(f.kind==LyricMotion::Sonnet){x+=turn*f.width*.14f*rush*motion;angle+=turn*9.f*rush*motion;size*=1+.30f*rush*motion;}
                else if(f.kind==LyricMotion::Tempera){x-=f.width*.10f*rush*motion;y+=f.height*.07f*rush*motion;}
                else if(f.kind==LyricMotion::Lumiere){y+=f.height*.085f*rush*motion;size*=1-.18f*rush*motion;}
                else if(f.kind==LyricMotion::Cadenza||f.kind==LyricMotion::Tilt){x+=turn*f.width*.08f*rush*motion;angle+=turn*7.f*rush*motion;}
                else if(f.kind==LyricMotion::Classic||f.kind==LyricMotion::Partita){y+=f.height*.055f*rush*motion;}
            }
            if((f.kind==LyricMotion::Tempera||f.kind==LyricMotion::Lumiere)&&!f.reduced&&f.following){
                const auto info=programs.Info(f.kind==LyricMotion::Tempera?FoliaProgramKind::Tempera:FoliaProgramKind::Lumiere,Variant(f,line));
                const float progress=Smooth(static_cast<float>(f.positionMs-page.start)/std::max(1.f,static_cast<float>(page.end-page.start)));
                const float zoom=std::clamp(info.zoomStart+(info.zoomEnd-info.zoomStart)*progress,.8f,1.16f);
                const float dx=f.kind==LyricMotion::Tempera?-std::cos(.4f)*info.travel*progress:info.travelX*progress;
                const float dy=f.kind==LyricMotion::Tempera?-std::sin(.4f)*info.travel*progress:info.travelY*progress;
                x=(x-f.width*.5f)*zoom+f.width*(.5f+std::clamp(dx,-.045f,.045f));y=(y-f.height*.5f)*zoom+f.height*(.5f+std::clamp(dy,-.035f,.035f));size*=zoom;
            }
            if(f.kind==LyricMotion::Mineradio){
                const float t=f.reduced?0:static_cast<float>(f.positionMs)*.001f,settle=Ease(static_cast<float>(f.positionMs-page.start)/1000.f/(f.motionFx==1?.72f:f.motionFx==2?.86f:f.motionFx==3?.36f:.62f));
                x=s.anchorX;y=s.anchorY+role*f.height*.13f+(1-settle)*f.height*.09f*motion;
                const float amp=f.motionFx==2?1.45f:f.motionFx==1?.55f:.82f;
                y+=(std::sin(t*.55f+line.hash)*.046f+std::sin(t*1.35f+line.hash)*.012f)*f.height*.22f*amp*motion;
                angle=std::sin(t*.34f+line.hash)*(f.motionFx==1?.006f:f.motionFx==2?.026f:.018f)*180/Pi*motion;
                yaw=std::sin(t*.48f+line.hash)*3*motion;size=s.baseScale;
            }
            if(f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Fume&&f.kind!=LyricMotion::Still&&line.timed&&!waiting){
                const float elapsed=std::max(age,0.f),kick=std::sin(std::min(elapsed*13.f,Pi))*std::exp(-elapsed*4.f);
                size*=1+.12f*kick*motion;y-=Desired(f)*.16f*kick*motion;angle+=(rnd<.5f?-1.f:1.f)*4.f*kick*motion;
            }
            if(f.kind==LyricMotion::Mineradio&&motion>0){
                y+=(1-arrival)*Desired(f)*.24f;size*=.84f+.16f*arrival;
            }
            if(syllable){
                // Keep the original whole-scene camera, but replace large
                // per-letter flights with a small, one-shot syllable entry.
                const float entry=f.reduced||!f.following?1.f:Ease(age/.17f);
                x=f.width*.5f+(world.x+s.anchorX-f.width*.5f-view.x)*view.scale;
                y=f.height*.47f+(world.y+s.anchorY-f.height*.47f-view.y)*view.scale+(1-entry)*std::min(8.f,Desired(f)*.055f);
                size=s.baseScale*(.96f+.04f*entry);angle=s.tilt*motion;yaw=0;depth=0;
            }
            // Keep the choreography and directions; compress oversized entry
            // offsets only. Quiet/rotating/clock/stacked layouts keep their poses.
            if(!syllable&&f.kind!=LyricMotion::Claddagh&&f.kind!=LyricMotion::Pendolo&&f.kind!=LyricMotion::Monet&&f.kind!=LyricMotion::Cappella&&f.kind!=LyricMotion::Still){
                const float restX=f.width*.5f+(world.x+s.anchorX-f.width*.5f-view.x)*view.scale;
                const float restY=f.height*.47f+(world.y+s.anchorY-f.height*.47f-view.y)*view.scale;
                x=restX+BalancedLyricTravel(x-restX,Desired(f)*.12f);
                y=restY+BalancedLyricTravel(y-restY,Desired(f)*.10f);
                size=s.baseScale*(1+BalancedLyricTravel(size/std::max(.001f,s.baseScale)-1,.055f));
                angle=s.tilt*motion+BalancedLyricTravel(angle-s.tilt*motion,3.f);
            }
            // Audio still drives the scenery. It must not offset, rotate or
            // resize already visible lyric ink. Timed entrance gestures above
            // are the original 0.7.19 choreography.
            const float w=s.quadW*size*(syllable?1.f:stretch)*view.scale,h=s.quadH*size/(1+(f.kind==LyricMotion::Rift&&!syllable?impact*.24f*motion:0))*view.scale;
            if(!anchorOnly&&role==0&&line.timed&&f.following&&!f.reduced&&f.kind!=LyricMotion::Diorama){
                // Keep the syllable on its actual singing span readable while
                // the world camera is still travelling to the incoming phrase.
                // Clamping is continuous; release it gradually after the note.
                const float hold=1-Ease((sungAge-duration)/.18f);
                if(sungAge>=0&&hold>0){const float a=angle*Pi/180,yy=yaw*Pi/180;
                    float left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;
                    for(int i=0;i<4;++i){const float px=(i==0||i==3?-1.f:1.f)*w*.5f,py=(i<2?-1.f:1.f)*h*.5f;
                        const float perspective=1100.f/std::max(450.f,1100.f-depth-px*std::sin(yy));
                        const float rx=(px*std::cos(yy)*std::cos(a)-py*std::sin(a))*perspective,ry=(px*std::cos(yy)*std::sin(a)+py*std::cos(a))*perspective;
                        left=std::min(left,rx);right=std::max(right,rx);top=std::min(top,ry);bottom=std::max(bottom,ry);
                    }
                    const float minX=f.width*.04f-left,maxX=f.width*.96f-right,minY=f.height*.15f-top,maxY=f.height*.78f-bottom;
                    if(minX<=maxX)x+=(std::clamp(x,minX,maxX)-x)*hold;
                    if(minY<=maxY)y+=(std::clamp(y,minY,maxY)-y)*hold;
                }
            }
            if(!anchorOnly&&f.kind==LyricMotion::Rift&&!syllable&&role==0&&f.quality<3&&!f.reduced&&(longNote>.05f||pulse>.12f)){
                const int echoes=f.quality>=2?1:3;
                for(int k=echoes;k>=1;--k){auto c=f.accent;c.a=alpha*(longNote*.26f+pulse*.12f)/(k*.8f);
                    const float zoom=1+k*(.10f+.08f*longNote);
                    Quad(x,y,w*zoom,h*zoom,angle,yaw,depth-15*k,c,s.u0,s.v0,s.u1,s.v1,1,2.f);}
            }
            float fill=1.f;
            if(f.following){
                if(waiting)fill=0.f;
                else if(line.timed){const float progress=Unit(float(f.positionMs-s.cell.start)/float(std::max<int64_t>(1,s.cell.duration)));
                    fill=Unit((progress*s.cell.wordAdvance-s.cell.wordOffset)/std::max(.001f,s.cell.advance));}
                else fill=f.positionMs>=f.lyrics->lines[line.index].startMs?1.f:0.f;
            }
            auto color=f.ink;color.a=alpha;
            const float singingFill=fill;if(syllable)fill=1.f;
            if(BackdropContrast(f))color={.96f,.945f,.91f,alpha};
            if(f.kind==LyricMotion::Diorama){
                const float unit=.62f/std::max(1.f,Desired(f));const float dx=s.anchorX-f.width*.5f,dy=f.height*.47f-s.anchorY;
                const std::array<D2D1_POINT_2F,4> points={{{dx-w*.5f,dy+h*.5f},{dx+w*.5f,dy+h*.5f},{dx+w*.5f,dy-h*.5f},{dx-w*.5f,dy-h*.5f}}};
                Vertex quad[4]{};bool visible=true;for(size_t i=0;i<4;++i){const auto q=diorama.Text(line.index,points[i].x*unit,points[i].y*unit);visible&=q.visible;
                    quad[i]={q.x,q.y,i==0||i==3?s.u0:s.u1,i<2?s.v0:s.v1,color.r,color.g,color.b,color.a,BackdropContrast(f)?3.f:0.f,1,s.u1,(s.u1-s.u0)*.035f};}
                const auto first=vertices.size();if(visible)for(int i:{0,1,2,0,2,3})vertices.push_back(quad[i]);
                if(role==0)CaptureGlyph(first,s,fill,anchorOnly);
                if(anchorOnly){IncludeAuxiliaryQuad(first,s);vertices.resize(first);continue;}
                Karaoke(first,s,fill);if(role==0){const bool full=MeasureGlyph(f,first,s,fill);if(line.timed&&f.positionMs>=s.cell.start&&f.positionMs<AddTime(s.cell.start,s.cell.duration)){reading.activeText=s.cell.text;reading.activeFullyVisible=full;}}Ornament(f,first,seed,fill,age,Unit(sungAge/std::max(.001f,duration)),singingFill,role==0);continue;
            }
            const auto first=vertices.size();
            Quad(x,y,w,h,angle,yaw,depth,color,s.u0,s.v0,s.u1,s.v1,f.kind==LyricMotion::Mineradio?4.f+std::clamp(f.motionFx,0,5):f.kind==LyricMotion::Tempera||BackdropContrast(f)?3.f:0.f,
                f.kind==LyricMotion::Mineradio?(line.timed?Unit(age/std::max(.001f,duration)):1.f):1.f,f.kind==LyricMotion::Still?1.f:enter);
            if(role==0)CaptureGlyph(first,s,fill,anchorOnly);
            if(anchorOnly){IncludeAuxiliaryQuad(first,s);vertices.resize(first);continue;}
            Karaoke(first,s,fill);
            Ornament(f,first,seed,fill,age,Unit(sungAge/std::max(.001f,duration)),singingFill,role==0);
            if(f.kind==LyricMotion::Rift&&enter<.95f&&motion>0){auto flare=f.accent;flare.a=alpha*(1-Ease(enter))*.85f;
                Quad(x,y,w*(.4f+.6f*enter),std::max(2.f,f.height*.003f),angle,0,0,flare);}
            if(role==0){const bool full=MeasureGlyph(f,first,s,fill);if(line.timed&&f.positionMs>=s.cell.start&&f.positionMs<AddTime(s.cell.start,s.cell.duration)){reading.activeText=s.cell.text;reading.activeFullyVisible=full;}}
        }
    }
    void CaptureGlyph(size_t first,const Sprite& sprite,float fill,bool anchor){
        if(vertices.size()<first+6)return;
        auto& poses=anchor?arrivalGlyphs:drawnGlyphs;
        if(poses.size()<64)poses.push_back({sprite.ordinal,{vertices[first],vertices[first+1],vertices[first+2],vertices[first+5]},fill});
    }
    void Reassemble(const RiftFrame& f){
        if(!Reassembles(f)||f.reduced||!f.following||departureGlyphs.empty()||arrivalGlyphs.empty()||current.pages.empty())return;
        const auto& page=current.pages[std::min(lastPage,current.pages.size()-1)];
        const float t=float(f.positionMs-assemblyAt)/float(LyricHandoffDuration(page.end-page.start));
        if(t<0||t>=1)return;
        const float travel=Smooth(t),oldAlpha=1-Smooth(t/.72f),newAlpha=Smooth((t-.18f)/.82f);
        const auto point=[](const std::array<Vertex,4>& q,float x,float y){return D2D1_POINT_2F{
            (q[0].x*(1-x)+q[1].x*x)*(1-y)+(q[3].x*(1-x)+q[2].x*x)*y,
            (q[0].y*(1-x)+q[1].y*x)*(1-y)+(q[3].y*(1-x)+q[2].y*x)*y};};
        const size_t count=std::min<size_t>(24,std::max(departureGlyphs.size(),arrivalGlyphs.size()));
        for(size_t n=0;n<count;++n){
            const auto& from=departureGlyphs[n%departureGlyphs.size()];const auto& to=arrivalGlyphs[n%arrivalGlyphs.size()];
            const auto source=std::find_if(oldSprites.begin(),oldSprites.end(),[&](const Sprite& s){return s.ordinal==from.ordinal;});
            const auto target=std::find_if(sprites.begin(),sprites.end(),[&](const Sprite& s){return s.ordinal==to.ordinal;});
            if(source==oldSprites.end()||target==sprites.end())continue;
            const bool appeared=!current.timed||f.positionMs>= (UsesSyllableEntrance(f.kind,f.particlePreset)?target->cell.start:LyricEntryStart(page.start,page.end-page.start,page.last-page.first,target->ordinal,target->cell.start));
            const int grid=f.quality>=2?3:4;
            for(int row=0;row<grid;++row)for(int col=0;col<grid;++col){
                const float seed=Seed(static_cast<uint32_t>(n*173+row*37+col*11));
                const float arc=std::sin(Pi*t)*Desired(f)*.12f,dx=(seed-.5f)*arc,dy=-arc*(.3f+seed);
                const float x0=float(col)/grid,x1=float(col+1)/grid,y0=float(row)/grid,y1=float(row+1)/grid;
                const float xx[4]={x0,x1,x1,x0},yy[4]={y0,y0,y1,y1};
                const auto submit=[&](const Sprite& atlas,const std::array<Vertex,4>& ink,float alpha,float fill){
                    if(alpha<=.001f||vertices.size()+6>=36000)return;Vertex tile[4]{};
                    for(int k=0;k<4;++k){const auto a=point(from.corners,xx[k],yy[k]),b=point(to.corners,xx[k],yy[k]);
                        tile[k]={a.x+(b.x-a.x)*travel+dx,a.y+(b.y-a.y)*travel+dy,
                            atlas.u0+(atlas.u1-atlas.u0)*xx[k],atlas.v0+(atlas.v1-atlas.v0)*yy[k],
                            f.accent.r,f.accent.g,f.accent.b,alpha*ink[0].a,0,1,atlas.u1,(atlas.u1-atlas.u0)*.035f};}
                    const size_t first=vertices.size();for(int k:{0,1,2,0,2,3})vertices.push_back(tile[k]);Karaoke(first,atlas,fill);++reading.reassemblyFragments;
                };
                // Different phrase lengths reuse a source/target. Divide that
                // ink's opacity so repeated mappings cannot flash it brighter.
                const float sourceUses=float((count-1-n%departureGlyphs.size())/departureGlyphs.size()+1);
                const float targetUses=float((count-1-n%arrivalGlyphs.size())/arrivalGlyphs.size()+1);
                submit(*source,from.corners,oldAlpha/sourceUses,from.fill);
                if(appeared)submit(*target,to.corners,newAlpha/targetUses,to.fill);
            }
        }
    }
    void FollowFirefly(const RiftFrame& f){
        const bool enabled=f.kind==LyricMotion::Lumiere||f.kind==LyricMotion::Classic||f.kind==LyricMotion::Partita||(f.kind==LyricMotion::Mineradio&&f.particlePreset==5);
        if(!enabled||f.reduced||!f.following||reading.glyphCount==0){fireflyReady=false;return;}
        size_t at=0;
        for(size_t i=0;i<reading.glyphCount;++i){if(reading.glyphs[i].fill>0)at=i;}
        const auto& g=reading.glyphs[at];const auto& next=reading.glyphs[std::min(at+1,reading.glyphCount-1)];
        const float progress=Smooth(g.fill),x0=(g.left+g.right)*.5f,y0=(g.top+g.bottom)*.5f;
        const float x1=(next.left+next.right)*.5f,y1=(next.top+next.bottom)*.5f;
        const D2D1_POINT_2F target{x0+(x1-x0)*progress,y0+(y1-y0)*progress};
        if(!fireflyReady||reading.reset||fireflyWidth!=f.width||fireflyHeight!=f.height){fireflyPosition=target;fireflyReady=true;}
        else if(f.playing){const float k=1-std::exp(-std::clamp(f.deltaSeconds,0.f,.1f)*16.f);
            fireflyPosition.x+=(target.x-fireflyPosition.x)*k;fireflyPosition.y+=(target.y-fireflyPosition.y)*k;}
        fireflyWidth=f.width;fireflyHeight=f.height;
        const float rx=std::clamp((g.right-g.left)*.62f,16.f,110.f),ry=std::clamp((g.bottom-g.top)*.63f,18.f,115.f);
        GlyphEmitter e;e.corners={D2D1_POINT_2F{fireflyPosition.x-rx,fireflyPosition.y-ry},{fireflyPosition.x+rx,fireflyPosition.y-ry},{fireflyPosition.x+rx,fireflyPosition.y+ry},{fireflyPosition.x-rx,fireflyPosition.y+ry}};
        e.color=f.accent;e.color.a=1;e.age=1;e.active=1;e.seed=13;e.padding=2;
        reading.fireflyVisible=glyphEffectFrame.Add(e);reading.fireflyPosition=fireflyPosition;
    }
    void Paint(const RiftFrame& f){
        drawnGlyphs.clear();arrivalGlyphs.clear();
        glyphEffectFrame={};glyphEffectFrame.width=f.width;glyphEffectFrame.height=f.height;
        glyphEffectFrame.seconds=float(f.positionMs)*.001f;
        glyphEffectFrame.stream=reinterpret_cast<uintptr_t>(f.lyrics.get())^f.revision;
        glyphEffectFrame.reset=reading.reset||f.discontinuity;
        glyphEffectFrame.phrase=(static_cast<uint64_t>(current.index+1)<<32)^static_cast<uint64_t>(lastPage+1);
        glyphEffectFrame.presentationMs=f.nowMs;glyphEffectFrame.advancing=f.playing;
        glyphEffectFrame.paused=f.paused;glyphEffectFrame.accent=f.accent;glyphEffectFrame.accent.a=1;
        glyphEffectFrame.allowGap=f.lyrics&&!f.lyrics->lines.empty();
        // Still keeps its static typography/background; only the newly requested
        // low-velocity letter light moves. The user's reduced-motion option wins.
        glyphEffectFrame.reduced=(f.kind==LyricMotion::Still?requestedReduced:f.reduced)||!f.following;
        glyphEffectFrame.quality=f.quality;glyphEffectFrame.clip={f.width*.025f,f.height*.12f,f.width*.975f,f.height*.84f};
        glyphEffectFrame.style=f.kind==LyricMotion::Mineradio?21+f.particlePreset:6;
        if(f.kind!=LyricMotion::Mineradio)for(int id:AvailableBuiltInStyles)if(id>=6&&MotionKind(id)==f.kind){glyphEffectFrame.style=id;break;}
        vertices.clear();reading.visibleClusters=reading.submittedClusters=reading.fullyVisibleClusters=0;reading.glyphLeft=reading.glyphTop=INFINITY;reading.glyphRight=reading.glyphBottom=-INFINITY;
        const float pulse=f.reduced?0.f:heldPulse;
        // Sparse architectural light: geometry responds to energy, not an idle starfield.
        if(!f.reduced&&f.kind==LyricMotion::Rift){const float t=static_cast<float>(f.positionMs)*.001f;auto c=f.accent;
            if(!current.pages.empty()){
                const auto& page=current.pages[std::min(lastPage,current.pages.size()-1)];
                const float opening=Unit(static_cast<float>(f.positionMs-page.start)/420.f);
                c.a=(1-Ease(opening))*.34f;
                Quad(f.width*(.48f+.42f*Ease(opening)),f.height*.44f,f.width*(.04f+.55f*opening),f.height*1.45f,-24,0,0,c);
            }
            const auto& view=camera.View();const float parallax=std::sin(view.x/std::max(1.f,f.width))*.15f;
            c.a=.04f+.13f*Unit(f.low*f.response);Quad(f.width*(.72f-parallax),f.height*.45f,f.width*.27f,f.height*1.2f,-24,0,0,c);
            c.a=.08f+.30f*pulse;Quad(f.width*.32f,f.height*.45f,std::max(1.f,f.width*.0015f),f.height*1.1f,-24,0,0,c);
            for(int i=0;i<(f.quality>=2?3:7);++i){const float at=Seed(static_cast<uint32_t>(i*479+11));
                c.a=(.04f+.12f*pulse)*(1-at);const float y=f.height*(.20f+.62f*at);
                Quad(f.width*(.1f+.8f*at)+std::fmod(std::max(0.f,t)*8.f,80.f),y,f.width*(.06f+.14f*at),1.f,-24,0,0,c);}
        }
        if(!Reassembles(f)&&f.following&&!f.reduced&&!f.focused&&f.kind!=LyricMotion::Still&&f.kind!=LyricMotion::Tempera&&f.kind!=LyricMotion::Lumiere&&f.kind!=LyricMotion::Sonnet&&contextCount==0&&f.positionMs-cutAt<240)DrawSprites(f,oldSprites,previous,-1);
        DrawSprites(f,sprites,current,0);
        auxiliaryPose={};
        if((f.showTranslation||f.showRomanization||f.kind==LyricMotion::Cappella||Reassembles(f))&&!sprites.empty())DrawSprites(f,sprites,current,0,true);
        Reassemble(f);
        if(!reading.visibleClusters)auxiliaryPose={};
        FollowFirefly(f);
        if(reading.visibleClusters==0&&!reading.awaitingWords&&!current.pages.empty()&&f.positionMs-current.pages[std::min(lastPage,current.pages.size()-1)].start>160)reading.valid=false;
    }
    void DrawAuxiliary(const RiftFrame& f,bool measureOnly=false){
        if(!auxiliaryPose.valid||!f.lyrics||current.index>=f.lyrics->lines.size())return;
        const auto& line=f.lyrics->lines[current.index];const auto& pose=auxiliaryPose;
        if(pose.scale<.05f||pose.scale>5.f)return;
        // The mirror stage projects a closer text plane; keep its secondary
        // type proportionally smaller before applying that same projection.
        const float baseSize=std::clamp(Desired(f)*.22f,13.f,27.f)*(f.kind==LyricMotion::Diorama?.85f:1.f);
        // Cache against local phrase geometry, not the visible glyph subset or
        // changing camera scale. Translation is never given invented word times.
        float localLeft=INFINITY,localRight=-INFINITY;
        for(const auto& glyph:sprites){localLeft=std::min(localLeft,glyph.anchorX-glyph.quadW*glyph.baseScale*.5f);localRight=std::max(localRight,glyph.anchorX+glyph.quadW*glyph.baseScale*.5f);}
        const float textWidth=std::clamp(localRight-localLeft,f.width*.26f,f.width*.72f);
        const std::wstring* texts[2]={f.showTranslation?&line.translation:nullptr,f.showRomanization?&line.romanization:nullptr};
        float total=0,inkWidth=0;
        for(int i=0;i<2;++i){if(!texts[i]||texts[i]->empty())continue;auto& cached=auxiliaryLayouts[i];const float size=baseSize*(i?.84f:1.f);
            if(!cached.layout||cached.text!=*texts[i]||cached.width!=textWidth||cached.size!=size){
                cached={};cached.text=*texts[i];cached.width=textWidth;cached.size=size;float fit=size;
                for(int attempt=0;attempt<8;++attempt){ComPtr<IDWriteTextFormat> format;
                    Check(write->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,fit,L"zh-CN",&format));
                    Check(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));cached.layout.Reset();
                    Check(write->CreateTextLayout(cached.text.c_str(),static_cast<UINT32>(cached.text.size()),format.Get(),textWidth,4096,&cached.layout));
                    DWRITE_TEXT_METRICS metrics{};Check(cached.layout->GetMetrics(&metrics));cached.height=metrics.height;cached.inkWidth=metrics.widthIncludingTrailingWhitespace;
                    if(cached.height<=f.height*.13f||fit<=10.f)break;fit=std::max(10.f,fit*.84f);}
            }total+=cached.height+4.f;inkWidth=std::max(inkWidth,cached.inkWidth);
        }
        if(total<=0)return;
        const float center=(pose.left+pose.right)*.5f;
        float bottom=pose.bottom+std::clamp(Desired(f)*.075f,6.f,14.f)*pose.scale;
        // Cappella's translation follows below the speech bubble, not inside
        // it. The full phrase bounds keep that gap stable during word entry.
        if(f.kind==LyricMotion::Cappella)bottom=std::max(pose.bottom,reading.glyphBottom)+12.f+std::clamp(Desired(f)*.09f,7.f,18.f);
        const float anchorX=center*pose.c-bottom*pose.s,anchorY=center*pose.s+bottom*pose.c;
        const auto transform=D2D1::Matrix3x2F(pose.c*pose.scale,pose.s*pose.scale,-pose.s*pose.scale,pose.c*pose.scale,anchorX,anchorY);
        reading.auxiliaryVisible=true;reading.auxiliaryTransform={transform._11,transform._12,transform._21,transform._22,transform._31,transform._32};reading.auxiliaryWidth=inkWidth;reading.auxiliaryHeight=total;
        if(measureOnly)return;
        atlasDraw->SetTarget(decorBitmap.Get());atlasDraw->SetDpi(96*f.dpi,96*f.dpi);atlasDraw->BeginDraw();atlasDraw->SetTransform(transform);
        float y=0;for(int i=0;i<2;++i){if(!texts[i]||texts[i]->empty())continue;const auto& cached=auxiliaryLayouts[i];
            if(BackdropContrast(f)||f.kind==LyricMotion::Mineradio){white->SetColor({0,0,0,.75f});atlasDraw->DrawTextLayout({-textWidth*.5f+1,y+1},cached.layout.Get(),white.Get());}
            auto color=f.ink;color.a=i?.64f:.86f;white->SetColor(color);atlasDraw->DrawTextLayout({-textWidth*.5f,y},cached.layout.Get(),white.Get());y+=cached.height+4.f;}
        atlasDraw->SetTransform(D2D1::Matrix3x2F::Identity());Check(atlasDraw->EndDraw());atlasDraw->SetTarget(nullptr);atlasDraw->SetDpi(96,96);
    }
    void Decorate(const RiftFrame& f){
        const auto line=[&](D2D1_POINT_2F a,D2D1_POINT_2F b,D2D1_COLOR_F color,float width=1.f){white->SetColor(color);atlasDraw->DrawLine(a,b,white.Get(),width);};
        auto muted=f.ink;muted.a=.16f;const float centerX=f.width*.17f,centerY=f.height*.47f,radius=std::min(f.width,f.height)*.34f;
        if(f.kind==LyricMotion::Monet){
            if(portraitSource!=f.cover||(f.cover&&portraitSequence!=f.cover->sequence)){portrait.Reset();portraitSource=f.cover;portraitSequence=f.cover?f.cover->sequence:0;
                if(f.cover){const auto& p=*f.cover;if(p.width&&p.height&&p.width<=3840&&p.height<=3840&&p.stride>=p.width*4&&uint64_t(p.stride)*p.height<=p.bgra.size()){
                    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));Check(atlasDraw->CreateBitmap({p.width,p.height},p.bgra.data(),p.stride,&props,&portrait));}}}
            if(portrait){const auto size=portrait->GetSize();const float edge=std::min(f.width*.35f,f.height*.60f),cx=f.width*.265f,cy=f.height*.47f;
                const float crop=std::min(size.width,size.height);atlasDraw->DrawBitmap(portrait.Get(),D2D1::RectF(cx-edge*.5f,cy-edge*.5f,cx+edge*.5f,cy+edge*.5f),.94f,D2D1_INTERPOLATION_MODE_LINEAR,D2D1::RectF((size.width-crop)*.5f,(size.height-crop)*.5f,(size.width+crop)*.5f,(size.height+crop)*.5f));}
        }
        if(f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Mineradio&&f.kind!=LyricMotion::Tempera&&f.kind!=LyricMotion::Lumiere&&f.kind!=LyricMotion::Sonnet&&f.kind!=LyricMotion::Diorama&&f.motionBackground==0){
            // GeometricBackground.tsx/CSS: 15 independent drift/turn loops and 20 rising particles.
            const auto seed=Hash(f.identity);const float t=f.reduced||!f.following?0.f:static_cast<float>(f.positionMs)*.001f;
            for(int i=0;i<15;++i){const auto n=seed+static_cast<uint32_t>(i)*713;const int kind=static_cast<int>(Seed(n)*4)%4;const float size=40+Seed(n+1)*100,dir=Seed(n+2)>.5f?-1.f:1.f;
                const float phase=std::fmod(std::max(0.f,t-Seed(n+3)*5)/(30+Seed(n+4)*30),1.f),wave=phase<.5f?phase*2:2-phase*2;
                const float x=Seed(n+5)*f.width+(-15+30*wave)*dir,y=Seed(n+6)*f.height+(30-60*wave)*dir,angle=Seed(n+7)*360+phase*360;
                auto c=f.accent;c.a=.025f+Seed(n+8)*.035f;white->SetColor(c);atlasDraw->SetTransform(D2D1::Matrix3x2F::Rotation(angle,{x,y}));
                const float band=kind==0?f.low:kind==3?f.high:f.mid,r=size*.5f*(.95f+band*.5f);
                if(kind==0){const auto ellipse=D2D1::Ellipse({x,y},r,r);if(Seed(n+9)<.3f)atlasDraw->FillEllipse(ellipse,white.Get());else atlasDraw->DrawEllipse(ellipse,white.Get(),1);}
                else if(kind==1){const D2D1_RECT_F rect{x-r,y-r,x+r,y+r};if(Seed(n+9)<.3f)atlasDraw->FillRectangle(rect,white.Get());else atlasDraw->DrawRectangle(rect,white.Get(),1);}
                else if(kind==2){atlasDraw->DrawLine({x,y-r},{x-r,y+r},white.Get(),1);atlasDraw->DrawLine({x-r,y+r},{x+r,y+r},white.Get(),1);atlasDraw->DrawLine({x+r,y+r},{x,y-r},white.Get(),1);}
                else {atlasDraw->DrawLine({x-r,y-r},{x+r,y+r},white.Get(),r*.35f);atlasDraw->DrawLine({x+r,y-r},{x-r,y+r},white.Get(),r*.35f);}
            }atlasDraw->SetTransform(D2D1::Matrix3x2F::Identity());
            for(int i=0;i<20;++i){const auto n=seed+static_cast<uint32_t>(i)*173;const float phase=std::fmod(std::max(0.f,t-Seed(n+3)*10)/(15+Seed(n+4)*20),1.f),r=.5f+Seed(n)*2;auto c=f.accent;c.a=Seed(n+5)*.3f*std::sin(phase*Pi);white->SetColor(c);atlasDraw->FillEllipse(D2D1::Ellipse({Seed(n+1)*f.width,Seed(n+2)*f.height-phase*100},r,r),white.Get());}
        }
        if(f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Mineradio&&f.motionBackground==4){
            // SoraBackground.tsx: same 150 seeded points, speed, size and blink equations.
            const auto hash=[](float x,float y){const double n=std::sin(double(x)*127.1+double(y)*311.7)*43758.5453123;return static_cast<float>(n-std::floor(n));};
            const float t=f.reduced||!f.following?0.f:static_cast<float>(f.positionMs)*.001f,aspect=f.width/f.height;
            for(int i=0;i<150;++i){const float seed=i*123.456f,speed=hash(seed,1.1f),size=hash(seed,2.2f),yr=hash(seed,3.3f),xr=hash(seed,4.4f),blink=hash(seed,5.5f),color=hash(seed,6.6f);
                const float x=std::fmod(xr+t*(.008f+speed*.024f),1.f)*(aspect+.1f)-(aspect*.5f+.05f),y=-.45f+yr*.9f+std::sin(t*.3f+yr*6.283f)*.005f;
                auto c=color>.90f?f.accent:f.ink;c.a=Unit(.3f+.7f*std::sin(t*(1+blink*2.5f)+xr*6.283f));white->SetColor(c);
                const float r=std::max(1.f,(.0006f+.0032f*std::pow(size,3.8f))*f.height*1.75f);atlasDraw->FillEllipse(D2D1::Ellipse({(x/aspect+.5f)*f.width,(.5f-y)*f.height},r,r),white.Get());}
        }
        if(f.kind==LyricMotion::Partita){auto c=f.accent;c.a=.13f;const float size=Desired(f);float railX=-10000;
            for(const auto& sprite:sprites)if(std::abs(sprite.anchorX-railX)>size*.8f){railX=sprite.anchorX;line({railX-size*.72f,f.height*.22f},{railX-size*.72f,f.height*.72f},c);}}
        if(f.kind==LyricMotion::Pendolo){
            const auto gear=[&](float x,float y,float r,int teeth,float rotation,float opacity){auto color=f.accent;color.a=opacity;white->SetColor(color);
                D2D1_POINT_2F previous{},first{};for(int tooth=0;tooth<teeth;++tooth)for(int corner=0;corner<4;++corner){const float offsets[]={-.22f,-.12f,.12f,.22f};const float a=rotation+(static_cast<float>(tooth)+offsets[corner])*2*Pi/teeth,rr=r*(corner==0||corner==3?.89f:1.f);
                    const D2D1_POINT_2F p{x+std::cos(a)*rr,y+std::sin(a)*rr};if(tooth||corner)atlasDraw->DrawLine(previous,p,white.Get(),1);else first=p;previous=p;}atlasDraw->DrawLine(previous,first,white.Get(),1);
                atlasDraw->DrawEllipse(D2D1::Ellipse({x,y},r*.26f,r*.26f),white.Get(),1);for(int spoke=0;spoke<6;++spoke){const float a=rotation+spoke*Pi/3;atlasDraw->DrawLine({x+std::cos(a)*r*.26f,y+std::sin(a)*r*.26f},{x+std::cos(a)*r*.78f,y+std::sin(a)*r*.78f},white.Get(),1);}};
            white->SetColor(muted);atlasDraw->DrawEllipse(D2D1::Ellipse({centerX,centerY},radius,radius),white.Get(),1);
            for(int tick=0;tick<60;++tick){const float a=static_cast<float>(tick)*Pi/30;line({centerX+std::cos(a)*radius*.94f,centerY+std::sin(a)*radius*.94f},{centerX+std::cos(a)*radius,centerY+std::sin(a)*radius},muted,tick%5?1.f:2.f);}
            gear(centerX,centerY,radius*.66f,48,clockPhase*.12f,.23f);
            gear(centerX+radius*.2f,centerY-radius*.75f,radius*.28f,20,std::sin(clockPhase)*(.15f+clockBass*.7f),.58f);
            gear(centerX+radius*.32f,centerY+radius*.78f,radius*.34f,30,-clockPhase*.17f,.42f);
            gear(centerX+radius*.68f,centerY+radius*.76f,radius*.16f,15,secondAngle,.58f);
            gear(centerX+radius*.54f,centerY+radius*.75f,radius*.08f,10,-secondAngle*1.5f,.28f);
        }
        auto bubble=[&](float cx,float cy,float width,float height,bool right,float alpha){auto c=right?f.accent:f.ink;c.a=alpha;white->SetColor(c);
            atlasDraw->FillRoundedRectangle(D2D1::RoundedRect({cx-width*.5f-15,cy-height*.5f-12,cx+width*.5f+15,cy+height*.5f+12},16,16),white.Get());
            c.a=alpha*2;white->SetColor(c);atlasDraw->FillEllipse(D2D1::Ellipse({cx+(right?1.f:-1.f)*(width*.5f+43),cy-height*.5f+10},17,17),white.Get());};
        if(f.kind==LyricMotion::Cappella&&auxiliaryPose.valid){
            // Full projected phrase, rather than untransformed layout anchors.
            // Only the original lyric belongs inside the speech bubble.
            float left=std::min(auxiliaryPose.left,reading.glyphLeft),right=std::max(auxiliaryPose.right,reading.glyphRight);
            float top=std::min(auxiliaryPose.top,reading.glyphTop),bottom=std::max(auxiliaryPose.bottom,reading.glyphBottom);
            if(right>left){bubble((left+right)*.5f,(top+bottom)*.5f,right-left,bottom-top,(current.hash&1)!=0,.15f);
                reading.bubbleVisible=true;reading.bubbleBounds={left-15,top-12,right+15,bottom+12};}}
        for(const auto& c:contextLines){
            const bool earlier=c.index<current.index;
            const float x=f.width*.5f,y=f.height*(earlier?.14f:.83f);
            auto color=f.ink;color.a=.19f;white->SetColor(color);
            const auto clip=D2D1::RectF(f.width*.12f,y-f.height*.04f,f.width*.88f,y+f.height*.04f);
            atlasDraw->PushAxisAlignedClip(clip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            atlasDraw->DrawTextLayout({x-c.width*.5f,y-c.height*.5f},c.layout.Get(),white.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            atlasDraw->PopAxisAlignedClip();
        }atlasDraw->SetTransform(D2D1::Matrix3x2F::Identity());white->SetColor(D2D1::ColorF(1,1,1,1));
    }
    void CompositeProgram(UINT w,UINT h){
        if(!layerVS){Check(device->CreateVertexShader(g_FoliaTransitionVS,sizeof(g_FoliaTransitionVS),nullptr,&layerVS));Check(device->CreatePixelShader(g_FoliaLayerPS,sizeof(g_FoliaLayerPS),nullptr,&layerPS));}
        auto* output=outputView.Get();context->OMSetRenderTargets(1,&output,nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        const D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(layerVS.Get(),nullptr,0);context->PSSetShader(layerPS.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        auto* source=programView.Get();auto* s=sampler.Get();context->PSSetShaderResources(1,1,&source);context->PSSetSamplers(0,1,&s);context->Draw(3,0);ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(1,1,&none);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void BackgroundDetail(const RiftFrame& f,UINT w,UINT h){
        if(!detailPS){Check(device->CreatePixelShader(g_ThemeDetailPS,sizeof(g_ThemeDetailPS),nullptr,&detailPS));Check(device->CreateVertexShader(g_FoliaTransitionVS,sizeof(g_FoliaTransitionVS),nullptr,&detailVS));
            D3D11_BUFFER_DESC bd{};bd.ByteWidth=64;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Check(device->CreateBuffer(&bd,nullptr,&detailConstants));}
        const int style=f.kind==LyricMotion::Mineradio?21+f.particlePreset:6+static_cast<int>(f.kind);
        const float moving=f.reduced||!f.following?0.f:1.f;
        const float v[16]={f.width,f.height,float(f.positionMs)*.001f*moving,float(style),f.low*f.response*moving,f.high*f.response*moving,f.pulse*f.response*moving,float(f.quality),f.accent.r,f.accent.g,f.accent.b,1,f.background.r,f.background.g,f.background.b,1};
        context->UpdateSubresource(detailConstants.Get(),0,nullptr,v,0,0);auto* cb=detailConstants.Get();context->PSSetConstantBuffers(0,1,&cb);
        auto* rt=outputView.Get();context->OMSetRenderTargets(1,&rt,nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        const D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(detailVS.Get(),nullptr,0);context->PSSetShader(detailPS.Get(),nullptr,0);
        context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->Draw(3,0);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void DrawGlyphVertices(const RiftFrame& f,UINT w,UINT h,ID3D11RenderTargetView* rt,ID3D11PixelShader* shader){
        context->OMSetRenderTargets(1,&rt,nullptr);context->OMSetBlendState(blend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        const D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
        const auto light=CinemaLyricLight(f.accent,.50f);
        const float values[12]={f.width,f.height,1/Atlas,1/Atlas,float(f.positionMs)*.001f,f.reduced?0.f:heldPulse,f.reduced?0.f:1.f,float(current.hash%10000),light.r,light.g,light.b,1};
        context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);auto* cb=constants.Get();context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(shader,nullptr,0);context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        context->IASetInputLayout(inputLayout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride=sizeof(Vertex),offset=0;auto* vb=vertexBuffer.Get();context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
        auto* srv=atlasView.Get();auto* sm=sampler.Get();context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&sm);
        ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(1,1,&empty);
        if(!vertices.empty())context->Draw(static_cast<UINT>(vertices.size()),0);
        context->PSSetShaderResources(0,1,&empty);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void SoftGlyphLight(const RiftFrame& f,UINT w,UINT h){
        if(vertices.empty())return;
        // One reusable, bounded screen mask for every theme. The blur extends
        // outside the glyph quad without sampling neighbouring atlas entries.
        const float ratio=std::min({f.quality>=2?.25f:.5f,1280.f/w,720.f/h});
        const UINT gw=std::max(1u,UINT(std::ceil(w*ratio))),gh=std::max(1u,UINT(std::ceil(h*ratio)));
        if(!glowPS){Check(device->CreatePixelShader(g_RiftGlowPS,sizeof(g_RiftGlowPS),nullptr,&glowPS));
            Check(atlasDraw->CreateEffect(CLSID_D2D1DpiCompensation,&glowDpi));
            Check(glowDpi->SetValue(D2D1_DPICOMPENSATION_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD));
            Check(atlasDraw->CreateEffect(CLSID_D2D1GaussianBlur,&glowNear));Check(atlasDraw->CreateEffect(CLSID_D2D1GaussianBlur,&glowFar));
            Check(atlasDraw->CreateEffect(CLSID_D2D1ColorMatrix,&glowGain));
            glowNear->SetInputEffect(0,glowDpi.Get());glowFar->SetInputEffect(0,glowDpi.Get());
            for(auto* effect:{glowNear.Get(),glowFar.Get()})Check(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED));}
        if(gw!=glowWidth||gh!=glowHeight||f.width!=glowDipWidth||f.height!=glowDipHeight){
            glowDpi->SetInput(0,nullptr);glowBitmap.Reset();glowTarget.Reset();glowTexture.Reset();
            D3D11_TEXTURE2D_DESC d{};d.Width=gw;d.Height=gh;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            Check(device->CreateTexture2D(&d,nullptr,&glowTexture));Check(device->CreateRenderTargetView(glowTexture.Get(),nullptr,&glowTarget));
            ComPtr<IDXGISurface> surface;Check(glowTexture.As(&surface));const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(d.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96*gw/f.width,96*gh/f.height);
            Check(atlasDraw->CreateBitmapFromDxgiSurface(surface.Get(),&props,&glowBitmap));glowWidth=gw;glowHeight=gh;glowDipWidth=f.width;glowDipHeight=f.height;
            // Effects consume bitmap pixels independently of its DPI metadata.
            // Restore this reduced mask to stage DIPs before applying the blur;
            // otherwise its light lands above/left of the actual letters.
            Check(glowDpi->SetValue(D2D1_DPICOMPENSATION_PROP_INPUT_DPI,D2D1::Vector2F(96*gw/f.width,96*gh/f.height)));
            glowDpi->SetInput(0,glowBitmap.Get());}
        const float clear[4]={};context->ClearRenderTargetView(glowTarget.Get(),clear);DrawGlyphVertices(f,gw,gh,glowTarget.Get(),glowPS.Get());
        // The pixels were written through D3D; invalidate the dependent D2D
        // effects explicitly even when the bitmap and its dimensions persist.
        glowDpi->SetInput(0,glowBitmap.Get());
        const float radius=LyricGlowRadius(Desired(f));
        Check(glowNear->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,radius));Check(glowFar->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,radius*2.5f));
        atlasDraw->SetTarget(decorBitmap.Get());atlasDraw->SetDpi(96*f.dpi,96*f.dpi);atlasDraw->SetTransform(D2D1::Matrix3x2F::Identity());atlasDraw->BeginDraw();
        const bool light=f.background.r*.2126f+f.background.g*.7152f+f.background.b*.0722f>.45f;
        const auto mode=light?D2D1_COMPOSITE_MODE_SOURCE_OVER:D2D1_COMPOSITE_MODE_PLUS;
        // Attenuate after diffusion, preserving a faint broad halo around ink.
        // The same gain on RGB and alpha preserves premultiplied compositing.
        for(auto* effect:{glowFar.Get(),glowNear.Get()}){
            const float gain=LyricGlowGain(effect==glowFar.Get(),light);
            const D2D1_MATRIX_5X4_F m={gain,0,0,0,0,gain,0,0,0,0,gain,0,0,0,0,gain,0,0,0,0};
            glowGain->SetInputEffect(0,effect);Check(glowGain->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,m));
            atlasDraw->DrawImage(glowGain.Get(),D2D1_INTERPOLATION_MODE_LINEAR,mode);
        }
        Check(atlasDraw->EndDraw());atlasDraw->SetTarget(nullptr);atlasDraw->SetDpi(96,96);
    }
    void Output(const RiftFrame& f,ID2D1DeviceContext* destination){
        const UINT w=static_cast<UINT>(std::clamp(std::ceil(f.width*f.dpi),1.f,8192.f)),h=static_cast<UINT>(std::clamp(std::ceil(f.height*f.dpi),1.f,8192.f));
        if(w!=pixelWidth||h!=pixelHeight||pixelDpi!=f.dpi){
            hasOutput=false;transitionAt=-10000;sceneView.Reset();departingView.Reset();departingTexture.Reset();transitionTexture.Reset();transitionOutput.Reset();transitionBitmap.Reset();
            outputBitmap.Reset();decorBitmap.Reset();outputView.Reset();outputTexture.Reset();programBitmap.Reset();programView.Reset();programOutput.Reset();programTexture.Reset();
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            Check(device->CreateTexture2D(&d,nullptr,&outputTexture));Check(device->CreateShaderResourceView(outputTexture.Get(),nullptr,&sceneView));Check(device->CreateRenderTargetView(outputTexture.Get(),nullptr,&outputView));
            ComPtr<IDXGISurface> surface;Check(outputTexture.As(&surface));
            auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(d.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96*f.dpi,96*f.dpi);
            Check(destination->CreateBitmapFromDxgiSurface(surface.Get(),&props,&outputBitmap));pixelWidth=w;pixelHeight=h;pixelDpi=f.dpi;
            props.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET;Check(atlasDraw->CreateBitmapFromDxgiSurface(surface.Get(),&props,&decorBitmap));
        }
        const bool needsProgram=f.kind==LyricMotion::Tempera||f.kind==LyricMotion::Lumiere||f.kind==LyricMotion::Sonnet||f.kind==LyricMotion::Diorama;
        if(needsProgram&&!programTexture){D3D11_TEXTURE2D_DESC d{};outputTexture->GetDesc(&d);
            Check(device->CreateTexture2D(&d,nullptr,&programTexture));Check(device->CreateShaderResourceView(programTexture.Get(),nullptr,&programView));Check(device->CreateRenderTargetView(programTexture.Get(),nullptr,&programOutput));
            ComPtr<IDXGISurface> surface;Check(programTexture.As(&surface));const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(d.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96*f.dpi,96*f.dpi);
            Check(atlasDraw->CreateBitmapFromDxgiSurface(surface.Get(),&props,&programBitmap));
        }else if(!needsProgram){programBitmap.Reset();programView.Reset();programOutput.Reset();programTexture.Reset();}
        if(captureTransition&&hasOutput){
            if(!departingTexture){D3D11_TEXTURE2D_DESC d{};outputTexture->GetDesc(&d);Check(device->CreateTexture2D(&d,nullptr,&departingTexture));Check(device->CreateShaderResourceView(departingTexture.Get(),nullptr,&departingView));
                Check(device->CreateTexture2D(&d,nullptr,&transitionTexture));Check(device->CreateRenderTargetView(transitionTexture.Get(),nullptr,&transitionOutput));ComPtr<IDXGISurface> surface;Check(transitionTexture.As(&surface));
                const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(d.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96*f.dpi,96*f.dpi);Check(destination->CreateBitmapFromDxgiSurface(surface.Get(),&props,&transitionBitmap));}
            // If another phrase arrives during a handoff, retain the actual
            // displayed composite rather than jumping back to its raw layer.
            context->CopyResource(departingTexture.Get(),presentBitmap==transitionBitmap.Get()?transitionTexture.Get():outputTexture.Get());transitionAt=f.positionMs;
            transitionDuration=LyricHandoffDuration(current.pages.empty()?960:current.pages[lastPage].end-current.pages[lastPage].start);
            transitionKind=f.kind==LyricMotion::Tempera?static_cast<int>(current.hash%3):f.kind==LyricMotion::Sonnet?3+static_cast<int>(current.hash%3):1;
        }
        if(vertices.size()>vertexCapacity){vertexCapacity=std::max<size_t>(vertices.size(),4096);vertexBuffer.Reset();
            D3D11_BUFFER_DESC d{};d.ByteWidth=static_cast<UINT>(vertexCapacity*sizeof(Vertex));d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=D3D11_BIND_VERTEX_BUFFER;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;Check(device->CreateBuffer(&d,nullptr,&vertexBuffer));}
        if(!vertices.empty()){D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(vertexBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));std::memcpy(mapped.pData,vertices.data(),vertices.size()*sizeof(Vertex));context->Unmap(vertexBuffer.Get(),0);}
        const float clear[4]={0,0,0,0};context->ClearRenderTargetView(outputView.Get(),clear);
        if(f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Mineradio){if(!backdrop){backdrop=std::make_unique<FoliaBackdrop>();backdrop->Create(device.Get());}backdrop->Draw(f,outputView.Get(),w,h);}
        if(f.kind==LyricMotion::Mineradio){if(!mineradio){mineradio=std::make_unique<MineradioStage>();mineradio->Create(device.Get());}mineradio->Draw(f,outputView.Get(),w,h,reading.reset);}
        const bool hasProgram=(f.kind==LyricMotion::Tempera||f.kind==LyricMotion::Lumiere||f.kind==LyricMotion::Sonnet)&&current.index!=SIZE_MAX&&!current.pages.empty();
        // SetTarget does not inherit the bitmap DPI. Every D2D stage pass must
        // use the same DIP-to-pixel scale as the GPU glyph projection.
        atlasDraw->SetDpi(96*f.dpi,96*f.dpi);atlasDraw->SetTransform(D2D1::Matrix3x2F::Identity());
        if(hasProgram){const auto& range=current.pages[lastPage];const float elapsed=f.following?static_cast<float>(f.positionMs-range.start)/1000.f:2.f;
            const float duration=static_cast<float>(std::max<int64_t>(1,range.end-range.start))/1000.f;
            const size_t variant=Variant(f,current);
            if(f.kind==LyricMotion::Lumiere){
                if(!lightVS){Check(device->CreateVertexShader(g_LumiereVS,sizeof(g_LumiereVS),nullptr,&lightVS));Check(device->CreatePixelShader(g_LumierePS,sizeof(g_LumierePS),nullptr,&lightPS));
                    D3D11_BUFFER_DESC bd{};bd.ByteWidth=52*16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Check(device->CreateBuffer(&bd,nullptr,&lightConstants));}
                const auto values=programs.LightConstants(variant,elapsed,duration,f.width,f.height,f.reduced||!f.following,f.low,f.quality);
                context->UpdateSubresource(lightConstants.Get(),0,nullptr,values.data(),0,0);auto* cb=lightConstants.Get();context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
                auto* rt=programOutput.Get();context->ClearRenderTargetView(rt,clear);context->OMSetRenderTargets(1,&rt,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);
                const D3D11_VIEWPORT lightViewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&lightViewport);context->RSSetState(raster.Get());
                context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(lightVS.Get(),nullptr,0);context->PSSetShader(lightPS.Get(),nullptr,0);context->GSSetShader(nullptr,nullptr,0);
                context->Draw(3,0);context->OMSetRenderTargets(0,nullptr,nullptr);
            }
            atlasDraw->SetTarget(programBitmap.Get());atlasDraw->BeginDraw();if(f.kind!=LyricMotion::Lumiere)atlasDraw->Clear({0,0,0,0});
            programs.Draw(atlasDraw.Get(),f.kind==LyricMotion::Sonnet?FoliaProgramKind::Sonnet:f.kind==LyricMotion::Tempera?FoliaProgramKind::Tempera:FoliaProgramKind::Lumiere,
                variant,elapsed,duration,f.width,f.height,f.reduced||!f.following,f.low,f.quality,f.kind!=LyricMotion::Lumiere);
            Check(atlasDraw->EndDraw());atlasDraw->SetTarget(nullptr);CompositeProgram(w,h);}
        if(f.kind==LyricMotion::Diorama&&current.index!=SIZE_MAX){atlasDraw->SetTarget(programBitmap.Get());atlasDraw->BeginDraw();atlasDraw->Clear({0,0,0,0});diorama.Draw(atlasDraw.Get(),f);Check(atlasDraw->EndDraw());atlasDraw->SetTarget(nullptr);CompositeProgram(w,h);}
        BackgroundDetail(f,w,h);
        if(contextCount>0||f.kind==LyricMotion::Partita||f.kind==LyricMotion::Cappella||((f.kind!=LyricMotion::Rift&&f.kind!=LyricMotion::Mineradio)&&(f.motionBackground==0||f.motionBackground==4))){atlasDraw->SetTarget(decorBitmap.Get());atlasDraw->BeginDraw();Decorate(f);Check(atlasDraw->EndDraw());atlasDraw->SetTarget(nullptr);}
        atlasDraw->SetDpi(96,96);
        SoftGlyphLight(f,w,h);
        glyphEffects.Draw(context.Get(),outputView.Get(),w,h,glyphEffectFrame);
        reading.cometRelays=glyphEffects.RelayCount();reading.cometFlightSeconds=glyphEffects.FlightSeconds();reading.cometTrailSamples=glyphEffects.TrailSamples();
        if(reading.fireflyVisible)reading.fireflyTrailSegments=glyphEffects.TrailSamples();
        reading.particleEmitters=glyphEffectFrame.reduced?0:glyphEffectFrame.count;
        DrawGlyphVertices(f,w,h,outputView.Get(),ps.Get());
        DrawAuxiliary(f);
        ID3D11ShaderResourceView* none[2]={nullptr,nullptr};
        presentBitmap=outputBitmap.Get();hasOutput=true;
        if(transitionBitmap&&(f.reduced||f.positionMs-transitionAt>=transitionDuration||f.positionMs<transitionAt)){
            transitionBitmap.Reset();transitionOutput.Reset();departingView.Reset();departingTexture.Reset();transitionTexture.Reset();transitionAt=-10000;
        }
        const float transitionProgress=static_cast<float>(f.positionMs-transitionAt)/float(transitionDuration);
        if(transitionBitmap&&transitionProgress>=0&&transitionProgress<1&&!f.reduced){
            if(!transitionVS){Check(device->CreateVertexShader(g_FoliaTransitionVS,sizeof(g_FoliaTransitionVS),nullptr,&transitionVS));Check(device->CreatePixelShader(g_FoliaTransitionPS,sizeof(g_FoliaTransitionPS),nullptr,&transitionPS));
                D3D11_BUFFER_DESC d{};d.ByteWidth=32;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Check(device->CreateBuffer(&d,nullptr,&transitionConstants));}
            const float transitionValues[8]={float(w),float(h),0,0,transitionProgress,float(transitionKind),.4f,float(current.hash%100000)};
            context->UpdateSubresource(transitionConstants.Get(),0,nullptr,transitionValues,0,0);auto* c=transitionConstants.Get();context->VSSetConstantBuffers(0,1,&c);context->PSSetConstantBuffers(0,1,&c);
            auto* output=transitionOutput.Get();context->OMSetRenderTargets(1,&output,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);
            context->IASetInputLayout(nullptr);context->VSSetShader(transitionVS.Get(),nullptr,0);context->PSSetShader(transitionPS.Get(),nullptr,0);
            ID3D11ShaderResourceView* sources[2]={departingView.Get(),sceneView.Get()};context->PSSetShaderResources(0,2,sources);context->Draw(3,0);
            context->PSSetShaderResources(0,2,none);context->OMSetRenderTargets(0,nullptr,nullptr);presentBitmap=transitionBitmap.Get();
        }
    }
};

RiftStage::RiftStage():impl_(std::make_unique<Impl>()){}
RiftStage::~RiftStage()=default;
void RiftStage::Create(ID3D11Device* device,IDWriteFactory* write){impl_->Create(device,write);}
ID2D1Bitmap1* RiftStage::Render(const RiftFrame& input,ID2D1DeviceContext* destination){impl_->requestedReduced=input.reduced;const auto f=impl_->StableAudio(impl_->Presentation(input));impl_->Prepare(f);impl_->Paint(f);impl_->Output(f,destination);return impl_->presentBitmap;}
RiftReading RiftStage::Reading()const{return impl_->reading;}
}
