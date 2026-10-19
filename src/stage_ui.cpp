#include "stage_ui.h"
#include <dwrite_2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace stage::ui {
namespace {
float Finite(float value,float fallback=0.f) {return std::isfinite(value)?value:fallback;}
float Approach(float value,float goal,float dt,float rate) {
    return value+(goal-value)*(1.f-std::exp(-std::max(0.f,dt)*rate));
}
bool HighSurrogate(wchar_t c) {return c>=0xD800&&c<=0xDBFF;}
bool LowSurrogate(wchar_t c) {return c>=0xDC00&&c<=0xDFFF;}
// Caret positions never split a surrogate pair.
size_t PreviousStop(const std::wstring& text,size_t at) {
    if(at==0)return 0;
    --at;
    if(at>0&&LowSurrogate(text[at])&&HighSurrogate(text[at-1]))--at;
    return at;
}
size_t NextStop(const std::wstring& text,size_t at) {
    if(at>=text.size())return text.size();
    ++at;
    if(at<text.size()&&LowSurrogate(text[at])&&HighSurrogate(text[at-1]))++at;
    return at;
}
int CharClass(wchar_t c) {
    if(std::iswspace(c))return 0;
    if(c>=0x2E80)return 2; // CJK and other wide scripts: one class per run
    if(std::iswalnum(c)||c==L'_')return 1;
    return 3;
}
size_t WordLeft(const std::wstring& text,size_t at) {
    while(at>0&&CharClass(text[at-1])==0)at=PreviousStop(text,at);
    if(at==0)return 0;
    const int kind=CharClass(text[at-1]);
    while(at>0&&CharClass(text[at-1])==kind)at=PreviousStop(text,at);
    return at;
}
size_t WordRight(const std::wstring& text,size_t at) {
    if(at>=text.size())return text.size();
    const int kind=CharClass(text[at]);
    while(at<text.size()&&CharClass(text[at])==kind)at=NextStop(text,at);
    while(at<text.size()&&CharClass(text[at])==0)at=NextStop(text,at);
    return at;
}
bool CopyToClipboard(HWND owner,std::wstring_view text) {
    if(!OpenClipboard(owner))return false;
    bool ok=false;
    if(EmptyClipboard()){
        const size_t bytes=(text.size()+1)*sizeof(wchar_t);
        if(HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes)){
            if(auto* target=static_cast<wchar_t*>(GlobalLock(memory))){
                std::copy(text.begin(),text.end(),target);target[text.size()]=L'\0';GlobalUnlock(memory);
                ok=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;
            }
            if(!ok)GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}
std::wstring PasteFromClipboard(HWND owner) {
    std::wstring result;
    if(!IsClipboardFormatAvailable(CF_UNICODETEXT)||!OpenClipboard(owner))return result;
    if(HANDLE data=GetClipboardData(CF_UNICODETEXT)){
        if(const auto* text=static_cast<const wchar_t*>(GlobalLock(data))){
            const size_t limit=GlobalSize(data)/sizeof(wchar_t);
            size_t length=0;while(length<limit&&text[length])++length;
            result.assign(text,length);GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}
}

Color WithAlpha(Color color,float factor) {color.a=std::clamp(color.a*std::max(0.f,Finite(factor)),0.f,1.f);return color;}
Color Mix(Color a,Color b,float t) {
    t=std::clamp(Finite(t),0.f,1.f);
    return {a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t};
}
Rect Rect::Intersect(const Rect& other) const {
    Rect r{std::max(left,other.left),std::max(top,other.top),std::min(right,other.right),std::min(bottom,other.bottom)};
    if(r.right<r.left)r.right=r.left;
    if(r.bottom<r.top)r.bottom=r.top;
    return r;
}

void DrawList::Clear() {items_.clear();clips_=0;glass_=false;opacity_=1.f;}
void DrawList::SetOpacity(float opacity) {opacity_=std::clamp(Finite(opacity,1.f),0.f,1.f);}
void DrawList::Glass(Rect rect,float radius,Color tint,Color rim,float opacity,float sheen,Color sheenColor,bool frosted) {
    opacity*=opacity_;
    if(rect.Empty()||opacity<=0.f)return;
    Item item;item.op=Op::Glass;item.rect=rect;item.radius=radius;item.color=tint;item.second=rim;item.third=sheenColor;
    item.opacity=std::clamp(opacity,0.f,1.f);item.sheen=sheen;item.frosted=frosted;
    items_.push_back(std::move(item));glass_=glass_||frosted;
}
void DrawList::Fill(Rect rect,float radius,Color color) {
    color.a*=opacity_;
    if(rect.Empty()||color.a<=0.f)return;
    Item item;item.op=Op::Fill;item.rect=rect;item.radius=radius;item.color=color;items_.push_back(std::move(item));
}
void DrawList::Stroke(Rect rect,float radius,Color color,float width) {
    color.a*=opacity_;
    if(rect.Empty()||color.a<=0.f||width<=0.f)return;
    Item item;item.op=Op::Stroke;item.rect=rect;item.radius=radius;item.color=color;item.width=width;items_.push_back(std::move(item));
}
void DrawList::Text(const ComPtr<IDWriteTextLayout>& layout,Point origin,Color color) {
    color.a*=opacity_;
    if(!layout||color.a<=0.f)return;
    Item item;item.op=Op::Text;item.rect={origin.x,origin.y,origin.x,origin.y};item.color=color;item.layout=layout;items_.push_back(std::move(item));
}
void DrawList::Line(Point from,Point to,Color color,float width) {
    color.a*=opacity_;
    if(color.a<=0.f||width<=0.f)return;
    Item item;item.op=Op::Line;item.from=from;item.to=to;item.color=color;item.width=width;items_.push_back(std::move(item));
}
void DrawList::Light(Point center,float radius,Color color,float opacity) {
    opacity*=opacity_;
    if(radius<=0.f||opacity<=0.f||color.a<=0.f)return;
    Item item;item.op=Op::Light;item.rect={center.x-radius,center.y-radius,center.x+radius,center.y+radius};
    item.radius=radius;item.color=color;item.opacity=std::clamp(opacity,0.f,1.f);items_.push_back(std::move(item));
}
void DrawList::Cover(Rect rect,float radius,float opacity) {
    opacity*=opacity_;
    if(rect.Empty()||opacity<=0.f)return;
    Item item;item.op=Op::Cover;item.rect=rect;item.radius=radius;item.opacity=std::clamp(opacity,0.f,1.f);items_.push_back(std::move(item));
}
void DrawList::PushClip(Rect rect) {Item item;item.op=Op::Clip;item.rect=rect;items_.push_back(std::move(item));++clips_;}
void DrawList::PopClip() {if(clips_<=0)return;Item item;item.op=Op::Unclip;items_.push_back(std::move(item));--clips_;}

struct Context::Impl {
    struct Event {
        enum Kind : uint8_t { Move,Down,Up,Leave,Wheel,Key,Char } kind=Move;
        float x=0,y=0,value=0;
        UINT vk=0;bool ctrl=false,shift=false,doubleClick=false;wchar_t ch=0;
    };
    struct CachedLayout {
        std::wstring text;Font font=Font::Body;float width=0;Align align=Align::Leading;bool wrap=false,raw=false;
        ComPtr<IDWriteTextLayout> layout;uint64_t used=0;
    };
    struct Anim {float value=0,velocity=0;uint64_t frame=0;};
    struct TextState {size_t caret=0,anchor=0;float scroll=0;int64_t blinkAt=0;bool dragging=false;uint64_t frame=0;wchar_t high=0;};
    struct ScrollState {float offset=0,target=0,content=0,viewport=0,dragOffset=0,dragY=0;uint64_t frame=0;};
    struct ScrollFrame {uint32_t id=0;Rect viewport{};float offset=0;bool bar=false;float barLight=0;};
    struct FocusInfo {Rect rect{};uint32_t scroll=0;};

    ComPtr<IDWriteFactory2> factory;
    std::array<ComPtr<IDWriteTextFormat>,static_cast<size_t>(Font::Count)> formats{};
    std::vector<CachedLayout> layouts;
    std::deque<Event> queue;

    FrameInfo frame;int64_t lastNow=0;float dt=.016f;uint64_t frameIndex=0;
    DrawList draw;
    // Pointer state of this frame and between frames.
    Point mouse{-1e9f,-1e9f};bool mouseInside=false,buttonHeld=false;
    bool down=false,up=false,doubleClick=false,pressClaimed=false;Point downAt{},upAt{};
    float wheel=0;uint32_t wheelTarget=0;
    uint32_t active=0;bool activeSeen=false;
    std::vector<Rect> clips,hitAreas,nextHitAreas;
    // Keyboard.
    std::vector<Event> keys;
    uint32_t focused=0;bool focusVisible=false,revealFocus=false;
    std::vector<uint32_t> focusOrder,nextFocusOrder;
    std::unordered_map<uint32_t,FocusInfo> focusRects,nextFocusRects;
    std::unordered_set<uint32_t> textIds,nextTextIds,commitPending;
    // Widget state.
    std::unordered_map<uint64_t,Anim> anims;
    std::unordered_map<uint32_t,TextState> texts;
    std::unordered_map<uint32_t,ScrollState> scrolls;
    std::vector<ScrollFrame> scrollStack;
    bool animating=false,caretValid=false;Rect caret{};
    // Tooltip of the widget declared last.
    uint32_t lastId=0;Rect lastRect{};bool lastHovered=false;
    uint32_t hoverId=0;int64_t hoverSince=0;
    std::wstring tipText;Rect tipRect{};bool tipPending=false;
    std::vector<float> opacities;

    Impl() {
        ComPtr<IDWriteFactory> base;
        if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(base.GetAddressOf())))||
           FAILED(base.As(&factory)))throw std::runtime_error("Stage UI text factory unavailable");
        CreateFormats();
    }
    static bool Exists(IDWriteFontCollection* collection,const wchar_t* name) {
        UINT32 index=0;BOOL exists=FALSE;
        return collection&&SUCCEEDED(collection->FindFamilyName(name,&index,&exists))&&exists;
    }
    static bool RealWeight(IDWriteFontCollection* collection,const wchar_t* name,DWRITE_FONT_WEIGHT weight) {
        UINT32 index=0;BOOL exists=FALSE;
        if(!collection||FAILED(collection->FindFamilyName(name,&index,&exists))||!exists)return false;
        ComPtr<IDWriteFontFamily> family;ComPtr<IDWriteFont> font;
        if(FAILED(collection->GetFontFamily(index,&family))||FAILED(family->GetFirstMatchingFont(weight,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&font)))return false;
        return font->GetSimulations()==DWRITE_FONT_SIMULATIONS_NONE&&std::abs(static_cast<int>(font->GetWeight())-static_cast<int>(weight))<=100;
    }
    ComPtr<IDWriteFontFallback> CjkFallback(const wchar_t* cjk) {
        ComPtr<IDWriteFontFallbackBuilder> builder;
        if(FAILED(factory->CreateFontFallbackBuilder(&builder)))return {};
        static const DWRITE_UNICODE_RANGE ranges[]={{0x2E80,0x2FDF},{0x3000,0x30FF},{0x3100,0x31FF},{0x3200,0x33FF},
            {0x3400,0x4DBF},{0x4E00,0x9FFF},{0xF900,0xFAFF},{0xFE30,0xFE4F},{0xFF00,0xFFEF},{0x20000,0x2FA1F}};
        const wchar_t* names[]={cjk};
        if(FAILED(builder->AddMapping(ranges,static_cast<UINT32>(std::size(ranges)),names,1)))return {};
        ComPtr<IDWriteFontFallback> system;
        if(SUCCEEDED(factory->GetSystemFontFallback(&system))&&system&&FAILED(builder->AddMappings(system.Get())))return {};
        ComPtr<IDWriteFontFallback> result;
        if(FAILED(builder->CreateFontFallback(&result)))return {};
        return result;
    }
    void CreateFormats() {
        ComPtr<IDWriteFontCollection> system;
        if(FAILED(factory->GetSystemFontCollection(&system,FALSE)))system.Reset();
        const auto pick=[&](std::initializer_list<const wchar_t*> names,const wchar_t* fallback){
            for(const auto* name:names)if(Exists(system.Get(),name))return name;
            return fallback;
        };
        const wchar_t* text=pick({L"Segoe UI Variable Text",L"Segoe UI"},L"Segoe UI");
        const wchar_t* display=pick({L"Segoe UI Variable Display",L"Segoe UI"},L"Segoe UI");
        const wchar_t* cjk=pick({L"Microsoft YaHei UI",L"Microsoft YaHei"},L"Microsoft YaHei UI");
        const wchar_t* icons=pick({L"Segoe Fluent Icons",L"Segoe MDL2 Assets"},L"Segoe MDL2 Assets");
        const wchar_t* serif=nullptr;
        for(const auto* name:{L"Noto Serif SC",L"Source Han Serif SC",L"Source Han Serif CN",L"\x601D\x6E90\x5B8B\x4F53",L"Noto Serif CJK SC"})
            if(RealWeight(system.Get(),name,DWRITE_FONT_WEIGHT_SEMI_BOLD)){serif=name;break;}
        const auto fallback=CjkFallback(cjk);
        const auto make=[&](Font role,const wchar_t* family,DWRITE_FONT_WEIGHT weight,float size,bool withFallback){
            ComPtr<IDWriteTextFormat> format;
            if(FAILED(factory->CreateTextFormat(family,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-CN",&format)))
                throw std::runtime_error("Stage UI text format unavailable");
            if(withFallback&&fallback){ComPtr<IDWriteTextFormat1> format1;if(SUCCEEDED(format.As(&format1)))format1->SetFontFallback(fallback.Get());}
            formats[static_cast<size_t>(role)]=format;
        };
        make(Font::Body,text,DWRITE_FONT_WEIGHT_NORMAL,13.5f,true);
        make(Font::Strong,text,DWRITE_FONT_WEIGHT_SEMI_BOLD,13.5f,true);
        make(Font::Small,text,DWRITE_FONT_WEIGHT_NORMAL,12.f,true);
        make(Font::Caption,text,DWRITE_FONT_WEIGHT_NORMAL,11.f,true);
        make(Font::Overline,display,DWRITE_FONT_WEIGHT_SEMI_BOLD,10.5f,true);
        if(serif)make(Font::Title,serif,DWRITE_FONT_WEIGHT_SEMI_BOLD,30.f,false);
        else make(Font::Title,display,DWRITE_FONT_WEIGHT_SEMI_BOLD,30.f,true);
        make(Font::Value,display,DWRITE_FONT_WEIGHT_SEMI_LIGHT,26.f,true);
        make(Font::Icon,icons,DWRITE_FONT_WEIGHT_NORMAL,16.f,false);
        make(Font::IconSmall,icons,DWRITE_FONT_WEIGHT_NORMAL,12.f,false);
        make(Font::StageNav,text,DWRITE_FONT_WEIGHT_NORMAL,20.f,true);
    }
    ComPtr<IDWriteTextLayout> Layout(std::wstring_view text,Font font,float width,Align align,bool wrap,bool raw) {
        width=std::clamp(Finite(width,1.f),1.f,100000.f);
        const float key=raw?0.f:std::round(width*2.f)*.5f;
        for(auto& entry:layouts)if(entry.font==font&&entry.width==key&&entry.align==align&&entry.wrap==wrap&&entry.raw==raw&&entry.text==text){
            entry.used=frameIndex;return entry.layout;
        }
        const auto& format=formats[static_cast<size_t>(font)];
        ComPtr<IDWriteTextLayout> layout;
        if(FAILED(factory->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),format.Get(),raw?100000.f:key,4096.f,&layout)))return {};
        layout->SetTextAlignment(align==Align::Center?DWRITE_TEXT_ALIGNMENT_CENTER:align==Align::Trailing?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        if(!wrap&&!raw){
            ComPtr<IDWriteInlineObject> ellipsis;
            if(SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.Get(),&ellipsis))){
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
                layout->SetTrimming(&trimming,ellipsis.Get());
            }
        }
        if(font==Font::Overline){
            ComPtr<IDWriteTextLayout1> spaced;
            if(SUCCEEDED(layout.As(&spaced)))spaced->SetCharacterSpacing(0.f,1.8f,0.f,{0,static_cast<UINT32>(text.size())});
        }
        if(layouts.size()>=640){
            std::sort(layouts.begin(),layouts.end(),[](const auto& a,const auto& b){return a.used>b.used;});
            layouts.resize(480);
        }
        layouts.push_back({std::wstring(text),font,key,align,wrap,raw,layout,frameIndex});
        return layout;
    }
    static DWRITE_TEXT_METRICS Metrics(IDWriteTextLayout* layout) {
        DWRITE_TEXT_METRICS metrics{};
        if(!layout||FAILED(layout->GetMetrics(&metrics)))metrics={};
        return metrics;
    }
    float Animate(uint32_t id,int slot,float target,float rate,bool fromZero=false) {
        auto& anim=anims[(static_cast<uint64_t>(id)<<8)|static_cast<uint8_t>(slot)];
        if(!anim.frame)anim.value=fromZero?0.f:target;
        anim.frame=frameIndex;
        anim.value=Approach(anim.value,target,dt,frame.reducedMotion?rate*2.f:rate);
        if(std::abs(anim.value-target)<.002f)anim.value=target;else animating=true;
        return anim.value;
    }
    float Spring(uint32_t id,int slot,float target,float omega,float damping) {
        auto& anim=anims[(static_cast<uint64_t>(id)<<8)|static_cast<uint8_t>(slot)];
        if(!anim.frame||frame.reducedMotion){anim.value=target;anim.velocity=0;}
        anim.frame=frameIndex;
        float remaining=dt;
        while(remaining>0.f){
            const float h=std::min(remaining,.004f);remaining-=h;
            const float accel=-omega*omega*(anim.value-target)-2.f*damping*omega*anim.velocity;
            anim.velocity+=accel*h;anim.value+=anim.velocity*h;
        }
        if(std::abs(anim.value-target)<.0008f&&std::abs(anim.velocity)<.002f){anim.value=target;anim.velocity=0;}
        else animating=true;
        return anim.value;
    }
    bool TakeKey(UINT vk,bool* ctrl=nullptr,bool* shift=nullptr) {
        for(auto it=keys.begin();it!=keys.end();++it){
            if(it->kind!=Event::Key||it->vk!=vk)continue;
            if(ctrl)*ctrl=it->ctrl;if(shift)*shift=it->shift;
            keys.erase(it);return true;
        }
        return false;
    }
    void SetFocus(uint32_t id) {
        if(focused==id)return;
        if(focused&&textIds.count(focused))commitPending.insert(focused);
        focused=id;
    }
    void Tab(bool backwards) {
        if(focusOrder.empty())return;
        const auto it=std::find(focusOrder.begin(),focusOrder.end(),focused);
        size_t index=0;
        if(it==focusOrder.end())index=backwards?focusOrder.size()-1:0;
        else{
            const size_t current=static_cast<size_t>(it-focusOrder.begin()),n=focusOrder.size();
            index=backwards?(current+n-1)%n:(current+1)%n;
        }
        SetFocus(focusOrder[index]);focusVisible=true;revealFocus=true;
    }
    // At most one press and one release per frame; key and character events
    // stop at a Tab so later keys reach the widget focused by it next frame.
    void Consume() {
        down=up=doubleClick=pressClaimed=false;wheel=0;keys.clear();
        size_t used=0;bool sawDown=false,sawUp=false;
        for(const auto& e:queue){
            if(e.kind==Event::Down){
                if(sawDown||sawUp)break;
                sawDown=true;down=true;downAt={e.x,e.y};doubleClick=e.doubleClick;mouse=downAt;mouseInside=true;buttonHeld=true;
            }else if(e.kind==Event::Up){
                if(sawUp)break;
                sawUp=true;up=true;upAt={e.x,e.y};mouse=upAt;buttonHeld=false;
            }else if(e.kind==Event::Move){mouse={e.x,e.y};mouseInside=true;}
            else if(e.kind==Event::Leave){if(!buttonHeld)mouseInside=false;}
            else if(e.kind==Event::Wheel){wheel+=e.value;mouse={e.x,e.y};mouseInside=true;}
            else if(e.kind==Event::Key&&e.vk==VK_TAB){
                if(!keys.empty())break;
                Tab(e.shift);++used;break;
            }else keys.push_back(e);
            ++used;
        }
        queue.erase(queue.begin(),queue.begin()+static_cast<std::ptrdiff_t>(used));
    }
};

Context::Context():impl_(std::make_unique<Impl>()){}
Context::~Context()=default;

void Context::MouseMove(float x,float y) {
    auto& q=impl_->queue;
    if(!q.empty()&&q.back().kind==Impl::Event::Move){q.back().x=x;q.back().y=y;return;}
    Impl::Event e;e.kind=Impl::Event::Move;e.x=x;e.y=y;q.push_back(e);
}
void Context::MouseDown(float x,float y,bool doubleClick) {
    Impl::Event e;e.kind=Impl::Event::Down;e.x=x;e.y=y;e.doubleClick=doubleClick;impl_->queue.push_back(e);
}
void Context::MouseUp(float x,float y) {Impl::Event e;e.kind=Impl::Event::Up;e.x=x;e.y=y;impl_->queue.push_back(e);}
void Context::MouseLeave() {Impl::Event e;e.kind=Impl::Event::Leave;impl_->queue.push_back(e);}
void Context::Wheel(float x,float y,float notches) {
    if(!std::isfinite(notches)||notches==0.f)return;
    Impl::Event e;e.kind=Impl::Event::Wheel;e.x=x;e.y=y;e.value=notches;impl_->queue.push_back(e);
}
bool Context::KeyDown(UINT vk,bool ctrl,bool shift) {
    auto& s=*impl_;
    if(!s.focused)return false;
    const bool text=s.textIds.count(s.focused)>0;
    bool take=false;
    if(text)take=!(vk>=VK_F1&&vk<=VK_F24)&&vk!=VK_MENU&&vk!=VK_CONTROL&&vk!=VK_SHIFT;
    else switch(vk){
        case VK_TAB:case VK_RETURN:case VK_SPACE:case VK_LEFT:case VK_RIGHT:case VK_UP:case VK_DOWN:
        case VK_HOME:case VK_END:case VK_PRIOR:case VK_NEXT:take=!ctrl;break;
        case 'C':take=ctrl;break;
        default:break;
    }
    if(!take)return false;
    if(s.queue.size()>256)return true; // a stalled frame loop must not grow without bound
    Impl::Event e;e.kind=Impl::Event::Key;e.vk=vk;e.ctrl=ctrl;e.shift=shift;s.queue.push_back(e);
    return true;
}
void Context::Char(wchar_t ch) {
    auto& s=*impl_;
    if(!s.focused||!s.textIds.count(s.focused)||s.queue.size()>256)return;
    Impl::Event e;e.kind=Impl::Event::Char;e.ch=ch;s.queue.push_back(e);
}
void Context::FocusFirst() {
    auto& s=*impl_;
    if(s.focusOrder.empty())return;
    s.SetFocus(s.focusOrder.front());s.focusVisible=true;s.revealFocus=true;
}
void Context::ClearFocus() {impl_->SetFocus(0);impl_->focusVisible=false;}
bool Context::HitTest(float x,float y) const {
    for(const auto& area:impl_->hitAreas)if(area.Contains({x,y}))return true;
    return false;
}
bool Context::PointerCaptured() const {return impl_->active!=0&&impl_->buttonHeld;}
bool Context::WantsKeyboard() const {return impl_->focused!=0;}
bool Context::EditingText() const {return impl_->focused&&impl_->textIds.count(impl_->focused)>0;}
bool Context::CaretRect(Rect& caret) const {if(!impl_->caretValid)return false;caret=impl_->caret;return true;}
bool Context::Animating() const {return impl_->animating;}

void Context::Begin(const FrameInfo& info) {
    auto& s=*impl_;
    s.frame=info;
    s.frame.width=std::max(1.f,Finite(info.width,1.f));s.frame.height=std::max(1.f,Finite(info.height,1.f));
    s.dt=s.lastNow&&info.nowMs>=s.lastNow?std::clamp(static_cast<float>(info.nowMs-s.lastNow)/1000.f,0.f,.1f):.016f;
    s.lastNow=info.nowMs;++s.frameIndex;
    s.draw.Clear();s.clips.assign(1,Rect{0,0,s.frame.width,s.frame.height});s.scrollStack.clear();
    s.nextHitAreas.clear();s.nextFocusOrder.clear();s.nextFocusRects.clear();s.nextTextIds.clear();
    s.animating=false;s.caretValid=false;s.activeSeen=false;s.wheelTarget=0;
    s.lastId=0;s.lastHovered=false;s.tipPending=false;s.opacities.clear();
    s.Consume();
}
void Context::End() {
    auto& s=*impl_;
    s.opacities.clear();s.draw.SetOpacity(1.f);
    // Tooltip last, above everything.
    if(s.tipPending&&!s.tipText.empty()){
        const auto& t=s.frame.theme;
        auto layout=s.Layout(s.tipText,Font::Small,320.f,Align::Leading,true,false);
        const auto metrics=Impl::Metrics(layout.Get());
        const float w=std::min(320.f,metrics.widthIncludingTrailingWhitespace)+20.f,h=metrics.height+12.f;
        float x=std::clamp(s.tipRect.Center().x-w*.5f,8.f,std::max(8.f,s.frame.width-w-8.f));
        // Widgets in the lower half (the console) show their hint above.
        float y=s.tipRect.Center().y>s.frame.height*.5f?s.tipRect.top-8.f-h:s.tipRect.bottom+8.f;
        y=std::clamp(y,8.f,std::max(8.f,s.frame.height-h-8.f));
        const Rect bubble=Rect::Xywh(x,y,w,h);
        s.draw.Glass(bubble,8.f,Mix(t.glassTint,{0,0,0,.82f},.5f),t.glassRim,1.f);
        s.draw.Text(layout,{bubble.left+10.f,bubble.top+6.f},t.ink);
    }
    while(s.clips.size()>1){s.clips.pop_back();s.draw.PopClip();}
    if(s.active&&!s.activeSeen)s.active=0;
    // A press that no widget claimed clears keyboard focus (text commits).
    if(s.down&&!s.pressClaimed&&s.focused){s.SetFocus(0);s.focusVisible=false;}
    if(s.wheel!=0.f&&s.wheelTarget){
        auto& scroll=s.scrolls[s.wheelTarget];scroll.target-=s.wheel*56.f;
        scroll.target=std::clamp(scroll.target,0.f,std::max(0.f,scroll.content-scroll.viewport));
    }
    s.hitAreas.swap(s.nextHitAreas);s.focusOrder.swap(s.nextFocusOrder);s.focusRects.swap(s.nextFocusRects);s.textIds.swap(s.nextTextIds);
    if(s.focused&&std::find(s.focusOrder.begin(),s.focusOrder.end(),s.focused)==s.focusOrder.end()){
        // The focused widget disappeared (page change, disabled): drop focus.
        s.SetFocus(0);s.focusVisible=false;
    }
    s.revealFocus=false;
    std::erase_if(s.commitPending,[&](uint32_t id){return !s.textIds.count(id);});
    if(s.frameIndex%120==0){
        const auto stale=s.frameIndex>600?s.frameIndex-600:0;
        std::erase_if(s.anims,[&](const auto& entry){return entry.second.frame<stale;});
        std::erase_if(s.texts,[&](const auto& entry){return entry.second.frame<stale;});
        std::erase_if(s.scrolls,[&](const auto& entry){return entry.second.frame<stale;});
    }
}
const DrawList& Context::Output() const {return impl_->draw;}
DrawList& Context::Draw() {return impl_->draw;}
const FrameInfo& Context::Frame() const {return impl_->frame;}
void Context::SetTheme(const Theme& theme) {impl_->frame.theme=theme;}
float Context::DeltaSeconds() const {return impl_->dt;}

ComPtr<IDWriteTextLayout> Context::Layout(std::wstring_view text,Font font,float maxWidth,Align align,bool wrap) {
    return impl_->Layout(text,font,maxWidth,align,wrap,false);
}
float Context::TextWidth(std::wstring_view text,Font font) {
    if(text.empty())return 0.f;
    const auto metrics=Impl::Metrics(impl_->Layout(text,font,0.f,Align::Leading,false,true).Get());
    return metrics.widthIncludingTrailingWhitespace;
}
float Context::TextHeight(std::wstring_view text,Font font,float width) {
    if(text.empty())return 0.f;
    return Impl::Metrics(impl_->Layout(text,font,width,Align::Leading,true,false).Get()).height;
}
void Context::Label(Rect rect,std::wstring_view text,Font font,Color color,Align align,bool wrap) {
    if(text.empty()||rect.Width()<=1.f)return;
    auto layout=impl_->Layout(text,font,rect.Width(),align,wrap,false);
    if(!layout)return;
    float y=rect.top;
    if(!wrap){const auto metrics=Impl::Metrics(layout.Get());y=rect.top+(rect.Height()-metrics.height)*.5f;}
    impl_->draw.Text(layout,{rect.left,y},color);
}
void Context::Icon(Point center,wchar_t glyph,Font font,Color color) {
    const wchar_t text[2]={glyph,0};
    auto layout=impl_->Layout(std::wstring_view(text,1),font,64.f,Align::Center,false,false);
    if(!layout)return;
    const auto metrics=Impl::Metrics(layout.Get());
    impl_->draw.Text(layout,{center.x-32.f,center.y-metrics.height*.5f},color);
}

void Context::PushOpacity(float opacity) {
    auto& s=*impl_;s.opacities.push_back(s.draw.Opacity());
    s.draw.SetOpacity(s.draw.Opacity()*std::clamp(Finite(opacity,1.f),0.f,1.f));
}
void Context::PopOpacity() {
    auto& s=*impl_;if(s.opacities.empty())return;
    s.draw.SetOpacity(s.opacities.back());s.opacities.pop_back();
}
void Context::Block(Rect rect) {
    auto& s=*impl_;const auto visible=rect.Intersect(s.clips.back());
    if(!visible.Empty())s.nextHitAreas.push_back(visible);
}
Interaction Context::Interact(uint32_t id,Rect rect,uint32_t flags) {
    auto& s=*impl_;Interaction r;
    const bool disabled=(flags&FlagDisabled)!=0,pointer=(flags&FlagNoPointer)==0,focusable=!disabled&&(flags&FlagNoFocus)==0;
    const auto visible=rect.Intersect(s.clips.back());
    if(pointer&&!visible.Empty())s.nextHitAreas.push_back(visible);
    if(focusable){
        s.nextFocusOrder.push_back(id);
        s.nextFocusRects[id]={rect,s.scrollStack.empty()?0u:s.scrollStack.back().id};
    }
    if(s.active==id)s.activeSeen=true;
    if(pointer&&!disabled&&!visible.Empty()){
        r.hovered=s.mouseInside&&visible.Contains(s.mouse)&&(s.active==0||s.active==id);
        if(s.down&&!s.pressClaimed&&visible.Contains(s.downAt)){
            s.pressClaimed=true;s.active=id;s.activeSeen=true;r.pressed=true;r.doubleClicked=s.doubleClick;
            if(focusable){s.SetFocus(id);s.focusVisible=false;}
            else if(s.focused&&s.textIds.count(s.focused)){s.SetFocus(0);}
        }
        if(s.active==id){
            r.held=s.buttonHeld;
            if(s.up){r.clicked=visible.Contains(s.upAt);s.active=0;}
        }
    }else if(disabled&&s.active==id)s.active=0;
    if(disabled&&s.focused==id){s.SetFocus(0);}
    r.focused=s.focused==id;r.focusVisible=r.focused&&s.focusVisible;
    r.hover=s.Animate(id,0,r.hovered?1.f:0.f,16.f);
    r.press=s.Animate(id,1,(r.held||r.pressed)?1.f:0.f,28.f);
    r.focus=s.Animate(id,2,r.focusVisible?1.f:0.f,14.f);
    r.pointer=s.mouse;
    s.lastId=id;s.lastRect=visible;s.lastHovered=r.hovered;
    if(r.hovered){if(s.hoverId!=id){s.hoverId=id;s.hoverSince=s.frame.nowMs;}}
    else if(s.hoverId==id)s.hoverId=0;
    return r;
}
bool Context::Focused(uint32_t id) const {return impl_->focused==id;}
bool Context::Activate(uint32_t id) {
    auto& s=*impl_;
    return s.focused==id&&(s.TakeKey(VK_RETURN)||s.TakeKey(VK_SPACE));
}
float Context::Animate(uint32_t id,int slot,float target,float rate) {return impl_->Animate(id,slot,target,rate);}
float Context::Spring(uint32_t id,int slot,float target,float omega,float damping) {return impl_->Spring(id,slot,target,omega,damping);}
void Context::Tooltip(std::wstring_view text) {
    auto& s=*impl_;
    if(text.empty()||!s.lastHovered||s.hoverId!=s.lastId||s.buttonHeld)return;
    if(s.frame.nowMs-s.hoverSince<650){s.animating=true;return;}
    s.tipPending=true;s.tipText=std::wstring(text);s.tipRect=s.lastRect;
}
void Context::FocusRing(Rect rect,float radius,const Interaction& state) {
    if(state.focus<=.01f)return;
    impl_->draw.Stroke(rect.Inset(-3.f,-3.f),radius+3.f,WithAlpha(impl_->frame.theme.focus,state.focus),2.f);
}

bool Context::Button(uint32_t id,Rect rect,std::wstring_view text,ButtonKind kind,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;
    auto st=Interact(id,rect,flags);
    if(st.focused&&(s.TakeKey(VK_RETURN)||s.TakeKey(VK_SPACE)))st.clicked=true;
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.38f:1.f;
    const float radius=std::min(10.f,rect.Height()*.5f);
    Color fill{},ink=t.ink;
    if(kind==ButtonKind::Primary){
        fill=Mix(t.accent,{1,1,1,1},.12f*st.hover);fill=Mix(fill,{0,0,0,1},.12f*st.press);ink=t.onAccent;
    }else if(kind==ButtonKind::Secondary){
        fill=WithAlpha(t.field,1.f+1.2f*st.hover+.8f*st.press);
    }else{
        fill=WithAlpha(t.field,1.4f*st.hover+st.press);ink=Mix(t.accent,t.ink,.15f);
    }
    fill=WithAlpha(fill,alpha);
    s.draw.Fill(rect,radius,fill);
    if(kind==ButtonKind::Secondary)s.draw.Stroke(rect.Inset(.5f,.5f),radius,WithAlpha(t.glassRim,alpha*(1.f+.6f*st.hover)),1.f);
    Label(rect.Inset(10.f,0.f),text,Font::Strong,WithAlpha(ink,alpha),Align::Center);
    FocusRing(rect,radius,st);
    return st.clicked&&!disabled;
}
bool Context::IconButton(uint32_t id,Rect rect,wchar_t glyph,std::wstring_view tooltip,bool active,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;
    auto st=Interact(id,rect,flags);
    if(st.focused&&(s.TakeKey(VK_RETURN)||s.TakeKey(VK_SPACE)))st.clicked=true;
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.38f:1.f;
    const float radius=std::min(rect.Width(),rect.Height())*.5f;
    const float lit=s.Animate(id,3,active?1.f:0.f,12.f);
    s.draw.Fill(rect,radius,WithAlpha(t.field,(1.f+1.4f*st.hover+st.press)*alpha));
    if(lit>.01f){
        s.draw.Fill(rect,radius,WithAlpha(t.accent,.24f*lit*alpha));
        s.draw.Light(rect.Center(),rect.Width()*.9f,t.accent,.20f*lit*alpha);
    }
    Icon(rect.Center(),glyph,Font::Icon,WithAlpha(Mix(t.ink,t.accent,lit),alpha*(.82f+.18f*st.hover)));
    FocusRing(rect,radius,st);
    Tooltip(tooltip);
    return st.clicked&&!disabled;
}
bool Context::Switch(uint32_t id,Rect rect,std::wstring_view label,bool& value,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;
    auto st=Interact(id,rect,flags);
    if(st.focused&&(s.TakeKey(VK_RETURN)||s.TakeKey(VK_SPACE)))st.clicked=true;
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.38f:1.f;
    bool changed=false;
    if(st.clicked&&!disabled){value=!value;changed=true;}
    const float on=s.Spring(id,3,value?1.f:0.f,18.f,.72f);
    const float trackW=40.f,trackH=22.f;
    const Rect track=Rect::Xywh(rect.right-trackW,rect.Center().y-trackH*.5f,trackW,trackH);
    if(st.hover>.01f)s.draw.Fill(rect.Inset(-6.f,-2.f),10.f,WithAlpha(t.field,.8f*st.hover*alpha));
    Label(Rect{rect.left,rect.top,track.left-12.f,rect.bottom},label,Font::Body,WithAlpha(t.ink,alpha));
    const float lit=std::clamp(on,0.f,1.f);
    s.draw.Fill(track,trackH*.5f,WithAlpha(Mix(WithAlpha(t.ink,.16f),t.accent,lit),alpha));
    if(lit<.99f)s.draw.Stroke(track.Inset(.5f,.5f),trackH*.5f-.5f,WithAlpha(t.glassRim,(1.f-lit)*alpha),1.f);
    const float knob=16.f+2.f*st.press,travel=trackW-trackH;
    const Point center{track.left+trackH*.5f+travel*on,track.Center().y};
    if(lit>.01f)s.draw.Light(center,18.f,t.accent,.35f*lit*alpha);
    s.draw.Fill(Rect::Xywh(center.x-knob*.5f,center.y-knob*.5f,knob,knob),knob*.5f,WithAlpha(Mix({.92f,.92f,.92f,1},{1,1,1,1},lit),alpha));
    FocusRing(track,trackH*.5f,st);
    return changed;
}
bool Context::Slider(uint32_t id,Rect rect,float& value,float minimum,float maximum,float step,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;
    if(!(maximum>minimum)||!std::isfinite(value))value=minimum;
    auto st=Interact(id,rect,flags);
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.38f:1.f;
    const Rect track{rect.left+8.f,rect.Center().y-2.f,rect.right-8.f,rect.Center().y+2.f};
    const float before=value;
    const auto snap=[&](float v){
        v=std::clamp(v,minimum,maximum);
        if(step>0.f)v=minimum+std::round((v-minimum)/step)*step;
        return std::clamp(v,minimum,maximum);
    };
    if(!disabled&&(st.pressed||st.held)&&track.Width()>1.f)
        value=snap(minimum+(st.pointer.x-track.left)/track.Width()*(maximum-minimum));
    if(st.focused&&!disabled){
        const float unit=step>0.f?step:(maximum-minimum)/100.f;
        if(s.TakeKey(VK_LEFT)||s.TakeKey(VK_DOWN))value=snap(value-unit);
        if(s.TakeKey(VK_RIGHT)||s.TakeKey(VK_UP))value=snap(value+unit);
        if(s.TakeKey(VK_PRIOR))value=snap(value+unit*10.f);
        if(s.TakeKey(VK_NEXT))value=snap(value-unit*10.f);
        if(s.TakeKey(VK_HOME))value=minimum;
        if(s.TakeKey(VK_END))value=maximum;
    }
    const float shown=s.Animate(id,3,(value-minimum)/(maximum-minimum),st.held?60.f:22.f);
    const float x=track.left+track.Width()*std::clamp(shown,0.f,1.f);
    s.draw.Fill(track,2.f,WithAlpha(t.ink,.16f*alpha));
    s.draw.Fill(Rect{track.left,track.top,x,track.bottom},2.f,WithAlpha(t.accent,alpha));
    const float knob=12.f+4.f*std::max(st.hover,st.press);
    s.draw.Light({x,track.Center().y},16.f+6.f*st.press,t.accent,(.18f+.25f*st.hover)*alpha);
    s.draw.Fill(Rect::Xywh(x-knob*.5f,track.Center().y-knob*.5f,knob,knob),knob*.5f,WithAlpha({1,1,1,1},alpha));
    FocusRing(Rect{track.left-6.f,rect.top+4.f,track.right+6.f,rect.bottom-4.f},8.f,st);
    return value!=before;
}
bool Context::Segmented(uint32_t id,Rect rect,const std::vector<std::wstring>& options,int& index,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;
    if(options.empty())return false;
    const int count=static_cast<int>(options.size());
    const int before=index;index=std::clamp(index,0,count-1);
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.38f:1.f;
    auto st=Interact(id,rect,flags|FlagNoPointer);
    if(st.focused&&!disabled){
        if(s.TakeKey(VK_LEFT))index=std::max(0,index-1);
        if(s.TakeKey(VK_RIGHT))index=std::min(count-1,index+1);
        if(s.TakeKey(VK_HOME))index=0;
        if(s.TakeKey(VK_END))index=count-1;
    }
    const float radius=rect.Height()*.5f,width=rect.Width()/static_cast<float>(count);
    s.draw.Fill(rect,radius,WithAlpha(t.field,1.2f*alpha));
    s.draw.Stroke(rect.Inset(.5f,.5f),radius,WithAlpha(t.glassRim,alpha),1.f);
    for(int i=0;i<count;++i){
        const Rect cell=Rect::Xywh(rect.left+width*static_cast<float>(i),rect.top,width,rect.Height());
        auto c=Interact(id^(0x9E3779B9u*static_cast<uint32_t>(i+1)),cell,flags|FlagNoFocus);
        if(c.pressed&&!disabled){index=i;s.SetFocus(id);s.focusVisible=false;}
        if(c.hover>.01f&&i!=index)s.draw.Fill(cell.Inset(3.f,3.f),radius-3.f,WithAlpha(t.field,c.hover*alpha));
    }
    const float position=s.Spring(id,3,static_cast<float>(index),16.f,.78f);
    const Rect pill=Rect::Xywh(rect.left+width*position+3.f,rect.top+3.f,width-6.f,rect.Height()-6.f);
    s.draw.Fill(pill,radius-3.f,WithAlpha(t.accent,alpha));
    s.draw.Light(pill.Center(),width*.55f,t.accent,.16f*alpha);
    for(int i=0;i<count;++i){
        const Rect cell=Rect::Xywh(rect.left+width*static_cast<float>(i),rect.top,width,rect.Height());
        const float on=std::clamp(1.f-std::abs(position-static_cast<float>(i)),0.f,1.f);
        Label(cell.Inset(6.f,0.f),options[static_cast<size_t>(i)],Font::Strong,WithAlpha(Mix(t.muted,t.onAccent,on),alpha),Align::Center);
    }
    FocusRing(rect,radius,st);
    return index!=before;
}
TextEvent Context::TextBox(uint32_t id,Rect rect,std::wstring& text,std::wstring_view placeholder,size_t maxLength,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;TextEvent event;
    auto& state=s.texts[id];state.frame=s.frameIndex;
    s.nextTextIds.insert(id);
    if(text.size()>maxLength)text.resize(maxLength);
    state.caret=std::min(state.caret,text.size());state.anchor=std::min(state.anchor,text.size());
    if(s.commitPending.erase(id))event.committed=true;
    auto st=Interact(id,rect,flags);
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.38f:1.f;
    const Rect inner=rect.Inset(12.f,0.f);
    auto layout=s.Layout(text,Font::Body,0.f,Align::Leading,false,true);
    const auto caretX=[&](size_t position){
        if(!layout||text.empty())return 0.f;
        FLOAT x=0,y=0;DWRITE_HIT_TEST_METRICS metrics{};
        if(FAILED(layout->HitTestTextPosition(static_cast<UINT32>(std::min(position,text.size())),FALSE,&x,&y,&metrics)))return 0.f;
        return x;
    };
    const auto positionAt=[&](float x){
        if(!layout||text.empty())return size_t{0};
        BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS metrics{};
        if(FAILED(layout->HitTestPoint(x-inner.left+state.scroll,4.f,&trailing,&inside,&metrics)))return text.size();
        size_t position=metrics.textPosition+(trailing?metrics.length:0);
        return std::min(position,text.size());
    };
    if(!disabled){
        if(st.pressed){
            const size_t at=positionAt(st.pointer.x);
            if(st.doubleClicked){state.anchor=0;state.caret=text.size();}
            else{state.caret=at;if(!(GetKeyState(VK_SHIFT)&0x8000))state.anchor=at;}
            state.dragging=!st.doubleClicked;state.blinkAt=s.frame.nowMs;
        }else if(st.held&&state.dragging){state.caret=positionAt(st.pointer.x);}
        if(!st.held)state.dragging=false;
    }
    const auto replaceSelection=[&](std::wstring_view insert){
        const size_t from=std::min(state.caret,state.anchor),to=std::max(state.caret,state.anchor);
        std::wstring next=text.substr(0,from);
        const size_t room=maxLength>next.size()+(text.size()-to)?maxLength-next.size()-(text.size()-to):0;
        std::wstring_view clipped=insert.substr(0,std::min(insert.size(),room));
        if(!clipped.empty()&&HighSurrogate(clipped.back()))clipped.remove_suffix(1);
        next.append(clipped);next.append(text.substr(to));
        if(next!=text){text=std::move(next);event.changed=true;}
        state.caret=state.anchor=from+clipped.size();
    };
    if(st.focused&&!disabled){
        state.blinkAt=state.blinkAt?state.blinkAt:s.frame.nowMs;
        std::vector<Impl::Event> pending;pending.swap(s.keys);
        for(const auto& e:pending){
            if(e.kind==Impl::Event::Char){
                const wchar_t ch=e.ch;
                if(ch<0x20||ch==0x7F)continue; // control characters arrive as keys
                // A supplementary character arrives as two WM_CHAR halves.
                if(HighSurrogate(ch)){state.high=ch;continue;}
                if(LowSurrogate(ch)){
                    if(state.high){const wchar_t pair[2]={state.high,ch};replaceSelection(std::wstring_view(pair,2));}
                    state.high=0;state.blinkAt=s.frame.nowMs;continue;
                }
                state.high=0;
                const wchar_t one[1]={ch};replaceSelection(std::wstring_view(one,1));state.blinkAt=s.frame.nowMs;continue;
            }
            const bool shift=e.shift,ctrl=e.ctrl;
            const auto move=[&](size_t to){state.caret=to;if(!shift)state.anchor=to;state.blinkAt=s.frame.nowMs;};
            switch(e.vk){
                case VK_LEFT:
                    if(!shift&&state.caret!=state.anchor)move(std::min(state.caret,state.anchor));
                    else move(ctrl?WordLeft(text,state.caret):PreviousStop(text,state.caret));break;
                case VK_RIGHT:
                    if(!shift&&state.caret!=state.anchor)move(std::max(state.caret,state.anchor));
                    else move(ctrl?WordRight(text,state.caret):NextStop(text,state.caret));break;
                case VK_HOME:move(0);break;
                case VK_END:move(text.size());break;
                case VK_BACK:
                    if(state.caret==state.anchor)state.anchor=ctrl?WordLeft(text,state.caret):PreviousStop(text,state.caret);
                    replaceSelection({});state.blinkAt=s.frame.nowMs;break;
                case VK_DELETE:
                    if(state.caret==state.anchor)state.anchor=ctrl?WordRight(text,state.caret):NextStop(text,state.caret);
                    replaceSelection({});state.blinkAt=s.frame.nowMs;break;
                case 'A':if(ctrl){state.anchor=0;state.caret=text.size();}break;
                case 'C':case 'X':
                    if(ctrl&&state.caret!=state.anchor){
                        const size_t from=std::min(state.caret,state.anchor),to=std::max(state.caret,state.anchor);
                        if(CopyToClipboard(s.frame.window,std::wstring_view(text).substr(from,to-from))&&e.vk=='X')replaceSelection({});
                    }
                    break;
                case 'V':
                    if(ctrl){
                        auto pasted=PasteFromClipboard(s.frame.window);
                        std::erase_if(pasted,[](wchar_t c){return c==L'\r'||c==L'\n'||c==L'\t'||c<0x20;});
                        replaceSelection(pasted);
                    }
                    break;
                case VK_RETURN:event.submitted=true;break;
                case VK_ESCAPE:s.SetFocus(0);s.focusVisible=false;event.committed=true;break;
                default:break;
            }
        }
        layout=s.Layout(text,Font::Body,0.f,Align::Leading,false,true);
    }
    // Field.
    const float radius=10.f;
    s.draw.Fill(rect,radius,WithAlpha(t.field,(1.4f+.6f*st.hover)*alpha));
    const float focusLine=s.Animate(id,4,st.focused?1.f:0.f,16.f);
    s.draw.Stroke(rect.Inset(.5f,.5f),radius,WithAlpha(Mix(t.glassRim,t.accent,focusLine),alpha*(1.f+focusLine)),1.f);
    // Keep the caret visible.
    const float caretPixel=caretX(state.caret);
    if(caretPixel-state.scroll>inner.Width()-2.f)state.scroll=caretPixel-inner.Width()+2.f;
    if(caretPixel-state.scroll<0.f)state.scroll=caretPixel;
    const float textWidth=layout?Impl::Metrics(layout.Get()).widthIncludingTrailingWhitespace:0.f;
    state.scroll=std::clamp(state.scroll,0.f,std::max(0.f,textWidth-inner.Width()+2.f));
    s.draw.PushClip(inner);
    const float lineHeight=layout?std::max(14.f,Impl::Metrics(layout.Get()).height):18.f;
    const float textTop=rect.Center().y-lineHeight*.5f;
    if(st.focused&&state.caret!=state.anchor){
        const float a=caretX(std::min(state.caret,state.anchor))-state.scroll,b=caretX(std::max(state.caret,state.anchor))-state.scroll;
        s.draw.Fill(Rect{inner.left+a,textTop,inner.left+b,textTop+lineHeight},2.f,WithAlpha(t.accent,.35f));
    }
    if(text.empty())Label(inner,placeholder,Font::Body,WithAlpha(t.faint,alpha));
    else if(layout)s.draw.Text(layout,{inner.left-state.scroll,textTop},WithAlpha(t.ink,alpha));
    if(st.focused){
        const float x=inner.left+caretPixel-state.scroll;
        s.caretValid=true;s.caret=Rect{x,textTop,x+1.5f,textTop+lineHeight};
        const bool visible=((s.frame.nowMs-state.blinkAt)/530)%2==0;
        if(visible)s.draw.Fill(s.caret,0.f,t.ink);
        s.animating=true; // blinking caret
    }
    s.draw.PopClip();
    FocusRing(rect,radius,st);
    return event;
}
ListEvent Context::List(uint32_t id,Rect rect,const std::vector<ListRow>& rows,int& selected,uint32_t flags) {
    auto& s=*impl_;const auto& t=s.frame.theme;ListEvent event;
    const int count=static_cast<int>(rows.size());
    if(selected>=count)selected=count-1;
    const bool disabled=(flags&FlagDisabled)!=0;const float alpha=disabled?.45f:1.f;
    auto st=Interact(id,rect,flags|FlagNoPointer);
    const float rowHeight=54.f;
    const int before=selected;
    if(st.focused&&!disabled&&count>0){
        if(s.TakeKey(VK_UP))selected=std::max(0,selected-1);
        if(s.TakeKey(VK_DOWN))selected=std::min(count-1,selected<0?0:selected+1);
        if(s.TakeKey(VK_HOME))selected=0;
        if(s.TakeKey(VK_END))selected=count-1;
        const int page=std::max(1,static_cast<int>(rect.Height()/rowHeight)-1);
        if(s.TakeKey(VK_PRIOR))selected=std::max(0,(selected<0?0:selected)-page);
        if(s.TakeKey(VK_NEXT))selected=std::min(count-1,(selected<0?0:selected)+page);
        if(selected>=0&&(s.TakeKey(VK_RETURN)||s.TakeKey(VK_SPACE)))event.activated=true;
    }
    s.draw.Fill(rect,12.f,WithAlpha(t.field,1.2f*alpha));
    s.draw.Stroke(rect.Inset(.5f,.5f),12.f,WithAlpha(t.glassRim,alpha),1.f);
    auto& scroll=s.scrolls[id^0x51F15EEDu];
    if(selected!=before&&selected>=0){
        // Keyboard moves keep the selected row inside the viewport.
        const float top=static_cast<float>(selected)*rowHeight,bottom=top+rowHeight;
        if(top<scroll.target)scroll.target=top;
        if(bottom>scroll.target+rect.Height()-8.f)scroll.target=bottom-rect.Height()+8.f;
    }
    const float offset=-BeginScroll(id^0x51F15EEDu,rect.Inset(4.f,4.f));
    const Rect viewport=rect.Inset(4.f,4.f);
    for(int i=0;i<count;++i){
        const Rect row=Rect::Xywh(viewport.left,viewport.top+static_cast<float>(i)*rowHeight-offset,viewport.Width(),rowHeight);
        if(row.bottom<viewport.top||row.top>viewport.bottom)continue;
        const auto& r=rows[static_cast<size_t>(i)];
        auto c=Interact(id^(0x85EBCA6Bu*static_cast<uint32_t>(i+1)),row,flags|FlagNoFocus);
        if(c.pressed&&!disabled){selected=i;s.SetFocus(id);s.focusVisible=false;}
        if(c.doubleClicked&&!disabled){selected=i;event.activated=true;}
        const bool on=i==selected;
        const float lit=s.Animate(id^(0x27D4EB2Fu*static_cast<uint32_t>(i+1)),5,on?1.f:0.f,14.f);
        if(c.hover>.01f||lit>.01f)s.draw.Fill(row.Inset(2.f,2.f),9.f,WithAlpha(Mix(t.field,WithAlpha(t.accent,.26f),lit),(1.2f*c.hover+lit)*alpha));
        if(lit>.01f)s.draw.Fill(Rect::Xywh(row.left+3.f,row.top+12.f,3.f,rowHeight-24.f),1.5f,WithAlpha(t.accent,lit*alpha));
        Label(Rect{row.left+16.f,row.top+7.f,row.right-12.f,row.top+28.f},r.title,Font::Strong,WithAlpha(t.ink,alpha));
        Label(Rect{row.left+16.f,row.top+28.f,row.right-12.f,row.bottom-6.f},r.subtitle,Font::Small,
            WithAlpha(r.flagged?t.danger:t.muted,alpha));
    }
    EndScroll(static_cast<float>(count)*rowHeight);
    FocusRing(rect,12.f,st);
    if(selected!=before&&selected>=0)event.selected=true;
    return event;
}
void Context::TextBlock(uint32_t id,Rect rect,std::wstring_view text,Font font,Color color) {
    auto& s=*impl_;const auto& t=s.frame.theme;
    auto st=Interact(id,rect,FlagNone);
    if(st.focused){
        bool ctrl=false;
        if(s.TakeKey('C',&ctrl)&&ctrl)CopyToClipboard(s.frame.window,text);
        auto& scroll=s.scrolls[id^0x7E7B10C4u];
        if(s.TakeKey(VK_UP))scroll.target-=40.f;
        if(s.TakeKey(VK_DOWN))scroll.target+=40.f;
        if(s.TakeKey(VK_PRIOR))scroll.target-=rect.Height()*.85f;
        if(s.TakeKey(VK_NEXT))scroll.target+=rect.Height()*.85f;
        if(s.TakeKey(VK_HOME))scroll.target=0.f;
        if(s.TakeKey(VK_END))scroll.target=1e9f;
    }
    s.draw.Fill(rect,12.f,WithAlpha(t.field,1.1f));
    const Rect viewport=rect.Inset(12.f,10.f);
    const float y=BeginScroll(id^0x7E7B10C4u,viewport);
    auto layout=s.Layout(text,font,viewport.Width()-8.f,Align::Leading,true,false);
    const float height=layout?Impl::Metrics(layout.Get()).height:0.f;
    if(layout)s.draw.Text(layout,{viewport.left,viewport.top+y},color);
    EndScroll(height+4.f);
    FocusRing(rect,12.f,st);
}

float Context::BeginScroll(uint32_t id,Rect viewport) {
    auto& s=*impl_;
    auto& scroll=s.scrolls[id];scroll.frame=s.frameIndex;scroll.viewport=viewport.Height();
    const auto visible=viewport.Intersect(s.clips.back());
    Impl::ScrollFrame frame{id,viewport,0.f};
    // The scrollbar is declared before the content so it claims presses in
    // its lane; its extent comes from the last frame's content height.
    const float height=viewport.Height(),overflow=scroll.content-height;
    if(overflow>1.f&&height>1.f){
        const float thumbHeight=std::max(28.f,height*height/scroll.content);
        const float thumbTop=viewport.top+(height-thumbHeight)*std::clamp(scroll.offset/overflow,0.f,1.f);
        auto st=Interact(id^0x5C5C5C5Cu,Rect{viewport.right-10.f,viewport.top,viewport.right,viewport.bottom},FlagNoFocus);
        if(st.pressed){
            if(st.pointer.y<thumbTop||st.pointer.y>thumbTop+thumbHeight)
                scroll.target=std::clamp((st.pointer.y-viewport.top-thumbHeight*.5f)/std::max(1.f,height-thumbHeight)*overflow,0.f,overflow);
            scroll.dragY=st.pointer.y;scroll.dragOffset=scroll.target;
        }else if(st.held&&height>thumbHeight){
            scroll.target=std::clamp(scroll.dragOffset+(st.pointer.y-scroll.dragY)/(height-thumbHeight)*overflow,0.f,overflow);
        }
        frame.bar=true;frame.barLight=std::max(st.hover,st.press);
    }
    if(s.mouseInside&&visible.Contains(s.mouse))s.wheelTarget=id; // the innermost region is declared last
    if(s.revealFocus&&s.focused){
        const auto found=s.focusRects.find(s.focused);
        if(found!=s.focusRects.end()&&found->second.scroll==id){
            // Recorded last frame in shifted coordinates; undo that shift.
            const float top=found->second.rect.top-viewport.top+scroll.offset,bottom=top+found->second.rect.Height();
            if(top<scroll.target+8.f)scroll.target=top-16.f;
            else if(bottom>scroll.target+height-8.f)scroll.target=bottom-height+16.f;
        }
    }
    scroll.target=std::clamp(scroll.target,0.f,std::max(0.f,scroll.content-height));
    scroll.offset=s.frame.reducedMotion?scroll.target:Approach(scroll.offset,scroll.target,s.dt,18.f);
    if(std::abs(scroll.offset-scroll.target)<.3f)scroll.offset=scroll.target;else s.animating=true;
    frame.offset=scroll.offset;
    s.clips.push_back(visible);s.draw.PushClip(visible);
    Block(viewport);
    s.scrollStack.push_back(frame);
    return -scroll.offset;
}
void Context::EndScroll(float contentHeight) {
    auto& s=*impl_;
    if(s.scrollStack.empty())return;
    const auto frame=s.scrollStack.back();s.scrollStack.pop_back();
    if(s.clips.size()>1){s.clips.pop_back();s.draw.PopClip();}
    auto& scroll=s.scrolls[frame.id];
    scroll.content=std::max(0.f,Finite(contentHeight));
    const float height=frame.viewport.Height(),overflow=scroll.content-height;
    if(!frame.bar||overflow<=1.f||height<=1.f)return;
    const auto& t=s.frame.theme;
    const float thumbHeight=std::max(28.f,height*height/scroll.content);
    const float thumbTop=frame.viewport.top+(height-thumbHeight)*std::clamp(scroll.offset/overflow,0.f,1.f);
    const float width=3.f+2.f*frame.barLight;
    s.draw.Fill(Rect{frame.viewport.right-width-2.f,thumbTop,frame.viewport.right-2.f,thumbTop+thumbHeight},width*.5f,
        WithAlpha(t.ink,.22f+.25f*frame.barLight));
}

uint32_t Context::Id(std::string_view name,uint32_t index) {
    uint32_t hash=2166136261u;
    for(const char c:name){hash^=static_cast<uint8_t>(c);hash*=16777619u;}
    hash^=index+0x9E3779B9u+(hash<<6)+(hash>>2);
    return hash?hash:1u;
}
}
