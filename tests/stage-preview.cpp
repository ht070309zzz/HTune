// Offscreen stage previews from synthetic data, for development review only.
// Opens no window, reads no user data, plays or captures no audio and never
// touches NetEase or a running MusicStage. Writes PNG files to --out.
#include "renderer.h"
#include "stage_ui.h"
#include "stage_interface.h"
#include "theme_transition.h"
#include "original_stages.h"
#include "rift_stage.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace stage;
namespace fs = std::filesystem;

void Hr(HRESULT hr,const char* what) {
    if(FAILED(hr))throw std::runtime_error(std::string(what)+" failed: "+std::to_string(static_cast<unsigned long>(hr)));
}

ThemePalette BuiltInPalette(int style) {
    if(style==RiftStyle)return {ThemeRgb(0x08090C),ThemeRgb(0x111217),ThemeRgb(0xF4F2EE),ThemeRgb(0x929298),ThemeRgb(0xFF5A1F),ThemeRgb(0xFF5A1F)};
    if(style==WeaveStyle)return {ThemeRgb(0x101112),ThemeRgb(0x191819),ThemeRgb(0xF6F0E8),ThemeRgb(0x858181),ThemeRgb(0xF7877E),ThemeRgb(0x8F443D)};
    if(style==PosterStyle)return {ThemeRgb(0x073CDD),ThemeRgb(0x101112),ThemeRgb(0xFAFAF6),ThemeRgb(0xCFDDFD),ThemeRgb(0xDDFF00),ThemeRgb(0x1234AB)};
    if(style==EchoStyle)return {ThemeRgb(0xFCFDFC),ThemeRgb(0xEFF3F0),ThemeRgb(0x153E30),ThemeRgb(0x8DA699),ThemeRgb(0x28664D),ThemeRgb(0xCBDCD1)};
    if(style==0)return {ThemeRgb(0x111B24),ThemeRgb(0x17232E),ThemeRgb(0xF0F4F3),ThemeRgb(0x9CAFB8),ThemeRgb(0x90D4C7),ThemeRgb(0x457C8C)};
    if(style==1)return {ThemeRgb(0x151411),ThemeRgb(0x201E19),ThemeRgb(0xF4ECDF),ThemeRgb(0xB3A68D),ThemeRgb(0xDAB374),ThemeRgb(0x755B35)};
    return {ThemeRgb(0xF7F2E8),ThemeRgb(0xECE5D8),ThemeRgb(0x352719),ThemeRgb(0x8E7864),ThemeRgb(0xBA432B),ThemeRgb(0xE1C7A8)};
}

// A procedural cover with a gradient, a bright disc and dark bands: enough
// structure for the particle field, the palette and the thumbnail.
std::shared_ptr<BackgroundFrame> SyntheticCover(int variant) {
    struct Rgb { float r,g,b; };
    static const Rgb from[3]={{.95f,.45f,.25f},{.18f,.52f,.95f},{.86f,.22f,.56f}};
    static const Rgb to[3]={{.16f,.10f,.36f},{.04f,.10f,.18f},{.98f,.84f,.36f}};
    const int v=std::clamp(variant,0,2);
    auto frame=std::make_shared<BackgroundFrame>();
    constexpr uint32_t size=512;
    frame->width=frame->height=size;frame->stride=size*4;frame->sequence=static_cast<uint64_t>(v)+1;
    frame->bgra.resize(size_t(size)*size*4);
    for(uint32_t y=0;y<size;++y)for(uint32_t x=0;x<size;++x){
        const float u=x/float(size),w=y/float(size);
        const float t=std::clamp((u+w)*.5f,0.f,1.f);
        float r=from[v].r+(to[v].r-from[v].r)*t,g=from[v].g+(to[v].g-from[v].g)*t,b=from[v].b+(to[v].b-from[v].b)*t;
        const float dx=u-.62f,dy=w-.36f,d=std::sqrt(dx*dx+dy*dy);
        const float disc=std::clamp((.19f-d)/.06f,0.f,1.f);
        r+=(1.f-r)*disc*.85f;g+=(.96f-g)*disc*.85f;b+=(.88f-b)*disc*.85f;
        if(w>.66f&&(static_cast<int>(y)/18)%3==0){r*=.55f;g*=.55f;b*=.55f;}
        if(variant==3){
            // Original synthetic concrete planes, not a downloaded photograph.
            const float seam=.28f+.25f*w;
            float gray=u<seam?.16f:u<seam+.13f?.68f:.91f;
            if(w>.54f-u*.32f&&w<.73f-u*.32f)gray*=.17f;
            const float noise=std::sin(x*12.9898f+y*78.233f)*43758.5453f;
            gray+=(noise-std::floor(noise)-.5f)*.045f;
            if(x%64==0||y%128==0)gray*=.85f;
            r=g=b=gray;
        }
        uint8_t* p=&frame->bgra[(size_t(y)*size+x)*4];
        p[0]=static_cast<uint8_t>(std::clamp(b,0.f,1.f)*255.f+.5f);
        p[1]=static_cast<uint8_t>(std::clamp(g,0.f,1.f)*255.f+.5f);
        p[2]=static_cast<uint8_t>(std::clamp(r,0.f,1.f)*255.f+.5f);
        p[3]=255;
    }
    return frame;
}

LyricLine Line(int64_t start,std::initializer_list<std::pair<const wchar_t*,int>> words,const wchar_t* translation=L"") {
    LyricLine line;line.startMs=start;int64_t at=start;
    for(const auto& [text,ms]:words){line.words.push_back({at,ms,text});line.text+=text;at+=ms;}
    line.durationMs=at-start;line.translation=translation;
    return line;
}

std::shared_ptr<const LyricTimeline> SyntheticLyrics() {
    auto timeline=std::make_shared<LyricTimeline>();
    timeline->songId=L"100001";timeline->state=LyricLoadState::Ready;timeline->wordTimed=true;
    auto& lines=timeline->lines;
    lines.push_back(Line(1000,{{L"不想",400},{L"离开",500},{L" ",80},{L"也许",400},{L"尝试",450},{L"过",300},{L"被爱",900}}));
    lines.push_back(Line(4500,{{L"Falling ",450},{L"into ",350},{L"the ",250},{L"light ",500},{L"again",1100}},L"再一次坠入光里"));
    lines.push_back(Line(7600,{{L"君と",500},{L"笑う",600},{L"ため",450},{L"なんだよ",1200}},L"只是为了和你一起笑"));
    lines.push_back(Line(10800,{{L"是因为",600},{L" 那些旁白",800},{L" 那些姿态",800},{L" 那些伤害",800},{L" 都让我",600},{L"更加确定",900}}));
    LyricLine gap;gap.startMs=15800;gap.durationMs=2000;lines.push_back(gap);
    lines.push_back(Line(18000,{{L"会开始",600},{L"仰望",500},{L"未来",1200}}));
    lines.push_back(Line(21000,{{L"伤疤",500},{L" 就丢给",600},{L"回忆吧",1000}}));
    lines.push_back(Line(23500,{{L"放下",500},{L" 才得到",600},{L"更好啊",1100}}));
    return timeline;
}

PlaybackSnapshot Playback(int64_t now,int64_t position) {
    PlaybackSnapshot p;
    p.pid=4242;p.processCreatedAtTicks=1;p.title=L"好想爱这个世界啊 (Preview)";p.artist=L"合成数据";p.album=L"MusicStage";
    p.songId=L"100001";p.songIdVerified=true;p.catalogMetadataReady=true;
    p.state=PlayState::Playing;p.reliability=Reliability::NativeRead;p.nativeClockAdvanceAllowed=true;
    p.positionMs=std::max<int64_t>(0,position);p.durationMs=262000;p.sampledAtMs=now;p.observedAtMs=now;
    return p;
}

// 120 bpm kick on the low bands, gentle motion elsewhere.
AudioFeatures Audio(int64_t now,int64_t position) {
    AudioFeatures a;
    a.pid=4242;a.processCreatedAtTicks=1;a.active=true;a.timingTrusted=true;a.sampledAtMs=now;a.observedAtMs=now;
    const double phase=std::fmod(std::max<int64_t>(0,position)/1000.0,.5)/.5;
    const float kick=static_cast<float>(std::exp(-phase*7.0));
    for(size_t i=0;i<a.spectrum.size();++i){
        const double t=static_cast<double>(position);
        float value=i<6?.45f+.40f*kick:i<20?.38f+.12f*static_cast<float>(std::sin(t*.004+i)):.25f+.10f*static_cast<float>(std::sin(t*.009+i*1.7));
        a.spectrum[i]=std::clamp(value,0.f,1.f);
    }
    a.rms=.18f+.12f*kick;a.peak=.5f+.4f*kick;
    return a;
}

struct Shot {
    std::wstring name;
    int style=1;
    UINT width=1920,height=1080;
    float scale=1.f;
    int64_t positionMs=1650;
    bool panel=false;
    int mode=0;
    int quality=0;
    float lyricSize=60.f,response=1.f;
    int cover=0;
    int64_t warmupMs=2000;
    // Backstage opens this long before the capture (0: stays closed), so a
    // short lead captures the camera mid-flight.
    int64_t backstageLeadMs=0;
    int64_t backstageCloseLeadMs=0,backstageReopenLeadMs=0;
    int previousStyle=-1,intermediateStyle=-1;
    int64_t styleChangeLeadMs=0,intermediateLeadMs=0;
    bool glass=false,reducedMotion=false;
    bool gallery=false; // stage UI widget gallery over the stage
    bool console=false; // the bottom console, shown for the last moments
    int page=-1;        // Backstage page shown when it opens (0..3)
    bool pointer=false,originalLong=false,originalEmpty=false,songChange=false;
    bool windowChrome=false;
};

std::shared_ptr<const LyricTimeline> OriginalLyrics(const Shot& shot) {
    auto result=std::make_shared<LyricTimeline>();result->songId=L"100001";result->state=LyricLoadState::Ready;result->wordTimed=true;
    if(shot.originalEmpty){result->state=LyricLoadState::NoLyrics;return result;}
    if(shot.originalLong){
        result->lines.push_back(Line(82000,{{L"When the evening opens into another quiet landscape, every little sound leaves a mark along the path we have travelled together, and the words keep moving gently beyond the edge of the page.",12000}},L"晚风吹过城市，沿途的声音留下温柔的痕迹"));
        result->lines.push_back(Line(96000,{{L"让很长很长的下一句歌词，也能在新的舞台里找到自己的位置",8000}}));return result;
    }
    result->lines.push_back(Line(1000,{{L"风穿过林间",15000}}));
    result->lines.push_back(Line(26000,{{L"又回到你身旁",15000}}));
    result->lines.push_back(Line(52000,{{L"像一场温柔的回声",15000}}));
    if(shot.style==WeaveStyle){
        result->lines.push_back(Line(82000,{{L"把",600},{L"声音",2200},{L"织成晚风",5000}}));
        result->lines.push_back(Line(91000,{{L"让此刻缓缓展开",9000}}));
    }else if(shot.style==PosterStyle){
        result->lines.back().text=L"我们穿过喧闹";result->lines.back().words.clear();
        result->lines.push_back(Line(82000,{{L"让城市",1800},{L"跟着心跳",6500}}));
        result->lines.push_back(Line(91000,{{L"把每个平凡的夜晚都点亮",9000}}));
    }else{
        result->lines.push_back(Line(82000,{{L"声音走过的地方",9000}}));
        result->lines.push_back(Line(92000,{{L"留下一条属于今天的路",9000}}));
    }
    return result;
}

void SavePng(const fs::path& file,std::vector<uint8_t> bgra,UINT width,UINT height) {
    for(size_t i=3;i<bgra.size();i+=4)bgra[i]=255; // the target ignores alpha
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream),"WIC stream");
    Hr(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE),"WIC file");
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");
    Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG encoder init");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;
    Hr(encoder->CreateNewFrame(&frame,&properties),"PNG frame");Hr(frame->Initialize(properties.Get()),"PNG frame init");
    Hr(frame->SetSize(width,height),"PNG size");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;Hr(frame->SetPixelFormat(&format),"PNG format");
    if(format!=GUID_WICPixelFormat32bppBGRA)throw std::runtime_error("PNG encoder changed the pixel format");
    Hr(frame->WritePixels(height,width*4,static_cast<UINT>(bgra.size()),bgra.data()),"PNG pixels");
    Hr(frame->Commit(),"PNG frame commit");Hr(encoder->Commit(),"PNG commit");
}

SceneInput Scene(const Shot& shot,int64_t now,int64_t position,const std::shared_ptr<const LyricTimeline>& lyrics,
    const std::shared_ptr<const BackgroundFrame>& cover) {
    SceneInput scene;
    scene.playback=Playback(now,position);scene.audio=Audio(now,position);scene.lyrics=lyrics;
    scene.background.sourceRevision=1;scene.background.kind=BackgroundKind::Image;scene.background.frame=cover;
    scene.lyricRevision=1;scene.lyricReadingRevision=1;scene.lyricAssociation=LyricAssociation::VerifiedCurrent;
    scene.frameAtMs=now;scene.palette=BuiltInPalette(shot.style);
    scene.view.theme=shot.style;scene.view.lyricSize=shot.lyricSize;scene.view.response=shot.response;
    scene.view.panelVisible=shot.panel;scene.view.coverBackground=true;scene.view.showTranslation=true;
    scene.view.lyricDisplayMode=shot.mode;scene.view.qualityMode=shot.quality;scene.view.frameRateMode=0;
    scene.view.glass=shot.glass;scene.view.reducedMotion=shot.reducedMotion;
    if(IsOriginalStyle(shot.style)){
        scene.playback.title=shot.style==WeaveStyle?L"晚风":shot.style==PosterStyle?L"城市节拍":L"回声";
        scene.playback.artist=L"MUSICSTAGE";scene.view.showTranslation=shot.originalLong;
        scene.view.pointerActive=shot.pointer;scene.view.pointerX=shot.width/shot.scale*.88f;scene.view.pointerY=shot.height/shot.scale*.5f;
    }
    return scene;
}

// Synthetic settings snapshot for interface previews (no app state).
SettingsView SyntheticView(int style) {
    SettingsView v;
    v.title=L"好想爱这个世界啊 (Preview)";v.artist=L"合成数据";v.hasCover=true;v.audioLevel=.62f;
    v.coverStatus=L"已显示当前歌曲封面";v.backgroundNotice=L"当前使用歌曲封面";
    v.response=1.2f;v.lyricSize=60.f;v.backgroundShade=.2f;v.cameraMotion=1.f;
    v.themeStyle=style;v.canSearch=v.canFollow=v.canImport=v.canFetch=v.hasTrack=true;
    v.candidates={{L"1407551413",L"好想爱这个世界啊",L"华晨宇",L"我和我的家乡",262000,false},
        {L"1500183223",L"好想爱这个世界啊 (Live)",L"华晨宇",L"天天向上",271000,false},
        {L"2049512697",L"好想爱这个世界啊",L"翻唱",L"",0,true}};
    v.candidateGeneration=7;v.selectedCandidate=0;
    v.candidateDetails=L"歌名：好想爱这个世界啊 · 歌手：华晨宇 · 专辑：我和我的家乡 · 时长：4:22 · 歌曲 ID：1407551413";
    v.offsetMs=120;v.showTranslation=true;
    v.lyricMemoryButton=L"恢复歌词记忆保存";v.lyricMemoryNote=L"偏移和导入关联随歌曲保存在本机。记忆文件异常时，可在此恢复保存。";
    v.diagnostics=L"网易云 3.1.8 · 播放 · 原生只读\r\n歌词 8 行 · 已核验 · 逐字\r\n音频 · 当前进程 48 kHz · 已连接";
    v.settingsButton=L"恢复设置保存";v.settingsNote=L"设置随调整自动保存在本机。原文件异常时，可在此恢复设置保存。";
    v.usageHelp=L"使用说明与快捷键\r\n\r\n网易云负责播放，MusicStage 自动跟随。\r\n\r\nCtrl+, 打开或关闭 Backstage\r\nF11 全屏 · Esc 退出全屏\r\nCtrl+L 切换歌词呈现\r\n舞台滚轮浏览歌词，Home 返回当前句";
    return v;
}

// Toolkit smoke test: every widget kind on a glass panel, with scripted
// pointer and keyboard input (hover, a focused text field, a selection).
void Gallery(ui::Context& u,int64_t elapsed,int64_t span) {
    using namespace ui;
    const auto& f=u.Frame();const auto& t=f.theme;
    const Rect panel=Rect::Xywh(f.width-560.f,40.f,520.f,f.height-80.f);
    u.Draw().Glass(panel,22.f,t.glassTint,t.glassRim,1.f,static_cast<float>(elapsed%6000)/6000.f,WithAlpha(t.accent,.7f));
    u.Block(panel);
    float y=panel.top+28.f;const float x=panel.left+28.f,w=panel.Width()-56.f;
    u.Label(Rect::Xywh(x,y,w,14.f),L"VISUAL",Font::Overline,t.accent);y+=18.f;
    u.Label(Rect::Xywh(x,y,w,44.f),L"画面",Font::Title,t.ink);y+=56.f;
    static bool autoHide=true,reduced=false;
    u.Switch(Context::Id("g.switch1"),Rect::Xywh(x,y,w,36.f),L"全屏空闲时隐藏辅助信息",autoHide);y+=44.f;
    u.Switch(Context::Id("g.switch2"),Rect::Xywh(x,y,w,36.f),L"减少动态",reduced);y+=52.f;
    static float strength=1.2f;
    u.Label(Rect::Xywh(x,y,w*.6f,20.f),L"音频响应",Font::Body,t.muted);
    u.Label(Rect::Xywh(x+w*.6f,y-6.f,w*.4f,32.f),std::to_wstring(static_cast<int>(std::lround(strength*100)))+L"%",Font::Value,t.ink,Align::Trailing);y+=30.f;
    u.Slider(Context::Id("g.slider"),Rect::Xywh(x,y,w,28.f),strength,0.f,2.f,.01f);y+=44.f;
    static int quality=0;
    u.Segmented(Context::Id("g.segmented"),Rect::Xywh(x,y,w,36.f),{L"自动",L"高",L"中",L"低"},quality);y+=52.f;
    static std::wstring query;
    const Rect box=Rect::Xywh(x,y,w,38.f);
    static bool clicked=false,typed=false;
    if(elapsed<span/2){clicked=typed=false;query.clear();}
    if(!clicked&&elapsed>span-1500){clicked=true;u.MouseDown(box.left+40.f,box.Center().y,false);u.MouseUp(box.left+40.f,box.Center().y);}
    if(clicked&&!typed&&elapsed>span-1200){typed=true;for(const wchar_t ch:std::wstring(L"好想爱这个世界啊"))u.Char(ch);}
    u.TextBox(Context::Id("g.text"),box,query,L"歌名、歌曲 ID 或网易云链接",512);y+=50.f;
    const float half=(w-10.f)*.5f;
    u.Button(Context::Id("g.primary"),Rect::Xywh(x,y,half,38.f),L"查找 / 预览歌词",ButtonKind::Primary);
    u.Button(Context::Id("g.secondary"),Rect::Xywh(x+half+10.f,y,half,38.f),L"按当前歌曲重新匹配");y+=54.f;
    static int selected=1;
    const std::vector<ListRow> rows={{L"好想爱这个世界啊 — 华晨宇",L"我和我的家乡 · 4:22 · ID 1407551413"},
        {L"好想爱这个世界啊 (Live) — 华晨宇",L"天天向上 · 4:31 · ID 1500183223"},
        {L"好想爱这个世界啊 — 翻唱",L"最近播放记录 · 需确认 · ID 2049512697",true}};
    u.List(Context::Id("g.list"),Rect::Xywh(x,y,w,170.f),rows,selected);y+=184.f;
    u.TextBlock(Context::Id("g.block"),Rect::Xywh(x,y,w,std::max(60.f,panel.bottom-24.f-y)),
        L"网易云 3.1.8 · 播放中 · 原生只读\n歌词 42 行 · 已核验\n音频 · 当前进程 48 kHz",Font::Small,t.muted);
    // Console mock along the bottom of the stage.
    const Rect bar=Rect::Xywh(40.f,f.height-104.f,760.f,72.f);
    u.Draw().Glass(bar,24.f,t.glassTint,t.glassRim,1.f,static_cast<float>((elapsed+3000)%6000)/6000.f,WithAlpha(t.accent,.7f));
    u.Block(bar);
    u.Draw().Cover(Rect::Xywh(bar.left+12.f,bar.top+12.f,48.f,48.f),10.f);
    u.Label(Rect::Xywh(bar.left+72.f,bar.top+14.f,190.f,22.f),L"好想爱这个世界啊",Font::Strong,t.ink);
    u.Label(Rect::Xywh(bar.left+72.f,bar.top+36.f,190.f,20.f),L"华晨宇",Font::Small,t.muted);
    static int style=1;
    u.Segmented(Context::Id("g.style"),Rect::Xywh(bar.left+272.f,bar.top+18.f,228.f,36.f),{L"流光",L"剧场",L"排版"},style);
    const wchar_t glyphs[]={0xE8C1,0xE740,0xE7F4,0xE713};
    const wchar_t* tips[]={L"翻译",L"全屏",L"融入桌面",L"设置"};
    for(int i=0;i<4;++i){
        const Rect button=Rect::Xywh(bar.left+516.f+static_cast<float>(i)*58.f,bar.top+14.f,44.f,44.f);
        if(i==3&&elapsed>span-900)u.MouseMove(button.Center().x,button.Center().y);
        u.IconButton(Context::Id("g.icon",static_cast<uint32_t>(i)),button,glyphs[i],tips[i],i==0);
    }
}

// Runs the renderer in real time so springs, fades and the cover cross-fade
// settle; the playback clock is arranged to reach positionMs at the capture.
SceneRenderer& Render(SceneRenderer& renderer,const Shot& shot,bool software,int64_t extraMs=0) {
    renderer.CreateOffscreen(shot.width,shot.height,shot.scale,software);
    std::unique_ptr<ui::Context> stageUi;
    if(shot.gallery||shot.console||shot.page>=0||shot.windowChrome)stageUi=std::make_unique<ui::Context>();
    StageInterface stageInterface;InterfaceState interfaceState;
    ThemeTransition paletteTransition;
    auto view=SyntheticView(shot.style);
    const auto lyrics=IsOriginalStyle(shot.style)?OriginalLyrics(shot):SyntheticLyrics();
    const std::shared_ptr<const BackgroundFrame> cover=SyntheticCover(shot.cover);
    const int64_t span=shot.warmupMs+extraMs;
    if(shot.style==EchoStyle){
        // Recorded synthetic feature sequence for a visual history preview.
        // This does not read or capture audio; only the scene inputs advance.
        for(int i=0;i<164;++i){
            const auto now=MonotonicMs();auto history=Scene(shot,now,i*500,lyrics,cover);
            const float envelope=.10f+.25f*std::pow(.5f+.5f*std::sin(i*.13f),3.f)
                +.43f*std::pow(.5f+.5f*std::sin(i*.39f+.4f),16.f);
            history.audio.rms=envelope*.30f;history.audio.spectrum.fill(envelope);
            renderer.Draw(history,false,nullptr);std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto motionStarted=MonotonicMs();
    for(;;){
        const auto now=MonotonicMs(),elapsed=now-motionStarted;
        const int64_t position=shot.positionMs-std::max<int64_t>(0,span-elapsed);
        int style=shot.style;
        if(shot.previousStyle>=0&&shot.styleChangeLeadMs>0&&elapsed<span-shot.styleChangeLeadMs){
            style=shot.previousStyle;
            if(shot.intermediateStyle>=0&&shot.intermediateLeadMs>0&&elapsed>=span-shot.intermediateLeadMs)style=shot.intermediateStyle;
        }
        const auto palette=paletteTransition.Update(BuiltInPalette(style),now,shot.reducedMotion);
        auto scene=Scene(shot,now,position,lyrics,cover);
        scene.view.theme=style;scene.palette=palette;view.themeStyle=style;
        if(shot.songChange&&elapsed>=span-350){scene.playback.songId=L"100002";scene.playback.title=L"下一段声音";}
        scene.view.backstage=shot.backstageLeadMs>0&&elapsed>=span-shot.backstageLeadMs;
        if(shot.backstageCloseLeadMs>0&&elapsed>=span-shot.backstageCloseLeadMs)scene.view.backstage=false;
        if(shot.backstageReopenLeadMs>0&&elapsed>=span-shot.backstageReopenLeadMs)scene.view.backstage=true;
        if(stageUi){
            ui::FrameInfo info;info.width=shot.width/shot.scale;info.height=shot.height/shot.scale;info.nowMs=now;
            info.reducedMotion=shot.reducedMotion;
            info.theme.accent={.91f,.74f,.47f,1};info.theme.focus=info.theme.accent;
            stageUi->Begin(info);
            if(shot.gallery)Gallery(*stageUi,elapsed,span);
            else{
                InterfaceFrame frame;frame.width=info.width;frame.height=info.height;frame.nowMs=now;
                frame.windowChrome=shot.windowChrome;
                frame.style=style;frame.lightStage=.2126f*palette.background.r+.7152f*palette.background.g+.0722f*palette.background.b>.45f;
                frame.reducedMotion=shot.reducedMotion;
                frame.pointerActive=shot.pointer;frame.pointerX=scene.view.pointerX;frame.pointerY=scene.view.pointerY;
                frame.ink={palette.ink.r,palette.ink.g,palette.ink.b,1};frame.muted={palette.muted.r,palette.muted.g,palette.muted.b,1};
                frame.accent={palette.accent.r,palette.accent.g,palette.accent.b,1};
                frame.console=shot.console&&elapsed>=span-1400;
                interfaceState.backstage=scene.view.backstage;
                if(shot.page>=0)interfaceState.page=shot.page;
                std::vector<UiAction> actions;
                stageInterface.Build(*stageUi,view,interfaceState,frame,actions);
                scene.view.consoleShown=stageInterface.ConsoleShown();
            }
            stageUi->End();
            scene.ui=std::make_shared<ui::DrawList>(stageUi->Output());
        }
        renderer.Draw(scene,false,nullptr);
        if(elapsed>=span)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(14));
    }
    return renderer;
}

std::vector<Shot> Shots() {
    std::vector<Shot> shots;
    const wchar_t* originalNames[3]={L"weave",L"poster",L"echo"};
    // Synthetic integration fixtures for the shared camera. Each uses the
    // real renderer, palette transition and UI, without opening any window.
    for(int i=0;i<3;++i){
        const std::wstring name=std::wstring(L"motion-")+originalNames[i];
        Shot s;s.style=WeaveStyle+i;s.lyricSize=44;s.positionMs=84000;s.cover=i==1?3:i;
        s.page=1;s.windowChrome=true;
        s.name=name+L"-open-90";s.backstageLeadMs=90;shots.push_back(s);
        s.name=name+L"-open-200";s.backstageLeadMs=200;shots.push_back(s);
        s.name=name+L"-settled";s.backstageLeadMs=1400;shots.push_back(s);
        s.name=name+L"-close-180";s.backstageCloseLeadMs=180;shots.push_back(s);
        s.name=name+L"-reverse-80";s.backstageCloseLeadMs=500;s.backstageReopenLeadMs=80;shots.push_back(s);
        s.backstageCloseLeadMs=s.backstageReopenLeadMs=0;
        s.name=name+L"-minimum";s.width=680;s.height=440;shots.push_back(s);
        s.name=name+L"-dpi150";s.width=2560;s.height=1440;s.scale=1.5f;shots.push_back(s);
        s.width=1920;s.height=1080;s.scale=1;
        s.name=name+L"-reduced-mid";s.reducedMotion=true;s.backstageLeadMs=160;shots.push_back(s);
        s.name=name+L"-reduced-settled";s.backstageLeadMs=1400;shots.push_back(s);s.reducedMotion=false;
        s.name=name+L"-rapid-theme";s.previousStyle=i;s.intermediateStyle=WeaveStyle+(i+1)%3;
        s.intermediateLeadMs=400;s.styleChangeLeadMs=180;shots.push_back(s);
    }
    for(int i=0;i<3;++i){
        const std::wstring name=std::wstring(L"original-")+originalNames[i];
        Shot s;s.style=WeaveStyle+i;s.lyricSize=44;s.positionMs=84000;s.console=true;s.cover=i==1?3:i;
        s.name=name+L"-reference";shots.push_back(s);
        s.name=name+L"-pointer";s.pointer=true;shots.push_back(s);s.pointer=false;
        s.name=name+L"-backstage";s.page=1;s.backstageLeadMs=1400;shots.push_back(s);s.page=-1;s.backstageLeadMs=0;
        s.name=name+L"-compact";s.width=900;s.height=600;shots.push_back(s);
        s.name=name+L"-1440p-150";s.width=2560;s.height=1440;s.scale=1.5f;shots.push_back(s);
        s.width=1920;s.height=1080;s.scale=1;
        s.name=name+L"-long";s.originalLong=true;shots.push_back(s);s.originalLong=false;
        s.name=name+L"-empty";s.originalEmpty=true;shots.push_back(s);s.originalEmpty=false;
        s.name=name+L"-reduced";s.reducedMotion=true;shots.push_back(s);s.reducedMotion=false;
        s.name=name+L"-change";s.songChange=true;shots.push_back(s);
    }
    const wchar_t* styleNames[3]={L"flow",L"cinema",L"paper"};
    for(int style=0;style<3;++style){
        const std::wstring base=styleNames[style];
        Shot s;s.style=style;s.cover=style;
        s.name=base+L"-1080p-word";s.positionMs=1650;shots.push_back(s);
        s.name=base+L"-1080p-english";s.positionMs=5420;shots.push_back(s);
        s.name=base+L"-1080p-long";s.positionMs=12600;shots.push_back(s);
        s.name=base+L"-1080p-interlude";s.positionMs=16600;shots.push_back(s);
        Shot panel=s;panel.name=base+L"-1600x1080-125-panel";panel.width=2000;panel.height=1350;panel.scale=1.25f;panel.panel=true;panel.positionMs=8400;shots.push_back(panel);
        Shot wide=s;wide.name=base+L"-1440p-150";wide.width=2560;wide.height=1440;wide.scale=1.5f;wide.positionMs=18900;shots.push_back(wide);
        Shot compact=s;compact.name=base+L"-compact";compact.width=900;compact.height=600;compact.positionMs=21700;shots.push_back(compact);
        Shot backstage=s;backstage.name=base+L"-backstage";backstage.positionMs=8400;backstage.backstageLeadMs=1400;shots.push_back(backstage);
    }
    // Backstage camera in flight, other sizes, reduced motion and the glass
    // path (which must look exactly like the plain stage).
    Shot b;b.style=1;b.cover=1;b.positionMs=8400;
    b.name=L"cinema-backstage-early";b.backstageLeadMs=90;shots.push_back(b);
    b.name=L"cinema-backstage-mid";b.backstageLeadMs=200;shots.push_back(b);
    b.name=L"cinema-backstage-1440p-150";b.width=2560;b.height=1440;b.scale=1.5f;b.backstageLeadMs=1400;shots.push_back(b);
    b.name=L"cinema-backstage-1600x1200";b.width=1600;b.height=1200;b.scale=1.f;shots.push_back(b);
    b.name=L"cinema-backstage-reduced-mid";b.width=1920;b.height=1080;b.reducedMotion=true;b.backstageLeadMs=160;shots.push_back(b);
    Shot glass;glass.style=1;glass.cover=1;glass.positionMs=1650;glass.glass=true;glass.name=L"cinema-glass-identity";shots.push_back(glass);
    Shot gallery;gallery.style=1;gallery.cover=1;gallery.positionMs=8400;gallery.gallery=true;gallery.warmupMs=2600;
    gallery.name=L"ui-gallery-cinema";shots.push_back(gallery);
    gallery.style=0;gallery.cover=0;gallery.name=L"ui-gallery-flow";shots.push_back(gallery);
    gallery.style=2;gallery.cover=2;gallery.name=L"ui-gallery-paper";shots.push_back(gallery);
    gallery.style=1;gallery.cover=1;gallery.width=2560;gallery.height=1440;gallery.scale=1.5f;gallery.name=L"ui-gallery-1440p-150";shots.push_back(gallery);
    const wchar_t* consoleNames[3]={L"flow",L"cinema",L"paper"};
    for(int style:{1,2}){
        Shot c;c.style=style;c.cover=style;c.positionMs=8400;c.console=true;c.windowChrome=true;
        c.width=1600;c.height=1000;c.scale=1.25f;c.name=style==1?L"borderless-cinema":L"borderless-paper";shots.push_back(c);
        if(style==1){c.page=0;c.backstageLeadMs=1600;c.name=L"borderless-backstage";shots.push_back(c);}
    }
    for(int style=0;style<3;++style){
        Shot c;c.style=style;c.cover=style;c.positionMs=8400;c.console=true;c.name=std::wstring(L"ui-console-")+consoleNames[style];shots.push_back(c);
    }
    const wchar_t* pageNames[4]={L"picture",L"theme",L"lyrics",L"status"};
    for(int page=0;page<4;++page){
        Shot pageShot;pageShot.style=page==3?2:page==1?0:1;pageShot.cover=pageShot.style;pageShot.positionMs=8400;pageShot.page=page;pageShot.backstageLeadMs=1600;
        pageShot.name=std::wstring(L"ui-backstage-")+pageNames[page];shots.push_back(pageShot);
    }
    Shot compactPage;compactPage.style=1;compactPage.cover=1;compactPage.positionMs=8400;compactPage.page=2;compactPage.backstageLeadMs=1600;
    compactPage.width=1100;compactPage.height=700;compactPage.name=L"ui-backstage-lyrics-1100x700";shots.push_back(compactPage);
    Shot console1440=compactPage;console1440.page=-1;console1440.backstageLeadMs=0;console1440.console=true;console1440.width=2560;console1440.height=1440;console1440.scale=1.5f;
    console1440.name=L"ui-console-1440p-150";shots.push_back(console1440);
    Shot flowCompact;flowCompact.style=0;flowCompact.cover=0;flowCompact.width=1100;flowCompact.height=700;flowCompact.positionMs=8400;
    flowCompact.backstageLeadMs=1400;flowCompact.name=L"flow-backstage-1100x700";shots.push_back(flowCompact);
    Shot rift;rift.style=RiftStyle;rift.cover=3;rift.positionMs=8400;rift.console=true;rift.windowChrome=true;rift.name=L"rift-window-console";shots.push_back(rift);
    rift.console=false;rift.page=1;rift.backstageLeadMs=1600;rift.name=L"rift-backstage-themes";shots.push_back(rift);
    rift.width=680;rift.height=480;rift.name=L"rift-backstage-minimum";shots.push_back(rift);
    return shots;
}

struct Options { fs::path out; bool software=false,bench=false; std::wstring only; };

Options Parse(int argc,wchar_t** argv) {
    Options options;
    for(int i=1;i<argc;++i){
        const std::wstring arg=argv[i];
        if(arg==L"--out"&&i+1<argc)options.out=argv[++i];
        else if(arg==L"--software")options.software=true;
        else if(arg==L"--bench")options.bench=true;
        else if(arg==L"--only"&&i+1<argc)options.only=argv[++i];
        else throw std::runtime_error("Usage: MusicStagePreview --out <dir> [--software] [--bench] [--only <name-part>]");
    }
    if(options.out.empty())throw std::runtime_error("--out is required");
    return options;
}

int Run(int argc,wchar_t** argv) {
    const auto options=Parse(argc,argv);
    fs::create_directories(options.out);
    if(options.bench){
        // GPU cost of the cinema stage at 1080p for each fixed quality mode.
        const wchar_t* names[4]={L"auto",L"high",L"medium",L"low"};
        for(int quality=1;quality<=3;++quality){
            Shot shot;shot.name=std::wstring(L"bench-")+names[quality];shot.style=1;shot.quality=quality;shot.positionMs=12600;shot.warmupMs=4000;
            SceneRenderer renderer;Render(renderer,shot,options.software);
            std::wcout<<shot.name<<L": gpu "<<renderer.GpuFrameMs()<<L" ms, tier "<<renderer.QualityTier()<<L"\n";
        }
        return 0;
    }
    for(const auto& shot:Shots()){
        if(!options.only.empty()&&shot.name.find(options.only)==std::wstring::npos)continue;
        SceneRenderer renderer;Render(renderer,shot,options.software);
        std::vector<uint8_t> pixels;UINT width=0,height=0;
        if(!renderer.CaptureBgra(pixels,width,height))throw std::runtime_error("Capture failed");
        SavePng(options.out/(shot.name+L".png"),std::move(pixels),width,height);
        std::wcout<<shot.name<<L".png  gpu "<<renderer.GpuFrameMs()<<L" ms, tier "<<renderer.QualityTier()<<L"\n";
    }
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    int result=1;
    try{result=Run(argc,argv);}
    catch(const std::exception& error){std::cerr<<error.what()<<"\n";result=1;}
    if(SUCCEEDED(com))CoUninitialize();
    return result;
}
