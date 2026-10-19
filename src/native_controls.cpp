#include "native_controls.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace stage {
namespace {
ThemeColor Blend(ThemeColor a,ThemeColor b,float amount){return {a.r+(b.r-a.r)*amount,a.g+(b.g-a.g)*amount,a.b+(b.b-a.b)*amount,1};}
COLORREF Rgb(ThemeColor c){return RGB((BYTE)std::clamp(c.r*255,0.f,255.f),(BYTE)std::clamp(c.g*255,0.f,255.f),(BYTE)std::clamp(c.b*255,0.f,255.f));}
float Luminance(ThemeColor c){auto linear=[](float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);};return .2126f*linear(c.r)+.7152f*linear(c.g)+.0722f*linear(c.b);}
ThemeColor Readable(ThemeColor ink,ThemeColor paper){float a=Luminance(ink),b=Luminance(paper);if((std::max(a,b)+.05f)/(std::min(a,b)+.05f)>=4.5f)return ink;return b>.179f?ThemeRgb(0x202020):ThemeRgb(0xF8F8F8);}
}
struct NativeControls::Impl {
    struct BackBuffer {
        HDC memory=nullptr;HBITMAP bitmap=nullptr,previous=nullptr;int width=0,height=0;
        BackBuffer()=default;
        BackBuffer(const BackBuffer&)=delete;
        BackBuffer& operator=(const BackBuffer&)=delete;
        void ReleaseBuffer(){if(memory){if(previous)SelectObject(memory,previous);if(bitmap)DeleteObject(bitmap);DeleteDC(memory);}memory=nullptr;bitmap=previous=nullptr;width=height=0;}
        ~BackBuffer(){ReleaseBuffer();}
        HDC Acquire(HDC dc,int w,int h){
            if(!dc||w<=0||h<=0)return nullptr;
            if(memory&&width==w&&height==h)return memory;
            const auto nextMemory=CreateCompatibleDC(dc);if(!nextMemory)return nullptr;
            const auto nextBitmap=CreateCompatibleBitmap(dc,w,h);
            if(!nextBitmap){DeleteDC(nextMemory);return nullptr;}
            const auto nextPrevious=SelectObject(nextMemory,nextBitmap);
            if(!nextPrevious||nextPrevious==HGDI_ERROR){DeleteObject(nextBitmap);DeleteDC(nextMemory);return nullptr;}
            // A failed replacement must never be published as a usable buffer.
            ReleaseBuffer();memory=nextMemory;bitmap=nextBitmap;
            previous=static_cast<HBITMAP>(nextPrevious);width=w;height=h;return memory;
        }
    };
    struct Button {
        HWND window=nullptr;bool hover=false,pressed=false,skinInstalled=false;
        float hoverAmount=0,pressAmount=0;LONG_PTR originalStyle=0;BackBuffer paint;
    };
    struct Tabs {
        HWND window=nullptr;Impl* owner=nullptr;int hot=-1,itemWidth=0,itemHeight=0;BackBuffer paint;
    };
    struct Combo {
        HWND window=nullptr;Impl* owner=nullptr;bool hover=false;float hoverAmount=0;BackBuffer paint;
    };
    std::vector<std::unique_ptr<Button>> buttons;std::vector<std::unique_ptr<Tabs>> tabs;std::vector<std::unique_ptr<Combo>> combos;
    ThemeDefinition theme=BuiltInTheme(2);int64_t lastFrame=0;DWORD uiThread=GetCurrentThreadId();
    static bool PaintCombo(Combo& control,HDC target){
        RECT client{};if(!target||!GetClientRect(control.window,&client)||client.right<=0||client.bottom<=0)return false;
        auto dc=control.paint.Acquire(target,client.right,client.bottom);const bool buffered=dc!=nullptr;if(!dc)dc=target;
        const int saved=SaveDC(dc);if(!saved)return false;
        const auto& theme=control.owner->theme;const UINT reportedDpi=GetDpiForWindow(control.window),dpi=reportedDpi?reportedDpi:96;
        const int inset=std::max(1,MulDiv(2,dpi,96)),radius=std::max(2,MulDiv(9,dpi,96));
        const bool enabled=IsWindowEnabled(control.window)!=FALSE,focus=enabled&&GetFocus()==control.window;
        const bool open=SendMessageW(control.window,CB_GETDROPPEDSTATE,0,0)!=FALSE;
        const float response=enabled?(.08f*control.hoverAmount+(open?.06f:0.f))*theme.accent.a:0.f;
        const auto face=Blend(Blend(theme.panel,theme.background,.62f),theme.accent,response);
        SelectObject(dc,GetStockObject(DC_BRUSH));SelectObject(dc,GetStockObject(DC_PEN));
        SetDCBrushColor(dc,Rgb(theme.panel));FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc,Rgb(face));SetDCPenColor(dc,Rgb(Blend(theme.panel,focus?theme.accent:theme.ink,focus?.75f:.18f+.22f*control.hoverAmount)));
        RoundRect(dc,0,0,client.right,client.bottom,radius,radius);
        COMBOBOXINFO info{sizeof(info)};RECT arrow{std::max<LONG>(0,client.right-MulDiv(25,dpi,96)),0,client.right,client.bottom};
        if(GetComboBoxInfo(control.window,&info)&&info.rcButton.left>=0&&info.rcButton.right<=client.right&&
                info.rcButton.top>=0&&info.rcButton.bottom<=client.bottom&&info.rcButton.right>info.rcButton.left&&info.rcButton.bottom>info.rcButton.top)
            arrow=info.rcButton;
        const auto ink=enabled?Readable(theme.ink,face):Blend(Readable(theme.ink,face),face,.45f);
        const auto font=reinterpret_cast<HFONT>(SendMessageW(control.window,WM_GETFONT,0,0));
        SelectObject(dc,font?reinterpret_cast<HGDIOBJ>(font):GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,Rgb(ink));
        wchar_t label[2049]{};GetWindowTextW(control.window,label,static_cast<int>(std::size(label)));
        RECT text{MulDiv(10,dpi,96),0,std::max<LONG>(0,arrow.left-MulDiv(5,dpi,96)),client.bottom};
        if(text.right>text.left)DrawTextW(dc,label,-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        const LONG centerX=(arrow.left+arrow.right)/2,centerY=(arrow.top+arrow.bottom)/2;
        const LONG half=std::max(2,MulDiv(3,dpi,96)),rise=std::max(1,MulDiv(2,dpi,96));
        POINT points[3]{{centerX-half,centerY+(open?rise:-rise)},{centerX+half,centerY+(open?rise:-rise)},{centerX,centerY+(open?-rise:rise)}};
        SetDCBrushColor(dc,Rgb(ink));SetDCPenColor(dc,Rgb(ink));Polygon(dc,points,3);
        if(focus&&!(SendMessageW(control.window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)){
            RECT ring=client;InflateRect(&ring,-2*inset,-2*inset);if(ring.right>ring.left&&ring.bottom>ring.top)DrawFocusRect(dc,&ring);
        }
        RestoreDC(dc,saved);if(buffered)BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);return true;
    }
    static LRESULT CALLBACK ComboSubclass(HWND h,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR reference){
        auto& control=*reinterpret_cast<Combo*>(reference);
        switch(message){
            case WM_PAINT:{
                if(wp){if(PaintCombo(control,reinterpret_cast<HDC>(wp)))return 0;return DefSubclassProc(h,message,wp,lp);}
                PAINTSTRUCT paint{};const auto dc=BeginPaint(h,&paint);
                if(!PaintCombo(control,dc)&&dc)DefSubclassProc(h,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
                EndPaint(h,&paint);return 0;
            }
            case WM_PRINTCLIENT:if(PaintCombo(control,reinterpret_cast<HDC>(wp)))return 0;return DefSubclassProc(h,message,wp,lp);
            case WM_ERASEBKGND:return 1;
            case WM_MOUSEMOVE:if(!control.hover){control.hover=true;TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,nullptr,FALSE);}break;
            case WM_MOUSELEAVE:control.hover=false;InvalidateRect(h,nullptr,FALSE);break;
            case WM_ENABLE:if(!wp){control.hover=false;control.hoverAmount=0;}break;
            case WM_SHOWWINDOW:if(!wp){control.hover=false;control.hoverAmount=0;}break;
            case WM_NCDESTROY:RemoveWindowSubclass(h,ComboSubclass,id);control.window=nullptr;return DefSubclassProc(h,message,wp,lp);
        }
        // The standard combo owns input, popup, strings, selection, focus and
        // accessibility. Painting never sends a selection or notification.
        const auto result=DefSubclassProc(h,message,wp,lp);
        switch(message){
            case WM_SIZE:case WM_SETFONT:case WM_DPICHANGED_AFTERPARENT:case WM_SETFOCUS:case WM_KILLFOCUS:
            case WM_ENABLE:case WM_SHOWWINDOW:case WM_THEMECHANGED:case WM_UPDATEUISTATE:case WM_SETTEXT:
            case WM_KEYDOWN:case WM_KEYUP:case WM_SYSKEYDOWN:case WM_SYSKEYUP:case WM_CHAR:
            case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:case WM_MOUSEWHEEL:case WM_CANCELMODE:case WM_CAPTURECHANGED:
            case CB_SETCURSEL:case CB_SELECTSTRING:case CB_ADDSTRING:case CB_INSERTSTRING:case CB_DELETESTRING:case CB_RESETCONTENT:case CB_SHOWDROPDOWN:
                if(control.window)InvalidateRect(control.window,nullptr,FALSE);break;
        }
        return result;
    }
    static void SizeTabs(Tabs& control){
        RECT client{};if(!GetClientRect(control.window,&client))return;
        const int count=TabCtrl_GetItemCount(control.window);if(count<=0||client.right<=0)return;
        const auto dpi=GetDpiForWindow(control.window);
        const int width=std::max(1,(static_cast<int>(client.right)-MulDiv(6,dpi,96))/count),height=std::max(1,MulDiv(27,dpi,96));
        if(width==control.itemWidth&&height==control.itemHeight)return;
        control.itemWidth=width;control.itemHeight=height;TabCtrl_SetItemSize(control.window,width,height);
    }
    static void PaintTabs(Tabs& control,HDC target){
        RECT client{};if(!target||!GetClientRect(control.window,&client)||client.right<=0||client.bottom<=0)return;
        auto dc=control.paint.Acquire(target,client.right,client.bottom);const bool buffered=dc!=nullptr;if(!dc)dc=target;
        const int saved=SaveDC(dc);if(!saved)return;
        const auto& theme=control.owner->theme;const auto dpi=GetDpiForWindow(control.window);
        const int inset=std::max(1,MulDiv(2,dpi,96)),radius=std::max(2,MulDiv(8,dpi,96));
        SelectObject(dc,GetStockObject(DC_BRUSH));SelectObject(dc,GetStockObject(DC_PEN));
        SetDCBrushColor(dc,Rgb(theme.panel));FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const auto rail=Blend(theme.panel,theme.background,.25f);SetDCBrushColor(dc,Rgb(rail));SetDCPenColor(dc,Rgb(rail));
        RoundRect(dc,0,0,client.right,client.bottom,radius,radius);
        const int selected=TabCtrl_GetCurSel(control.window),focused=TabCtrl_GetCurFocus(control.window);
        const bool enabled=IsWindowEnabled(control.window)!=FALSE;
        const bool showFocus=GetFocus()==control.window&&!(SendMessageW(control.window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS);
        auto font=reinterpret_cast<HFONT>(SendMessageW(control.window,WM_GETFONT,0,0));
        SelectObject(dc,font?reinterpret_cast<HGDIOBJ>(font):GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);
        const int count=TabCtrl_GetItemCount(control.window);
        for(int i=0;i<count;i++){
            RECT item{};if(!TabCtrl_GetItemRect(control.window,i,&item))continue;
            item.left=std::max<LONG>(inset,item.left+inset);item.right=std::min(client.right-inset,item.right-inset);
            item.top=inset;item.bottom=client.bottom-inset;if(item.right<=item.left||item.bottom<=item.top)continue;
            const bool active=i==selected;const float amount=enabled?(active?.18f:i==control.hot?.08f:0.f):0.f;
            const auto face=Blend(rail,theme.accent,amount);
            SetDCBrushColor(dc,Rgb(face));SetDCPenColor(dc,Rgb(active?Blend(face,theme.accent,.55f):face));
            RoundRect(dc,item.left,item.top,item.right,item.bottom,radius,radius);
            wchar_t label[128]{};TCITEMW value{};value.mask=TCIF_TEXT;value.pszText=label;value.cchTextMax=static_cast<int>(std::size(label));
            if(TabCtrl_GetItem(control.window,i,&value)){
                auto ink=Readable(theme.ink,face);if(!enabled)ink=Blend(ink,face,.45f);
                SetTextColor(dc,Rgb(ink));RECT text=item;text.left+=inset;text.right-=inset;
                DrawTextW(dc,label,-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            }
            if(showFocus&&i==focused){RECT focus=item;InflateRect(&focus,-inset,-inset);DrawFocusRect(dc,&focus);}
        }
        RestoreDC(dc,saved);if(buffered)BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    }
    static LRESULT CALLBACK TabSubclass(HWND h,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR reference){
        auto& control=*reinterpret_cast<Tabs*>(reference);
        switch(message){
            case WM_PAINT:{
                if(wp){PaintTabs(control,reinterpret_cast<HDC>(wp));return 0;}
                PAINTSTRUCT paint{};const auto dc=BeginPaint(h,&paint);PaintTabs(control,dc);EndPaint(h,&paint);return 0;
            }
            case WM_PRINTCLIENT:PaintTabs(control,reinterpret_cast<HDC>(wp));return 0;
            case WM_ERASEBKGND:return 1;
            case WM_MOUSEMOVE:{
                TCHITTESTINFO hit{};hit.pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};const int index=TabCtrl_HitTest(h,&hit);
                if(index!=control.hot){control.hot=index;InvalidateRect(h,nullptr,FALSE);}
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);break;
            }
            case WM_MOUSELEAVE:control.hot=-1;InvalidateRect(h,nullptr,FALSE);break;
            case WM_SHOWWINDOW:if(!wp)control.hot=-1;break;
            case WM_NCDESTROY:RemoveWindowSubclass(h,TabSubclass,id);control.window=nullptr;return DefSubclassProc(h,message,wp,lp);
        }
        // Native hit testing, selection, keyboard and accessibility remain in
        // the standard tab procedure. Painting never synthesizes a selection.
        const auto result=DefSubclassProc(h,message,wp,lp);
        switch(message){
            case WM_SIZE:case WM_SETFONT:case WM_DPICHANGED_AFTERPARENT:
            case TCM_INSERTITEMW:case TCM_DELETEITEM:case TCM_DELETEALLITEMS:
                SizeTabs(control);InvalidateRect(h,nullptr,FALSE);break;
            case WM_SETFOCUS:case WM_KILLFOCUS:case WM_ENABLE:case WM_THEMECHANGED:case WM_UPDATEUISTATE:
            case WM_KEYDOWN:case WM_KEYUP:case WM_LBUTTONDOWN:case WM_LBUTTONUP:
            case TCM_SETCURSEL:case TCM_SETCURFOCUS:case TCM_SETITEMW:
                InvalidateRect(h,nullptr,FALSE);break;
        }
        return result;
    }
    static LRESULT CALLBACK Subclass(HWND h,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR reference){
        auto& button=*reinterpret_cast<Button*>(reference);
        switch(message){
            case WM_MOUSEMOVE:if(!button.hover){button.hover=true;TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,nullptr,FALSE);}break;
            case WM_MOUSELEAVE:button.hover=false;InvalidateRect(h,nullptr,FALSE);break;
            case WM_LBUTTONDOWN:button.pressed=true;InvalidateRect(h,nullptr,FALSE);break;
            case WM_LBUTTONUP:case WM_CAPTURECHANGED:case WM_CANCELMODE:button.pressed=false;InvalidateRect(h,nullptr,FALSE);break;
            case WM_KEYDOWN:if(wp==VK_SPACE){button.pressed=true;InvalidateRect(h,nullptr,FALSE);}break;
            case WM_KEYUP:if(wp==VK_SPACE){button.pressed=false;InvalidateRect(h,nullptr,FALSE);}break;
            case WM_KILLFOCUS:button.pressed=false;InvalidateRect(h,nullptr,FALSE);break;
            case WM_ENABLE:if(!wp){button.pressed=button.hover=false;}InvalidateRect(h,nullptr,FALSE);break;
            case WM_NCDESTROY:RemoveWindowSubclass(h,Subclass,id);button.window=nullptr;break;
        }
        return DefSubclassProc(h,message,wp,lp);
    }
    ~Impl(){for(auto& control:combos)if(control->window&&IsWindow(control->window)){RemoveWindowSubclass(control->window,ComboSubclass,reinterpret_cast<UINT_PTR>(control.get()));InvalidateRect(control->window,nullptr,FALSE);}for(auto& control:tabs)if(control->window&&IsWindow(control->window)){RemoveWindowSubclass(control->window,TabSubclass,reinterpret_cast<UINT_PTR>(control.get()));InvalidateRect(control->window,nullptr,FALSE);}for(auto& button:buttons)if(button->window&&IsWindow(button->window)){RemoveWindowSubclass(button->window,Subclass,reinterpret_cast<UINT_PTR>(button.get()));SetWindowLongPtrW(button->window,GWL_STYLE,button->originalStyle);InvalidateRect(button->window,nullptr,FALSE);}}
};
NativeControls::NativeControls():impl_(std::make_unique<Impl>()){}
NativeControls::~NativeControls()=default;
bool NativeControls::AttachButton(HWND window){
    DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(window,&pid);wchar_t name[32]{};
    if(thread!=impl_->uiThread||pid!=GetCurrentProcessId()||!GetClassNameW(window,name,32)||_wcsicmp(name,L"Button"))return false;
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);const auto type=style&BS_TYPEMASK;if(type!=BS_PUSHBUTTON&&type!=BS_DEFPUSHBUTTON)return false;
    for(const auto& button:impl_->buttons)if(button->window==window)return button->skinInstalled;
    auto button=std::make_unique<Impl::Button>();button->window=window;button->originalStyle=style;
    const auto raw=button.get();
    // Own the state before native callbacks can borrow it. Container allocation
    // failure then leaves the window untouched, rather than a dangling subclass.
    impl_->buttons.push_back(std::move(button));
    if(!SetWindowSubclass(window,Impl::Subclass,reinterpret_cast<UINT_PTR>(raw),reinterpret_cast<DWORD_PTR>(raw))){impl_->buttons.pop_back();return false;}
    SetLastError(0);
    if(!SetWindowLongPtrW(window,GWL_STYLE,(style&~BS_TYPEMASK)|BS_OWNERDRAW)&&GetLastError()){
        // Retain callback ownership even on a failed style change. The standard
        // button remains usable; WM_NCDESTROY or Impl teardown removes binding.
        return false;
    }
    raw->skinInstalled=true;InvalidateRect(window,nullptr,FALSE);return true;
}
bool NativeControls::AttachTabs(HWND window){
    DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(window,&pid);wchar_t name[32]{};
    if(thread!=impl_->uiThread||pid!=GetCurrentProcessId()||!GetClassNameW(window,name,32)||_wcsicmp(name,WC_TABCONTROLW))return false;
    for(const auto& control:impl_->tabs)if(control->window==window)return true;
    auto control=std::make_unique<Impl::Tabs>();control->window=window;control->owner=impl_.get();const auto raw=control.get();
    // Establish ownership before registering a callback that borrows raw.
    impl_->tabs.push_back(std::move(control));
    if(!SetWindowSubclass(window,Impl::TabSubclass,reinterpret_cast<UINT_PTR>(raw),reinterpret_cast<DWORD_PTR>(raw))){impl_->tabs.pop_back();return false;}
    Impl::SizeTabs(*raw);InvalidateRect(window,nullptr,FALSE);return true;
}
bool NativeControls::AttachCombo(HWND window){
    DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(window,&pid);wchar_t name[32]{};
    if(thread!=impl_->uiThread||pid!=GetCurrentProcessId()||!GetClassNameW(window,name,32)||_wcsicmp(name,L"ComboBox"))return false;
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    if((style&3)!=CBS_DROPDOWNLIST||(style&(CBS_OWNERDRAWFIXED|CBS_OWNERDRAWVARIABLE)))return false;
    for(const auto& control:impl_->combos)if(control->window==window)return true;
    auto control=std::make_unique<Impl::Combo>();control->window=window;control->owner=impl_.get();const auto raw=control.get();
    impl_->combos.push_back(std::move(control));
    if(!SetWindowSubclass(window,Impl::ComboSubclass,reinterpret_cast<UINT_PTR>(raw),reinterpret_cast<DWORD_PTR>(raw))){impl_->combos.pop_back();return false;}
    InvalidateRect(window,nullptr,FALSE);return true;
}
void NativeControls::SetTheme(const ThemeDefinition& theme){
    if(GetCurrentThreadId()!=impl_->uiThread)return;
    auto same=[](ThemeColor a,ThemeColor b){return a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a;};
    if(same(theme.panel,impl_->theme.panel)&&same(theme.background,impl_->theme.background)&&same(theme.accent,impl_->theme.accent)&&same(theme.ink,impl_->theme.ink))return;
    impl_->theme.panel=theme.panel;impl_->theme.background=theme.background;impl_->theme.accent=theme.accent;impl_->theme.ink=theme.ink;
    for(const auto& button:impl_->buttons)if(button->window&&IsWindowVisible(button->window))InvalidateRect(button->window,nullptr,FALSE);
    for(const auto& control:impl_->tabs)if(control->window&&IsWindowVisible(control->window))InvalidateRect(control->window,nullptr,FALSE);
    for(const auto& control:impl_->combos)if(control->window&&IsWindowVisible(control->window)){
        InvalidateRect(control->window,nullptr,FALSE);COMBOBOXINFO info{sizeof(info)};
        if(GetComboBoxInfo(control->window,&info)&&info.hwndList&&IsWindowVisible(info.hwndList))InvalidateRect(info.hwndList,nullptr,FALSE);
    }
}
void NativeControls::Advance(int64_t now){
    if(GetCurrentThreadId()!=impl_->uiThread)return;
    const float dt=impl_->lastFrame?std::clamp(float(now-impl_->lastFrame)/1000.f,0.f,.1f):1.f/60;impl_->lastFrame=now;
    for(const auto& button:impl_->buttons){if(!button->window||!IsWindowVisible(button->window))continue;const auto enabled=IsWindowEnabled(button->window)!=FALSE;
        const float hover=enabled&&button->hover?1.f:0.f,press=enabled&&button->pressed?1.f:0.f;
        const float oldHover=button->hoverAmount,oldPress=button->pressAmount;
        button->hoverAmount+=(hover-oldHover)*(1-std::exp(-dt*24));button->pressAmount+=(press-oldPress)*(1-std::exp(-dt*32));
        if(std::abs(button->hoverAmount-oldHover)>.0005f||std::abs(button->pressAmount-oldPress)>.0005f)InvalidateRect(button->window,nullptr,FALSE);
    }
    for(const auto& control:impl_->combos){if(!control->window||!IsWindowVisible(control->window))continue;
        const float target=control->hover&&IsWindowEnabled(control->window)?1.f:0.f,previous=control->hoverAmount;
        control->hoverAmount+=(target-previous)*(1-std::exp(-dt*24));
        if(std::abs(control->hoverAmount-previous)>.0005f)InvalidateRect(control->window,nullptr,FALSE);
    }
}
bool NativeControls::DrawItem(const DRAWITEMSTRUCT& draw,HFONT font){
    if(draw.CtlType!=ODT_BUTTON||GetCurrentThreadId()!=impl_->uiThread)return false;
    Impl::Button* button=nullptr;for(const auto& candidate:impl_->buttons)if(candidate->window==draw.hwndItem&&candidate->skinInstalled){button=candidate.get();break;}if(!button)return false;
    const int width=draw.rcItem.right-draw.rcItem.left,height=draw.rcItem.bottom-draw.rcItem.top;if(width<=0||height<=0)return true;
    auto dc=button->paint.Acquire(draw.hDC,width,height);const bool buffered=dc!=nullptr;if(!dc)dc=draw.hDC;const int saved=SaveDC(dc);if(!saved)return false;if(!buffered)SetViewportOrgEx(dc,draw.rcItem.left,draw.rcItem.top,nullptr);
    const auto& theme=impl_->theme;const bool disabled=(draw.itemState&ODS_DISABLED)!=0;const bool focus=(draw.itemState&ODS_FOCUS)!=0;
    const float response=disabled?0:(button->hoverAmount*.10f+button->pressAmount*.08f)*theme.accent.a;
    const auto face=Blend(Blend(theme.panel,theme.background,.62f),theme.accent,response);
    // Reuse the DC-owned stock brush and pen. Per-paint GDI allocation can
    // fail under desktop-wide handle pressure; selecting a failed brush or pen
    // would then leave an owner-draw button with undefined paint state.
    SelectObject(dc,GetStockObject(DC_BRUSH));SelectObject(dc,GetStockObject(DC_PEN));
    SetDCBrushColor(dc,Rgb(theme.panel));RECT background{0,0,width,height};
    FillRect(dc,&background,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc,Rgb(face));
    SetDCPenColor(dc,Rgb(Blend(theme.panel,focus?theme.accent:theme.ink,focus?.75f:.18f+button->hoverAmount*.22f)));
    const int radius=MulDiv(9,GetDpiForWindow(draw.hwndItem),96);RoundRect(dc,1,1,width-1,height-1,radius,radius);
    const int length=std::clamp(GetWindowTextLengthW(draw.hwndItem),0,2048);wchar_t label[2049]{};GetWindowTextW(draw.hwndItem,label,length+1);
    SelectObject(dc,font?reinterpret_cast<HGDIOBJ>(font):GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,Rgb(disabled?Blend(Readable(theme.ink,face),face,.45f):Readable(theme.ink,face)));
    RECT text{10,0,width-10,height};DrawTextW(dc,label,-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    if(focus&&!(draw.itemState&ODS_NOFOCUSRECT)){RECT ring{4,4,width-4,height-4};DrawFocusRect(dc,&ring);}
    RestoreDC(dc,saved);if(buffered)BitBlt(draw.hDC,draw.rcItem.left,draw.rcItem.top,width,height,dc,0,0,SRCCOPY);return true;
}
}
