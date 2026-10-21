#include "stage_interface.h"
#include "window_chrome.h"
#include "original_stages.h"
#include "rift_stage.h"
#include "optional_lyric_font.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace stage {
namespace {
using namespace ui;
float Luma(Color c) {return .2126f*c.r+.7152f*c.g+.0722f*c.b;}
Color Luminous(Color c,float minimum) {for(int i=0;i<8&&Luma(c)<minimum;++i)c=Mix(c,{1,1,1,c.a},.2f);return c;}
Color Deep(Color c,float maximum) {for(int i=0;i<8&&Luma(c)>maximum;++i)c=Mix(c,{0,0,0,c.a},.2f);return c;}
float Smooth(float t) {t=std::clamp(t,0.f,1.f);return t*t*(3.f-2.f*t);}
std::wstring Percent(float value) {return std::to_wstring(static_cast<int>(std::lround(value*100.f)))+L"%";}
void Push(std::vector<UiAction>& actions,UiCommand command,int value=0,float number=0.f,std::wstring text={},uint64_t generation=0) {
    UiAction action;action.command=command;action.value=value;action.number=number;action.text=std::move(text);action.generation=generation;
    actions.push_back(std::move(action));
}
// Segoe Fluent Icons / Segoe MDL2 Assets code points.
constexpr wchar_t IconSettings=0xE713,IconFullscreen=0xE740,IconWindowed=0xE73F,IconDesktop=0xE7F4,IconCharacters=0xE8C1,
    IconBack=0xE72B,IconHome=0xE80F,IconCheck=0xE8FB;

struct Row {float height=0;std::function<void(Rect)> draw;};
// A card of rows with a quiet heading. Backstage cards sit over the dark
// room, so they are unfrosted: the room's light shows through the tint.
float Section(Context& u,float x,float y,float width,std::wstring_view title,const std::vector<Row>& rows) {
    const auto& t=u.Frame().theme;
    constexpr float pad=20.f,gap=14.f;
    const float head=title.empty()?0.f:30.f;
    float height=pad*2.f+head;
    for(size_t i=0;i<rows.size();++i)height+=rows[i].height+(i?gap:0.f);
    const Rect card=Rect::Xywh(x,y,width,height);
    u.Draw().Glass(card,18.f,t.glassTint,t.glassRim,1.f,-1.f,{},false);
    if(!title.empty())u.Label(Rect::Xywh(x+pad,y+pad-3.f,width-pad*2.f,20.f),title,Font::Strong,t.muted);
    float rowY=y+pad+head;
    for(size_t i=0;i<rows.size();++i){
        if(i)rowY+=gap;
        rows[i].draw(Rect::Xywh(x+pad,rowY,width-pad*2.f,rows[i].height));
        rowY+=rows[i].height;
    }
    return y+height+18.f;
}
Row NoteRow(Context& u,std::wstring text,float width,bool warning=false) {
    const float height=std::max(16.f,u.TextHeight(text,Font::Small,width));
    return {height,[&u,text=std::move(text),warning](Rect r){
        const auto& t=u.Frame().theme;
        u.Label(r,text,Font::Small,warning?t.danger:t.muted,Align::Leading,true);
    }};
}
Row SwitchRow(Context& u,std::string_view key,std::wstring label,std::wstring note,bool value,float width,
    std::function<void(bool)> changed,uint32_t flags=FlagNone) {
    const float noteHeight=note.empty()?0.f:u.TextHeight(note,Font::Small,width-60.f)+2.f;
    return {36.f+noteHeight,[&u,id=Context::Id(key),label=std::move(label),note=std::move(note),value,noteHeight,changed=std::move(changed),flags](Rect r){
        const auto& t=u.Frame().theme;
        bool v=value;
        if(u.Switch(id,Rect{r.left,r.top,r.right,r.top+36.f},label,v,flags))changed(v);
        if(noteHeight>0.f)u.Label(Rect{r.left,r.top+34.f,r.right-60.f,r.bottom},note,Font::Small,t.muted,Align::Leading,true);
    }};
}
Row SliderRow(Context& u,std::string_view key,std::wstring label,std::wstring valueText,float value,float minimum,float maximum,float step,
    std::function<void(float)> changed,uint32_t flags=FlagNone) {
    return {56.f,[&u,id=Context::Id(key),label=std::move(label),valueText=std::move(valueText),value,minimum,maximum,step,changed=std::move(changed),flags](Rect r){
        const auto& t=u.Frame().theme;
        u.Label(Rect{r.left,r.top,r.right-110.f,r.top+22.f},label,Font::Body,t.muted);
        u.Label(Rect{r.right-110.f,r.top-2.f,r.right,r.top+24.f},valueText,Font::Strong,t.ink,Align::Trailing);
        float v=value;
        if(u.Slider(id,Rect{r.left-8.f,r.top+26.f,r.right+8.f,r.top+54.f},v,minimum,maximum,step,flags))changed(v);
    }};
}
Row SegmentedRow(Context& u,std::string_view key,std::wstring label,std::vector<std::wstring> options,int index,
    std::function<void(int)> changed,uint32_t flags=FlagNone) {
    const float head=label.empty()?0.f:26.f;
    return {head+36.f,[&u,id=Context::Id(key),label=std::move(label),options=std::move(options),index,head,changed=std::move(changed),flags](Rect r){
        const auto& t=u.Frame().theme;
        if(head>0.f)u.Label(Rect{r.left,r.top,r.right,r.top+22.f},label,Font::Body,t.muted);
        int i=index;
        if(u.Segmented(id,Rect{r.left,r.top+head,r.right,r.top+head+36.f},options,i,flags))changed(i);
    }};
}
struct ButtonSpec {std::string key;std::wstring text;ButtonKind kind=ButtonKind::Secondary;bool enabled=true;std::function<void()> clicked;};
Row ButtonRow(Context& u,std::vector<ButtonSpec> buttons) {
    return {38.f,[&u,buttons=std::move(buttons)](Rect r){
        if(buttons.empty())return;
        const float gap=10.f,width=(r.Width()-gap*static_cast<float>(buttons.size()-1))/static_cast<float>(buttons.size());
        for(size_t i=0;i<buttons.size();++i){
            const auto& b=buttons[i];
            const Rect cell=Rect::Xywh(r.left+static_cast<float>(i)*(width+gap),r.top,width,38.f);
            if(u.Button(Context::Id(b.key),cell,b.text,b.kind,b.enabled?FlagNone:FlagDisabled)&&b.clicked)b.clicked();
        }
    }};
}

struct StyleInfo {const wchar_t* name;const wchar_t* sample;Color background,glow,accent,ink;Font font;};
const auto Styles=[](){std::array<StyleInfo,BuiltInStyleCount> result={{
    {L"轻光流动",L"不想离开",{.067f,.106f,.141f,1},{.271f,.486f,.549f,1},{.565f,.831f,.780f,1},{.94f,.96f,.95f,1},Font::Value},
    {L"电影剧场",L"坠入光里",{.082f,.078f,.067f,1},{.459f,.357f,.208f,1},{.855f,.702f,.455f,1},{.957f,.925f,.875f,1},Font::Title},
    {L"声色排版",L"君と笑う",{.969f,.949f,.910f,1},{.882f,.780f,.659f,1},{.729f,.263f,.169f,1},{.208f,.153f,.098f,1},Font::Title},
    {L"声织",L"织成晚风",{.063f,.067f,.071f,1},{.56f,.27f,.24f,1},{.97f,.53f,.49f,1},{.96f,.94f,.91f,1},Font::Value},
    {L"节拍拼贴",L"跟着心跳",{.027f,.235f,.867f,1},{.027f,.235f,.867f,1},{.867f,1,0,1},{1,1,.97f,1},Font::Value},
    {L"回声长卷",L"留下回声",{.988f,.992f,.988f,1},{.79f,.86f,.82f,1},{.16f,.4f,.30f,1},{.08f,.24f,.19f,1},Font::Title},
    {L"裂光现场",L"让黑夜裂开",{.031f,.035f,.047f,1},{.24f,.08f,.035f,1},{1,.353f,.122f,1},{.957f,.949f,.933f,1},Font::Value}}};
    const auto rgb=[](unsigned c){return Color{((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,1};};
    for(size_t i=0;i<FoliaStyles.size();++i){const auto& v=FoliaStyles[i];result[FirstFoliaStyle+i]={v.name,v.sample,rgb(v.background),rgb(v.accent),rgb(v.accent),rgb(v.ink),Font::Value};}for(size_t i=0;i<MineradioNames.size();++i)if(MineradioNames[i])result[FirstMineradioStyle+i]={MineradioNames[i],L"音乐流经整个空间",rgb(0x070910),rgb(0x89cde8),rgb(0xa993e4),rgb(0xf0f5fc),Font::Value};return result;}();

// One selectable style card: the style's own light and type on its ground.
bool StyleCard(Context& u,uint32_t id,Rect r,const StyleInfo& style,bool selected,std::wstring_view tag,float seconds,int variant=0) {
    const auto& t=u.Frame().theme;
    auto st=u.Interact(id,r);
    const bool clicked=st.clicked||u.Activate(id);
    const float radius=16.f;
    u.Draw().Fill(r,radius,style.background);
    u.Draw().PushClip(r);
    const float drift=std::sin(seconds*.35f+r.left*.01f);
    if(variant==RiftStyle){
        const float slit=r.left+r.Width()*(.38f+.04f*drift);
        u.Draw().Line({slit,r.top},{slit+r.Width()*.18f,r.bottom},WithAlpha(style.accent,.9f),2.f);
        for(int i=0;i<4;++i)u.Draw().Line({slit-16.f-i*8.f,r.top+r.Height()*.23f+i*5.f},
            {r.right,r.top+r.Height()*.43f+i*5.f},WithAlpha(style.accent,.09f*(4-i)),1.f);
    }else if(variant==WeaveStyle){
        for(int i=0;i<36;++i){
            const float x=(i+.5f)/36.f;Point previous{};
            for(int j=0;j<=20;++j){
                const float y=j/20.f,open=.27f*std::exp(-std::pow(std::abs((y-.50f)/.23f),6.f));
                Point p{r.left+r.Width()*(.5f+(x<.5f?-1.f:1.f)*(open+std::abs(x-.5f)*(1-1.9f*open))),r.top+r.Height()*y};
                if(j)u.Draw().Line(previous,p,WithAlpha(style.accent,.55f),.65f);previous=p;
            }
        }
    }else if(variant==PosterStyle){
        u.Draw().Fill(Rect{r.left+r.Width()*.62f,r.top,r.right,r.bottom},0,{.97f,.98f,.95f,1});
        u.Draw().Fill(Rect::Xywh(r.left+r.Width()*.74f,r.top+r.Height()*.43f,r.Width()*.30f,r.Height()*.27f),0,style.accent);
        u.Draw().Fill(Rect{r.right-14.f,r.top,r.right,r.bottom},0,{.05f,.06f,.07f,1});
    }else if(variant==EchoStyle){
        for(int i=0;i<14;++i){Point previous{};for(int j=0;j<=28;++j){
            const float x=j/28.f;Point p{r.left+x*r.Width(),r.top+r.Height()*(.46f+.10f*std::sin(x*12.f)+i*.012f*(1-x))};
            if(j)u.Draw().Line(previous,p,WithAlpha(style.accent,.5f),.65f);previous=p;
        }}
    }else{
    u.Draw().Light({r.left+r.Width()*(.68f+.08f*drift),r.top+r.Height()*.38f},r.Height()*.95f,style.glow,.75f);
    u.Draw().Light({r.left+r.Width()*(.22f-.05f*drift),r.top+r.Height()*.86f},r.Height()*.55f,style.accent,.28f);
    for(int i=0;i<16;++i){
        const float a=std::fmod(i*0.618034f,1.f),b=std::fmod(i*0.414214f+.13f,1.f);
        const float twinkle=.5f+.5f*std::sin(seconds*(1.1f+a)+static_cast<float>(i));
        const float size=1.2f+1.4f*b;
        u.Draw().Fill(Rect::Xywh(r.left+r.Width()*a,r.top+r.Height()*(.12f+.6f*b),size,size),size*.5f,WithAlpha(style.accent,.25f+.45f*twinkle));
    }
    }
    u.Draw().PopClip();
    u.Label(Rect{r.left+14.f,r.top+10.f,r.right-14.f,r.top+30.f},style.name,Font::Small,WithAlpha(style.ink,.75f));
    u.Label(Rect{r.left+14.f,r.bottom-50.f,r.right-10.f,r.bottom-12.f},style.sample,style.font,style.ink);
    const float lit=u.Animate(id,5,selected?1.f:0.f,10.f);
    if(st.hover>.01f)u.Draw().Fill(r,radius,WithAlpha({1,1,1,1},.05f*st.hover));
    u.Draw().Stroke(r.Inset(.5f,.5f),radius,WithAlpha(Mix(t.glassRim,t.accent,lit),1.f+lit*6.f),1.f+lit);
    if(lit>.01f){
        // The selected style is lit like a stage: a light above, a tag below.
        u.Draw().Light({r.Center().x,r.top-6.f},r.Width()*.55f,t.accent,.35f*lit);
        if(!tag.empty()){
            const float w=u.TextWidth(tag,Font::Caption)+20.f;
            const Rect pill=Rect::Xywh(r.right-w-10.f,r.top+10.f,w,20.f);
            u.Draw().Fill(pill,10.f,WithAlpha(t.accent,lit));
            u.Label(pill,tag,Font::Caption,WithAlpha(t.onAccent,lit),Align::Center);
        }
    }
    u.FocusRing(r,radius,st);
    return clicked;
}
}

ui::Theme StageInterface::ConsoleTheme(const InterfaceFrame& f) {
    Theme t;
    if(f.lightStage){
        t.ink=f.ink;t.muted=f.muted;t.faint=WithAlpha(f.muted,.75f);
        t.accent=Deep(f.accent,.30f);t.onAccent={1,1,1,1};t.focus=t.accent;
        t.glassTint={1,1,1,.52f};t.glassRim={0,0,0,.09f};t.field={0,0,0,.05f};t.danger={.75f,.22f,.16f,1};t.light=true;
    }else{
        t.ink={.97f,.96f,.94f,1};t.muted={.80f,.78f,.75f,1};t.faint={.60f,.59f,.57f,1};
        t.accent=Luminous(f.accent,.42f);t.onAccent=Luma(t.accent)>.5f?Color{.08f,.07f,.06f,1}:Color{1,1,1,1};t.focus=t.accent;
        t.glassTint={.05f,.05f,.06f,.48f};t.glassRim={1,1,1,.12f};t.field={1,1,1,.07f};
    }
    return t;
}
ui::Theme StageInterface::BackstageTheme(const InterfaceFrame& f) {
    Theme t;
    t.ink={.96f,.95f,.93f,1};t.muted={.70f,.69f,.67f,1};t.faint={.50f,.49f,.48f,1};
    t.accent=Luminous(f.lightStage?Mix(f.accent,{1,1,1,1},.15f):f.accent,.45f);
    t.onAccent=Luma(t.accent)>.5f?Color{.08f,.07f,.06f,1}:Color{1,1,1,1};t.focus=t.accent;
    t.glassTint={1,1,1,.045f};t.glassRim={1,1,1,.09f};t.field={1,1,1,.06f};
    return t;
}

void StageInterface::Build(Context& u,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions) {
    originalVisible_=IsOriginalStyle(frame.style)&&!frame.desktop;
    if(frame.desktop){consolePresence_=backstagePresence_=0.f;return;}
    if(IsOriginalStyle(frame.style)){
        consolePresence_=0;consoleTarget_=false;
        Backstage(u,view,state,frame,actions);
        OriginalControls(u,view,state,frame,actions);
        Toast(u,view,frame);WindowChrome(u,frame,actions);return;
    }
    Pills(u,view,frame,actions);
    Console(u,view,state,frame,actions);
    Backstage(u,view,state,frame,actions);
    Toast(u,view,frame);
    WindowChrome(u,frame,actions);
}

void StageInterface::WindowChrome(Context& u,const InterfaceFrame& frame,std::vector<UiAction>& actions) {
    if(!frame.windowChrome)return;
    const auto t=backstagePresence_>.01f?BackstageTheme(frame):ConsoleTheme(frame);u.SetTheme(t);
    const auto dpi=u.Frame().window?GetDpiForWindow(u.Frame().window):96u;
    const auto safeDpi=dpi?dpi:96u;const float scale=safeDpi/96.f;
    const int pixels=static_cast<int>(std::lround(frame.width*scale));
    const wchar_t* hints[]={L"最小化",frame.maximized?L"还原窗口":L"最大化",L"关闭"};
    const UiCommand commands[]={UiCommand::MinimizeWindow,UiCommand::MaximizeWindow,UiCommand::CloseWindow};
    for(int i=0;i<3;++i){
        const auto native=WindowChromeButtonRect(pixels,safeDpi,i);
        const Rect r{native.left/scale,native.top/scale,native.right/scale,native.bottom/scale};
        const auto id=Context::Id("window.chrome",i);const auto st=u.Interact(id,r);
        const float lit=std::max(st.hover,st.focus);
        u.Draw().Fill(r,7.f,i==2?Color{.78f,.12f,.16f,.90f*lit}:WithAlpha(t.ink,.10f*lit+.06f*st.press));
        const Color ink=i==2?Mix(WithAlpha(t.ink,frame.windowActive?.68f:.42f),Color{1,1,1,1},lit):
            WithAlpha(t.ink,(frame.windowActive?.68f:.42f)+.30f*lit);
        const auto c=r.Center();const float stroke=1.2f;
        if(i==0)u.Draw().Line({c.x-5.f,c.y+3.f},{c.x+5.f,c.y+3.f},ink,stroke);
        else if(i==1){
            if(frame.maximized){
                u.Draw().Line({c.x-2.f,c.y-5.f},{c.x+5.f,c.y-5.f},ink,stroke);
                u.Draw().Line({c.x+5.f,c.y-5.f},{c.x+5.f,c.y+2.f},ink,stroke);
                u.Draw().Stroke(Rect::Xywh(c.x-5.f,c.y-2.f,7.f,7.f),.5f,ink,stroke);
            }else u.Draw().Stroke(Rect::Xywh(c.x-4.5f,c.y-4.5f,9.f,9.f),.5f,ink,stroke);
        }else{
            u.Draw().Line({c.x-4.f,c.y-4.f},{c.x+4.f,c.y+4.f},ink,stroke);
            u.Draw().Line({c.x+4.f,c.y-4.f},{c.x-4.f,c.y+4.f},ink,stroke);
        }
        u.FocusRing(r,7.f,st);u.Tooltip(hints[i]);
        if(st.clicked||u.Activate(id))Push(actions,commands[i]);
    }
}

void StageInterface::OriginalControls(Context& u,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions) {
    const float visibility=1.f-Smooth(backstagePresence_);
    if(visibility<.01f)return;
    // These are full-stage controls, not controls inside the projected card.
    // Keep their fade, but give Backstage sole input ownership until closed.
    const uint32_t flags=state.backstage||backstagePresence_>.01f?FlagDisabled:FlagNone;
    const auto g=ComposeOriginal(frame.style,frame.width,frame.height);
    auto t=ConsoleTheme(frame);u.SetTheme(t);u.PushOpacity(visibility);
    const float x=g.controlsX,w=frame.style==EchoStyle?78.f:std::max(28.f,g.width-g.controlsX-8.f);
    const float y=frame.style==EchoStyle?std::max(96.f,frame.height*.15f):std::max(frame.windowChrome?48.f:38.f,frame.height*.07f);
    const wchar_t* names[3]={L"舞台",L"字幕",L"外观"};const int pages[3]={0,2,1};
    for(int i=0;i<3;++i){
        const auto r=Rect::Xywh(x,y+i*44.f,w,32.f);const uint32_t id=Context::Id("original.edge",i);
        const auto st=u.Interact(id,r,flags);
        const float hover=u.Animate(id,5,st.hovered||st.focused?1.f:0.f,12.f);
        u.Label(r,names[i],g.compact||w<48.f?Font::Body:Font::StageNav,Mix(t.muted,t.ink,hover),Align::Leading);
        if(hover>.01f)u.Draw().Fill(Rect::Xywh(x-10.f,r.top+8.f,2.f,16.f),0,WithAlpha(t.accent,hover));
        u.FocusRing(r,2.f,st);
        if(st.clicked||u.Activate(id)){state.page=pages[i];Push(actions,UiCommand::OpenBackstage,pages[i]);}
    }
    // Native interaction is identical in window and fullscreen; desktop is
    // deliberately input-transparent and never receives these controls.
    const float iconsY=std::min(frame.height-130.f,y+154.f);
    if(frame.console&&frame.height>420.f){
        if(u.IconButton(Context::Id("original.fullscreen"),Rect::Xywh(x-6.f,iconsY,34.f,34.f),view.fullscreen?IconWindowed:IconFullscreen,L"切换全屏",false,flags))Push(actions,UiCommand::ToggleFullscreen);
        if(u.IconButton(Context::Id("original.desktop"),Rect::Xywh(x-6.f,iconsY+40.f,34.f,34.f),IconDesktop,L"融入桌面",false,flags))Push(actions,UiCommand::EnterDesktop);
    }
    if(frame.style==EchoStyle){
        const float sliderWidth=std::min(200.f,frame.width*.19f),left=frame.width-g.margin-sliderWidth;
        float value=view.response;
        u.Label(Rect::Xywh(left-56.f,frame.height-65.f,48.f,30.f),L"起伏",Font::Small,t.muted);
        if(u.Slider(Context::Id("original.response"),Rect::Xywh(left,frame.height-65.f,sliderWidth,30.f),value,0.f,2.f,.01f,flags))Push(actions,UiCommand::SetResponse,0,value);
    }else{
        const float cx=frame.style==WeaveStyle?frame.width*.88f:x+12.f;
        const float cy=frame.style==WeaveStyle?frame.height*.5f:frame.height*.63f;
        const Rect r=Rect::Xywh(cx-18.f,cy-60.f,36.f,120.f);const uint32_t id=Context::Id("original.vertical-response");
        const auto st=u.Interact(id,r,flags);
        const bool pointerNearby=frame.pointerActive&&std::abs(frame.pointerX-cx)<120.f&&std::abs(frame.pointerY-cy)<160.f;
        const float show=u.Animate(id,5,frame.style==PosterStyle||pointerNearby||st.held||st.focused?1.f:0.f,10.f);
        u.PushOpacity(show);
        u.Draw().Line({cx,r.top+10.f},{cx,r.bottom-10.f},WithAlpha(t.muted,.5f),1.f);
        const float value=std::clamp(view.response*.5f,0.f,1.f),thumbY=r.bottom-10.f-value*(r.Height()-20.f);
        for(int k=0;k<7;++k){const float yy=r.top+10.f+k*(r.Height()-20.f)/6.f;u.Draw().Line({cx-5.f,yy},{cx+5.f,yy},WithAlpha(t.muted,.5f),1.f);}
        u.Draw().Fill(Rect::Xywh(cx-5.f,thumbY-2.f,10.f,4.f),0,t.accent);
        u.FocusRing(r,2.f,st);
        if(st.pressed||st.held){const float next=std::clamp((r.bottom-10.f-st.pointer.y)/(r.Height()-20.f)*2.f,0.f,2.f);Push(actions,UiCommand::SetResponse,0,next);}
        if(u.Activate(id))Push(actions,UiCommand::SetResponse,0,view.response>=1.99f?0.f:std::min(2.f,view.response+.1f));
        u.PopOpacity();
    }
    // Keep actionable lyric recovery available without covering the artwork.
    if(view.resumeLyrics&&u.Button(Context::Id("original.resume"),Rect::Xywh(g.margin,g.railY-50.f,170.f,30.f),L"返回当前句 · Home",ButtonKind::Quiet,flags|(view.resumeEnabled?FlagNone:FlagDisabled)))Push(actions,UiCommand::ResumeLyrics);
    if(view.matchLyrics&&u.Button(Context::Id("original.match"),Rect::Xywh(g.margin+(view.resumeLyrics?180.f:0.f),g.railY-50.f,150.f,30.f),L"核对歌词版本",ButtonKind::Quiet,flags))Push(actions,UiCommand::MatchLyrics);
    u.PopOpacity();
}

// Transient app notices (the native status area is gone): a quiet glass
// note at the top, fading in and out with the notice's own lifetime.
void StageInterface::Toast(Context& u,const SettingsView& view,const InterfaceFrame& frame) {
    if(!view.notice.empty())toastText_=view.notice;
    const float presence=u.Animate(Context::Id("toast"),0,view.notice.empty()?0.f:1.f,view.notice.empty()?5.f:9.f);
    if(presence<.01f||toastText_.empty())return;
    const auto t=backstagePresence_>.5f?BackstageTheme(frame):ConsoleTheme(frame);u.SetTheme(t);
    const float maxWidth=std::min(620.f,frame.width-64.f);
    const float textWidth=std::min(maxWidth-40.f,u.TextWidth(toastText_,Font::Body));
    const float textHeight=std::max(18.f,u.TextHeight(toastText_,Font::Body,textWidth+2.f));
    const float w=textWidth+40.f,h=textHeight+20.f;
    // Over the stage card in Backstage (the pages own the top right).
    const float centerX=backstagePresence_>.5f?frame.width*.25f:frame.width*.5f;
    const Rect r=Rect::Xywh(std::clamp(centerX-w*.5f,16.f,std::max(16.f,frame.width-w-16.f)),(frame.windowChrome?48.f:18.f)+(1.f-Smooth(presence))*-10.f,w,h);
    u.PushOpacity(Smooth(presence));
    u.Draw().Glass(r,std::min(18.f,h*.5f),Mix(t.glassTint,t.light?Color{1,1,1,.7f}:Color{0,0,0,.7f},.4f),t.glassRim,1.f);
    u.Draw().Fill(Rect::Xywh(r.left+14.f,r.Center().y-3.f,6.f,6.f),3.f,t.accent);
    u.Label(Rect{r.left+28.f,r.top+10.f,r.right-12.f,r.bottom-10.f},toastText_,Font::Body,t.ink,Align::Leading,true);
    u.PopOpacity();
}

void StageInterface::Pills(Context& u,const SettingsView& view,const InterfaceFrame& frame,std::vector<UiAction>& actions) {
    if(backstagePresence_>.5f)return;
    const auto t=ConsoleTheme(frame);u.SetTheme(t);
    const auto pill=[&](std::string_view key,Rect r,wchar_t icon,std::wstring_view text,bool enabled)->bool{
        const uint32_t id=Context::Id(key);
        auto st=u.Interact(id,r,enabled?FlagNone:FlagDisabled);
        const float alpha=enabled?1.f:.5f;
        u.PushOpacity(u.Animate(id,6,1.f,8.f)*alpha);
        u.Draw().Glass(r,r.Height()*.5f,t.glassTint,WithAlpha(t.glassRim,1.f+st.hover),1.f);
        if(st.hover>.01f)u.Draw().Fill(r,r.Height()*.5f,WithAlpha(t.field,1.5f*st.hover));
        u.Icon({r.left+20.f,r.Center().y},icon,Font::IconSmall,t.accent);
        u.Label(Rect{r.left+34.f,r.top,r.right-12.f,r.bottom},text,Font::Strong,t.ink);
        u.PopOpacity();
        u.FocusRing(r,r.Height()*.5f,st);
        return st.clicked&&enabled;
    };
    const auto wide=[&](std::wstring_view text){return std::min(320.f,u.TextWidth(text,Font::Strong)+50.f);};
    if(view.matchLyrics){
        const std::wstring text=view.lyricMismatch?L"修正歌词版本":L"核对歌词版本";
        if(pill("pill.match",Rect::Xywh(36.f,66.f,wide(text),32.f),IconCheck,text,true))Push(actions,UiCommand::MatchLyrics);
    }
    if(view.resumeLyrics){
        const std::wstring text=L"返回当前句 · Home";const float w=wide(text);
        if(pill("pill.resume",Rect::Xywh(frame.width-w-36.f,66.f,w,32.f),IconHome,text,view.resumeEnabled))Push(actions,UiCommand::ResumeLyrics);
    }
}

void StageInterface::Console(Context& u,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions) {
    const bool wanted=frame.console&&!state.backstage&&frame.width>=468.f;
    consoleTarget_=wanted;
    consolePresence_=u.Animate(Context::Id("console"),0,wanted?1.f:0.f,wanted?10.f:6.f);
    if(consolePresence_<.01f)return;
    const auto t=ConsoleTheme(frame);u.SetTheme(t);
    const float p=Smooth(consolePresence_);
    const float width=std::min(1000.f,frame.width-48.f),height=76.f;
    if(width<420.f)return;
    const Rect bar=Rect::Xywh((frame.width-width)*.5f,frame.height-24.f-height+(frame.reducedMotion?0.f:(1.f-p)*26.f),width,height);
    u.PushOpacity(p);
    u.Draw().Glass(bar,26.f,t.glassTint,t.glassRim,1.f,static_cast<float>(frame.nowMs%9000)/9000.f,WithAlpha(t.accent,.60f));
    u.Block(bar);
    // Now playing: cover and title.
    const Rect cover=Rect::Xywh(bar.left+14.f,bar.top+14.f,48.f,48.f);
    u.Draw().Cover(cover,12.f);
    // Room for everything at about 1000 DIPs; narrower bars drop the lyric
    // mode, then the style switch, then the fader.
    const bool roomy=width>=900.f,medium=width>=700.f;
    const float titleWidth=roomy?std::min(240.f,width-824.f):medium?150.f:std::max(90.f,width-420.f);
    u.Label(Rect::Xywh(cover.right+12.f,bar.top+16.f,titleWidth,22.f),view.title,Font::Strong,t.ink);
    u.Label(Rect::Xywh(cover.right+12.f,bar.top+38.f,titleWidth,20.f),view.artist,Font::Small,t.muted);
    float x=cover.right+12.f+titleWidth+14.f;
    // Style switch.
    if(medium){
        if(IsGlyphMotionStyle(view.themeStyle)){
            const Rect button=Rect::Xywh(x,bar.top+20.f,roomy?186.f:168.f,36.f);
            const wchar_t* label=MotionName(view.themeStyle);
            if(u.Button(Context::Id("console.rift-style"),button,view.customTheme?L"自定义主题":label,ButtonKind::Secondary))Push(actions,UiCommand::OpenBackstage,1);
            x=button.right+12.f;
        }else{
        int style=view.customTheme?-1:std::clamp(view.themeStyle,0,2);
        int shown=style<0?std::clamp(view.themeStyle,0,2):style;
        const Rect seg=Rect::Xywh(x,bar.top+20.f,roomy?186.f:168.f,36.f);
        u.PushOpacity(style<0?.65f:1.f);
        if(u.Segmented(Context::Id("console.style"),seg,{L"流光",L"剧场",L"排版"},shown))Push(actions,UiCommand::SelectStyle,shown);
        u.PopOpacity();
        x=seg.right+12.f;
        }
    }
    if(roomy){
        int mode=std::clamp(view.lyricDisplayMode,0,2);
        const Rect seg=Rect::Xywh(x,bar.top+20.f,156.f,36.f);
        if(u.Segmented(Context::Id("console.mode"),seg,{L"流动",L"聚焦",L"双行"},mode,view.compactLayout?FlagDisabled:FlagNone))
            Push(actions,UiCommand::SetLyricMode,mode);
        x=seg.right+12.f;
    }
    // Right-aligned actions, laid out from the right edge but declared left
    // to right afterwards so keyboard focus follows the visual order.
    const auto slot=[&](int fromRight){return Rect::Xywh(bar.right-14.f-44.f-50.f*static_cast<float>(fromRight),bar.top+16.f,44.f,44.f);};
    const float right=slot(3).left-6.f;
    // Response fader with LED points that follow the music.
    const float faderWidth=std::min(170.f,right-x-4.f);
    if(faderWidth>=96.f){
        const Rect r=Rect::Xywh(right-faderWidth,bar.top+14.f,faderWidth,48.f);
        float response=view.response;
        if(u.Slider(Context::Id("console.response"),Rect{r.left,r.top+20.f,r.right,r.bottom},response,0.f,2.f,.01f))
            Push(actions,UiCommand::SetResponse,0,response);
        u.Tooltip(L"音频响应 "+Percent(view.response));
        const int leds=10;const float lit=std::clamp(view.audioLevel,0.f,1.f)*static_cast<float>(leds);
        const float span=r.Width()-16.f;
        for(int i=0;i<leds;++i){
            const float on=std::clamp(lit-static_cast<float>(i),0.f,1.f);
            const Point c{r.left+8.f+span*(static_cast<float>(i)+.5f)/static_cast<float>(leds),r.top+12.f};
            u.Draw().Fill(Rect::Xywh(c.x-2.f,c.y-2.f,4.f,4.f),2.f,WithAlpha(Mix(WithAlpha(t.ink,.18f),t.accent,on),1.f));
            if(on>.05f)u.Draw().Light(c,9.f,t.accent,.45f*on);
        }
    }
    if(u.IconButton(Context::Id("console.translation"),slot(3),IconCharacters,view.showTranslation?L"隐藏歌词翻译":L"显示歌词翻译",view.showTranslation))
        Push(actions,UiCommand::SetTranslation,view.showTranslation?0:1);
    if(u.IconButton(Context::Id("console.fullscreen"),slot(2),view.fullscreen?IconWindowed:IconFullscreen,
        view.fullscreen?L"退出全屏（Esc）":L"全屏（F11）",view.fullscreen))Push(actions,UiCommand::ToggleFullscreen);
    if(u.IconButton(Context::Id("console.desktop"),slot(1),IconDesktop,L"融入桌面"))Push(actions,UiCommand::EnterDesktop);
    if(u.IconButton(Context::Id("console.backstage"),slot(0),IconSettings,L"设置（Ctrl+,）"))Push(actions,UiCommand::OpenBackstage);
    u.PopOpacity();
}

void StageInterface::Backstage(Context& u,const SettingsView& view,InterfaceState& state,const InterfaceFrame& frame,std::vector<UiAction>& actions) {
    const float target=state.backstage?1.f:0.f;
    // Same dynamics as the renderer's camera; reduced motion fades instead,
    // in step with the renderer's dip through the room.
    const float presence=frame.reducedMotion?u.Animate(Context::Id("backstage"),1,target,4.5f):
        std::clamp(u.Spring(Context::Id("backstage"),0,target,13.f,.78f),0.f,1.f);
    backstagePresence_=presence;
    if(presence<.01f)return;
    const auto t=BackstageTheme(frame);u.SetTheme(t);
    const float W=frame.width,H=frame.height;
    // The whole canvas belongs to Backstage while it is open.
    u.Block(Rect{0,0,W,H});
    // The stage card (left half): clicking it returns to the stage.
    {
        const uint32_t id=Context::Id("backstage.card");
        auto st=u.Interact(id,Rect{0,0,W*.5f,H},FlagNoFocus);
        if(st.clicked&&state.backstage)Push(actions,UiCommand::CloseBackstage);
        u.PushOpacity(presence*(.55f+.45f*st.hover));
        u.Label(Rect{0,H*.86f,W*.5f,H*.86f+24.f},L"点击舞台返回 · Esc",Font::Small,t.muted,Align::Center);
        u.PopOpacity();
    }
    // The minimum 680-DIP window still leaves at least 300 DIPs for a page.
    const float x0=W*.5f+16.f,x1=W-(W<760.f?16.f:40.f);
    if(x1-x0<300.f)return;
    const bool narrow=x1-x0<470.f;
    const float slide=frame.reducedMotion?0.f:(1.f-presence)*36.f;
    u.PushOpacity(Smooth(presence));
    // Page tabs and the way back.
    static const wchar_t* names[4]={L"画面",L"主题",L"歌词",L"状态"};
    static const wchar_t* overline[4]={L"VISUAL",L"THEME",L"LYRICS",L"STATUS"};
    static const wchar_t* lines[4]={L"呈现位置、背景素材与性能",L"舞台风格、配色与主题文件",L"查找、核对与调整这首歌的歌词",L"接入状态、诊断与使用说明"};
    const float headerInset=frame.windowChrome?32.f:0.f;
    float tabX=x0+slide;
    for(int i=0;i<4;++i){
        const float w=u.TextWidth(names[i],Font::Strong)+28.f;
        const Rect r=Rect::Xywh(tabX,28.f+headerInset,w,34.f);tabX+=w+4.f;
        const uint32_t id=Context::Id("backstage.tab",static_cast<uint32_t>(i));
        auto st=u.Interact(id,r);
        if((st.clicked||u.Activate(id))&&state.page!=i)state.page=i;
        const bool on=state.page==i;
        const float lit=u.Animate(id,5,on?1.f:0.f,12.f);
        if(st.hover>.01f)u.Draw().Fill(r,17.f,WithAlpha(t.field,st.hover));
        u.Label(r,names[i],Font::Strong,Mix(t.muted,t.ink,std::max(lit,st.hover)),Align::Center);
        if(lit>.01f){
            u.Draw().Fill(Rect::Xywh(r.Center().x-10.f,r.bottom+2.f,20.f,2.f),1.f,WithAlpha(t.accent,lit));
            u.Draw().Light({r.Center().x,r.bottom+3.f},18.f,t.accent,.4f*lit);
        }
        u.FocusRing(r,17.f,st);
    }
    {
        const std::wstring back=L"返回舞台";
        const float w=u.TextWidth(back,Font::Strong)+52.f;
        const Rect r=Rect::Xywh(x1-w,(narrow?67.f:28.f)+headerInset,w,34.f);
        if(u.Button(Context::Id("backstage.back"),r,back,ButtonKind::Quiet))Push(actions,UiCommand::CloseBackstage);
    }
    // Page heading; a page change (here or from the app) fades the new page in.
    const int page=std::clamp(state.page,0,3);
    if(page!=shownPage_){shownPage_=page;pageChangedAt_=frame.nowMs;}
    const float enter=Smooth(static_cast<float>(std::max<int64_t>(0,frame.nowMs-pageChangedAt_))/240.f);
    const float headY=(narrow?112.f:86.f)+headerInset+(1.f-enter)*8.f;
    u.PushOpacity(enter);
    u.Label(Rect::Xywh(x0+slide,headY,x1-x0,14.f),overline[page],Font::Overline,t.accent);
    u.Label(Rect::Xywh(x0+slide,headY+16.f,x1-x0,44.f),names[page],Font::Title,t.ink);
    u.Label(Rect::Xywh(x0+slide,headY+60.f,x1-x0,20.f),lines[page],Font::Small,t.muted);
    // Content.
    const Rect viewport{x0-8.f,headY+92.f,x1+8.f,H-28.f};
    const float offset=u.BeginScroll(Context::Id("backstage.scroll",static_cast<uint32_t>(page)),viewport);
    const Rect column=Rect::Xywh(x0+slide,viewport.top+offset+4.f,std::min(680.f,x1-x0),0.f);
    float height=0.f;
    switch(page){
        case 0:height=PicturePage(u,view,column,actions);break;
        case 1:height=ThemePage(u,view,column,actions);break;
        case 2:height=LyricsPage(u,view,state,column,actions);break;
        default:height=StatusPage(u,view,column,actions);break;
    }
    u.EndScroll(height-column.top+8.f);
    u.PopOpacity();
    u.PopOpacity();
}

float StageInterface::PicturePage(Context& u,const SettingsView& view,Rect column,std::vector<UiAction>& actions) {
    const float x=column.left,w=column.Width(),inner=w-40.f;float y=column.top;
    {
        const int index=view.desktop?2:view.fullscreen?1:0;
        y=Section(u,x,y,w,L"呈现位置",{
            SegmentedRow(u,"picture.mode",{},{L"窗口",L"全屏",L"融入桌面"},index,[&,index](int next){
                if(next==index)return;
                if(next==2)Push(actions,UiCommand::EnterDesktop);
                else if(next==1)Push(actions,UiCommand::ToggleFullscreen);
                else Push(actions,index==2?UiCommand::WindowMode:UiCommand::ToggleFullscreen);
            }),
            NoteRow(u,L"F11 切换全屏，Esc 退出；桌面模式可从系统托盘恢复窗口。",inner)});
    }
    {
        std::vector<Row> rows;
        const int kind=std::clamp(view.backgroundKind,0,2);
        rows.push_back(SegmentedRow(u,"picture.background",{},{L"歌曲封面",L"图片",L"视频"},kind,[&,kind](int next){
            if(next==0)Push(actions,UiCommand::UseCover,0);
            else if(next==1)Push(actions,UiCommand::ChooseImage);
            else Push(actions,UiCommand::ChooseVideo);
            (void)kind;
        }));
        std::vector<ButtonSpec> buttons;
        if(kind==0)buttons.push_back({"picture.cover",L"使用 / 重试歌曲封面",ButtonKind::Secondary,true,[&]{Push(actions,UiCommand::UseCover,1);}});
        else buttons.push_back({"picture.change",kind==1?L"更换图片…":L"更换视频…",ButtonKind::Secondary,true,
            [&,kind]{Push(actions,kind==1?UiCommand::ChooseImage:UiCommand::ChooseVideo);}});
        if(view.backgroundRetry)buttons.push_back({"picture.retry",L"重试当前背景素材",ButtonKind::Primary,true,[&]{Push(actions,UiCommand::RetryBackground);}});
        rows.push_back(ButtonRow(u,std::move(buttons)));
        if(!view.coverStatus.empty())rows.push_back(NoteRow(u,L"歌曲封面 · "+view.coverStatus,inner));
        if(!view.backgroundNotice.empty())rows.push_back(NoteRow(u,view.backgroundNotice,inner,view.backgroundRetry));
        y=Section(u,x,y,w,L"背景",rows);
    }
    y=Section(u,x,y,w,L"舞台",{
        SliderRow(u,"picture.response",L"音频响应",Percent(view.response),view.response,0.f,2.f,.01f,[&](float v){Push(actions,UiCommand::SetResponse,0,v);}),
        SliderRow(u,"picture.size",L"歌词字号",std::to_wstring(static_cast<int>(std::lround(view.lyricSize))),view.lyricSize,26.f,80.f,1.f,
            [&](float v){Push(actions,UiCommand::SetLyricSize,0,v);}),
        SliderRow(u,"picture.shade",L"背景淡化",Percent(view.backgroundShade),view.backgroundShade,0.f,1.f,.01f,[&](float v){Push(actions,UiCommand::SetBackgroundShade,0,v);}),
        SliderRow(u,"picture.camera",L"镜头幅度",Percent(view.cameraMotion),view.cameraMotion,0.f,2.f,.01f,[&](float v){Push(actions,UiCommand::SetCameraMotion,0,v);}),
        SwitchRow(u,"picture.motion",L"减少动态",L"减少镜头与粒子运动，Backstage 改为淡入淡出",view.reducedMotion,inner,
            [&](bool v){Push(actions,UiCommand::SetReducedMotion,v?1:0);}),
        ButtonRow(u,{{"picture.reset",L"恢复背景调节默认值",ButtonKind::Quiet,true,[&]{Push(actions,UiCommand::ResetBackdrop);}}})});
    y=Section(u,x,y,w,L"歌词字体",{
        SegmentedRow(u,"picture.font",L"选择字体",{L"主题原字体",L"文楷",L"黄油体",L"快乐体",L"自选"},
            view.lyricFontPath.empty()?(view.lyricFont==L"@wenkai"?1:view.lyricFont==L"@huangyou"?2:view.lyricFont==L"@kuaile"?3:view.lyricFont.empty()?0:4):4,
            [&](int index){if(index<4)Push(actions,UiCommand::SetLyricFont,index);else Push(actions,UiCommand::ChooseSystemLyricFont);}),
        NoteRow(u,L"当前："+LyricFontLabel(view.lyricFont,view.lyricFontPath),inner),
        ButtonRow(u,{{"picture.systemfont",L"本机字体…",ButtonKind::Secondary,true,[&]{Push(actions,UiCommand::ChooseSystemLyricFont);}},
            {"picture.importfont",L"导入 TTF / OTF…",ButtonKind::Secondary,true,[&]{Push(actions,UiCommand::ImportLyricFont);}}}),
        NoteRow(u,view.lyricFontNotice.empty()?L"字体选择随自定义主题保存。导入字体使用本地文件引用，分享主题时请同时保留字体文件。":view.lyricFontNotice,inner)});
    y=Section(u,x,y,w,L"性能",{
        SegmentedRow(u,"picture.quality",L"画质（自动会按显卡负载调整）",{L"自动",L"高",L"中",L"低"},std::clamp(view.qualityMode,0,3),
            [&](int v){Push(actions,UiCommand::SetQuality,v);}),
        SegmentedRow(u,"picture.framerate",L"帧率",{L"自动 ≤60",L"跟随屏幕",L"省电 30"},std::clamp(view.frameRateMode,0,2),
            [&](int v){Push(actions,UiCommand::SetFrameRate,v);}),
        SwitchRow(u,"picture.pause",L"被全屏游戏或最大化窗口挡住时暂停舞台",L"暂停期间不绘制、不分析音频；歌曲识别与歌词时间轴照常",view.pauseForFullscreenApps,inner,
            [&](bool v){Push(actions,UiCommand::SetPauseForFullscreen,v?1:0);})});
    return y;
}

float StageInterface::ThemePage(Context& u,const SettingsView& view,Rect column,std::vector<UiAction>& actions) {
    const float x=column.left,w=column.Width(),inner=w-40.f;float y=column.top;
    if(view.themeStyle==8||view.themeStyle==9||view.themeStyle==7||view.themeStyle==19){
        const int count=view.themeStyle==8?121:view.themeStyle==9?100:view.themeStyle==7?100:13;
        y=Section(u,x,y,w,L"场景编排",{SliderRow(u,"motion.variant",L"子预设",view.motionVariant<0?L"随歌词变化":std::to_wstring(view.motionVariant+1)+L" / "+std::to_wstring(count),
            static_cast<float>(view.motionVariant),-1.f,static_cast<float>(count-1),1.f,[&](float v){Push(actions,UiCommand::SetMotionVariant,static_cast<int>(std::lround(v)));})});
    }
    if(IsMineradioStyle(view.themeStyle)){
        y=Section(u,x,y,w,L"歌词与镜头",{
            SegmentedRow(u,"motion.fx",L"文字动效",{L"玻璃",L"柔顺",L"漂浮",L"快切",L"流光",L"故障"},view.motionFx,[&](int v){Push(actions,UiCommand::SetMotionFx,v);}),
            SegmentedRow(u,"motion.camera",L"镜头",{L"环绕",L"指针运镜",L"焦点跟随"},view.motionCamera,[&](int v){Push(actions,UiCommand::SetMotionCamera,v);}),
            SliderRow(u,"motion.lines",L"可见歌词行数",std::to_wstring(view.motionLines),static_cast<float>(view.motionLines),1,10,1,[&](float v){Push(actions,UiCommand::SetMotionLines,static_cast<int>(std::lround(v)));})});
    }
    if(IsFoliaStyle(view.themeStyle))y=Section(u,x,y,w,L"场景背景",{SegmentedRow(u,"motion.background",L"背景效果",{L"通用",L"莫奈",L"漫游",L"隐现",L"空"},view.motionBackground,[&](int v){Push(actions,UiCommand::SetMotionBackground,v);})});
    if(IsFoliaStyle(view.themeStyle)&&view.motionBackground==2)y=Section(u,x,y,w,L"漫游滤镜",{SegmentedRow(u,"motion.filter",L"图像处理",{L"抖色",L"玻璃",L"纸纹",L"网点",L"透镜"},view.motionFilter,[&](int v){Push(actions,UiCommand::SetMotionFilter,v);})});
    const float seconds=static_cast<float>(u.Frame().nowMs%600000)/1000.f;
    const bool custom=view.customTheme||view.themeRestorePending;
    const int selected=view.customTheme?-1:std::clamp(view.themeStyle,0,BuiltInStyleCount-1);
    const int columns=inner<470.f?2:3;
    const float cardWidth=(inner-12.f*(columns-1))/columns,cardHeight=std::clamp(cardWidth*.78f,120.f,170.f);
    std::vector<Row> rows;
    for(int row=0;row<(static_cast<int>(AvailableBuiltInStyles.size())+columns-1)/columns;++row)rows.push_back({cardHeight,[&,cardWidth,selected,seconds,row,columns](Rect r){
        for(int col=0;col<columns;++col){
            const int choice=row*columns+col;if(choice>=static_cast<int>(AvailableBuiltInStyles.size()))break;const int i=AvailableBuiltInStyles[choice];
            const Rect card=Rect::Xywh(r.left+static_cast<float>(col)*(cardWidth+12.f),r.top,cardWidth,r.Height());
            if(StyleCard(u,Context::Id("theme.card",static_cast<uint32_t>(i)),card,Styles[i],i==selected,L"使用中",seconds,i))
                Push(actions,UiCommand::SelectStyle,i);
        }
    }});
    if(custom){
        const std::wstring name=view.themeRestorePending?L"已保存的主题（暂时无法载入）":(view.customThemeName.empty()?L"我的音乐舞台":view.customThemeName);
        rows.push_back({52.f,[&,name](Rect r){
            const auto& t=u.Frame().theme;
            const uint32_t id=Context::Id("theme.custom");
            auto st=u.Interact(id,r);
            if(u.Activate(id))st.clicked=true;
            const float lit=u.Animate(id,5,view.customTheme?1.f:0.f,10.f);
            u.Draw().Fill(r,14.f,WithAlpha(t.field,1.f+st.hover));
            u.Draw().Stroke(r.Inset(.5f,.5f),14.f,WithAlpha(Mix(t.glassRim,t.accent,lit),1.f+lit*6.f),1.f+lit);
            u.Label(Rect{r.left+16.f,r.top,r.right-120.f,r.bottom},L"我的自定义主题 · "+name,Font::Strong,t.ink);
            u.Label(Rect{r.right-150.f,r.top,r.right-16.f,r.bottom},view.customTheme?L"使用中":view.themeRestorePending?L"点击重试":L"",
                Font::Small,view.customTheme?t.accent:t.muted,Align::Trailing);
            u.FocusRing(r,14.f,st);
            if(st.clicked&&!view.customTheme)Push(actions,UiCommand::SelectStyle,CustomThemeChoice);
        }});
    }
    y=Section(u,x,y,w,L"舞台风格",rows);
    std::vector<Row> files{
        ButtonRow(u,{{"theme.import",L"载入本地主题…",ButtonKind::Secondary,true,[&]{Push(actions,UiCommand::ImportTheme);}},
                     {"theme.export",L"导出当前主题…",ButtonKind::Secondary,true,[&]{Push(actions,UiCommand::ExportTheme);}},
                     {"theme.accent",L"选择强调色…",ButtonKind::Secondary,true,[&]{Push(actions,UiCommand::ChooseAccent);}}}),
        NoteRow(u,L"主题保存在你的电脑：配色、字号、动效和背景都可自定义；导入时保留原始主题文件。背景素材使用本地引用，移动主题时请一起保留素材。",inner)};
    if(view.themeRestorePending){
        files.push_back(ButtonRow(u,{{"theme.restore",L"重试已保存主题",ButtonKind::Primary,true,[&]{Push(actions,UiCommand::RetryTheme);}}}));
        if(!view.themeNotice.empty())files.push_back(NoteRow(u,view.themeNotice,inner,true));
    }
    y=Section(u,x,y,w,L"主题文件",files);
    return y;
}

float StageInterface::LyricsPage(Context& u,const SettingsView& view,InterfaceState& state,Rect column,std::vector<UiAction>& actions) {
    const float x=column.left,w=column.Width(),inner=w-40.f;float y=column.top;
    {
        std::vector<Row> rows;
        rows.push_back({38.f,[&](Rect r){
            const auto event=u.TextBox(Context::Id("lyrics.query"),r,state.query,L"歌名、歌曲 ID（如 soda-ID）或网易云链接",512);
            if(event.submitted)Push(actions,UiCommand::FindLyrics,0,0.f,state.query);
        }});
        rows.push_back(ButtonRow(u,{{"lyrics.find",L"查找 / 预览歌词",ButtonKind::Primary,true,[&]{Push(actions,UiCommand::FindLyrics,0,0.f,state.query);}},
            {"lyrics.search",L"按当前歌曲重新匹配",ButtonKind::Secondary,view.canSearch,[&]{Push(actions,UiCommand::FollowCurrent);}}}));
        y=Section(u,x,y,w,L"查找歌词",rows);
    }
    {
        std::vector<Row> rows;
        if(view.candidates.empty())rows.push_back(NoteRow(u,L"还没有候选版本。重新匹配或查找后，在这里核对歌名、歌手、专辑与时长，再采用所选歌词。",inner));
        else{
            const float listHeight=std::min(260.f,54.f*static_cast<float>(view.candidates.size())+8.f);
            rows.push_back({listHeight,[&](Rect r){
                std::vector<ListRow> list;list.reserve(view.candidates.size());
                for(const auto& c:view.candidates){
                    std::wstring subtitle=c.clientRecord?L"最近播放记录 · 需确认 · ":L"";
                    subtitle+=c.album.empty()?L"—":c.album;
                    if(c.durationMs>0){const int64_t s=c.durationMs/1000;subtitle+=L" · "+std::to_wstring(s/60)+L":"+(s%60<10?L"0":L"")+std::to_wstring(s%60);}
                    subtitle+=L" · ID "+c.id;
                    list.push_back({(c.title.empty()?std::wstring(L"—"):c.title)+L" — "+(c.artist.empty()?std::wstring(L"—"):c.artist),subtitle,c.clientRecord});
                }
                int selected=view.selectedCandidate;
                const auto event=u.List(Context::Id("lyrics.candidates"),r,list,selected);
                if(event.selected&&selected>=0)Push(actions,UiCommand::SelectCandidate,selected,0.f,{},view.candidateGeneration);
                if(event.activated&&selected>=0)Push(actions,UiCommand::FetchCandidate,selected,0.f,{},view.candidateGeneration);
            }});
            if(!view.candidateDetails.empty())rows.push_back(NoteRow(u,view.candidateDetails,inner));
            rows.push_back(ButtonRow(u,{{"lyrics.fetch",L"采用所选版本的歌词",ButtonKind::Primary,view.canFetch,
                [&]{Push(actions,UiCommand::FetchCandidate,view.selectedCandidate,0.f,{},view.candidateGeneration);}}}));
        }
        y=Section(u,x,y,w,L"候选版本",rows);
    }
    y=Section(u,x,y,w,L"歌词来源",{
        ButtonRow(u,{{"lyrics.import",L"导入 LRC / YRC 歌词…",ButtonKind::Secondary,view.canImport,[&]{Push(actions,UiCommand::ImportLyrics);}},
            {"lyrics.online",view.independentPreview?L"跟随当前歌曲":L"恢复在线歌词",ButtonKind::Secondary,view.canFollow||view.independentPreview,
             [&]{Push(actions,UiCommand::RestoreOnline);}}})});
    {
        // Offset: steps, an exact field and reset; only this song changes.
        if(!u.Focused(Context::Id("lyrics.offset"))&&!state.offsetEditing)state.offsetText=std::to_wstring(view.offsetMs);
        const uint32_t flags=view.hasTrack?FlagNone:FlagDisabled;
        std::vector<Row> rows;
        rows.push_back({44.f,[&,flags](Rect r){
            const auto& t=u.Frame().theme;
            const std::wstring value=(view.offsetMs>0?L"+":L"")+std::to_wstring(view.offsetMs)+L" ms";
            u.Label(Rect{r.left,r.top,r.left+170.f,r.bottom},value,Font::Value,view.hasTrack?t.ink:t.faint);
            const float right=r.right;
            const auto step=[&](std::string_view key,float left,std::wstring_view text,int delta){
                if(u.Button(Context::Id(key),Rect::Xywh(left,r.top+3.f,64.f,38.f),text,ButtonKind::Secondary,flags))
                    Push(actions,UiCommand::SetOffset,std::clamp(view.offsetMs+delta,-30000,30000));
            };
            step("lyrics.offset.minus",right-64.f*2.f-10.f-170.f,L"−50",-50);
            step("lyrics.offset.plus",right-64.f-170.f,L"+50",50);
            const Rect field=Rect::Xywh(right-160.f,r.top+3.f,160.f,38.f);
            const auto event=u.TextBox(Context::Id("lyrics.offset"),field,state.offsetText,L"毫秒",7,flags);
            state.offsetEditing=u.Focused(Context::Id("lyrics.offset"));
            if(event.changed){
                // Like the native field: every valid signed integer applies at
                // once; partial input waits and is restored when editing ends.
                const auto& text=state.offsetText;size_t i=0;int sign=1;
                if(i<text.size()&&(text[i]==L'-'||text[i]==L'+'||text[i]==L'−')){sign=text[i]==L'+'?1:-1;++i;}
                bool valid=i<text.size();long parsed=0;
                for(;i<text.size()&&valid;++i){if(text[i]<L'0'||text[i]>L'9')valid=false;else parsed=std::min(30001L,parsed*10+(text[i]-L'0'));}
                if(valid)Push(actions,UiCommand::SetOffset,std::clamp(static_cast<int>(sign*parsed),-30000,30000));
            }
            if(event.submitted||event.committed){
                state.offsetText=std::to_wstring(view.offsetMs);state.offsetEditing=false;
                if(event.submitted)u.ClearFocus();
            }
        }});
        rows.push_back(ButtonRow(u,{{"lyrics.offset.reset",L"重置为 0",ButtonKind::Quiet,view.hasTrack,[&]{Push(actions,UiCommand::ResetOffset);}}}));
        rows.push_back(NoteRow(u,view.hasTrack?L"正值让歌词更晚出现。只影响这首歌，自动保存在本机。":L"识别到当前歌曲后可调整这首歌的偏移。",inner));
        y=Section(u,x,y,w,L"时间偏移",rows);
    }
    y=Section(u,x,y,w,L"显示",{
        SwitchRow(u,"lyrics.translation",L"显示歌词翻译",{},view.showTranslation,inner,[&](bool v){Push(actions,UiCommand::SetTranslation,v?1:0);}),
        SwitchRow(u,"lyrics.romanization",L"显示罗马音 / 音译",{},view.showRomanization,inner,[&](bool v){Push(actions,UiCommand::SetRomanization,v?1:0);}),
        SegmentedRow(u,"lyrics.mode",view.compactLayout?L"小窗口采用紧凑呈现，放大后恢复偏好":L"呈现方式（Ctrl+L 切换）",{L"流动歌词",L"聚焦单句",L"双行预读"},
            std::clamp(view.lyricDisplayMode,0,2),[&](int v){Push(actions,UiCommand::SetLyricMode,v);})});
    y=Section(u,x,y,w,L"歌词记忆",{
        ButtonRow(u,{{"lyrics.memory",view.lyricMemoryButton.empty()?L"恢复歌词记忆保存":view.lyricMemoryButton,
            view.lyricMemoryActionable?ButtonKind::Primary:ButtonKind::Secondary,view.lyricMemoryActionable,[&]{Push(actions,UiCommand::RepairLyricMemory);}}}),
        NoteRow(u,view.lyricMemoryNote,inner,view.lyricMemoryActionable)});
    return y;
}

float StageInterface::StatusPage(Context& u,const SettingsView& view,Rect column,std::vector<UiAction>& actions) {
    const float x=column.left,w=column.Width(),inner=w-40.f;float y=column.top;
    y=Section(u,x,y,w,L"当前状态",{{150.f,[&](Rect r){
        u.TextBlock(Context::Id("status.diagnostics"),r,view.diagnostics.empty()?L"读取播放器、歌词与音频状态中…":view.diagnostics,Font::Small,u.Frame().theme.muted);
    }}});
    y=Section(u,x,y,w,L"选项",{
        SwitchRow(u,"status.diagnostics.switch",L"显示接入诊断",L"在舞台上显示时间来源、缓存等跟随细节",view.showDiagnostics,inner,
            [&](bool v){Push(actions,UiCommand::SetDiagnostics,v?1:0);}),
        SwitchRow(u,"status.clock",L"允许原生只读内部时钟",L"只有严格校验通过才会驱动歌词",view.nativeClock,inner,
            [&](bool v){Push(actions,UiCommand::SetNativeClock,v?1:0);}),
        SwitchRow(u,"status.autohide",L"全屏空闲时隐藏辅助信息和指针",{},view.fullscreenAutoHide,inner,
            [&](bool v){Push(actions,UiCommand::SetFullscreenAutoHide,v?1:0);})});
    y=Section(u,x,y,w,L"设置保存",{
        ButtonRow(u,{{"status.settings",view.settingsButton.empty()?L"恢复设置保存":view.settingsButton,
            view.settingsActionable?ButtonKind::Primary:ButtonKind::Secondary,view.settingsActionable,[&]{Push(actions,UiCommand::RepairSettings);}}}),
        NoteRow(u,view.settingsNote,inner,view.settingsActionable)});
    y=Section(u,x,y,w,L"使用说明",{{300.f,[&](Rect r){
        u.TextBlock(Context::Id("status.help"),r,view.usageHelp,Font::Small,u.Frame().theme.muted);
    }}});
    return y;
}
}
