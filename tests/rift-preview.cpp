// Development only. Synthetic snapshots, no HWND, user profile, capture or player.
#include "renderer.h"
#include "rift_stage.h"
#include "glyph_effects.h"
#include "lyric_fairies.h"
#include "folia_camera.h"
#include "folia_programs.h"
#include "theme_transition.h"
#include "themes.h"
#include "lyric_performance.h"
#include "lyric_typography.h"
#include "optional_lyric_font.h"
#include "lyric_motion_balance.h"
#include "shaders/g_RiftVS.h"
#include "shaders/g_RiftPS.h"
#include "third_party/json.hpp"
#include <d2d1_1helper.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <io.h>

// This tool never saves themes or reads a normal profile. Fail closed if a
// future preview accidentally calls a product user-data path operation.
namespace stage {std::filesystem::path UserDataDirectory(){throw std::runtime_error("User profile access is forbidden in the synthetic preview");}}

namespace {
using namespace stage;using Microsoft::WRL::ComPtr;namespace fs=std::filesystem;
void Check(bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);}
void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Preview HRESULT "+std::to_string(static_cast<unsigned long>(hr)));}
std::shared_ptr<LyricTimeline> Demo(){
    auto t=std::make_shared<LyricTimeline>();t->songId=L"100001";t->state=LyricLoadState::Ready;t->wordTimed=true;
    const std::array<std::wstring,7> texts={L"让黑夜裂开",L"把心跳推向光里",L"这一秒由我发声",L"让所有沉默沸腾",L"停一下听见自己",L"向前向前别回头",L"把世界唱成现场"};
    const std::array<int64_t,8> starts={500,4400,8400,12400,16600,21600,25000,30000};
    const std::array<std::wstring,7> subs={L"BREAK OPEN THE NIGHT",L"PUSH THE PULSE INTO LIGHT",L"THIS MOMENT IS MY VOICE",L"LET THE SILENCE IGNITE",L"HOLD. HEAR YOURSELF.",L"FORWARD. DON'T LOOK BACK.",L"TURN THE WORLD INTO A STAGE"};
    for(size_t i=0;i<texts.size();++i){LyricLine line;line.text=texts[i];line.translation=subs[i];line.startMs=starts[i];line.durationMs=starts[i+1]-starts[i];
        std::vector<float> weights;float total=0;for(size_t j=0;j<line.text.size();++j){const float weight=j+1==line.text.size()?3.2f:j==1?1.3f:.75f+float((j*7+i*3)%5)*.13f;weights.push_back(weight);total+=weight;}
        int64_t at=line.startMs;const int64_t span=line.durationMs-260;
        for(size_t j=0;j<line.text.size();++j){const int64_t end=j+1==line.text.size()?line.startMs+span:at+static_cast<int64_t>(span*weights[j]/total);
            line.words.push_back({at,end-at,line.text.substr(j,1)});at=end;}
        t->lines.push_back(std::move(line));}
    return t;
}
struct Features{float rms=0,peak=0;std::array<float,32> bands{};};
std::vector<Features> FeaturesFile(const fs::path& path){
    std::vector<Features> rows;if(path.empty())return rows;std::ifstream file(path);Check(bool(file),"Cannot read synthetic features");std::string row;
    while(std::getline(file,row)){std::replace(row.begin(),row.end(),',',' ');std::istringstream in(row);Features f;Check(bool(in>>f.rms>>f.peak),"Malformed features");for(auto& band:f.bands)Check(bool(in>>band),"Missing feature band");rows.push_back(f);}
    return rows;
}
std::shared_ptr<BackgroundFrame> Cover(){
    auto cover=std::make_shared<BackgroundFrame>();cover->width=cover->height=192;cover->stride=768;cover->sequence=1;cover->bgra.resize(192*192*4);
    for(int y=0;y<192;++y)for(int x=0;x<192;++x){const bool slash=x+y/3>87&&x+y/3<104;const size_t p=(y*192+x)*4;
        cover->bgra[p]=slash?31:12;cover->bgra[p+1]=slash?90:9;cover->bgra[p+2]=slash?255:8;cover->bgra[p+3]=255;}
    return cover;
}
SceneInput Scene(const std::shared_ptr<LyricTimeline>& lyrics,int64_t now,int64_t pos,const Features& a){
    SceneInput s;s.lyrics=lyrics;s.lyricRevision=s.lyricReadingRevision=1;s.lyricAssociation=LyricAssociation::VerifiedCurrent;s.frameAtMs=now;
    auto& p=s.playback;p.pid=424242;p.processCreatedAtTicks=1;p.songId=L"100001";p.songIdVerified=true;p.catalogMetadataReady=true;
    p.title=L"裂光现场 / ORIGINAL MOTION";p.artist=L"原创合成节奏 · 合成歌词时序";p.album=L"HTune";
    p.state=PlayState::Playing;p.reliability=Reliability::NativeRead;p.nativeClockAdvanceAllowed=true;p.positionMs=pos;p.durationMs=30000;p.sampledAtMs=p.observedAtMs=now;
    s.audio.pid=p.pid;s.audio.processCreatedAtTicks=1;s.audio.active=true;s.audio.timingTrusted=true;s.audio.sampledAtMs=s.audio.observedAtMs=now;
    s.audio.rms=a.rms;s.audio.peak=a.peak;s.audio.spectrum=a.bands;
    s.view.theme=RiftStyle;s.view.lyricDisplayMode=0;s.view.panelVisible=false;s.view.fullscreen=true;s.view.coverBackground=true;
    s.view.lyricSize=48;s.view.response=1.35f;s.view.cameraMotion=1.35f;s.view.qualityMode=1;
    s.background.kind=BackgroundKind::Image;s.background.sourceRevision=1;
    return s;
}
void Png(const fs::path& path,std::vector<uint8_t>& pixels,UINT w,UINT h){
    for(size_t i=3;i<pixels.size();i+=4)pixels[i]=255;
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(w,h));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;Hr(frame->SetPixelFormat(&format));Check(format==GUID_WICPixelFormat32bppBGRA,"Unexpected PNG format");
    Hr(frame->WritePixels(h,w*4,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
struct Options{fs::path out,features,timeline;float still=-1,seconds=30,dpi=1,lyricSize=48,start=0;UINT w=1920,h=1080;int scenario=0,style=6,variant=-1,fx=0,camera=0,lines=3,background=0,filter=0;bool checks=false,software=false,geometry=false,live=false,scope=false,performance=false,refinement=false,particles=false,balance=false,halo=false,ink=false;};
Options Parse(int argc,wchar_t** argv){Options o;for(int i=1;i<argc;++i){const std::wstring a=argv[i];
    if(a==L"--ink-check"){o.checks=o.ink=true;continue;}
    if(a==L"--halo-check"){o.checks=o.halo=true;continue;}
    if(a==L"--balance-check"){o.checks=o.balance=true;continue;}
    if(a==L"--refinement-check"){o.checks=o.refinement=true;continue;}
    if(a==L"--particle-check"){o.checks=o.particles=true;continue;}
    if(a==L"--performance-check"){o.checks=o.performance=true;continue;}
    if(a==L"--scope-check"){o.checks=o.scope=true;}else if(a==L"--timeline"&&i+1<argc){o.timeline=argv[++i];}else if(a==L"--start"&&i+1<argc){o.start=std::stof(argv[++i]);}else if(a==L"--live-check"){o.checks=o.live=true;}else if(a==L"--geometry-check"){o.checks=o.geometry=true;}else if(a==L"--out"&&i+1<argc)o.out=argv[++i];else if(a==L"--features"&&i+1<argc)o.features=argv[++i];
    else if(a==L"--still"&&i+1<argc)o.still=std::stof(argv[++i]);else if(a==L"--seconds"&&i+1<argc)o.seconds=std::stof(argv[++i]);
    else if(a==L"--width"&&i+1<argc)o.w=std::stoul(argv[++i]);else if(a==L"--height"&&i+1<argc)o.h=std::stoul(argv[++i]);
    else if(a==L"--dpi"&&i+1<argc)o.dpi=std::stof(argv[++i]);else if(a==L"--scenario"&&i+1<argc)o.scenario=std::stoi(argv[++i]);
    else if(a==L"--style"&&i+1<argc)o.style=std::stoi(argv[++i]);else if(a==L"--variant"&&i+1<argc)o.variant=std::stoi(argv[++i]);
    else if(a==L"--fx"&&i+1<argc)o.fx=std::stoi(argv[++i]);else if(a==L"--camera"&&i+1<argc)o.camera=std::stoi(argv[++i]);else if(a==L"--lines"&&i+1<argc)o.lines=std::stoi(argv[++i]);else if(a==L"--background"&&i+1<argc)o.background=std::stoi(argv[++i]);
    else if(a==L"--lyric-size"&&i+1<argc)o.lyricSize=std::clamp(std::stof(argv[++i]),26.f,80.f);
    else if(a==L"--filter"&&i+1<argc)o.filter=std::stoi(argv[++i]);
    else if(a==L"--check")o.checks=true;else if(a==L"--software")o.software=true;else throw std::runtime_error("Unknown native-preview option");}
    Check(o.w>=320&&o.w<=3840&&o.h>=240&&o.h<=2160&&std::isfinite(o.seconds)&&o.seconds>0&&o.seconds<=30&&std::isfinite(o.still)&&o.still<=30&&std::isfinite(o.start)&&o.start>=0&&o.start<=86400&&std::isfinite(o.lyricSize)&&std::isfinite(o.dpi)&&o.dpi>=.5f&&o.dpi<=3,"Invalid preview geometry/time");Check(o.style>=0&&o.style<BuiltInStyleCount&&!IsRemovedBuiltInStyle(o.style)&&o.variant>=-1&&o.variant<256,"Invalid native motion selection");Check(o.fx>=0&&o.fx<6&&o.camera>=0&&o.camera<3&&o.lines>=1&&o.lines<=10&&o.background>=0&&o.background<=4&&o.filter>=0&&o.filter<=4,"Invalid motion options");return o;}
void Scenarios(SceneInput& s,int scenario,int frame){
    if(scenario==1)s.view.reducedMotion=true;
    if(scenario==2){s.view.desktop=true;s.view.fullscreen=false;}
    if(scenario==3){s.view.backstage=true;s.view.panelVisible=false;}
    if(scenario==4){s.playback.state=PlayState::Paused;s.playback.positionMs=7900;}
    if(scenario==5){s.view.manualLyricBrowse=true;s.view.previewIndex=0;s.view.previewScrollOffset=static_cast<float>(frame/30)*300.f;}
    if(scenario==6){s.audio.pid=1;s.audio.sampledAtMs-=5000;s.audio.observedAtMs-=5000;}
    if(scenario==7&&frame>=100){s.playback.positionMs=17000+(frame-100)*1000/60;s.lyricRevision=s.lyricReadingRevision=2;s.playback.songId=L"100002";}
    if(scenario==12){s.playback.state=PlayState::Unknown;s.playback.nativeClockAdvanceAllowed=false;const auto pos=s.playback.positionMs;s.playback.positionMs=pos/100*100;s.playback.observedAtMs-=pos-s.playback.positionMs;s.playback.sampledAtMs=s.playback.observedAtMs;}
    if(scenario==8){s.view.showRomanization=true;s.view.lyricDisplayMode=2;}
}
uint64_t Pixels(ID2D1DeviceContext* dc,ID2D1Bitmap1* source){
    const auto size=source->GetPixelSize();ComPtr<ID2D1Bitmap1> copy;
    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,source->GetPixelFormat());
    Hr(dc->CreateBitmap(size,nullptr,0,&props,&copy));Hr(copy->CopyFromBitmap(nullptr,source,nullptr));
    D2D1_MAPPED_RECT mapped{};Hr(copy->Map(D2D1_MAP_OPTIONS_READ,&mapped));uint64_t hash=1469598103934665603ull;
    for(UINT y=0;y<size.height;++y)for(UINT x=0;x<size.width*4;++x){hash^=mapped.bits[size_t(y)*mapped.pitch+x];hash*=1099511628211ull;}
    Hr(copy->Unmap());return hash;
}
std::vector<uint8_t> BitmapPixels(ID2D1DeviceContext* dc,ID2D1Bitmap1* source){
    const auto size=source->GetPixelSize();ComPtr<ID2D1Bitmap1> copy;
    const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,source->GetPixelFormat());
    Hr(dc->CreateBitmap(size,nullptr,0,&props,&copy));Hr(copy->CopyFromBitmap(nullptr,source,nullptr));
    D2D1_MAPPED_RECT m{};Hr(copy->Map(D2D1_MAP_OPTIONS_READ,&m));std::vector<uint8_t> pixels(size_t(size.width)*size.height*4);
    for(UINT y=0;y<size.height;++y)std::copy_n(m.bits+size_t(y)*m.pitch,size.width*4,pixels.data()+size_t(y)*size.width*4);
    Hr(copy->Unmap());return pixels;
}
size_t ChangedPixels(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){
    Check(a.size()==b.size(),"Pixel comparison dimensions differ");size_t count=0;
    for(size_t i=0;i<a.size();i+=4){int delta=0;for(int c=0;c<4;++c)delta+=std::abs(int(a[i+c])-int(b[i+c]));if(delta>12)++count;}return count;
}
void Checks(bool geometry,bool live,bool scope,bool performance,bool refinement,bool particles,bool balance,bool checkHalo,bool checkInk){
    int passed=0;auto expect=[&](bool okay,const char* why){Check(okay,why);++passed;};
    folia::CameraMotion flight;folia::CameraInput ci;ci.width=1280;ci.height=720;ci.target=ci.entry={0,0};ci.reset=true;flight.Step(ci);
    ci.reset=false;ci.block=1;ci.target=ci.entry={1280,720};ci.dt=1.f/60;ci.time=.016;flight.Step(ci);
    expect(std::hypot(flight.View().x,flight.View().y)<1,"Folia retarget teleported instead of retaining the current pose");
    for(int i=1;i<=120;++i){ci.time=.016+i/60.;flight.Step(ci);}
    expect(flight.View().x>1200&&std::isfinite(flight.View().scale),"Folia camera failed to fly towards the new lyric block");
    const auto held=flight.View();ci.playing=false;ci.target={9000,9000};ci.time+=1;flight.Step(ci);
    expect(flight.View().x==held.x&&flight.View().y==held.y&&flight.View().scale==held.scale,"Paused Folia camera kept moving");
    ci.reset=true;flight.Step(ci);expect(flight.View().x==9000&&flight.View().y==9000,"Seek did not reset the Folia camera directly");
    SceneRenderer unopened;expect(!unopened.SetOffscreenTimeMs(1000),"Live/uncreated renderer accepted development clock");
    expect(BuiltInStyleCount==34&&CustomThemeChoice==34,"Theme choices shifted incorrectly");
    for(int i:AvailableBuiltInStyles){const auto t=BuiltInTheme(i);expect(t.baseStyle==i,"Built-in style ID changed");}
    expect(AvailableBuiltInStyles.size()==28,"Removed themes remain in the catalog");
    for(int id:{23,27,30,31,32,33}){expect(IsRemovedBuiltInStyle(id)&&!IsMineradioStyle(id),"Removed theme remains selectable");
        const auto parsed=ParseThemeJson(std::string("{\"schema\":1,\"baseStyle\":")+std::to_string(id)+"}");
        expect(parsed.ok&&parsed.definition.baseStyle==21&&BuiltInTheme(id).baseStyle==21,"Legacy removed theme did not fall back");}
    for(int i=0;i<28;++i)expect(ThemeChoiceIndex(ThemeIdAtChoice(i))==i,"Native combo index changed a surviving theme ID");
    expect(ThemeIdAtChoice(28)==CustomThemeChoice&&ThemeChoiceIndex(CustomThemeChoice)==28,"Custom theme choice broke after removal");
    expect(ParseThemeJson(R"({"schema":1,"baseStyle":6,"name":"Rift"})").ok,"New style JSON rejected");
    expect(!ParseThemeJson(R"({"schema":1,"baseStyle":34})").ok,"Custom choice persisted as style ID");
    const auto parsedVariant=ParseThemeJson(R"({"schema":1,"baseStyle":8,"motionVariant":120})");expect(parsedVariant.ok&&parsedVariant.definition.motionVariant==120,"Native subpreset selection was lost");
    for(const auto pair:{std::pair{7,100},std::pair{8,121},std::pair{9,100},std::pair{19,13}}){const auto json=std::string("{\"schema\":1,\"baseStyle\":")+std::to_string(pair.first)+",\"motionVariant\":"+std::to_string(pair.second)+"}";expect(!ParseThemeJson(json).ok,"Out-of-range per-theme subpreset was accepted");}
    for(const auto field:{"motionFx","motionCamera","motionLines","motionBackground","motionFilter"})expect(!ParseThemeJson(std::string("{\"schema\":1,\"baseStyle\":21,\"")+field+"\":999}").ok,"Out-of-range motion option was accepted");
    GUID fixtureId{};Hr(CoCreateGuid(&fixtureId));wchar_t fixtureName[40]{};Check(StringFromGUID2(fixtureId,fixtureName,40)>0,"Fixture ID failed");
    const auto fixture=fs::current_path()/L"build"/L"rift-theme-checks"/fixtureName;fs::create_directories(fixture);
    const auto themeFile=fixture/L"theme6.json";
    expect(SaveThemeNewFile(themeFile,BuiltInTheme(6)).ok,"Theme export failed");const auto loaded=LoadTheme(themeFile);
    expect(loaded.ok&&loaded.definition.baseStyle==6&&loaded.definition.name==L"裂光现场","Theme export/import lost the new style");
    expect(!SaveThemeNewFile(themeFile,BuiltInTheme(0)).ok,"Immutable theme fixture was overwritten");
    auto optionTheme=BuiltInTheme(19);optionTheme.motionVariant=12;optionTheme.motionFx=5;optionTheme.motionCamera=2;optionTheme.motionLines=10;optionTheme.motionBackground=2;optionTheme.motionFilter=4;
    expect(SaveThemeNewFile(fixture/L"options.json",optionTheme).ok,"Motion option theme export failed");const auto options=LoadTheme(fixture/L"options.json");
    expect(options.ok&&options.definition.motionVariant==12&&options.definition.motionFx==5&&options.definition.motionCamera==2&&options.definition.motionLines==10&&options.definition.motionBackground==2&&options.definition.motionFilter==4,"Motion options did not survive file roundtrip");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&ctx));
    ComPtr<IDXGIDevice> dxgi;Hr(device.As(&dxgi));ComPtr<ID2D1Factory1> factory;Hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));
    ComPtr<ID2D1Device> d2d;ComPtr<ID2D1DeviceContext> dc;Hr(factory->CreateDevice(dxgi.Get(),&d2d));Hr(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc));
    ComPtr<IDWriteFactory> write;Hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.GetAddressOf())));
    if(checkInk){
#include "clean-ink-checks.inl"
        return;
    }
    if(checkHalo){
#include "soft-halo-checks.inl"
        return;
    }
    if(balance){
#include "motion-balance-checks.inl"
#include "art-trails-checks.inl"
        return;
    }
    if(particles){
#include "glyph-effects-checks.inl"
        return;
    }
    if(refinement){
#include "cinema-glow-checks.inl"
#include "visual-refinement-checks.inl"
        return;
    }
    if(performance){
#include "lyric-performance-checks.inl"
        return;
    }
    if(scope){
#include "rift-scope-checks.inl"
        return;
    }
    FoliaPrograms programs;expect(programs.Info(FoliaProgramKind::Tempera,0).count==121,"Tempera composition inventory is incomplete");
    expect(programs.Info(FoliaProgramKind::Lumiere,0).count==100,"Lumiere rig inventory is incomplete");
    ComPtr<ID2D1Bitmap1> programTarget;const auto programProps=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    Hr(dc->CreateBitmap({320,180},nullptr,0,&programProps,&programTarget));
    for(const auto kind:{FoliaProgramKind::Tempera,FoliaProgramKind::Lumiere})for(size_t i=0;i<programs.Info(kind,0).count;++i){
        dc->SetTarget(programTarget.Get());dc->BeginDraw();dc->Clear({0,0,0,0});programs.Draw(dc.Get(),kind,i,2.f,4.f,320,180,false,.3f,2);Hr(dc->EndDraw());dc->SetTarget(nullptr);
        expect(programs.Info(kind,i).index==i,"A motion subpreset silently fell back to another preset");}
    RiftStage layer;layer.Create(device.Get(),write.Get());RiftFrame f;f.lyrics=Demo();f.identity=L"synthetic-A";f.revision=1;f.width=960;f.height=540;f.following=f.precise=f.playing=true;
    f.nowMs=100000;f.positionMs=300;layer.Render(f,dc.Get());auto first=layer.Reading();
    ComPtr<IDWriteFontCollection> defaultFonts;Hr(write->GetSystemFontCollection(&defaultFonts));
    expect(!first.embeddedFont&&first.fontFamily==ResolveLyricTypeface(defaultFonts.Get(),LyricTypefaceKind::Serif).family,"Cinema default font was not loaded");
    expect(first.visibleClusters==0,"Future glyphs appeared early");
    f.nowMs+=350;f.positionMs+=350;layer.Render(f,dc.Get());expect(layer.Reading().visibleClusters>0,"First timed glyph did not appear");const auto cameraEarly=layer.Reading();
    f.nowMs+=2000;f.positionMs+=2000;layer.Render(f,dc.Get());expect(layer.Reading().visibleClusters>first.visibleClusters,"Glyph sequence did not advance");
    expect(std::hypot(layer.Reading().cameraX-cameraEarly.cameraX,layer.Reading().cameraY-cameraEarly.cameraY)>1,"Native lyrics stayed in a fixed screen composition");
    const auto beforePause=Pixels(dc.Get(),layer.Render(f,dc.Get()));
    f.playing=false;f.nowMs+=500;const auto paused=Pixels(dc.Get(),layer.Render(f,dc.Get()));const auto pause=layer.Reading();expect(!pause.reset,"Pause was treated as a seek");
    expect(paused==beforePause,"Paused glyph geometry kept animating");
    f.nowMs+=17;f.positionMs=23000;layer.Render(f,dc.Get());expect(layer.Reading().reset&&layer.Reading().line==5,"Seek replayed historical lines");
    const auto sought=Pixels(dc.Get(),layer.Render(f,dc.Get()));RiftStage fresh;fresh.Create(device.Get(),write.Get());
    expect(Pixels(dc.Get(),fresh.Render(f,dc.Get()))==sought,"Seek retained old pixels or pose history");
    f.identity=L"synthetic-B";f.nowMs+=17;f.positionMs=900;layer.Render(f,dc.Get());expect(layer.Reading().reset&&layer.Reading().line==0,"Track identity retained old state");
    auto longTimeline=Demo();auto& line=longTimeline->lines[0];line.text.clear();for(int i=0;i<70;++i)line.text+=L"让文字穿过光";line.words={{500,12000,line.text}};line.durationMs=13000;longTimeline->lines.resize(1);
    f.lyrics=longTimeline;f.revision=2;f.identity=L"long";f.positionMs=800;f.nowMs+=100;layer.Render(f,dc.Get());const auto longFirst=layer.Reading();expect(longFirst.pages>1&&longFirst.page==0,"Long timed span skipped its opening page");
    f.positionMs=11000;f.nowMs+=10200;layer.Render(f,dc.Get());expect(layer.Reading().page>0,"Long line never advanced pages");
    size_t covered=0;f.following=f.playing=false;
    for(size_t page=0;page<longFirst.pages;++page){f.browseOffset=static_cast<float>(page)*f.height*.60f+.1f;f.nowMs+=20;layer.Render(f,dc.Get());covered+=layer.Reading().visibleClusters;}
    expect(covered==line.text.size(),"Long-line paging dropped or repeated CJK clusters");f.browseOffset=0;f.following=true;
    auto mixed=Demo();mixed->lines.resize(1);auto& m=mixed->lines[0];m.text=L"光 e\u0301 motion 世界";m.words={{500,600,L"光"},{1100,600,L"e\u0301"},{1700,700,L"motion"},{2400,800,L"世界"}};m.durationMs=4000;
    f.lyrics=mixed;f.identity=L"mixed";f.revision=3;f.positionMs=2100;f.nowMs+=100;layer.Render(f,dc.Get());expect(layer.Reading().valid&&layer.Reading().wordTimed,"Mixed shaped text lost real spans");
    expect(layer.Reading().visibleClusters==3,"Fallback combining mark detached from its base character");
    m.words.clear();mixed->wordTimed=false;f.revision=4;f.nowMs+=17;layer.Render(f,dc.Get());expect(!layer.Reading().wordTimed&&layer.Reading().visibleClusters>0,"Line-only lyrics fabricated word timing");
    f.following=false;f.reduced=true;f.positionMs=0;f.nowMs+=17;layer.Render(f,dc.Get());expect(layer.Reading().visibleClusters>0,"Static preview hid loaded lyrics");
    auto adjacent=std::make_shared<LyricTimeline>();LyricLine one;one.text=L"光穿过夜";one.startMs=0;one.durationMs=3000;one.words={{0,2900,one.text}};
    adjacent->lines={one,one};adjacent->lines[1].startMs=3000;adjacent->lines[1].words[0].startMs=3000;
    f.lyrics=adjacent;f.identity=L"adjacent";f.revision=5;f.following=f.precise=f.playing=true;f.reduced=false;f.positionMs=2960;f.nowMs+=3000;
    layer.Render(f,dc.Get());const auto composition=layer.Reading().composition;
    f.positionMs+=60;f.nowMs+=60;layer.Render(f,dc.Get());expect(!layer.Reading().reset&&layer.Reading().composition!=composition,"Adjacent equal-content lines repeated their composition");
    auto latin=std::make_shared<LyricTimeline>();one.text.assign(450,L'm');one.words={{0,12000,one.text}};one.durationMs=13000;latin->lines={one};
    f.lyrics=latin;f.identity=L"long-latin";f.revision=6;f.positionMs=100;f.nowMs+=100;layer.Render(f,dc.Get());expect(layer.Reading().pages>1,"Long unbroken Latin token collapsed to an unreadable single page");
    auto timedLatin=std::make_shared<LyricTimeline>();one.text=L"AB";one.startMs=0;one.durationMs=1000;one.words={{0,500,L"A"},{500,500,L"B"}};timedLatin->lines={one};
    f.lyrics=timedLatin;f.identity=L"timed-latin";f.revision=7;f.positionMs=250;f.nowMs+=150;layer.Render(f,dc.Get());const auto earlyLatin=layer.Reading().visibleClusters;
    f.positionMs=750;f.nowMs+=500;layer.Render(f,dc.Get());expect(earlyLatin==1&&layer.Reading().visibleClusters==2,"Latin grouping erased distinct lyric timing spans");
    expect(programs.Info(FoliaProgramKind::Sonnet,0).count==100,"Sonnet geometry inventory incomplete");
    for(size_t variant=0;variant<100;++variant){dc->SetTarget(programTarget.Get());dc->BeginDraw();dc->Clear({0,0,0,0});programs.Draw(dc.Get(),FoliaProgramKind::Sonnet,variant,2.f,4.f,320,180,false,.3f,2);Hr(dc->EndDraw());dc->SetTarget(nullptr);expect(programs.Info(FoliaProgramKind::Sonnet,variant).index==variant,"Sonnet variant fell back");}
    f.lyrics=Demo();f.identity=L"full-motion-matrix";f.revision=20;f.width=480;f.height=270;f.positionMs=7000;f.nowMs=107000;f.following=f.precise=f.playing=true;f.reduced=false;f.quality=2;
    for(int style=FirstFoliaStyle;style<BuiltInStyleCount;++style){if(IsRemovedBuiltInStyle(style))continue;f.positionMs=7000;f.nowMs+=1000;f.kind=MotionKind(style);f.particlePreset=IsMineradioStyle(style)?style-FirstMineradioStyle:0;f.variant=-1;
        layer.Render(f,dc.Get());expect(layer.Reading().reset,"Changing a motion theme did not reset its previous stage");f.positionMs+=800;f.nowMs+=800;const auto bitmap=layer.Render(f,dc.Get());const auto reading=layer.Reading();if(!bitmap||!reading.valid||reading.visibleClusters==0)std::cerr<<"Theme "<<style<<" line "<<reading.line<<" page "<<reading.page<<" position "<<f.positionMs<<" valid "<<reading.valid<<" visible "<<reading.visibleClusters<<"\n";expect(bitmap&&reading.valid&&reading.visibleClusters>0,"A native theme failed to display current lyrics");
        const auto drawn=Pixels(dc.Get(),bitmap);f.playing=false;f.low=.91f;f.mid=.73f;f.high=.66f;f.pulse=1;f.spectrum.fill(.84f);f.nowMs+=500;
        expect(Pixels(dc.Get(),layer.Render(f,dc.Get()))==drawn,"Native theme moved during pause while audio envelopes changed");f.playing=true;f.low=f.mid=f.high=f.pulse=0;f.spectrum.fill(0);}
    f.kind=LyricMotion::Lumiere;f.quality=3;for(int variant=0;variant<100;++variant){f.variant=variant;const auto cb=programs.LightConstants(variant,2,4,480,270,false,.3f,3);bool finite=true;for(const auto& row:cb)for(auto value:row)finite&=std::isfinite(value);
        expect(finite&&layer.Render(f,dc.Get()),"Invalid or failed original light-field GPU rig");}
    f.kind=LyricMotion::Fume;f.variant=-1;f.cover=Cover();
    for(int background=0;background<5;++background)for(int filter=0;filter<(background==2?5:1);++filter){f.motionBackground=background;f.motionFilter=filter;f.positionMs+=17;f.nowMs+=17;auto bitmap=layer.Render(f,dc.Get());expect(bitmap&&layer.Reading().valid,"Native background/filter did not render");const auto before=Pixels(dc.Get(),bitmap);f.playing=false;f.nowMs+=200;expect(Pixels(dc.Get(),layer.Render(f,dc.Get()))==before,"Background/filter continued during pause");f.playing=true;}
    f.motionBackground=0;f.kind=LyricMotion::Mineradio;f.particlePreset=5;
    for(int fx=0;fx<6;++fx){f.motionFx=fx;f.motionLines=fx+1;f.positionMs+=17;f.nowMs+=17;const auto bitmap=layer.Render(f,dc.Get());expect(bitmap&&layer.Reading().valid,"Mineradio lyric surface failed");const auto before=Pixels(dc.Get(),bitmap);f.playing=false;f.nowMs+=200;expect(Pixels(dc.Get(),layer.Render(f,dc.Get()))==before,"Mineradio surface moved during pause");f.playing=true;}
    for(int lines=1;lines<=10;++lines){f.motionLines=lines;f.positionMs+=17;f.nowMs+=17;expect(layer.Render(f,dc.Get())&&layer.Reading().valid,"Mineradio lyric row count failed");}
    f.kind=LyricMotion::Sonnet;f.variant=-1;f.lyrics=std::make_shared<LyricTimeline>();f.revision=21;layer.Render(f,dc.Get());expect(layer.Reading().visibleClusters==0,"Empty lyrics retained the previous scene");
    if(geometry){
        auto stress=std::make_shared<LyricTimeline>();stress->state=LyricLoadState::Ready;stress->wordTimed=true;
        LyricLine text;text.text=L"让交错的文字也能清晰流向远处";text.startMs=0;text.durationMs=6000;text.words={{0,5900,text.text}};stress->lines={text};
        f={};f.lyrics=stress;f.identity=L"measured-crossed-layout";f.revision=100;f.following=f.precise=f.playing=true;f.reduced=true;f.positionMs=2400;f.nowMs=200000;f.quality=3;
        for(const auto size:{std::pair{1920.f,1080.f},std::pair{540.f,960.f},std::pair{1280.f,540.f}}){f.width=size.first;f.height=size.second;f.lyricSize=80;
            for(int style=RiftStyle;style<FirstMineradioStyle;++style){f.kind=MotionKind(style);const int count=style==8?121:style==7||style==9?100:1;
                for(int variant=0;variant<count;++variant){f.variant=count==1?-1:variant;f.nowMs+=20;layer.Render(f,dc.Get());const auto r=layer.Reading();
                    if(r.submittedClusters!=r.fullyVisibleClusters)std::cerr<<"Bounds style="<<style<<" variant="<<variant<<" size="<<f.width<<"x"<<f.height<<" submitted="<<r.submittedClusters<<" full="<<r.fullyVisibleClusters<<" box="<<r.glyphLeft<<","<<r.glyphTop<<","<<r.glyphRight<<","<<r.glyphBottom<<"\n";
                    if(r.layoutOverlaps)std::cerr<<"Overlaps style="<<style<<" variant="<<variant<<" count="<<r.layoutOverlaps<<"\n";expect(r.layoutOverlaps==0,"Settled glyph boxes overlap");
                    expect(r.submittedClusters>0&&r.fullyVisibleClusters==r.submittedClusters,"Settled glyphs exceed the stage safe area");
                }
            }
        }
        // Long songs and line transitions: inspect transformed glyph quads,
        // not just the number of vertices submitted outside the viewport.
        stress->lines.clear();for(int i=0;i<60;++i){auto t=text;t.text=i%2?L"沿着每次心跳让光越过漫长的夜":L"文字抵达下一段光里";t.startMs=i*5000;t.durationMs=5000;t.words.clear();
            for(size_t c=0;c<t.text.size();++c)t.words.push_back({t.startMs+static_cast<int64_t>(c)*220,static_cast<int64_t>(c+1==t.text.size()?1800:220),t.text.substr(c,1)});stress->lines.push_back(std::move(t));}
        f.width=960;f.height=540;f.lyricSize=48;f.variant=-1;f.reduced=false;f.camera=1.35f;f.revision=101;
        for(const auto kind:{LyricMotion::Rift,LyricMotion::Fume}){f.kind=kind;f.identity=kind==LyricMotion::Rift?L"long-song-rift":L"long-song-fume";
            std::vector<std::vector<bool>> seen;for(const auto& t:stress->lines)seen.emplace_back(t.text.size(),false);
            size_t priorLine=SIZE_MAX,priorPage=SIZE_MAX;
            for(int ms=0;ms<300000;ms+=100){f.positionMs=ms;f.nowMs=400000+ms;layer.Render(f,dc.Get());const auto r=layer.Reading();
                if(r.activeFullyVisible&&r.line<seen.size()&&r.activeText<seen[r.line].size())seen[r.line][r.activeText]=true;
                // A wider font may start a new page exactly on this sample.
                // Its first glyph has zero entry opacity at its own timestamp;
                // the retiring page is still drawn but isn't a current glyph.
                const bool pageOnset=r.awaitingWords&&(r.line!=priorLine||r.page!=priorPage);
                if(ms%5000>=800&&!pageOnset){if(!r.visibleClusters)std::cerr<<"Invisible kind="<<static_cast<int>(kind)<<" time="<<ms<<" submitted="<<r.submittedClusters<<" camera="<<r.cameraX<<","<<r.cameraY<<"\n";expect(r.visibleClusters>0,"Continuous song lost all current lyric glyphs");}
                priorLine=r.line;priorPage=r.page;
            }
            for(size_t row=0;row<seen.size();++row)for(size_t word=0;word<seen[row].size();++word){if(!seen[row][word])std::cerr<<"Unreadable kind="<<static_cast<int>(kind)<<" line="<<row<<" word="<<word<<"\n";expect(seen[row][word],"A real timed syllable never became fully visible during its own span");}
        }
        // Pixels must respond to ink changes at an identical playback pose.
        f.kind=LyricMotion::Fume;f.playing=false;f.positionMs=56000;f.nowMs+=1000;f.ink=f.accent={1,1,1,1};const auto bright=Pixels(dc.Get(),layer.Render(f,dc.Get()));
        // Fully appeared syllables use the sung accent, while unsung KTV ink
        // uses f.ink. Exercise both channels instead of only the unused one.
        f.ink=f.accent={0,0,0,1};expect(Pixels(dc.Get(),layer.Render(f,dc.Get()))!=bright,"Visible glyph counter has no corresponding ink pixels");
    }
    if(live){
        // Regression: actual NetEase logs have progressing native samples with
        // Unknown transport state. The old camera/audio path froze this case.
        RiftStage sampled;sampled.Create(device.Get(),write.Get());
        RiftFrame u;u.lyrics=Demo();u.identity=L"sample-only";u.revision=1;u.width=960;u.height=540;
        u.following=u.precise=u.sampled=true;u.playing=false;u.camera=1;u.kind=LyricMotion::Fume;
        for(int i=0;i<840;++i){u.nowMs=100000+i*1000/60;u.positionMs=(i/6)*100;u.sampledAtMs=100000+u.positionMs;sampled.Render(u,dc.Get());}
        expect(sampled.Reading().line==3&&sampled.Reading().visibleClusters>0,"Unknown transport lost moving lyrics after several real sampled lines");
        expect(std::abs(sampled.Reading().cameraX)>100,"Unknown transport froze camera at first line");
        for(int cadence:{33,250,500}){
            RiftStage varied;varied.Create(device.Get(),write.Get());
            u.identity=L"cadence"+std::to_wstring(cadence);u.kind=LyricMotion::Rift;u.playing=false;
            for(int frame=0;frame<240;++frame){const int elapsed=frame*1000/60;u.positionMs=9000+(elapsed/cadence)*cadence;u.nowMs=200000+elapsed;u.sampledAtMs=200000+(elapsed/cadence)*cadence;
                varied.Render(u,dc.Get());const auto state=varied.Reading();expect(state.renderPositionMs<=u.positionMs&&state.renderPositionMs>=u.positionMs-140,"Presentation extrapolated or lagged more than its bounded observed range");
            }
            expect(varied.Reading().advancing,"Ordinary native sample cadence was treated as pause");
        }
        u.identity=L"sample-only";u.kind=LyricMotion::Fume;u.nowMs=214000;u.positionMs=14000;u.sampledAtMs=u.nowMs;sampled.Render(u,dc.Get());
        const auto heldPosition=u.positionMs;
        for(int i=0;i<30;++i){u.nowMs+=17;u.sampledAtMs=u.nowMs;sampled.Render(u,dc.Get());}
        const auto stopped=Pixels(dc.Get(),sampled.Render(u,dc.Get()));u.nowMs+=100;u.sampledAtMs=u.nowMs;u.low=.8f;
        expect(Pixels(dc.Get(),sampled.Render(u,dc.Get()))==stopped,"Stationary native samples did not freeze presentation");
        u.positionMs=900;u.nowMs+=17;u.sampledAtMs=u.nowMs;sampled.Render(u,dc.Get());
        expect(sampled.Reading().reset&&sampled.Reading().line==0,"Sample-only seek retained the previous scene");
        u.identity=L"sample-only-new-track";u.positionMs=heldPosition;u.nowMs+=17;u.sampledAtMs=u.nowMs;sampled.Render(u,dc.Get());expect(sampled.Reading().reset,"Sample-only track change did not reset");
        for(int style=6;style<BuiltInStyleCount;++style){
            if(IsRemovedBuiltInStyle(style))continue;
            RiftStage quiet,loud;quiet.Create(device.Get(),write.Get());loud.Create(device.Get(),write.Get());
            auto t=Demo();t->lines.resize(1);auto& phrase=t->lines[0];phrase.text=L"冲破黑夜向光走";phrase.startMs=0;phrase.durationMs=15000;phrase.words={{0,15000,phrase.text}};
            RiftFrame a;a.lyrics=t;a.identity=L"matrix";a.revision=1;a.following=a.precise=a.sampled=true;a.width=640;a.height=360;a.camera=1.15f;a.response=1.3f;a.cover=Cover();a.kind=MotionKind(style);a.particlePreset=IsMineradioStyle(style)?style-FirstMineradioStyle:0;
            a.background={.04f,.05f,.07f,1};a.ink={.97f,.95f,.91f,1};a.accent={.9f,.45f,.18f,1};
            std::vector<uint8_t> earlier,atQuiet,atLoud;
            for(int frame=0;frame<120;++frame){a.nowMs=200000+frame*1000/60;a.positionMs=3000+(frame/6)*100;a.sampledAtMs=200000+(frame/6)*100;
                auto b=a;b.rms=.12f;b.low=.22f;b.mid=.16f;b.high=.12f;b.pulse=.55f;b.spectrum.fill(.25f);
                auto qa=quiet.Render(a,dc.Get());auto qb=loud.Render(b,dc.Get());
                if(frame==65)earlier=BitmapPixels(dc.Get(),qa);
                if(frame==119){atQuiet=BitmapPixels(dc.Get(),qa);atLoud=BitmapPixels(dc.Get(),qb);}
            }
            const auto moving=ChangedPixels(earlier,atQuiet),reacting=ChangedPixels(atQuiet,atLoud);
            std::cout<<"STYLE "<<style<<" motionPixels="<<moving<<" audioPixels="<<reacting<<" visible="<<quiet.Reading().visibleClusters<<"\n";
            expect(quiet.Reading().valid&&quiet.Reading().visibleClusters>0,"Sample-only theme lost the current lyric");
            if(style!=20){expect(moving>20,"Theme has no sampled-time motion");expect(reacting>20,"Theme has no current audio response");}else expect(reacting==0,"Still theme reacts to audio; KTV colour may follow real word timing");
        }
        // Real screenshot regression: the same opaque glyph pixels must not
        // change to black when a photograph crosses the contrast threshold.
        auto oneGlyph=Demo();oneGlyph->lines.resize(1);oneGlyph->lines[0].text=L"蚀";oneGlyph->lines[0].words={{0,4000,L"蚀"}};oneGlyph->lines[0].startMs=0;oneGlyph->lines[0].durationMs=5000;
        auto photo=Cover();for(UINT y=0;y<photo->height;++y)for(UINT x=0;x<photo->width;++x){const auto n=size_t(y)*photo->stride+x*4;const uint8_t shade=((x/24+y/24)%2)?245:40;photo->bgra[n]=photo->bgra[n+1]=photo->bgra[n+2]=shade;}
        RiftFrame contrast;contrast.identity=L"contrast";contrast.lyrics=oneGlyph;contrast.revision=1;contrast.width=640;contrast.height=360;contrast.positionMs=2000;contrast.nowMs=500000;contrast.following=contrast.precise=true;contrast.reduced=true;contrast.kind=LyricMotion::Classic;contrast.ink=contrast.accent={.96f,.945f,.91f,1};contrast.cover=photo;
        RiftStage mask;mask.Create(device.Get(),write.Get());const auto opaqueMask=BitmapPixels(dc.Get(),mask.Render(contrast,dc.Get()));
        for(int ground=1;ground<=3;++ground){RiftStage photographed;photographed.Create(device.Get(),write.Get());contrast.motionBackground=ground;
            const auto pixels=BitmapPixels(dc.Get(),photographed.Render(contrast,dc.Get()));size_t inkPixels=0,blackIslands=0;
            // Additive soft light can saturate to alpha=1 outside the glyph.
            // Select the actual opaque ink tone on all three channels, rather
            // than including saturated halo pixels in the stroke mask.
            for(size_t n=0;n<pixels.size();n+=4)if(opaqueMask[n+3]>254&&std::abs(int(opaqueMask[n+2])-245)<=2&&
                    std::abs(int(opaqueMask[n+1])-241)<=2&&std::abs(int(opaqueMask[n])-232)<=2){
                ++inkPixels;if(pixels[n+2]<210||pixels[n+1]<200)++blackIslands;}
            std::cout<<"Photographic glyph ground="<<ground<<" ink="<<inkPixels<<" black="<<blackIslands<<"\n";
            expect(inkPixels>100&&blackIslands==0,"Photographic contrast punched black islands inside the glyph");
        }
        // Exercise the full product SceneRenderer (including its sample-only
        // handoff), not just the isolated glyph layer.
        for(int style=0;style<6;++style){
            SceneRenderer quiet,loud;quiet.CreateOffscreen(640,360,1,true);loud.CreateOffscreen(640,360,1,true);
            std::vector<uint8_t> early,qa,qb;UINT w=0,h=0;
            for(int frame=0;frame<95;++frame){const auto now=300000+frame*1000/60;auto a=Scene(Demo(),now,1000+(frame/6)*100,{});
                a.view.theme=style;a.playback.state=PlayState::Unknown;a.playback.nativeClockAdvanceAllowed=false;a.playback.sampledAtMs=a.playback.observedAtMs=300000+(frame/6)*100;a.background.frame=Cover();
                auto b=a;b.audio.rms=.15f;b.audio.peak=.4f;b.audio.spectrum.fill(.25f);
                quiet.SetOffscreenTimeMs(now);loud.SetOffscreenTimeMs(now);quiet.Draw(a,false);loud.Draw(b,false);
                if(frame==50)quiet.CaptureBgra(early,w,h);if(frame==94){quiet.CaptureBgra(qa,w,h);loud.CaptureBgra(qb,w,h);}
            }
            const auto moving=ChangedPixels(early,qa),reacting=ChangedPixels(qa,qb);
            std::cout<<"STYLE "<<style<<" motionPixels="<<moving<<" audioPixels="<<reacting<<" fullScene=1\n";
            expect(moving>20,"Original theme has no sampled-time motion");expect(reacting>20,"Original theme has no audio response");
        }

    }
    std::cout<<"PASS "<<passed<<" synthetic native checks; no window/player/profile access\n";
}
int Run(const Options& o){
    if(o.checks){Checks(o.geometry,o.live,o.scope,o.performance,o.refinement,o.particles,o.balance,o.halo,o.ink);return 0;}
    auto lyrics=Demo();
    if(o.scenario==13){lyrics->lines.clear();const std::array<std::wstring,3> text={L"幸福吧",L"别让文字调换顺序",L"唱到哪里亮到哪里"};
        for(size_t i=0;i<text.size();++i){LyricLine l;l.text=text[i];l.startMs=500+i*3500;l.durationMs=3500;l.translation=L"SYNTHETIC WORD TIMING / NATIVE RENDERER";
            int64_t at=l.startMs;for(size_t n=0;n<l.text.size();++n){const auto duration=i==0?(n==0?1500:n==1?500:1200):3200/static_cast<int64_t>(l.text.size());l.words.push_back({at,duration,l.text.substr(n,1)});at+=duration;}lyrics->lines.push_back(std::move(l));}}
    if(o.scenario==5){auto& line=lyrics->lines[0];line.text.clear();for(int i=0;i<22;++i)line.text+=L"让文字穿过光";line.words={{500,26000,line.text}};line.durationMs=29500;lyrics->lines.resize(1);}
    if(o.scenario==8){auto& line=lyrics->lines[1];line.text=L"光 e\u0301 motion 世界";line.words={{4400,600,L"光"},{5000,600,L"e\u0301"},{5600,700,L"motion"},{6300,1800,L"世界"}};line.romanization=L"guang / motion / shi jie";}
    if(o.scenario==9){for(auto& line:lyrics->lines)line.words.clear();lyrics->wordTimed=false;}
    if(o.scenario==10){lyrics->lines.clear();lyrics->state=LyricLoadState::NoLyrics;}
    if(o.scenario==11){lyrics->lines.resize(1);auto& t=lyrics->lines[0];t.text=L"让交错的文字也能清晰流向远处";t.startMs=500;t.durationMs=29500;t.words={{500,29000,t.text}};t.translation=L"MEASURED LAYOUT / SYNTHETIC TIMING";}
    if(!o.timeline.empty()){
        std::ifstream in(o.timeline);Check(bool(in),"Cannot open replay lyric fixture");nlohmann::json j;in>>j;
        const auto wide=[](const std::string& text){const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);std::wstring w(n,L' ');if(n)MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),w.data(),n);return w;};
        lyrics->lines.clear();lyrics->wordTimed=j.value("wordTimed",false);
        for(const auto& v:j.at("lines")){LyricLine l;l.text=wide(v.at("text"));l.startMs=v.at("startMs");l.durationMs=v.at("durationMs");
            for(const auto& word:v.at("words"))l.words.push_back({word.at("startMs"),word.at("durationMs"),wide(word.at("text"))});lyrics->lines.push_back(std::move(l));}
        Check(!lyrics->lines.empty(),"Replay lyric fixture is empty");
    }
    const auto features=FeaturesFile(o.features);
    SceneRenderer renderer;renderer.CreateOffscreen(o.w,o.h,o.dpi,o.software);
    const int count=o.still>=0?static_cast<int>(std::ceil(o.still*60))+1:static_cast<int>(std::lround(o.seconds*60));
    if(o.still<0){Check(_setmode(_fileno(stdout),_O_BINARY)!=-1,"Cannot set raw video pipe");std::setvbuf(stdout,nullptr,_IOFBF,4*1024*1024);}
    std::vector<uint8_t> pixels;UINT w=0,h=0;std::shared_ptr<BackgroundFrame> cover=Cover();
    for(int i=0;i<count;++i){const int64_t pos=static_cast<int64_t>(o.start*1000)+i*1000/60,now=100000+pos;const auto a=features.empty()?Features{}:features[std::min<size_t>(i,features.size()-1)];
        auto scene=Scene(lyrics,now,pos,a);scene.view.theme=o.style;scene.view.lyricSize=o.lyricSize;scene.view.motionVariant=o.variant;scene.view.motionFx=o.fx;scene.view.motionCamera=o.camera;scene.view.motionLines=o.lines;scene.view.motionBackground=o.background;scene.view.motionFilter=o.filter;scene.playback.title=o.timeline.empty()?L"HTune 动效核对":L"真实歌词数据回放";if(!o.timeline.empty())scene.playback.artist=L"复现用采样时序 · 非实窗录制";scene.playback.durationMs=std::max<int64_t>(30000,pos+10000);if(o.variant>=0){auto theme=std::make_shared<ThemeDefinition>(BuiltInTheme(o.style));theme->motionVariant=o.variant;scene.view.customTheme=theme;}scene.background.frame=cover;Scenarios(scene,o.scenario,i);Check(renderer.SetOffscreenTimeMs(now),"Offscreen clock rejected");
        Check(renderer.Draw(scene,false),"Synthetic renderer did not draw");
        if(o.still>=0&&i+1<count)continue;
        Check(renderer.CaptureBgra(pixels,w,h),"Native capture failed");
        if(o.still>=0){Check(!o.out.empty(),"Still requires --out");Png(o.out,pixels,w,h);std::wcerr<<L"Wrote "<<o.out<<L"\n";}
        else Check(std::fwrite(pixels.data(),1,pixels.size(),stdout)==pixels.size(),"Raw video pipe closed");
    }
    if(o.still<0)Check(std::fflush(stdout)==0,"Raw video pipe flush failed");return 0;
}
}
int wmain(int argc,wchar_t** argv){const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);int code=1;
    try{code=Run(Parse(argc,argv));}catch(const std::exception& e){std::cerr<<e.what()<<"\n";}
    if(SUCCEEDED(com))CoUninitialize();return code;}

