#pragma once
#include <windows.h>
#include <algorithm>

namespace stage {
// Shared DIP geometry for painting and native hit testing. The stage itself
// still fills the client area; no title bar background is added.
inline constexpr float WindowChromeHeight=40.f;
inline RECT WindowChromeButtonRect(int clientWidth,UINT dpi,int index) {
    const auto px=[dpi](int dip){return MulDiv(dip,static_cast<int>(dpi),96);};
    const int right=clientWidth-px(10+(2-index)*44);
    return {right-px(40),px(8),right,px(36)};
}
inline bool HasWindowChrome(HWND window) {
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    return (style&WS_CAPTION)==WS_CAPTION&&(style&WS_CHILD)==0;
}
inline LRESULT WindowChromeHitTest(POINT p,SIZE client,UINT dpi,bool maximized) {
    if(p.x<0||p.y<0||p.x>=client.cx||p.y>=client.cy)return HTNOWHERE;
    if(!maximized){
        const int border=std::max(1,MulDiv(6,static_cast<int>(dpi),96));
        const bool left=p.x<border,right=p.x>=client.cx-border,top=p.y<border,bottom=p.y>=client.cy-border;
        if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;
        if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;
        if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;
    }
    for(int i=0;i<3;++i){const auto rect=WindowChromeButtonRect(client.cx,dpi,i);if(PtInRect(&rect,p))return HTCLIENT;}
    if(p.y<MulDiv(static_cast<int>(WindowChromeHeight),static_cast<int>(dpi),96))return HTCAPTION;
    return HTCLIENT;
}
}
