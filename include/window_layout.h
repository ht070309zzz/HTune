#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace stage {
inline UINT WindowDpi(HWND window) {
    const auto dpi=GetDpiForWindow(window);return dpi>=48&&dpi<=768?dpi:96;
}
inline std::optional<MONITORINFO> WindowMonitor(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if(!monitor||!GetMonitorInfoW(monitor,&info)||info.rcWork.right<=info.rcWork.left||info.rcWork.bottom<=info.rcWork.top)return {};
    return info;
}
inline SIZE MinimumWindowSize(UINT dpi,const RECT& work) {
    dpi=std::clamp<UINT>(dpi,48,768);
    const auto width=std::max<int64_t>(1,int64_t(work.right)-work.left),height=std::max<int64_t>(1,int64_t(work.bottom)-work.top);
    return {static_cast<LONG>(std::min<int64_t>(width,MulDiv(680,dpi,96))),static_cast<LONG>(std::min<int64_t>(height,MulDiv(440,dpi,96)))};
}
inline RECT FitWindowToWorkArea(RECT desired,const RECT& work,UINT dpi,bool center=false) {
    const auto minimum=MinimumWindowSize(dpi,work);
    const auto workWidth=std::max<int64_t>(1,int64_t(work.right)-work.left),workHeight=std::max<int64_t>(1,int64_t(work.bottom)-work.top);
    const auto width=std::clamp<int64_t>(int64_t(desired.right)-desired.left,minimum.cx,workWidth);
    const auto height=std::clamp<int64_t>(int64_t(desired.bottom)-desired.top,minimum.cy,workHeight);
    const auto left=center?int64_t(work.left)+(workWidth-width)/2:std::clamp<int64_t>(desired.left,work.left,int64_t(work.right)-width);
    const auto top=center?int64_t(work.top)+(workHeight-height)/2:std::clamp<int64_t>(desired.top,work.top,int64_t(work.bottom)-height);
    return {static_cast<LONG>(left),static_cast<LONG>(top),static_cast<LONG>(left+width),static_cast<LONG>(top+height)};
}
inline bool FitOwnWindow(HWND window,bool fullscreen=false,std::optional<SIZE> preferred={},bool center=false) {
    if(!window||!IsWindow(window)||IsIconic(window)||(!fullscreen&&IsZoomed(window)))return false;
    const auto monitor=WindowMonitor(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST));if(!monitor)return false;
    RECT current{};if(!GetWindowRect(window,&current))return false;auto desired=current;
    if(fullscreen)desired=monitor->rcMonitor;
    else {
        if(preferred){desired.right=desired.left+std::clamp<LONG>(preferred->cx,1,16384);desired.bottom=desired.top+std::clamp<LONG>(preferred->cy,1,16384);}
        desired=FitWindowToWorkArea(desired,monitor->rcWork,WindowDpi(window),center);
    }
    if(EqualRect(&current,&desired))return true;
    return SetWindowPos(window,nullptr,desired.left,desired.top,desired.right-desired.left,desired.bottom-desired.top,SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE;
}
inline float SettingsRailWidth(float width) {return std::max(0.f,std::min({340.f,std::max(240.f,width-400.f),width}));}
inline bool CompactStageLayout(float stageWidth,float height) {return stageWidth<480.f||height<560.f;}
} // namespace stage
