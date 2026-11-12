#pragma once
// Prefer the trigger's horizontal center only when the whole popup fits in the
// same work area. Keep the requested alignment at display edges; never span
// monitors merely to preserve centering.
struct NativePopupPlacement {int x=0;bool centered=false;float origin=0;};
inline NativePopupPlacement NativePlacePopup(int anchor,int width,RECT const &work,std::wstring const &align,float scale)
{
    int fallback=anchor-(align==L"Center"?width/2:align==L"End"?width:0);
    int candidate=anchor-width/2;bool fits=candidate>=work.left&&candidate+width<=work.right;
    int x=std::clamp(fits?candidate:fallback,static_cast<int>(work.left),std::max(static_cast<int>(work.left),static_cast<int>(work.right)-width));
    return {x,fits,std::clamp((anchor-x)/scale,10.f,std::max(10.f,width/scale-10))};
}

inline HMONITOR NativeStartMenuMonitor(POINT anchor,bool followForeground,HWND foreground){
    if(followForeground&&foreground){
        wchar_t name[96]{};GetClassNameW(foreground,name,96);
        if(foreground==GetDesktopWindow()||IsIconic(foreground)||!wcscmp(name,L"Progman")||!wcscmp(name,L"WorkerW")||!wcscmp(name,L"Shell_TrayWnd")||!wcscmp(name,L"Shell_SecondaryTrayWnd")||!wcscmp(name,L"MagicCompositionTrialWindow")||!wcscmp(name,L"MagicCompositionPopupWindow"))foreground=nullptr;
    }
    auto target=followForeground&&foreground?MonitorFromWindow(foreground,MONITOR_DEFAULTTONULL):nullptr;
    return target?target:MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST);
}
