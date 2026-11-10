#pragma once
#include "native-popup.h"
#include "native-tray-menu-anchor.h"
#include <dwmapi.h>
#include "native-toolbar-assets.h"
#include "native-dissolve.h"
inline JsonObject ShellObject(JsonObject const &data, wchar_t const *key)
{
    auto value = data.TryLookup(key);
    return value && value.ValueType() == JsonValueType::Object ? value.GetObject() : JsonObject{};
}
struct NativeBarSettingsEntry{wchar_t const *label,*verb,*icon;};
inline constexpr NativeBarSettingsEntry NativeBarSettingsEntries[]={{L"Windows 设置",L"windows-settings",L"BsWindows"},{L"magic视界设置",L"settings",L"RiSettings4Fill"}};
inline bool NativeNotificationMuted(JsonObject const &snapshot,std::wstring const &widget,double value){return value<0&&widget==L"@magic/notifications"&&Bool(snapshot,L"magicNotificationQuiet");}
inline bool NativeTrueFullscreen(RECT client,RECT monitor,bool maximizedWithFrame){
    return !maximizedWithFrame&&client.left<=monitor.left+2&&client.top<=monitor.top+2&&client.right>=monitor.right-2&&client.bottom>=monitor.bottom-2;
}
inline bool NativeToolbarWindowCoversWorkArea(RECT bounds,RECT work,bool maximized){
    if(bounds.right<=bounds.left||bounds.bottom<=bounds.top)return false;
    return maximized||(bounds.left<=work.left+2&&bounds.top<=work.top+2&&bounds.right>=work.right-2&&bounds.bottom>=work.bottom-2);
}
inline bool NativeMoveYieldsBar(bool moveOnMonitor,bool hooksAvailable,bool intersects,bool backendMove){
    // MoveSizeStart/End is authoritative. GetAsyncKeyState can report zero
    // across UIPI and is also false for a valid keyboard-driven move.
    return moveOnMonitor||(!hooksAvailable&&intersects&&backendMove);
}
struct NativeShell;
struct NativePreview;
struct NativeBar;
inline bool NativeDockPointerHold(RECT body,RECT screen,POINT point,std::wstring const &side){
    if(PtInRect(&body,point))return true;
    if(!PtInRect(&screen,point))return false;
    // The reveal strip spans the display edge. Keep that strip and the gap
    // to the floating Dock stable while the pointer travels into its body.
    if(side==L"Left")return point.x<body.left;
    if(side==L"Right")return point.x>=body.right;
    if(side==L"Top")return point.y<body.top;
    return point.y>=body.bottom;
}
void InstallNativeBarDrop(NativeBar &bar);
struct NativeBar : NativePopup
{
    static constexpr UINT_PTR DockBodyTimer=104,DockPreviewTimer=106;
    static_assert(DockBodyTimer!=DockPreviewTimer);
    NativeShell &shell;
    std::wstring monitor;
    bool dock = false, edge = false, reserved = false, menuOpen = false, hidePending=false, showPending=false,edgePointerWatch=false;
    bool dpiRefreshPending=false;float pendingDpi=96;
    void RefreshDpi();
    JsonObject data{nullptr}, settings{nullptr};
    ContainerVisual toolbarRoot{nullptr},toolbarLayer{nullptr},barHost{nullptr};
    NativeDissolve::Session dockDissolve;NativeHoverVisual barInteraction;
    bool toolbarOccluded=false,occlusionAnimating=false,occlusionLower=false;
    void FinishToolbarOcclusion(){
        if(!occlusionAnimating)return;occlusionAnimating=false;KillTimer(hwnd,99);dockDissolve.Reset(barHost,toolbarRoot);
        if(toolbarRoot){toolbarRoot.StopAnimation(L"Opacity");toolbarRoot.Opacity(1);}
        if(toolbarOccluded){if(occlusionLower)SetWindowPos(hwnd,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);else ShowWindow(hwnd,SW_HIDE);visible=false;Release();}
    }
    void SetToolbarOccluded(bool value,bool lower=false){
        occlusionLower=lower;if(dock||toolbarOccluded==value)return;toolbarOccluded=value;ReportExtensions();
        bool interrupted=occlusionAnimating;occlusionAnimating=false;KillTimer(hwnd,99);
        if(hidingToolbar){hidingToolbar=false;edge=false;toolbarRoot.StopAnimation(L"Offset");toolbarRoot.Offset({0,0,0});}
        if(value){KillTimer(hwnd,91);HideBarTooltip();barInteraction.Release(toolbarRoot);}
        else{if(!interrupted&&toolbarRoot){toolbarRoot.StopAnimation(L"Opacity");toolbarRoot.Opacity(0);}visible=true;PositionBar(SWP_NOACTIVATE|SWP_SHOWWINDOW);Layout();Draw();ScheduleClock();}
        bool animate=toolbarRoot&&NativeFeedbackMotion(app.model)&&(!value||visible&&!edge);
        // A bar that dissolves while a window is dragged is a taste, not a given.
        // Switched off, it simply steps out of the way with no motion at all.
        if(animate&&!Bool(app.model.preferences,L"toolbarDissolve",true))animate=false;
        if(animate){
            bool grain=false;
            if(NativeMotion(app.model)){if(dockDissolve.Active()){dockDissolve.Play(app.graphics.compositor,!value,true);grain=true;}else grain=dockDissolve.Start(app.graphics,barHost,toolbarRoot,width,height,dpi/96,!value);}
            if(grain){toolbarRoot.StopAnimation(L"Opacity");toolbarRoot.Opacity(1);}
            else{dockDissolve.Reset(barHost,toolbarRoot);if(!interrupted){toolbarRoot.StopAnimation(L"Opacity");toolbarRoot.Opacity(value?1.f:0.f);}NativeInteractionMotion::Opacity(app.graphics.compositor,toolbarRoot,value?0.f:1.f,NativeDissolve::DurationMs);}
            occlusionAnimating=true;if(SetTimer(hwnd,99,NativeDissolve::DurationMs+16,nullptr))return;
        }
        occlusionAnimating=true;FinishToolbarOcclusion();
    }
    void SyncBarInteraction(float x,float y,int index){if(dock)return;auto region=index>=0&&index<static_cast<int>(rows.size())?NativeRegion(rows[index].rect,{0,0,width,height},NativeMetrics::ControlRadius,rows[index].enabled):NativeInteractionRegion{};barInteraction.Sync(app.graphics.compositor,toolbarRoot,region,{x-contentX,y-contentY},dpi/96,NativeMotion(app.model),dark);}
    void FinishToolbarHide(){if(!hidingToolbar)return;hidingToolbar=false;KillTimer(hwnd,99);toolbarRoot.StopAnimation(L"Offset");toolbarRoot.Offset({0,0,0});Hide(false,false);}
    float dockHoverAxis=-FLT_MAX;bool dockMotionEnabled=false,dockBackdropDirty=false,dockReorderHoverBlocked=false;std::wstring dockHoverTop;std::set<std::wstring> dockRegionItems;RECT dockRegionBody{};float dockRegionDpi=0;
    void InvalidateDockBackdrop(){if(dock){dockBackdropDirty=true;toolbarBackgroundKey.clear();}}
    ULONGLONG dockEnterAt=0,dockResizeAt=0;bool dockEnterPending=false,dockBodyPending=false,dockHasShown=false;
    float dockFromWidth=0,dockFromHeight=0,dockShiftX=0,dockShiftY=0;RECT dockEnvelope{};InsetClip dockViewportClip{nullptr};std::wstring dockViewportSide;
    void StartDockEntrance();void PrepareDockReflow(float,float,RECT const &);void ApplyDockReflow();void ResetDockBodyMotion();
    void PositionBar(UINT flags);void SyncDockHover();void UpdateDockRegion(bool force=false);
    void DockPointer(float x,float y);void ClearDockHover();void LockDockReorderHover();void DockPointerLeft(bool outside);
    NativeInteractionMotion::DockPose DockPose(int index,bool maximum=false)const;
    D2D1_RECT_F VisualRowBounds(int index)const override;

    std::map<std::wstring,NativeToolbarVisual> toolbarVisuals;
    NativeToolbarSvg toolbarSvg;
    std::map<std::wstring,float> textWidths;
    std::wstring toolbarBackgroundKey,dropId;std::set<std::wstring> dropRows;
    // Last backdrop decision written to the log; the tuple only changes on a switch.
    std::wstring backdropTraceKey;
    std::vector<std::wstring> dragOrder,dragOriginalOrder;
    // The tray is one module made of a button and a list of icons. Pressing the button
    // moves the block; pressing an icon reorders that list, so the gesture needs its own
    // order, its own target slots and its own commit.
    bool dragTray=false;std::wstring dragTrayOwner,dragTraySection;
    std::vector<std::wstring> dragTrayOrder,dragTrayOriginal;
    // Composition and SetWindowRgn can dispatch input while the row and visual
    // tables are being rebuilt. Keep gesture messages in order across that frame.
    struct PointerContact {POINTER_INPUT_TYPE type;POINT screen;};
    struct DragMessage {UINT message;WPARAM w;LPARAM l;std::optional<PointerContact> contact;};
    std::deque<DragMessage> deferredDragInput;
    bool replayingDragInput=false;
    static bool IsPointerContact(UINT msg){return msg==WM_POINTERDOWN||msg==WM_POINTERUPDATE||msg==WM_POINTERUP;}
    static bool IsDragMessage(UINT msg,WPARAM w){
        return msg==WM_MOUSEMOVE||msg==WM_MOUSELEAVE||msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||
            msg==WM_LBUTTONDBLCLK||msg==WM_CAPTURECHANGED||msg==WM_MOUSEWHEEL||IsPointerContact(msg)||
            msg==WM_POINTERCAPTURECHANGED||(msg==WM_KEYDOWN&&w==VK_ESCAPE);
    }
    void DeferDragInput(DragMessage const &event){
        const bool move=event.message==WM_MOUSEMOVE||event.message==WM_POINTERUPDATE;
        if(move&&!deferredDragInput.empty()&&deferredDragInput.back().message==event.message&&deferredDragInput.back().w==event.w)
            deferredDragInput.back()=event;
        else if(deferredDragInput.size()<32)deferredDragInput.push_back(event);
        else {deferredDragInput.clear();deferredDragInput.push_back({WM_KEYDOWN,VK_ESCAPE,0,{}});}
        app.QueueRenderWork();
    }
    void FlushDragInput();
    std::vector<JsonObject> dragEntries;
    std::vector<D2D1_RECT_F> dragRects;
    std::vector<std::pair<std::wstring,float>> dragTargets;
    // T36: one coordinate basis per gesture. dragTargetScroll is fixed at pointer-down;
    // dragRowScroll remembers the scroll the current rows were laid out with, so a
    // refresh can map slot centres back into the fixed basis instead of resetting it.
    float dragTargetScroll=0,dragRowScroll=0;std::wstring dragTarget;
    // T36: the dragged row's own rect in the fixed drag frame. A wide row keeps the
    // pointer outside its slot centre, so containment, not centre distance, decides
    // whether the pointer is still on the row.
    D2D1_RECT_F dragSelfRect{};bool dragSelfRectValid=false;
    // T36: last swap position; a refresh cannot swap again without real pointer travel.
    float lastSwapCursor=FLT_MAX;
    ULONGLONG dropUntil=0;
    // T39: the whole-block tray drag folds its icons into the button the pointer
    // holds. trayCollapseActive marks that gesture; trayCollapsedGhosts are the
    // ghost visuals still owed their unfold after the drop.
    bool trayCollapseActive=false;std::set<std::wstring> trayCollapsedGhosts;
    std::map<std::wstring,float> extensionWidths;bool extensionWidthsReady=false;
    std::map<std::wstring,std::wstring> extensionPreparedSessions,extensionReports;
    std::wstring extensionPressId,extensionPressSession,extensionPressRevision,extensionPressAction;
    float ExtensionTextWidth(std::wstring const &,bool bold=false);float ExtensionNaturalWidth(JsonObject const &);
    void BudgetExtensions();void PaintExtensionItem(ID2D1DeviceContext*,int);std::wstring ExtensionActionAt(int,float,bool context=false);
    void SyncExtensionMotion(NativeToolbarVisual &,JsonObject const &);void ReportExtensions(bool remove=false);
    bool paintedTransparentToolbar=false,toolbarCoveredByApplication=false,paintedBlackBar=false;
    // The two covered modes that are not black: "transparent" paints no surface at all,
    // "acrylic" keeps the blur and only lays a thin scrim over it. toolbarGlass says
    // whether the compositor gave us the blur this paint, which decides whether the
    // acrylic mode can stay thin or has to fall back to a readable panel.
    bool paintedCoveredClear=false,paintedCoveredGlass=false,toolbarGlass=false;
    // Under a maximised application the original stops showing the wallpaper and paints
    // the bar out in black. The desktop state wants the wallpaper, the covered state is a
    // choice, so both ends are named here and the ink follows whichever one wins.
    std::wstring CoveredBackdrop()const{auto mode=Str(app.model.preferences,L"coveredBackdrop");return mode==L"acrylic"||mode==L"transparent"?mode:L"black";}
    bool TransparentToolbar()const{return !dock&&Bool(app.model.preferences,L"transparentToolbar",true)&&!toolbarCoveredByApplication;}
    bool BlackBar()const{return !dock&&toolbarCoveredByApplication&&CoveredBackdrop()==L"black";}
    bool CoveredClear()const{return !dock&&toolbarCoveredByApplication&&CoveredBackdrop()==L"transparent";}
    bool CoveredGlass()const{return !dock&&toolbarCoveredByApplication&&CoveredBackdrop()==L"acrylic";}
    bool ToolbarCoveredByApplication()const{
        auto display=MonitorFromRect(&full,MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};
        if(!GetMonitorInfoW(display,&info))return false;
        auto covers=[&](HWND window){
            if(!IsWindow(window)||!IsWindowVisible(window)||IsIconic(window)||GetAncestor(window,GA_ROOT)!=window)return false;
            DWORD pid=0;GetWindowThreadProcessId(window,&pid);wchar_t name[80]{};GetClassNameW(window,name,80);
            if(!pid||pid==GetCurrentProcessId()||pid==static_cast<DWORD>(Num(data,L"hostPid"))||!wcscmp(name,L"Progman")||!wcscmp(name,L"WorkerW")||!wcscmp(name,L"Shell_TrayWnd")||!wcscmp(name,L"Shell_SecondaryTrayWnd"))return false;
            RECT bounds{};if(!GetWindowRect(window,&bounds))return false;
            bool maximized=IsZoomed(window)&&MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)==display;
            // Most windows are small; avoid DWM queries for every unrelated
            // window on each coalesced foreground/location event.
            if(!NativeToolbarWindowCoversWorkArea(bounds,info.rcWork,maximized))return false;
            DWORD cloaked=0;if(SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return false;
            RECT frame{};if(SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame))))bounds=frame;
            return NativeToolbarWindowCoversWorkArea(bounds,info.rcWork,maximized);
        };
        // Only tracked application windows can change the desktop material.
        // Control Center and other shell flyouts can have a monitor-sized
        // transparent HWND; their foreground bounds are not app occlusion.
        // Reuse the existing event-driven list so our own popups also cannot
        // switch the ink above a still-maximized application.
        for(auto const &entry:data.GetNamedArray(L"appWindows",JsonArray{}))if(covers(reinterpret_cast<HWND>(static_cast<INT_PTR>(Num(entry.GetObject(),L"hwnd")))))return true;
        return false;
    }
    bool hidingToolbar=false,clockSeconds=false;std::wstring clockConfiguration;
    void ScheduleClock(){
        if(!hwnd)return;KillTimer(hwnd,91);
        if(!visible||edge||!std::any_of(entries.begin(),entries.end(),[&](auto const &e){return dock?Bool(e,L"dockClock"):Bool(e,L"clock");}))return;
        auto configuration=Str(data,L"dateFormat")+L"|"+Str(data,L"language")+L"|"+std::to_wstring(NativeClockShowSeconds());
        if(configuration!=clockConfiguration){clockConfiguration=configuration;clockSeconds=ToolbarClockSeconds(data);}
        SYSTEMTIME now{};GetLocalTime(&now);SetTimer(hwnd,91,clockSeconds?1000-now.wMilliseconds:60000-now.wSecond*1000-now.wMilliseconds,nullptr);
    }
    void SyncToolbarVisuals();void ToolbarFade(Visual const &,bool);
    void AnimateToolbarOffset(Visual const &,float3,bool drop=false);
    float MeasureToolbarText(std::wstring const &,float,int weight=500);
    JsonArray ToolbarContent(JsonObject const &);float ToolbarContentWidth(JsonObject const &);float ToolbarMetricWidth(JsonObject const &);
    void PaintToolbarItem(ID2D1DeviceContext *,int);void PaintDockItem(ID2D1DeviceContext *,int,bool retainedMedia=false);void SyncDockState(NativeToolbarVisual &,int,float);void SyncMediaTitle(NativeToolbarVisual &,int,float);void SyncMediaSize(NativeToolbarVisual &,int,float,bool);
    void Release(){occlusionAnimating=false;if(toolbarRoot){toolbarRoot.StopAnimation(L"Opacity");toolbarRoot.Opacity(1);}ResetDockBodyMotion();barInteraction.Release(toolbarRoot);dockDissolve.Reset(barHost,toolbarRoot);if(toolbarRoot)ReleaseNativeGlass(toolbarRoot,materialVisual);NativePopup::Release();if(toolbarLayer)toolbarLayer.Children().RemoveAll();toolbarVisuals.clear();dockHoverTop.clear();dockHoverAxis=-FLT_MAX;dockRegionItems.clear();dockRegionDpi=0;toolbarSvg.documents.clear();toolbarBackgroundKey.clear();dropId.clear();dropRows.clear();dropUntil=0;if(hwnd){KillTimer(hwnd,97);KillTimer(hwnd,99);KillTimer(hwnd,103);KillTimer(hwnd,DockPreviewTimer);KillTimer(hwnd,105);edgePointerWatch=false;}hidingToolbar=false;if(toolbarRoot){toolbarRoot.StopAnimation(L"Offset");toolbarRoot.Offset({0,0,0});}}
    std::wstring batteryMeasureText;float batteryMeasureWidth=0;
    std::wstring lastPage;int pageAlpha=100;bool pageWaiting=false;
    std::vector<JsonObject> entries;
    RECT full{}, requestedBar{}, reservedBar{}, notifiedBar{};
    bool reservationDirty=true, positioningBar=false,readyNotified=false,themeRefreshPending=false;
    int down=-1,mediaHover=-1,mediaDown=-1;
    int MediaButtonAt(int,float,float)const;
    std::wstring downId;
    POINT downPoint{},contactPoint{};
    float iconSize = 40, gap = 8, padding = 8, tooltipDpi=0;
    long long paints = 0,itemRedraws=0;
    size_t SurfaceBytes()const{size_t total=surfaceBytes+dockDissolve.maskBytes+dockDissolve.layerBytes;for(auto const &[_,v]:toolbarVisuals)total+=v.bytes;if(nativeTooltip)total+=nativeTooltip->surfaceBytes;return total;}
    HWND tooltip = nullptr;
    std::wstring tooltipText;
    std::unique_ptr<NativePopup> nativeTooltip;
    POINTER_INPUT_TYPE pointerKind=PT_MOUSE;UINT32 pointerId=0;ULONGLONG pointerDownAt=0;bool pointerCancelled=false,pointerDispatch=false,touchPrimary=false;
    void ShowBarTooltip();
    void HideBarTooltip(){if(hwnd)KillTimer(hwnd,100);if(nativeTooltip)nativeTooltip->Hide(false);}

    NativeBar(App &app, NativeShell &shell, std::wstring monitor, bool dock)
        : NativePopup(app), shell(shell), monitor(std::move(monitor)), dock(dock)
    {
    }
    ~NativeBar()
    {
        Close();
    }
    void Close()
    {
        visible=false;
        if (reserved)
        {
            APPBARDATA bar{sizeof(bar)};
            bar.hWnd = hwnd;
            SHAppBarMessage(ABM_REMOVE, &bar);
            reserved = false;
        }
        if (life)
            life->owner = nullptr;
        if (hwnd)
            RevokeDragDrop(hwnd);
        if (tooltip)
        {
            DestroyWindow(tooltip);
            tooltip = nullptr;
        }
        HideBarTooltip();nativeTooltip.reset();
        if(hwnd){auto closingWindow=hwnd;WTSUnRegisterSessionNotification(closingWindow);
            SetWindowLongPtrW(closingWindow,GWLP_USERDATA,0);Release();NativeGlassStates().erase(closingWindow);
            if(target){try{target.Root(nullptr);}catch(...){}}target=nullptr;visual=nullptr;toolbarRoot=nullptr;toolbarLayer=nullptr;barHost=nullptr;
            hwnd=nullptr;DestroyWindow(closingWindow);}
        hwnd = nullptr;
    }
    Board *PageBoard(){if(auto b=app.model.Find(monitor))return b;for(auto &b:app.model.boards)if(full.left>=b.x&&full.left<b.x+b.width&&full.top>=b.y&&full.top<b.y+b.height)return &b;return nullptr;}
    std::wstring BarText(wchar_t const *key)const{return NativeShellText(Str(data,L"language"),(dock?L"dock.":L"toolbar.")+std::wstring(key));}
    void Create();
    void Update(JsonObject const &snapshot, JsonObject const &screen,bool notifyReady=true);
    void Layout();
    bool barDrag=false,toolbarSequential=false,dragSequential=false;
    JsonObject dragEntry{nullptr};std::wstring dragId,dragSection=L"left";int dragIndex=0;
    float dragWidth=0,dragGrab=0,dragPointer=0;float toolbarBoundaries[2]{};bool dragSlotsDirty=false;
    // T33: the ghost's placement is reported, never polled. One line per real position
    // change while a bar drag is live, so a bad field names itself instead of a guess.
    float dragGhostTraceX=FLT_MAX;
    // T34: one drag-swap line per real change of the swap decision's inputs.
    float dragSwapTraceX=FLT_MAX,dragSwapTraceCenter=FLT_MAX,dragSwapTraceBest=FLT_MAX,dragSwapTraceCurrent=FLT_MAX,dragSwapTraceScroll=FLT_MAX,dragSwapTraceTargetScroll=FLT_MAX,dragSwapTraceSelfLeft=FLT_MAX,dragSwapTraceSelfRight=FLT_MAX;
    std::wstring dragSwapTraceTarget,dragSwapTraceDragTarget;bool dragSwapTraceTray=false,dragSwapTraceSelf=false,dragSwapTraceSelfValid=false;size_t dragSwapTraceSlots=0;
    void TraceDragGhost(float x,float start,float originalLeft,float extra){
        if(std::abs(start-dragGhostTraceX)<.01f)return;dragGhostTraceX=start;
        JsonObject detail;Put(detail,L"surface",std::wstring(dock?L"dock":L"toolbar"));
        Put(detail,L"dpi",static_cast<double>(dpi));Put(detail,L"x",static_cast<double>(x));
        Put(detail,L"dragPointer",static_cast<double>(dragPointer));Put(detail,L"dragGrab",static_cast<double>(dragGrab));
        Put(detail,L"originalLeft",static_cast<double>(originalLeft));Put(detail,L"start",static_cast<double>(start));Put(detail,L"center",static_cast<double>(x-dragGrab+dragWidth/2+barScroll-dragTargetScroll));
        Put(detail,L"extra",static_cast<double>(extra));Put(detail,L"offsetPx",static_cast<double>((start-extra)*dpi/96));
        Put(detail,L"cursorPx",static_cast<double>((x+contentX)*dpi/96));Put(detail,L"contentX",static_cast<double>(contentX));
        Put(detail,L"fullLeft",static_cast<double>(full.left));Put(detail,L"fullTop",static_cast<double>(full.top));
        Put(detail,L"width",static_cast<double>(width));Put(detail,L"height",static_cast<double>(height));
        if(hwnd)Put(detail,L"windowDpi",static_cast<double>(GetDpiForWindow(hwnd)));
        POINT cursor{};if(GetCursorPos(&cursor)){Put(detail,L"cursorScreenX",static_cast<double>(cursor.x));Put(detail,L"cursorScreenY",static_cast<double>(cursor.y));}
        if(barHost){auto offset=barHost.Offset();Put(detail,L"hostOffsetX",static_cast<double>(offset.x));Put(detail,L"hostOffsetY",static_cast<double>(offset.y));}
        if(toolbarRoot){auto offset=toolbarRoot.Offset();Put(detail,L"rootOffsetX",static_cast<double>(offset.x));Put(detail,L"rootOffsetY",static_cast<double>(offset.y));}
        if(toolbarLayer){auto offset=toolbarLayer.Offset();Put(detail,L"layerOffsetX",static_cast<double>(offset.x));Put(detail,L"layerOffsetY",static_cast<double>(offset.y));}
        detail.Insert(L"tray",JsonValue::CreateBooleanValue(dragTray));app.Telemetry(L"drag-ghost",detail);
    }
    int Hit(float x,float y)const override;
    void ToolbarDrag(float x,float y);
    bool MoveToolbarDrag(float x,float y);
    void EndToolbarDrag(bool commit);void RefreshDragTargets();
    static bool TrayRoot(JsonObject const &e){return Str(e,L"nativeModule")==L"magic.tray"||Str(e,L"id")==L"toolbar:@seelen/tb-system-tray";}
    static std::wstring CompoundOwner(JsonObject const &e){auto id=Str(e,L"id");auto owner=Str(e,L"module");return TrayRoot(e)?id:owner;}
    float ToolbarGroupWidth(std::vector<JsonObject> const &);
    float ItemWidth(JsonObject const &item);float DockGroupWidth(wchar_t const *section);
    std::vector<JsonObject> GroupItems(wchar_t const *section,JsonObject const &source=nullptr);
    bool primary=true,attentionReveal=false,gridSuppressed=false,gridWasEdge=false,fullscreenSuppressed=false;
    bool ForegroundFullscreen()const{
        HWND window=GetForegroundWindow();if(!window)return fullscreenSuppressed;
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);wchar_t name[80]{};GetClassNameW(window,name,80);
        if(!pid||pid==GetCurrentProcessId()||pid==static_cast<DWORD>(Num(data,L"hostPid"))||!IsWindowVisible(window)||IsIconic(window)||!wcscmp(name,L"Progman")||!wcscmp(name,L"WorkerW")||!wcscmp(name,L"Shell_TrayWnd")||!wcscmp(name,L"Shell_SecondaryTrayWnd"))return false;
        auto monitor=MonitorFromRect(&full,MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};
        if(!GetMonitorInfoW(monitor,&info))return false;
        RECT client{};if(!GetClientRect(window,&client))return false;MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&client),2);
        auto style=GetWindowLongPtrW(window,GWL_STYLE);return NativeTrueFullscreen(client,info.rcMonitor,IsZoomed(window)&&((style&WS_CAPTION)==WS_CAPTION||(style&WS_THICKFRAME)!=0));
    }
    bool SuppressFullscreen();
    void SetGridSuppressed(bool value);
    std::set<long long> attentionWindows;
    std::vector<float> sectionSeparators;
    float barScroll=0,contentLength=0;
    bool Vertical()const{return dock&&(Str(settings,L"position")==L"Left"||Str(settings,L"position")==L"Right");}
    void PaintPreview70(ID2D1DeviceContext *c);
    void Draw() override;
    void Activate(int index) override;
    void Hide(bool notify = true, bool restore = false) override
    {
        if(!dock&&toolbarOccluded)return;if(occlusionAnimating)FinishToolbarOcclusion();
        ResetDockBodyMotion();ClearDockHover();KillTimer(hwnd,DockPreviewTimer);KillTimer(hwnd,91);KillTimer(hwnd,105);edgePointerWatch=false;HideBarTooltip();
        if(!edge&&NativeMotion(app.model)){
            hidingToolbar=true;edge=true;ReportExtensions();
            if(dock&&Bool(app.model.preferences,L"dockDissolve")&&dockDissolve.Start(app.graphics,barHost,toolbarRoot,width,height,dpi/96)){
                if(SetTimer(hwnd,99,NativeDissolve::DurationMs+16,nullptr))return;
                hidingToolbar=false; // Timer failure must not strand a transparent hit target.
            }else{
            auto animation=app.graphics.compositor.CreateVector3KeyFrameAnimation();animation.InsertExpressionKeyFrame(0,L"this.StartingValue");
            auto side=Str(settings,L"position");float extent=(Vertical()?width:height)+2*static_cast<float>(Num(settings,L"margin"));float3 hidden=Vertical()?float3{extent*dpi/96*(side==L"Left"?-1.f:1.f),0,0}:float3{0,extent*dpi/96*(side==L"Bottom"?1.f:-1.f),0};
            animation.InsertKeyFrame(1,hidden,app.graphics.compositor.CreateCubicBezierEasingFunction({.42f,0},{.58f,1}));animation.Duration(std::chrono::milliseconds(200));toolbarRoot.StartAnimation(L"Offset",animation);if(SetTimer(hwnd,99,210,nullptr))return;hidingToolbar=false;
            }
        }
        if(hidingToolbar)return;
        edge = true;ReportExtensions();
        Release();
        auto side=Str(settings,L"position");MONITORINFO screen{sizeof(screen)};
        GetMonitorInfoW(MonitorFromRect(&full,MONITOR_DEFAULTTONEAREST),&screen);
        RECT r=screen.rcMonitor;
        if(side==L"Left")r.right=r.left+1;else if(side==L"Right")r.left=r.right-1;else if(side==L"Top")r.bottom=r.top+1;else r.top=r.bottom-1;
        SetWindowRgn(hwnd,nullptr,FALSE);
        NativeLayerPosition(hwnd,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|(visible?SWP_SHOWWINDOW:0));
    }
    void Reveal()
    {
        if(gridSuppressed||(dock&&SuppressFullscreen()))return;
        KillTimer(hwnd,92);hidePending=false;
        if (!edge)
            return;
        bool dissolved=dockDissolve.Active();dockDissolve.Reset(barHost,toolbarRoot);
        bool animate=NativeMotion(app.model)&&!dissolved;bool midway=hidingToolbar;hidingToolbar=false;KillTimer(hwnd,99);
        edge = false;
        PositionBar(SWP_NOACTIVATE|(visible?SWP_SHOWWINDOW:0));
        Layout();ScheduleClock();Draw();
        if(dock){dockEnterPending=false;if(animate)StartDockEntrance();return;}
        if(animate){if(!midway){auto side=Str(settings,L"position");float extent=(Vertical()?width:height)+2*static_cast<float>(Num(settings,L"margin"));toolbarRoot.Offset(Vertical()?float3{extent*dpi/96*(side==L"Left"?-1.f:1.f),0,0}:float3{0,extent*dpi/96*(side==L"Bottom"?1.f:-1.f),0});}
            auto a=app.graphics.compositor.CreateVector3KeyFrameAnimation();a.InsertExpressionKeyFrame(0,L"this.StartingValue");a.InsertKeyFrame(1,{0,0,0},app.graphics.compositor.CreateCubicBezierEasingFunction({.42f,0},{.58f,1}));a.Duration(std::chrono::milliseconds(200));toolbarRoot.Offset({0,0,0});toolbarRoot.StartAnimation(L"Offset",a);}
    }
    void Send(int index, std::wstring verb = L"activate", long long target = 0, std::wstring destination = L"");
    std::wstring menuItemKey,menuJumpStamp;
    // Everything a standing menu is built from: the item's own fields (brand mark,
    // style, wordmark, device, label) and the preferences that shape it. A publish
    // that changes neither leaves the open menu alone. Only what the builder reads is
    // compared: the item carries window titles and jump lists that change on their own.
    std::wstring MenuStamp(JsonObject const &item)const{
        std::wstring value;for(auto const *key:{L"id",L"brandIcon",L"brandIconHide",L"brandLabel",L"brandText",L"brandStyle",L"brandDevice",L"customIcon",L"icon",L"label",L"pinned",L"enabled"}){value+=key;value+=L"=";value+=Str(item,key);value+=L"|";}
        // Jump lists refresh in place too (the dock's app menu), so they belong to
        // the same stamp; they are empty for a brand cell, so they never trigger it.
        value+=JumpStamp(item);
        if(app.model.preferences)value+=std::wstring(app.model.preferences.Stringify());
        return value;
    }
    static std::wstring JumpStamp(JsonObject const &item){std::wstring value;for(auto key:{L"jumpPinned",L"jumpFrequent",L"jumpRecent",L"jumpTasks"})value+=item.GetNamedArray(key,JsonArray{}).Stringify();return value;}
    void Menu(int index,bool refresh=false);
    void SendBar(std::wstring const &verb,std::wstring const &destination=L"",int item=-1);
    void ScheduleHide();
    bool PointerKeepsOpen(POINT point){
        if(!dock)return PtInRect(&full,point);
        MONITORINFO screen{sizeof(screen)};if(!GetMonitorInfoW(MonitorFromRect(&full,MONITOR_DEFAULTTONEAREST),&screen))return PtInRect(&full,point);
        return NativeDockPointerHold(full,screen.rcMonitor,point,Str(settings,L"position"));
    }
    void WatchEdgePointer(POINT point){
        bool needed=dock&&visible&&!edge&&!menuOpen&&!barDrag&&PointerKeepsOpen(point)&&!PtInRect(&full,point);
        if(needed&&!edgePointerWatch)edgePointerWatch=SetTimer(hwnd,105,100,nullptr)!=0;
        else if(!needed){KillTimer(hwnd,105);edgePointerWatch=false;}
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
    static LRESULT HandleMessage(HWND window,UINT message,WPARAM w,LPARAM l,std::optional<PointerContact> contact);
};
struct NativeShell
{
    App &app;
    std::map<std::wstring, std::unique_ptr<NativeBar>> bars;
    // A tray icon is drawn exactly as its app ships it, so a black glyph on nothing
    // (Tailscale) is a hole once the bar goes black, and a white one needs the
    // opposite treatment. Two numbers tell them apart: how much of the square is
    // painted at all, and how bright that paint is. They have to be measured while
    // the PNG is still pixels - an ID2D1Bitmap cannot be read back later.
    struct IconTone{float coverage=1,ink=1;};
    std::map<std::wstring, Icon> icons;std::map<std::wstring,IconTone> tones;
    std::set<std::wstring> pending;
    std::unique_ptr<NativePopup> toast;
    double lastVolume = -1;
    bool hintsInitialized = false, lastPaused = false;
    std::wstring lastPlayer, lastNotification;
    std::map<std::wstring, double> lastBrightness;
    std::map<std::wstring, std::wstring> lastWorkspaces;
    std::map<std::wstring, double> brightness;
    size_t iconBytes = 0;
    long long iconAge = 0;
    bool stopping = false;
    static inline NativeShell *eventOwner=nullptr;
    std::vector<HWINEVENTHOOK> windowHooks;
    NativeClockPreferenceWatch clockWatch, regionalClockWatch;
    void ClockPreferenceChanged(){
        clockWatch.Arm();
        regionalClockWatch.Arm();
        InvalidateNativeClockSeconds();
        for(auto const &[_,bar]:bars){
            if(!bar->visible||bar->edge||!std::any_of(bar->entries.begin(),bar->entries.end(),[&](auto const &entry){return Bool(entry,bar->dock?L"dockClock":L"clock");}))continue;
            bar->Layout();bar->ScheduleClock();bar->Draw();
        }
    }
    HWND movingWindow=nullptr;
    bool geometryQueued=false,geometryDeferred=false;
    void ScheduleGeometry(){if(!stopping&&!geometryQueued&&app.relay){geometryQueued=true;PostMessageW(app.relay,WM_APP+22,0,0);}}
    static void CALLBACK WindowEvent(HWINEVENTHOOK,DWORD event,HWND window,LONG object,LONG child,DWORD,DWORD){
        auto owner=eventOwner;if(!owner||owner->stopping)return;
        if(event==EVENT_OBJECT_LOCATIONCHANGE&&(object!=OBJID_WINDOW||child!=CHILDID_SELF||window!=GetForegroundWindow()))return;
        if(event==EVENT_SYSTEM_MOVESIZESTART)owner->movingWindow=window;
        if(event==EVENT_SYSTEM_MOVESIZEEND){owner->movingWindow=nullptr;for(auto const &[_,bar]:owner->bars)bar->reservationDirty=true;}
        if(event==EVENT_SYSTEM_FOREGROUND||event==EVENT_SYSTEM_MINIMIZEEND)for(auto const &[_,bar]:owner->bars)bar->InvalidateDockBackdrop();
        owner->ScheduleGeometry();
    }
    void RefreshGeometry(){
        if(NativeRenderScope::busy){geometryDeferred=true;app.QueueRenderWork();return;}
        geometryDeferred=false;geometryQueued=false;if(stopping||!authoritative)return;
        // Refresh existing bars only: no icon requests, toasts or new windows.
        for(auto const &[_,bar]:bars)if(bar->data)for(auto const &value:authoritative.GetNamedArray(L"monitors",JsonArray{})){
            auto screen=value.GetObject();if(Str(screen,L"id")==bar->monitor){bar->Update(bar->data,screen,false);break;}
        }
    }
    struct PendingOrder {std::wstring token;bool dock;std::vector<std::wstring> items;};
    std::vector<PendingOrder> pendingOrders;
    JsonObject authoritative{nullptr};
    unsigned long long orderSequence=0;
    std::vector<std::wstring> provisionalOrder;
    HWND provisionalOwner=nullptr;
    // Tray icons are ordered by the adapter, not by the block order, so an icon drag
    // needs its own slot. The deadline keeps a freshly dropped arrangement on screen
    // until the saved order comes back through a snapshot.
    std::vector<std::wstring> provisionalTray;HWND provisionalTrayOwner=nullptr;ULONGLONG provisionalTrayUntil=0;
    // Reorder the tray rows inside a built projection, leaving the button and every
    // other row where they are. Ids are the row ids (`tray:...`), in visual order.
    static JsonObject OrderTrayIcons(JsonObject const &source,std::vector<std::wstring> const &iconIds){
        if(iconIds.size()<2)return source;
        JsonObject result;
        for(auto group:{L"left",L"center",L"right"}){
            auto rows=source.GetNamedArray(group,JsonArray{});
            std::vector<JsonObject> icons;for(auto const &v:rows)if(Str(v.GetObject(),L"type")==L"Tray")icons.push_back(v.GetObject());
            std::vector<JsonObject> ordered;for(auto const &id:iconIds)for(auto const &row:icons)if(Str(row,L"id")==id){ordered.push_back(row);break;}
            if(icons.size()<2||ordered.size()!=icons.size()){result.Insert(group,rows);continue;}
            JsonArray rebuilt;size_t next=0;
            for(auto const &v:rows){auto row=v.GetObject();if(Str(row,L"type")==L"Tray")rebuilt.Append(ordered[next++]);else rebuilt.Append(row);}
            result.Insert(group,rebuilt);
        }
        return result;
    }
    // The tray block is mirrored when it crosses into the left group, so its button stays at
    // the outer edge. Doing that while a drag is in flight re-orders rows the ghost offsets
    // were measured against, and the grabbed icon jumps away from the pointer; the caller
    // therefore suppresses the mirror for the provisional model and lets the committed order
    // from the adapter carry it.
    static JsonObject ToolbarOrderModel(JsonObject const &source,std::vector<std::wstring> const &order,bool mirrorTray=true){
        std::map<std::wstring,std::vector<JsonObject>> blocks;std::wstring traySection,trayOwner;
        for(auto group:{L"left",L"center",L"right"})for(auto const &v:source.GetNamedArray(group,JsonArray{})){
            auto item=v.GetObject();auto owner=Str(item,L"module"),id=Str(item,L"id");blocks[owner.empty()?id:owner].push_back(item);
            if(id.empty())blocks[Str(item,L"key")].push_back(item);
            if(NativeBar::TrayRoot(item)){traySection=group;trayOwner=owner.empty()?id:owner;}
        }
        auto a=std::find(order.begin(),order.end(),L"@boundary:left"),b=std::find(order.begin(),order.end(),L"@boundary:right");
        if(a==order.end()||b==order.end())return source;if(a>b)std::swap(a,b);
        JsonObject projected;for(auto section:{L"left",L"center",L"right"})projected.Insert(section,JsonArray{});
        for(auto it=order.begin();it!=order.end();++it){if(it==a||it==b)continue;auto section=it<a?L"left":it<b?L"center":L"right";
            auto block=blocks[*it];if(mirrorTray&&*it==trayOwner&&((traySection==L"left")!=(std::wstring(section)==L"left")))std::reverse(block.begin(),block.end());
            for(auto const &row:block)projected.GetNamedArray(section).Append(row);
        }for(auto group:{L"left",L"center",L"right"})for(auto const &v:source.GetNamedArray(group,JsonArray{}))if(Str(v.GetObject(),L"type")==L"Uac")projected.GetNamedArray(L"left").Append(v);return projected;
    }
    void PreviewToolbarOrder(NativeBar &owner,std::vector<std::wstring> const &order,std::vector<std::wstring> const *trayIcons=nullptr){
        NativeRenderScope projecting(app.relay);
        provisionalOrder=order;provisionalOwner=owner.hwnd;
        // While the drag runs there is no deadline: the list lives exactly as long as
        // the gesture does. A commit switches to a deadline instead, so the drop does
        // not flash back to the saved order while the write is in flight.
        provisionalTray=trayIcons?*trayIcons:std::vector<std::wstring>{};provisionalTrayOwner=owner.hwnd;provisionalTrayUntil=trayIcons?~0ull:0;
        for(auto const &[_,bar]:bars)if(bar->dock==owner.dock&&bar.get()!=&owner&&bar->data){
            JsonObject projected;for(auto const &v:authoritative)projected.Insert(v.Key(),v.Value());ProjectToolbar(projected);ProjectDock(projected);bar->data=projected;bar->Layout();bar->Draw();
        }
    }
    void ReorderToolbarSequence(NativeBar &bar,std::vector<std::wstring> const &order){
        provisionalOrder.clear();provisionalOwner=nullptr;
        if(!authoritative||pendingOrders.size()>=32){if(authoritative)Update(authoritative);return;}
        auto token=std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++orderSequence);
        pendingOrders.push_back({token,bar.dock,order});
        AllowSetForegroundWindow(static_cast<DWORD>(Num(bar.data,L"hostPid")));app.inputEpoch++;
        JsonObject command,request;JsonArray ids;for(auto const &id:order)ids.Append(Text(id));
        Put(command,L"id",std::wstring(bar.dock?L"bar:dock":L"bar:toolbar"));Put(command,L"verb",std::wstring(L"order"));Put(command,L"orderToken",token);command.Insert(L"order",ids);
        Put(request,L"op",std::wstring(L"shell-command"));Put(request,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(bar.hwnd)));request.Insert(L"command",command);
        // Applying a snapshot may remove a monitor and destroy its bar.
        Update(authoritative);
        JsonObject trace;Put(trace,L"token",token);Put(trace,L"stage",std::wstring(L"projected"));app.Telemetry(L"bar-reorder",trace);
        app.Request(request,[this,token](JsonObject const &response){if(!Bool(response,L"ok")){std::erase_if(pendingOrders,[&](auto const &p){return p.token==token;});if(authoritative)Update(authoritative);JsonObject d;Put(d,L"error",Str(response,L"error"));app.Telemetry(L"toolbar-order-rejected",d);}});
        Put(trace,L"stage",std::wstring(L"queued"));app.Telemetry(L"bar-reorder",trace);
    }
    // The tray icons are a list the adapter owns, so this commit carries the visual
    // order and lets the adapter persist it. In the left group the adapter mirrors the
    // block, which means the order that gets saved is the reverse of the one on screen.
    void ReorderTraySequence(NativeBar &bar,std::vector<std::wstring> const &rows,std::wstring const &section){
        provisionalOrder.clear();provisionalOwner=nullptr;
        std::vector<std::wstring> visual=rows;if(section==L"left")std::reverse(visual.begin(),visual.end());
        JsonArray values;
        for(auto const &id:visual){std::wstring stable;for(auto const &entry:bar.entries)if(Str(entry,L"id")==id){stable=Str(entry,L"stableId");break;}if(!stable.empty())values.Append(Text(stable));}
        if(values.Size()<2)return;
        provisionalTray=rows;provisionalTrayOwner=bar.hwnd;provisionalTrayUntil=GetTickCount64()+1500;
        AllowSetForegroundWindow(static_cast<DWORD>(Num(bar.data,L"hostPid")));app.inputEpoch++;
        JsonObject command,request;Put(command,L"id",std::wstring(L"bar:toolbar"));Put(command,L"verb",std::wstring(L"tray-order"));command.Insert(L"trayOrder",values);
        Put(request,L"op",std::wstring(L"shell-command"));Put(request,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(bar.hwnd)));request.Insert(L"command",command);
        app.Request(request,[this](JsonObject const &response){
            if(Bool(response,L"ok"))return;
            provisionalTray.clear();if(authoritative)Update(authoritative);
            JsonObject detail;Put(detail,L"error",Str(response,L"error"));app.Telemetry(L"tray-order-rejected",detail);
        });
        if(authoritative)Update(authoritative);
        JsonObject trace;Put(trace,L"stage",std::wstring(L"tray"));Put(trace,L"count",static_cast<double>(values.Size()));app.Telemetry(L"bar-reorder",trace);
    }
    void ProjectToolbar(JsonObject const &data){
        auto receipts=data.GetNamedArray(L"orderReceipts",JsonArray{});
        for(auto const &v:receipts){auto receipt=v.GetObject();auto token=Str(receipt,L"token");
            bool found=std::any_of(pendingOrders.begin(),pendingOrders.end(),[&](auto const &order){return order.token==token;});
            if(found&&!Bool(receipt,L"ok")){JsonObject detail;Put(detail,L"error",Str(receipt,L"error"));app.Telemetry(L"toolbar-order-save-failed",detail);}
            // A successful save can precede publication of its new configuration.
            // Keep the optimistic order until that exact revision is visible.
            if(!Bool(receipt,L"ok")||Num(data,L"configurationRevision")>=Num(receipt,L"revision"))std::erase_if(pendingOrders,[&](auto const &order){return order.token==token;});
        }
        auto toolbar=ShellObject(data,L"toolbar");
        for(auto const &order:pendingOrders)if(!order.dock)toolbar=ToolbarOrderModel(toolbar,order.items);
        if(!provisionalOrder.empty()){auto owner=std::find_if(bars.begin(),bars.end(),[&](auto const &v){return v.second->hwnd==provisionalOwner;});if(owner!=bars.end()&&!owner->second->dock)toolbar=ToolbarOrderModel(toolbar,provisionalOrder,!owner->second->barDrag);}
        if(!provisionalTray.empty()){
            if(GetTickCount64()>provisionalTrayUntil)provisionalTray.clear();
            else {auto owner=std::find_if(bars.begin(),bars.end(),[&](auto const &v){return v.second->hwnd==provisionalTrayOwner;});if(owner!=bars.end()&&!owner->second->dock)toolbar=OrderTrayIcons(toolbar,provisionalTray);}
        }
        data.Insert(L"toolbar",toolbar);
    }
    void ProjectDock(JsonObject const &data){
        auto dock=ShellObject(data,L"dock");
        for(auto const &order:pendingOrders)if(order.dock)dock=ToolbarOrderModel(dock,order.items);
        if(!provisionalOrder.empty()){auto owner=std::find_if(bars.begin(),bars.end(),[&](auto const &v){return v.second->hwnd==provisionalOwner;});if(owner!=bars.end()&&owner->second->dock)dock=ToolbarOrderModel(dock,provisionalOrder,!owner->second->barDrag);}
        data.Insert(L"dock",dock);
    }
    std::map<std::wstring,std::wstring> iconSources;
    std::set<std::wstring> refreshIcons;
    static std::wstring IconSource(JsonObject const &item){
        auto windows=item.GetNamedArray(L"windows",JsonArray{});
        return Str(item,L"icon")+L":"+Str(item,L"imageVersion")+L":"+Str(item,L"umid")+L":"+
            (windows.Size()?std::to_wstring(static_cast<long long>(Num(windows.GetAt(0).GetObject(),L"hwnd"))):L"");
    }
    std::unique_ptr<NativePreview> preview;
    NativeShell(App &app,bool observeWindows=true);
    ~NativeShell();
    void ShowPreview(NativeBar &bar, int index);void RefreshPreview();
    void HidePreview(NativeBar *owner = nullptr);
    bool PreviewHasCursor();
    bool PreviewFor(NativeBar const &bar,int index);bool PreviewHasPoint(POINT);
    void Update(JsonObject const &incoming)
    {
        authoritative=JsonObject::Parse(incoming.Stringify());
        auto data=JsonObject::Parse(authoritative.Stringify());
        ProjectToolbar(data);ProjectDock(data);
        if(app.model.recoveryCount>0){auto right=data.GetNamedObject(L"dock").GetNamedArray(L"right");JsonObject sep,item;
            Put(sep,L"type",std::wstring(L"Separator"));Put(sep,L"key",std::wstring(L"@magic/recovery-separator"));right.Append(sep);
            Put(item,L"type",std::wstring(L"Recovery"));Put(item,L"key",std::wstring(L"@magic/display-recovery"));Put(item,L"id",std::wstring(L"@magic/display-recovery"));Put(item,L"label",std::wstring(L"副屏内容 · ")+std::to_wstring(app.model.recoveryCount));Put(item,L"icon",std::wstring(L"RiComputerLine"));item.Insert(L"enabled",JsonValue::CreateBooleanValue(true));Put(item,L"recoveryCount",app.model.recoveryCount);right.Append(item);}
        if(Bool(data,L"windowMoving"))HidePreview();
        if (data.GetNamedArray(L"monitors").Size() == 0)
        {
            HidePreview();
            bars.clear();
            if (toast)
                toast->Hide(false);
            return;
        }
        std::map<std::wstring,std::wstring> sources;
        auto dockData=ShellObject(data,L"dock");
        auto toolbarData=ShellObject(data,L"toolbar");
        for(auto group:{L"left",L"center",L"right"})for(auto const &value:dockData.GetNamedArray(group,JsonArray{})){
            auto item=value.GetObject();auto key=Str(item,L"key"),path=IconSource(item);sources[key]=path;
            auto badge=ShellObject(item,L"badge");if(Str(badge,L"kind")==L"image"){auto badgeKey=key+L":badge",stamp=Str(badge,L"value")+L":"+std::wstring(badge.Stringify());sources[badgeKey]=stamp;if(!iconSources.contains(badgeKey)||iconSources[badgeKey]!=stamp){refreshIcons.insert(badgeKey);pending.erase(badgeKey);}}

            if(!iconSources.contains(key)||iconSources[key]!=path){refreshIcons.insert(key);pending.erase(key);}
        }
        for(auto group:{L"left",L"center",L"right"})for(auto const &value:toolbarData.GetNamedArray(group,JsonArray{})){
            auto item=value.GetObject();if(Str(item,L"icon").empty())continue;auto key=Str(item,L"id"),source=Str(item,L"icon")+L":"+Str(item,L"imageVersion");sources[key]=source;
            if(!iconSources.contains(key)||iconSources[key]!=source){auto found=icons.find(key);if(found!=icons.end()){iconBytes-=found->second.bytes;icons.erase(found);}pending.erase(key);}
        }
        std::erase_if(refreshIcons,[&](auto const &key){return !sources.contains(key);});
        std::erase_if(pending,[&](auto const &key){return !sources.contains(key);});
        iconSources=std::move(sources);
        double volume = Num(data, L"volume", -1);
        auto flyouts = ShellObject(data, L"flyouts");
        if (lastVolume >= 0 && volume >= 0 && std::abs(volume - lastVolume) > 0.001 &&
            Bool(flyouts, L"enabled", true) && Bool(flyouts, L"showVolumeChange", true))
            Toast(data, L"音量", volume);
        lastVolume = volume;
        std::set<std::wstring> live;
        for (auto const &value : data.GetNamedArray(L"monitors"))
        {
            auto screen = value.GetObject();
            auto id = Str(screen, L"id");
            for (bool dock : {false, true})
            {
                auto key = id + (dock ? L":dock" : L":toolbar");
                if (!Bool(screen, dock ? L"dock" : L"toolbar"))
                    continue;
                live.insert(key);
                auto &bar = bars[key];
                if (!bar)
                    bar = std::make_unique<NativeBar>(app, *this, id, dock);
                bar->Update(data, screen);
            }
        }
        for (auto it = bars.begin(); it != bars.end();)
            if (!live.contains(it->first))
            {
                HidePreview(it->second.get());
                if(provisionalOwner==it->second->hwnd){provisionalOrder.clear();provisionalOwner=nullptr;provisionalTray.clear();}
                // The menu's owner is gone, so its rebuild identity is stale.
                if(app.menus&&app.menus->itemBar==it->second->hwnd){app.menus->Close(false);app.menus->itemKey.clear();app.menus->itemStamp.clear();app.menus->itemBar=nullptr;}
                it = bars.erase(it);
            }
            else
                ++it;
        RefreshPreview();RequestIcons();
        if(Bool(data,L"magicNotificationQuiet")&&toast&&toast->visible&&Str(toast->Data(),L"sourceWidget")==L"@magic/notifications")toast->Hide(false);
        auto sessionStatus=Str(data,L"notificationSessionStatus");
        if(!sessionStatus.empty()&&sessionStatus!=notificationSessionStatus){
            notificationSessionStatus=sessionStatus;
            Info(data,L"通知会话",sessionStatus,L"@magic/notifications");
        }
        auto playerValue = data.TryLookup(L"player"), notificationValue = data.TryLookup(L"notification");
        auto player =
            playerValue && playerValue.ValueType() == JsonValueType::Object ? playerValue.GetObject() : JsonObject{};
        auto notification = notificationValue && notificationValue.ValueType() == JsonValueType::Object
                                ? notificationValue.GetObject()
                                : JsonObject{};
        if (hintsInitialized && Bool(flyouts, L"enabled", true))
        {
            if (Bool(flyouts, L"showMediaPlayerChange", true) && !Str(player, L"key").empty() &&
                lastPlayer != Str(player, L"key"))
                Info(data, Str(player, L"title"), Str(player, L"detail"), L"@magic/media-popup");
            if (Bool(flyouts, L"showNotifications", true) && !Str(notification, L"key").empty() &&
                lastNotification != Str(notification, L"key"))
                Info(data, Str(notification, L"title"), Str(notification, L"detail"), L"@magic/notifications");
            if (lastPaused != Bool(data, L"shortcutsPaused"))
                Info(data, L"快捷键", Bool(data, L"shortcutsPaused") ? L"已暂停" : L"已恢复", L"");
        }
        lastPlayer = Str(player, L"key");
        lastNotification = Str(notification, L"key");
        lastPaused = Bool(data, L"shortcutsPaused");
        std::map<std::wstring,double> brightness;
        // Only a hand deserves a card, but no vendor signal separates one from the
        // ambient sensor: adaptive brightness is on here, absent on a desktop, and an
        // OEM's own service can ignore the setting altogether. So the gate rests on the
        // one fact that travels between them - the provider marks the writes we made.
        // Those speak by default; "跟随系统全部变化" in the settings window brings back
        // every external change, which is the old behaviour and exactly what an
        // auto-brightness machine turns into noise.
        auto brightnessPayload=ShellObject(data,L"brightness");
        auto hintPrefs=app.model.preferences;
        bool hints=Bool(hintPrefs,L"brightnessHints",true),follow=Bool(hintPrefs,L"brightnessFollowSystem");
        for(auto const &v:brightnessPayload.GetNamedArray(L"displays",JsonArray{})){
            auto row=v.GetObject();auto id=Str(row,L"id");double level=Num(row,L"value",-1);if(level<0||level>1)continue;brightness[id]=level;
            bool ours=Bool(row,L"manual");
            if(hintsInitialized&&hints&&(follow||ours)&&lastBrightness.contains(id)&&std::abs(lastBrightness[id]-level)>.001&&Bool(flyouts,L"enabled",true))Toast(data,L"亮度",level,L"",L"",id);
        }
        lastBrightness = std::move(brightness);
        std::map<std::wstring, std::wstring> workspaces;
        if (data.HasKey(L"workspaces"))
            for (auto const &entry : data.GetNamedObject(L"workspaces"))
            {
                auto workspace = entry.Value().GetObject();
                std::wstring key{entry.Key()};
                auto active = Str(workspace, L"key");
                workspaces[key] = active;
                if (hintsInitialized && lastWorkspaces.contains(key) && lastWorkspaces[key] != active &&
                    Bool(flyouts, L"enabled", true) && Bool(flyouts, L"showWorkspaceChange", true))
                    Info(data, L"工作区", Str(workspace, L"title"), L"");
            }
        lastWorkspaces = std::move(workspaces);
        hintsInitialized = true;
    }
    std::wstring notificationSessionStatus;
    void Info(JsonObject const &snapshot, std::wstring const &title, std::wstring const &detail,
              std::wstring const &widget)
    {
        Toast(snapshot,title,-1,detail.empty()?title:detail,widget);
    }
    void Toast(JsonObject const &snapshot,std::wstring const &label,double value,std::wstring text=L"",std::wstring widget=L"",std::wstring displayId=L"")
    {
        if(NativeNotificationMuted(snapshot,widget,value))return;
        if(bars.empty())return;NativeBar *bar=nullptr;auto focused=Str(ShellObject(snapshot,L"focused"),L"monitor");
        for(auto const &[id,candidate]:bars)if(!candidate->dock&&(!bar||candidate->monitor==focused))bar=candidate.get();
        if(!bar)bar=bars.begin()->second.get();if(!bar->hwnd)return;if(!toast)toast=std::make_unique<NativePopup>(app);
        auto levelKind=value<0?L"info":label==L"亮度"?L"brightness":L"volume";
        if(toast->visible&&!toast->dismissing&&value>=0&&Str(toast->Data(),L"levelKind")==levelKind&&Str(toast->Data(),L"displayId")==displayId){
            KillTimer(toast->hwnd,84);if(!toast->flyoutDragging)SetTimer(toast->hwnd,84,4200,nullptr);
            if(!toast->flyoutDragging&&!toast->flyoutInFlight&&!toast->flyoutPending){if(GetTickCount64()<toast->flyoutEchoUntil&&std::abs(value-Num(toast->Data(),L"value"))>.0051)return;toast->flyoutEchoUntil=0;Put(toast->Data(),L"value",value);Put(toast->Data(),L"title",label+L" "+std::to_wstring(static_cast<int>(std::lround(value*100)))+L"%");toast->caption=Str(toast->Data(),L"title");toast->BuildRows();toast->Draw();}return;
        }
        JsonObject packet,payload,position,data,account;Put(packet,L"kind",std::wstring(L"@magic/flyouts"));Put(packet,L"token",std::wstring(L"passive"));
        MONITORINFO screen{sizeof(screen)};GetMonitorInfoW(MonitorFromRect(&bar->full,MONITOR_DEFAULTTONEAREST),&screen);
        Put(position,L"x",static_cast<double>((screen.rcWork.left+screen.rcWork.right)/2));Put(position,L"y",static_cast<double>(screen.rcWork.top+12*bar->dpi/96));
        payload.Insert(L"desiredPosition",position);Put(payload,L"alignX",std::wstring(L"Center"));packet.Insert(L"payload",payload);
        Put(data,L"title",value>=0?label+L" "+std::to_wstring(static_cast<int>(std::lround(value*100)))+L"%":label);
        if(text.size()>32768)text.resize(32768);Put(data,L"text",text);Put(data,L"value",value);Put(data,L"levelKind",std::wstring(levelKind));Put(data,L"levelLabel",label);Put(data,L"displayId",displayId);
        Put(data,L"sourceWidget",widget);data.Insert(L"canOpen",JsonValue::CreateBooleanValue(!widget.empty()));data.Insert(L"dark",JsonValue::CreateBooleanValue(Bool(snapshot,L"dark",Preview70Theme::systemDark)));
        Put(data,L"duration",label==L"已复制图片"?1400:std::clamp(Num(ShellObject(snapshot,L"flyouts"),L"timeToShow",4)*1000,1000.,10000.));data.Insert(L"rows",JsonArray{});
        Put(data,L"levelKind",std::wstring(levelKind));packet.Insert(L"data",data);packet.Insert(L"account",account);toast->passiveClick={};toast->Show(packet);
        auto owner=toast->life;
        if(value>=0)toast->flyoutAdjust=[this,owner,mode=std::wstring(levelKind),displayId](double n,bool feedback,std::function<void(bool,double)> done){
            auto self=owner?owner->owner:nullptr;if(!self||!self->visible){done(false,-1);return;}
            JsonObject r;Put(r,L"op",std::wstring(L"flyout-adjust"));Put(r,L"mode",mode);Put(r,L"displayId",displayId);Put(r,L"value",n);r.Insert(L"feedback",JsonValue::CreateBooleanValue(feedback));Put(r,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(self->hwnd)));
            app.Request(r,[done](JsonObject const &reply){auto result=reply.GetNamedObject(L"result",JsonObject{});double actual=Num(result,L"value",-1);done(Bool(reply,L"ok")&&Bool(result,L"accepted")&&std::isfinite(actual)&&actual>=0&&actual<=1,actual);});
        };
        if(!widget.empty())toast->passiveClick=[this,widget]{
            JsonObject command,r;Put(command,L"id",std::wstring(L""));Put(command,L"widget",widget);
            auto bar=bars.begin()->second.get();Put(r,L"op",std::wstring(L"shell-command"));Put(r,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(bar->hwnd)));r.Insert(L"command",command);AllowSetForegroundWindow(static_cast<DWORD>(Num(bar->data,L"hostPid")));app.Request(r);
        };
    }
    void ClearIcons()
    {
        app.honor.Clear();icons.clear();tones.clear();
        pending.clear();
        iconBytes = 0;
    }
    void RequestIcons()
    {
        if (stopping)
            return;
        JsonArray keys;
        for (auto const &[id, bar] : bars)
            if (!bar->edge)
                for (auto const &entry : bar->entries)
                {
                    if(bar->dock&&Str(entry,L"type")==L"Media")continue;
                    auto key = bar->dock?Str(entry,L"key"):Str(entry,L"id");
                    bool official=bar->dock?!HonorDockFile(entry,HonorScheme(app.model.preferences)).empty():!HonorAppFile(entry,HonorScheme(app.model.preferences)).empty();
                    if (!official&&(Str(entry,L"type")==L"AppOrFile"||!Str(entry, L"icon").empty()) && (!icons.contains(key)||refreshIcons.contains(key)) && !pending.contains(key))
                    {
                        keys.Append(Text(key));
                        pending.insert(key);
                        if (keys.Size() == 16)
                            break;
                    }
                    auto badge=ShellObject(entry,L"badge");auto badgeKey=key+L":badge";
                    if(keys.Size()<16&&Str(badge,L"kind")==L"image"&&(!icons.contains(badgeKey)||refreshIcons.contains(badgeKey))&&!pending.contains(badgeKey)){keys.Append(Text(badgeKey));pending.insert(badgeKey);}
                    if (keys.Size() == 16)
                        break;
                }
        if (!keys.Size())
            return;
        JsonObject request;
        Put(request, L"op", std::wstring(L"shell-icons"));
        request.Insert(L"keys", keys);
        auto requestedSources=iconSources;
        app.Request(request, [this,requestedSources](JsonObject const &response) {
            if (stopping)
                return;
            if (!Bool(response, L"ok"))
            {
                pending.clear();
                return;
            }
            auto result = response.GetNamedObject(L"result");
            bool updated = false;
            for (auto const &value : result.GetNamedArray(L"icons"))
            {
                auto row = value.GetObject();
                auto key = Str(row, L"key"), png = Str(row, L"png");
                if(!iconSources.contains(key)||!requestedSources.contains(key)||iconSources[key]!=requestedSources.at(key)){pending.erase(key);continue;}
                if (png.empty())
                    continue;
                DWORD size = 0;
                if (!CryptStringToBinaryW(png.c_str(), static_cast<DWORD>(png.size()), CRYPT_STRING_BASE64, nullptr,
                                          &size, nullptr, nullptr) ||
                    size > 65536)
                    continue;
                std::vector<BYTE> bytes(size);
                if (!CryptStringToBinaryW(png.c_str(), static_cast<DWORD>(png.size()), CRYPT_STRING_BASE64,
                                          bytes.data(), &size, nullptr, nullptr))
                    continue;
                com_ptr<IStream> stream;
                stream.attach(SHCreateMemStream(bytes.data(), size));
                com_ptr<IWICBitmapDecoder> decoder;
                check_hresult(app.graphics.wic->CreateDecoderFromStream(stream.get(), nullptr,
                                                                        WICDecodeMetadataCacheOnLoad, decoder.put()));
                com_ptr<IWICBitmapFrameDecode> frame;
                check_hresult(decoder->GetFrame(0, frame.put()));
                UINT w, h;
                check_hresult(frame->GetSize(&w, &h));
                if (w > 256 || h > 256 || !w || !h)
                    continue;
                size_t allocation = static_cast<size_t>(w) * h * 4;
                while (!icons.empty() && (icons.size() >= 128 || iconBytes + allocation > 8 * 1024 * 1024))
                {
                    auto old = std::min_element(icons.begin(), icons.end(), [](auto const &a, auto const &b) {
                        return a.second.age < b.second.age;
                    });
                    iconBytes -= old->second.bytes;tones.erase(old->first);
                    icons.erase(old);
                }
                com_ptr<IWICFormatConverter> converter;
                check_hresult(app.graphics.wic->CreateFormatConverter(converter.put()));
                check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                                    nullptr, 0, WICBitmapPaletteTypeCustom));
                com_ptr<ID2D1DeviceContext> context;
                check_hresult(app.graphics.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.put()));
                Icon icon;
                check_hresult(context->CreateBitmapFromWicBitmap(converter.get(), nullptr, icon.bitmap.put()));
                std::vector<BYTE> pixels(static_cast<size_t>(w)*h*4);if(SUCCEEDED(converter->CopyPixels(nullptr,w*4,static_cast<UINT>(pixels.size()),pixels.data()))){
                    // PBGRA is premultiplied, so a pixel's colour has to be divided back
                    // by its own alpha before it can be compared to anything. The old
                    // formula averaged over the whole canvas, which mixed "how much is
                    // painted" into "how bright the paint is" - a white glyph covering a
                    // third of the square scored 0.33 and looked as dark as navy.
                    double painted=0,brightness=0;
                    for(size_t k=0;k<pixels.size();k+=4){
                        double alpha=pixels[k+3];if(alpha<=4)continue;
                        painted+=alpha/255;
                        brightness+=alpha/255*(.299*pixels[k+2]+.587*pixels[k+1]+.114*pixels[k])/alpha;
                    }
                    IconTone tone;
                    if(painted>0){tone.coverage=static_cast<float>(painted/(w*h));tone.ink=static_cast<float>(brightness/painted);}
                    tones[key]=tone;
                }
                icon.bytes = allocation;
                icon.age = ++iconAge;
                if(auto old=icons.find(key);old!=icons.end())iconBytes-=old->second.bytes;
                refreshIcons.erase(key);iconBytes += allocation;
                icons[key] = std::move(icon);
                pending.erase(key);
                updated = true;
            }
            if (updated)
                for (auto const &[id, bar] : bars)
                    if (!bar->edge)
                        bar->Draw();
            RequestIcons();
        });
    }
};
inline void NativeBar::Create()
{
    if (hwnd)
        return;
    WNDCLASSEXW type{sizeof(type)};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = Proc;
    type.lpszClassName = L"MagicCompositionNativeBar";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.style = CS_DBLCLKS;
    RegisterClassExW(&type);
    caption = dock ? L"Magic 视界 · 原生 Dock" : L"Magic 视界 · 原生顶栏";
    kind = dock ? L"dock" : L"toolbar";
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, type.lpszClassName, caption.c_str(),
                           WS_POPUP, full.left, full.top, full.right - full.left, full.bottom - full.top, nullptr,
                           nullptr, type.hInstance, this);
    if (!hwnd)
        throw_last_error();
    check_hresult(
        app.graphics.compositor.as<abiComposition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
            hwnd, TRUE, reinterpret_cast<abiComposition::Desktop::IDesktopWindowTarget **>(put_abi(target))));
    // Attach each visual once: the container owns the background before becoming the target root.
    visual = app.graphics.compositor.CreateSpriteVisual();
    life = std::make_shared<PopupLife>();
    life->owner = this;
    visible = true;
    {toolbarRoot=app.graphics.compositor.CreateContainerVisual();toolbarLayer=app.graphics.compositor.CreateContainerVisual();toolbarRoot.Children().InsertAtBottom(visual);toolbarRoot.Children().InsertAtTop(toolbarLayer);barHost=app.graphics.compositor.CreateContainerVisual();barHost.Children().InsertAtTop(toolbarRoot);target.Root(barHost);}
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    if (dock)
        InstallNativeBarDrop(*this);

}
inline void NativeBar::RefreshDpi(){
    if(!dpiRefreshPending||!hwnd||app.stopping)return;
    if(NativeRenderScope::busy){app.QueueRenderWork();return;}
    {NativeRenderScope updating(app.relay);dpiRefreshPending=false;dpi=pendingDpi;Release();}
    {JsonObject detail;Put(detail,L"stage",std::wstring(L"dpichanged"));Put(detail,L"dpi",static_cast<double>(dpi));Put(detail,L"monitor",monitor);detail.Insert(L"dock",JsonValue::CreateBooleanValue(dock));Put(detail,L"windowDpi",static_cast<double>(GetDpiForWindow(hwnd)));app.Telemetry(L"bar-dpi",detail);}
    Draw();
}
inline void NativeBar::Update(JsonObject const &snapshot, JsonObject const &screen,bool notifyReady)
{
    float previousWidth=width,previousHeight=height;bool existed=hwnd!=nullptr;
    auto old = data ? to_string(data.GetNamedObject(dock ? L"dock" : L"toolbar").Stringify()) +
                          to_string(data.GetNamedObject(dock ? L"dockSettings" : L"toolbarSettings").Stringify()) +
                          std::to_string(Bool(data, L"dark")) +
                          std::to_string(Num(data.GetNamedObject(L"focused"), L"hwnd"))
                    : std::string{};
    if(data&&Num(data.GetNamedObject(L"focused"),L"hwnd")!=Num(snapshot.GetNamedObject(L"focused"),L"hwnd"))HideBarTooltip();
    data = snapshot;
    primary=Bool(screen,L"primary",true);
    settings = snapshot.GetNamedObject(dock ? L"dockSettings" : L"toolbarSettings");
    dark = Bool(snapshot, L"dark",Preview70Theme::systemDark);
    auto rect = screen.GetNamedObject(L"rect");
    float scale = static_cast<float>(Num(screen, L"scale", 1));
    float previousDpi=dpi;dpi = 96 * scale;
    if(dpi!=previousDpi){JsonObject detail;Put(detail,L"stage",std::wstring(L"update"));Put(detail,L"dpi",static_cast<double>(dpi));Put(detail,L"scale",static_cast<double>(scale));Put(detail,L"monitor",monitor);detail.Insert(L"dock",JsonValue::CreateBooleanValue(dock));if(hwnd)Put(detail,L"windowDpi",static_cast<double>(GetDpiForWindow(hwnd)));app.Telemetry(L"bar-dpi",detail);}
    int left = static_cast<int>(Num(rect, L"left")), top = static_cast<int>(Num(rect, L"top")),
        right = static_cast<int>(Num(rect, L"right")), bottom = static_cast<int>(Num(rect, L"bottom"));
    if(dock&&Bool(screen,L"toolbar")){auto toolbar=ShellObject(snapshot,L"toolbarSettings");int reserve=static_cast<int>((Num(toolbar,L"itemSize",16)+2*Num(toolbar,L"padding",8)+2*Num(toolbar,L"margin"))*scale);if(Str(toolbar,L"position")==L"Bottom")bottom-=reserve;else top+=reserve;}
    padding = static_cast<float>(Num(settings, L"padding", 8));
    gap = static_cast<float>(Num(settings, L"spaceBetweenItems", 8));
    iconSize = static_cast<float>(Num(settings, dock ? L"size" : L"itemSize", dock ? 40 : 16));
    int total = 0;
    if (dock)
        for (auto const *section : {L"left", L"center", L"right"})
            total += snapshot.GetNamedObject(L"dock").GetNamedArray(section).Size();
    float h=iconSize+2*padding;
    float length=2*padding;bool hasDockItems=false;
    if(dock)for(auto const *section:{L"left",L"center",L"right"}){
        auto values=GroupItems(section);hasDockItems|=!values.empty();length+=DockGroupWidth(section);
    }
    if(dock&&!hasDockItems)length=MeasureToolbarText(BarText(L"weg.empty"),12.8f)+6*padding;
    contentLength=length;
    float available=static_cast<float>(Vertical()?bottom-top:right-left)/scale-2*static_cast<float>(Num(settings,L"margin",8));
    float w=dock?std::min(length,available):available;
    if(dock&&Str(settings,L"mode")==L"FullWidth")w=available;
    if(Vertical()){h=w;w=iconSize+2*padding;}
    width = w;
    height = h;
    bool bottomBar = dock || Str(settings, L"position") == L"Bottom";
    int margin = static_cast<int>(Num(settings, L"margin", dock ? 8 : 0) * scale);
    auto side=Str(settings,L"position");
    RECT next{dock?(side==L"Left"?left+margin:side==L"Right"?right-margin-static_cast<int>(w*scale):left+((right-left)-static_cast<int>(w*scale))/2):left+margin,
              dock?(Vertical()?top+((bottom-top)-static_cast<int>(h*scale))/2:side==L"Top"?top+margin:bottom-margin-static_cast<int>(h*scale)):bottomBar?bottom-margin-static_cast<int>(h*scale):top+margin,0,0};
    next.right = next.left + static_cast<int>(w * scale);
    next.bottom = next.top + static_cast<int>(h * scale);
    RECT previous=full;
    bool requestChanged=memcmp(&requestedBar,&next,sizeof(next))!=0;
    requestedBar=next;full=next;
    Create();if(dock&&!existed)dockEnterPending=true;
    if(dock&&existed&&!edge&&!barDrag&&(previousWidth!=width||previousHeight!=height))PrepareDockReflow(previousWidth,previousHeight,previous);
    if(tooltip){
        if(tooltipDpi!=dpi){if(inputFont)DeleteObject(inputFont);inputFont=CreateFontW(-static_cast<int>(12.8f*dpi/96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,NativeUiFont::Family().c_str());tooltipDpi=dpi;SendMessageW(tooltip,WM_SETFONT,reinterpret_cast<WPARAM>(inputFont),FALSE);}
        auto t=Preview70Theme{dark};auto bg=t.Raised(),fg=t.Ink();SendMessageW(tooltip,TTM_SETTIPBKCOLOR,RGB(static_cast<int>(bg.r*255),static_cast<int>(bg.g*255),static_cast<int>(bg.b*255)),0);SendMessageW(tooltip,TTM_SETTIPTEXTCOLOR,RGB(static_cast<int>(fg.r*255),static_cast<int>(fg.g*255),static_cast<int>(fg.b*255)),0);
        SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,300);
    }
    bool reserve=!dock&&!Bool(snapshot,L"preview")&&Str(settings,L"hideMode")==L"Never";
    // A hidden taskbar keeps its work-area reservation, so the edge is already
    // taken. Reserving it again makes ABM_QUERYPOS/SETPOS clamp the bar down by
    // the taskbar thickness. Only reserve when the edge is actually free.
    if(reserve){
        RECT workArea{};
        if(SystemParametersInfoW(SPI_GETWORKAREA,0,&workArea,0)){
            LONG occupied=bottomBar?(bottom-workArea.bottom):(workArea.top-top);
            if(occupied>=static_cast<LONG>(h*scale))reserve=false;
        }
    }
    if(reserved&&!reserve){APPBARDATA bar{sizeof(bar)};bar.hWnd=hwnd;SHAppBarMessage(ABM_REMOVE,&bar);reserved=false;}
    if(reserve){
        APPBARDATA bar{sizeof(bar)};bar.hWnd=hwnd;bar.uCallbackMessage=WM_APP+21;bar.uEdge=bottomBar?ABE_BOTTOM:ABE_TOP;
        bool added=!reserved;if(added)reserved=SHAppBarMessage(ABM_NEW,&bar)!=0;
        if(reserved&&(requestChanged||added||reservationDirty)){
            positioningBar=true;reservationDirty=false;
            // Keep the work area stable during window drags. Changing the
            // reservation to align Snap UI also relayouts the desktop/apps.
            LONG inset=static_cast<LONG>(h*scale)+2*margin;
            bar.rc={left,bottomBar?bottom-inset:top,right,bottomBar?bottom:top+inset};
            SHAppBarMessage(ABM_QUERYPOS,&bar);
            // The shell still treats a docked-but-hidden taskbar as occupying that
            // edge, so ABM_QUERYPOS pushes us inward by the taskbar thickness.
            // The bar must sit on the monitor edge (rcMonitor), so restore it.
            if(bottomBar){bar.rc.top=bottom-inset;bar.rc.bottom=bottom;}
            else{bar.rc.top=top;bar.rc.bottom=top+inset;}
            if(added||memcmp(&reservedBar,&bar.rc,sizeof(bar.rc))!=0)SHAppBarMessage(ABM_SETPOS,&bar);
            reservedBar=bar.rc;positioningBar=false;
        }
        if(reserved){full={reservedBar.left+margin,reservedBar.top+margin,reservedBar.right-margin,reservedBar.top+margin+static_cast<LONG>(h*scale)};}
    }
    bool moved=memcmp(&previous,&full,sizeof(full))!=0;
    if(occlusionAnimating&&(moved||!NativeFeedbackMotion(app.model)))FinishToolbarOcclusion();
    if(hidingToolbar&&(moved||!NativeMotion(app.model)||(dockDissolve.Active()&&!Bool(app.model.preferences,L"dockDissolve"))))FinishToolbarHide();
    Layout();
    if(dock){auto mode=Str(settings,L"revealOnAttention");std::set<long long> live;bool flashing=false;auto focused=static_cast<long long>(Num(data.GetNamedObject(L"focused"),L"hwnd"));
        for(auto const &entry:entries)if(entry.HasKey(L"windows"))for(auto const &v:entry.GetNamedArray(L"windows")){auto window=v.GetObject();auto id=static_cast<long long>(Num(window,L"hwnd"));live.insert(id);if((Str(window,L"attention")==L"Flashing"||Str(window,L"attention")==L"Highlighted")&&id!=focused){flashing|=Str(window,L"attention")==L"Flashing";attentionWindows.insert(id);}}
        std::erase_if(attentionWindows,[&](long long id){return !live.contains(id)||id==focused;});if(mode!=L"UntilActivated")attentionWindows.clear();attentionReveal=mode==L"WhileFlashing"?flashing:mode==L"UntilActivated"&&!attentionWindows.empty();if(attentionReveal)Reveal();
    }
    // "Covered" is a fact about the screen, not a consequence of the transparency
    // switch above it: with that switch off the three backdrop choices still have to
    // apply, which is the only way "fullscreen turns the bar black" can be tested.
    if(!dock)toolbarCoveredByApplication=ToolbarCoveredByApplication();
    bool fullscreen=ForegroundFullscreen();
    // Read geometry at the event, rather than trusting a cached focus payload.
    // A valid created bar is ready even while a fullscreen entrance overlay
    // temporarily hides it. The overlay itself waits for this handshake.
    if(notifyReady&&!readyNotified){
        JsonObject request;
        Put(request,L"op",std::wstring(L"shell-ready"));
        Put(request,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));
        app.Request(request);readyNotified=true;
    }
    HWND foreground=GetForegroundWindow();DWORD foregroundPid=0;GetWindowThreadProcessId(foreground,&foregroundPid);
    wchar_t foregroundClass[80]{};GetClassNameW(foreground,foregroundClass,80);
    bool application=foregroundPid&&foregroundPid!=GetCurrentProcessId()&&foregroundPid!=static_cast<DWORD>(Num(data,L"hostPid"))&&
        wcscmp(foregroundClass,L"Progman")&&wcscmp(foregroundClass,L"WorkerW")&&wcscmp(foregroundClass,L"Shell_TrayWnd")&&wcscmp(foregroundClass,L"Shell_SecondaryTrayWnd");
    fullscreenSuppressed=dock&&fullscreen;
    auto focused=snapshot.GetNamedObject(L"focused");
    RECT movingBounds{};bool intersects=application&&GetWindowRect(foreground,&movingBounds)&&movingBounds.left<full.right&&movingBounds.right>full.left&&movingBounds.top<full.bottom&&movingBounds.bottom>full.top;
    bool activeMove=shell.movingWindow==foreground||(shell.windowHooks.empty()&&Bool(snapshot,L"windowMoving")&&reinterpret_cast<HWND>(static_cast<INT_PTR>(Num(focused,L"hwnd")))==foreground);
    HWND dragOwner=shell.movingWindow;
    bool dragOnMonitor=IsWindow(dragOwner)&&MonitorFromWindow(dragOwner,MONITOR_DEFAULTTONEAREST)==MonitorFromRect(&full,MONITOR_DEFAULTTONEAREST);
    // Snap's own popup can become foreground and the pointer can enter it.
    // Retain the real move owner until MoveSizeEnd instead of raising the bar
    // again merely because the foreground/point left the title-bar strip.
    bool moving=NativeMoveYieldsBar(dragOnMonitor,!shell.windowHooks.empty(),intersects,activeMove);
    if (fullscreen || (!dock&&moving))
    {
        if(dock){SuppressFullscreen();return;}
        KillTimer(hwnd,91);KillTimer(hwnd,92);KillTimer(hwnd,93);hidePending=false;showPending=false;
        HideBarTooltip();
        // Keep the AppBar visible to Shell while dragging, so Snap calculates
        // against the reserved work area. Lower it to let the Snap UI receive input.
        SetToolbarOccluded(true,moving&&!fullscreen);
        return;
    }
    if(!dock&&toolbarOccluded)SetToolbarOccluded(false);
    if(dock&&gridSuppressed){if(!edge&&!hidingToolbar)Hide(false);return;}
    visible = true;ScheduleClock();
    if (Str(settings,L"hideMode")!=L"Never")ScheduleHide();else Reveal();
    if (!edge)
        PositionBar(SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        Hide(false,false);
    if (!edge && (moved || dockBackdropDirty || paintedTransparentToolbar!=TransparentToolbar() || paintedBlackBar!=BlackBar() ||
                  paintedCoveredClear!=CoveredClear() || paintedCoveredGlass!=CoveredGlass() ||
                  old != to_string(snapshot.GetNamedObject(dock ? L"dock" : L"toolbar").Stringify()) +
                             to_string(settings.Stringify()) + std::to_string(dark) +
                             std::to_string(Num(snapshot.GetNamedObject(L"focused"), L"hwnd")) ||
                  !surface))
        Draw();
    if(dock&&dockEnterPending&&!edge){dockEnterPending=false;StartDockEntrance();}
    // A standing menu is owned by one bar item. When that item's data (or the
    // preferences that shape the menu) actually changes, regenerate it from the same
    // builder and let the cascade restore the level the reader is on.
    if(menuOpen&&app.menus&&app.menus->itemBar==hwnd&&!app.menus->itemKey.empty()&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000)){
        auto &menus=*app.menus;
        for(size_t i=0;i<rows.size()&&i<entries.size();++i)if(rows[i].id==menus.itemKey){
            if(MenuStamp(entries[i])!=menus.itemStamp)Menu(static_cast<int>(i),true);
            break;}
    }
}
inline std::vector<JsonObject> NativeBar::GroupItems(wchar_t const *section,JsonObject const &projected)
{
    std::vector<JsonObject> result;
    auto source=projected?projected:data.GetNamedObject(dock?L"dock":L"toolbar");
    if(!projected&&barDrag){source=NativeShell::ToolbarOrderModel(source,dragOrder,false);if(dragTray)source=NativeShell::OrderTrayIcons(source,dragTrayOrder);}
    for(auto const &v:source.GetNamedArray(section)){
        auto item=JsonObject::Parse(v.GetObject().Stringify());
        if(Str(item,L"nativeModule")==L"magic.recycle")Put(item,L"trashCount",app.model.recycleFull?1.0:0.0);
        if(!dock&&Str(item,L"type")==L"Extension"&&(!Bool(item,L"extensionReady")||(extensionWidthsReady&&extensionWidths.contains(Str(item,L"id"))&&extensionWidths[Str(item,L"id")]<=0)))continue;
        if(dock&&!primary&&Str(settings,L"pinnedItemsVisibility")==L"WhenPrimary"&&Str(item,L"type")!=L"AppOrFile"&&Str(item,L"type")!=L"Uac")continue;
        if(dock&&Str(item,L"type")==L"AppOrFile"){
            JsonArray windows;
            if(item.HasKey(L"windows"))for(auto const &w:item.GetNamedArray(L"windows"))
                if(Str(settings,L"temporalItemsVisibility")!=L"OnMonitor"||Str(w.GetObject(),L"monitor")==monitor)windows.Append(w);
            bool hidePinned=!primary&&Str(settings,L"pinnedItemsVisibility")==L"WhenPrimary";
            if(!windows.Size()&&(!Bool(item,L"pinned")||hidePinned))continue;
            item.Insert(L"windows",windows);
            if(Bool(settings,L"splitWindows")&&windows.Size()>1){
                for(auto const &w:windows){auto split=JsonObject::Parse(item.Stringify());Put(split,L"commandId",Str(item,L"key"));Put(split,L"visualId",Str(item,L"key")+L":window:"+std::to_wstring(static_cast<long long>(Num(w.GetObject(),L"hwnd"))));JsonArray one;one.Append(w);split.Insert(L"windows",one);result.push_back(split);}continue;
            }
        }
        if(!dock&&(Bool(item,L"inline")||Str(item,L"id")==L"toolbar:@default/focused-app-title")&&Str(item,L"label").empty())continue;
        result.push_back(item);
    }
    if(!dock&&std::wstring(section)==L"left"&&Bool(app.model.preferences,L"showPages",true))if(auto board=PageBoard();board&&board->pages.size()>1){
        JsonObject page;Put(page,L"id",std::wstring(L"@magic/pages"));Put(page,L"label",std::to_wstring(std::distance(board->pages.begin(),std::find(board->pages.begin(),board->pages.end(),board->pageId))+1)+L" / "+std::to_wstring(board->pages.size()));result.push_back(page);
        if(lastPage!=board->pageId){lastPage=board->pageId;pageAlpha=100;pageWaiting=true;if(hwnd)SetTimer(hwnd,96,2000,nullptr);}
    }
    if(dock&&std::wstring(section)==L"center"&&(!result.empty()||!GroupItems(L"left",source).empty()||!GroupItems(L"right",source).empty())){
        JsonObject a,b;Put(a,L"id",std::wstring(L"@boundary:left"));Put(a,L"key",std::wstring(L"@boundary:left"));Put(a,L"type",std::wstring(L"Separator"));Put(b,L"id",std::wstring(L"@boundary:right"));Put(b,L"key",std::wstring(L"@boundary:right"));Put(b,L"type",std::wstring(L"Separator"));result.insert(result.begin(),a);result.push_back(b);
    }
    return result;
}
#include "native-dock-motion.h"
#include "native-dock-body-motion.h"
#include "native-music-layout.h"
#include "native-dock.h"
#include "native-toolbar-extension.h"
#include "native-toolbar.h"
inline float NativeBar::ItemWidth(JsonObject const &item)
{
    if(Str(item,L"type")==L"Extension"){float w=ExtensionNaturalWidth(item);if(extensionWidthsReady&&extensionWidths.contains(Str(item,L"id")))w=std::min(w,extensionWidths[Str(item,L"id")]);return w>0?w+padding:0;}
    if(Str(item,L"id")==L"@magic/pages")return 78;
    if(item.HasKey(L"dragWidth"))return static_cast<float>(Num(item,L"dragWidth"));
    if(dock){
        if(Str(item,L"type")==L"Separator")return 1+gap;
        if(Str(item,L"type")==L"Media"&&Bool(item,L"hasPlayer"))return iconSize*3+gap*3;
        if(!Vertical()&&Bool(settings,L"showWindowTitle")&&item.HasKey(L"windows")&&item.GetNamedArray(L"windows").Size()){
            auto title=Str(item.GetNamedArray(L"windows").GetAt(0).GetObject(),L"title");
            return std::clamp(iconSize*.85f+MeasureToolbarText(title,std::max(iconSize/3,12.f)),iconSize,iconSize*3+gap*2)+gap;
        }
        return iconSize+gap;
    }
    return std::max(ToolbarContentWidth(item),ToolbarMetricWidth(item))+padding;

}
inline float NativeBar::DockGroupWidth(wchar_t const *section) {
    auto values=GroupItems(section);if(values.empty())return 0;
    float result=-gap;for(auto const &item:values)result+=ItemWidth(item);
    if(std::wstring(section)==L"center"){
        if(GroupItems(L"left").empty())result-=gap;
        if(GroupItems(L"right").empty())result-=gap;
    }
    return std::max(0.f,result);
}
inline void NativeBar::Layout()
{
    NativeRenderScope layout(app.relay);
    if(!dock)BudgetExtensions();
    rows.clear();entries.clear();sectionSeparators.clear();if(barDrag)dragSlotsDirty=true;
    float axis=Vertical()?height:width;
    auto source=data.GetNamedObject(dock?L"dock":L"toolbar");
    // The toolbar has no floating ghost row: the gesture shows itself by re-flowing the
    // slots, so the tray order has to reach the layout or a dragged icon would only dim.
    if(barDrag){source=NativeShell::ToolbarOrderModel(source,dragOrder,false);if(dragTray)source=NativeShell::OrderTrayIcons(source,dragTrayOrder);}
    std::array groups{GroupItems(L"left",source),GroupItems(L"center",source),GroupItems(L"right",source)};
    float groupWidths[3]{};
    if(!dock){
        float g=iconSize*.25f;
        for(int i=0;i<3;i++)groupWidths[i]=ToolbarGroupWidth(groups[i]);
        // Two 8 DIP boundaries belong to the centered group in Seelen's full
        // sortable list. They must participate in capacity and drop targeting.
        groupWidths[1]+=16+(groups[1].empty()?3:4)*g;
        toolbarSequential=2*std::max(groupWidths[0],groupWidths[2])+groupWidths[1]>width-4*padding+.5f;
        contentLength=groupWidths[0]+groupWidths[1]+groupWidths[2]+4*padding+2*g;
        barScroll=std::clamp(barScroll,0.f,std::max(0.f,contentLength-width));dragRowScroll=barScroll;
        float cursor=2*padding-(toolbarSequential?barScroll:0),spare=std::max(0.f,width-contentLength);
        for(int gi=0;gi<3;gi++){
            auto section=gi==0?L"left":gi==1?L"center":L"right";auto const &values=groups[gi];
            float start=toolbarSequential?cursor:gi==0?2*padding:gi==1?(width-groupWidths[1])/2:width-2*padding-groupWidths[2];
            if(toolbarSequential&&gi==2)start+=spare;
            auto boundary=[&](int n){PopupRow r;r.id=r.key=n==0?L"@boundary:left":L"@boundary:right";r.control=L"bar-separator";r.rect={start,padding/2,start+8,height-padding/2};r.enabled=false;JsonObject e;Put(e,L"section",std::wstring(section));Put(e,L"id",r.id);Put(e,L"passive",true);rows.push_back(r);entries.push_back(e);toolbarBoundaries[n]=start+4;start+=8+g;};
            if(gi==1){start+=g;boundary(0);}
            for(size_t i=0;i<values.size();++i){auto item=values[i];auto compound=CompoundOwner(item);auto previous=i?CompoundOwner(values[i-1]):L"",next=i+1<values.size()?CompoundOwner(values[i+1]):L"";
                if(!compound.empty()&&compound!=previous){
                    float bundle=padding;for(size_t j=i;j<values.size()&&CompoundOwner(values[j])==compound;++j)bundle+=ItemWidth(values[j]);
                    PopupRow parent;parent.id=parent.key=compound+L":parent";parent.control=L"module-parent";parent.rect={start,padding/2,start+bundle,height-padding/2};parent.label=L"System Tray";
                    JsonObject e;Put(e,L"id",parent.id);Put(e,L"module",compound);Put(e,L"commandId",compound);e.Insert(L"compoundParent",JsonValue::CreateBooleanValue(true));Put(e,L"passive",true);Put(e,L"section",std::wstring(section));rows.push_back(parent);entries.push_back(e);start+=padding/2;
                }
                float w=ItemWidth(item);PopupRow r;r.id=r.key=Str(item,L"id");r.label=Bool(item,L"clock")?ToolbarClock(data):Str(item,L"label");r.rect={start,padding/2,start+w,height-padding/2};r.enabled=Str(item,L"type")!=L"Extension"||Bool(item,L"extensionGranted");r.offscreen=r.rect.right<=0||r.rect.left>=width;
                Put(item,L"section",std::wstring(section));rows.push_back(r);entries.push_back(item);start+=w;
                if(!compound.empty()&&compound!=next)start+=padding/2;
                if(i+1<values.size()&&(compound.empty()||compound!=next))start+=g;
            }
            if(gi==1){if(!values.empty())start+=g;boundary(1);}
            cursor=start+g;
        }
        if(barDrag)for(size_t i=0;i<dragEntries.size();++i){PopupRow r;r.id=r.key=Str(dragEntries[i],L"id");r.label=Str(dragEntries[i],L"label");r.control=L"drag-ghost";
            auto original=dragRects[i];float start=dragPointer-dragGrab+original.left;
            if(Str(dragEntries[i],L"id")==dragId)TraceDragGhost(dragPointer,start,original.left,dock?8.f:CompoundOwner(dragEntries[i]).empty()?0.f:2.f);
            r.rect={start,padding/2,start+original.right-original.left,height-padding/2};rows.push_back(r);entries.push_back(dragEntries[i]);}
        for(int side=0;side<2;side++){PopupRow r;r.id=r.key=side?L"@corner:right":L"@corner:left";r.control=L"corner";r.enabled=true;r.rect={side?width-10:0,0,side?width:10,height};JsonObject e;e.Insert(L"corner",JsonValue::CreateBooleanValue(true));Put(e,L"tooltip",BarText(L"show_desktop"));rows.push_back(r);entries.push_back(e);}
        if(focus>=static_cast<int>(rows.size()))focus=-1;
        return;
    }
    barScroll=std::clamp(barScroll,0.f,std::max(0.f,contentLength-axis));dragRowScroll=barScroll;float x=padding-((dock||toolbarSequential)?barScroll:0);bool priorGroup=false;int sectionIndex=0;
    for(auto const *section:{L"left",L"center",L"right"}){
        auto const &values=groups[sectionIndex];float groupWidth=0;
        bool full=dock&&Str(settings,L"mode")==L"FullWidth"&&contentLength<=axis;
        if(full&&!values.empty()){
            groupWidth=-gap;for(auto const &item:values)groupWidth+=ItemWidth(item);
            if(sectionIndex==1)groupWidth-=gap*(groups[0].empty()+groups[2].empty());
            groupWidth=std::max(0.f,groupWidth);
        }
        if((!dock&&!toolbarSequential)||full)x=std::wstring(section)==L"left"?padding:std::wstring(section)==L"center"?(axis-groupWidth)/2:axis-padding-groupWidth;
        else if(dock&&priorGroup){x+=0;}
        if(!dock&&toolbarSequential&&sectionIndex){toolbarBoundaries[sectionIndex-1]=x+8;x+=16;}
        priorGroup=true;
        for(auto const &item:values){float size=ItemWidth(item);PopupRow row;
            if(Str(item,L"key")==L"@boundary:right"&&groups[2].empty())x-=gap;
            row.id=Str(item,L"visualId").empty()?(dock?Str(item,L"key"):Str(item,L"id")):Str(item,L"visualId");row.key=row.id;row.label=Bool(item,L"dockClock")?ToolbarClock(data):Str(item,L"label");
            row.rect=Vertical()?D2D1_RECT_F{padding,x,width-padding,x+size-gap}:D2D1_RECT_F{x,padding,x+size-(dock?gap:0),height-padding};row.enabled=true;row.control=Str(item,L"id")==L"@magic/drag-gap"?L"drag-gap":Str(item,L"type")==L"Separator"?L"bar-separator":L"";
            row.offscreen=Vertical()?(row.rect.bottom<=padding||row.rect.top>=height-padding):(row.rect.right<=padding||row.rect.left>=width-padding);
            auto copy=item;Put(copy,L"section",std::wstring(section));rows.push_back(row);entries.push_back(copy);x+=size;
            if(row.key==L"@boundary:left"&&groups[0].empty())x-=gap;
        }
        if(dock&&!values.empty())x-=gap;
        ++sectionIndex;
    }
    if(!dock&&!toolbarSequential){toolbarBoundaries[0]=(padding+groupWidths[0]+(width-groupWidths[1])/2)/2;toolbarBoundaries[1]=((width+groupWidths[1])/2+width-padding-groupWidths[2])/2;}
    if(dock&&barDrag)for(size_t i=0;i<dragEntries.size();++i){PopupRow r;r.id=r.key=Str(dragEntries[i],L"visualId").empty()?Str(dragEntries[i],L"key"):Str(dragEntries[i],L"visualId");r.label=Str(dragEntries[i],L"label");r.control=L"drag-ghost";auto original=dragRects[i];float start=dragPointer-dragGrab+(Vertical()?original.top:original.left);
        r.rect=Vertical()?D2D1_RECT_F{padding,start,width-padding,start+original.bottom-original.top}:D2D1_RECT_F{start,padding,start+original.right-original.left,height-padding};rows.push_back(r);entries.push_back(dragEntries[i]);}
    if(focus>=static_cast<int>(rows.size()))focus=-1;
}
inline void NativeBar::RefreshDragTargets()
{
    if(!barDrag)dragTargetScroll=barScroll; // T36: basis fixed once per gesture
    dragTargets.clear();dragSelfRect={};dragSelfRectValid=false;float frame=dragRowScroll-dragTargetScroll;
    // Slots and the dragged row are stored in the fixed dragTargetScroll frame. The rows
    // were laid out minus dragRowScroll, so add the difference here; a refresh then only
    // updates geometry and never moves cursorContent.
    auto record=[&](std::wstring const &id,D2D1_RECT_F const &rect){D2D1_RECT_F r=rect;if(Vertical()){r.top+=frame;r.bottom+=frame;}else{r.left+=frame;r.right+=frame;}
        dragTargets.emplace_back(id,Vertical()?(r.top+r.bottom)/2:(r.left+r.right)/2);
        if(id!=dragId)return;if(!dragSelfRectValid){dragSelfRect=r;dragSelfRectValid=true;}else{dragSelfRect.left=std::min(dragSelfRect.left,r.left);dragSelfRect.right=std::max(dragSelfRect.right,r.right);dragSelfRect.top=std::min(dragSelfRect.top,r.top);dragSelfRect.bottom=std::max(dragSelfRect.bottom,r.bottom);}};
    if(dragTray){
        for(size_t j=0;j<rows.size();++j){auto const &r=rows[j];if(r.control==L"drag-ghost"||Str(entries[j],L"type")!=L"Tray"||Str(entries[j],L"module")!=dragTrayOwner)continue;
            record(r.id,r.rect);}
    }else for(size_t j=0;j<rows.size();++j){auto const &r=rows[j];if((!CompoundOwner(entries[j]).empty()&&!Bool(entries[j],L"compoundParent"))||r.control==L"drag-ghost"||r.control==L"corner"||r.id==L"@magic/pages")continue;
        auto id=Str(entries[j],dock?L"key":L"module");if(id.empty())id=r.id;
        if(std::find(dragOrder.begin(),dragOrder.end(),id)!=dragOrder.end())record(id,r.rect);}
    dragSlotsDirty=false;
}
inline void NativeBar::ToolbarDrag(float x,float y)
{
    bool redraw;
    {NativeRenderScope move(app.relay);redraw=MoveToolbarDrag(x,y);}
    if(redraw)Draw();
}
inline bool NativeBar::MoveToolbarDrag(float x,float y)
{
    x-=contentX;y-=contentY;
    if(!Bool(data,dock?L"reorderDock":L"reorderToolbar")||downId.empty())return false;
    bool started=!barDrag,changed=false;float oldScroll=barScroll;
    if(!barDrag){
        auto found=std::find_if(rows.begin(),rows.end(),[&](auto const &r){return r.id==downId;});if(found==rows.end())return false;
        int i=static_cast<int>(found-rows.begin());if(found->id==L"@magic/pages"||found->control==L"corner"||Str(entries[i],L"type")==L"Recovery"||Str(entries[i],L"key")==L"@magic/recovery-separator")return false;
        // An icon inside the tray block drags on its own; the block's button keeps
        // dragging the whole module, so both gestures survive the split.
        bool trayRow=!dock&&Str(entries[i],L"type")==L"Tray";
        dragTray=trayRow;dragTrayOrder.clear();dragTrayOriginal.clear();
        if(trayRow){dragId=found->id;dragTrayOwner=Str(entries[i],L"module");dragTraySection=Str(entries[i],L"section");}
        else dragId=Str(entries[i],dock?L"commandId":L"module");
        if(dragId.empty())dragId=dock?Str(entries[i],L"key"):found->id;
        dragEntries.clear();dragRects.clear();float left=Vertical()?height:width,right=0;
        if(trayRow){dragEntries.push_back(entries[i]);dragRects.push_back(rows[i].rect);left=Vertical()?rows[i].rect.top:rows[i].rect.left;right=Vertical()?rows[i].rect.bottom:rows[i].rect.right;dragSection=Str(entries[i],L"section");dragIndex=static_cast<int>(Num(entries[i],L"configurationIndex"));}
        else for(size_t j=0;j<entries.size();++j)if(rows[j].id==dragId||Str(entries[j],dock?L"key":L"module")==dragId){dragEntries.push_back(entries[j]);dragRects.push_back(rows[j].rect);left=std::min(left,Vertical()?rows[j].rect.top:rows[j].rect.left);right=std::max(right,Vertical()?rows[j].rect.bottom:rows[j].rect.right);dragSection=Str(entries[j],L"section");dragIndex=static_cast<int>(Num(entries[j],L"configurationIndex"));}
        if(dragEntries.empty()&&found->control==L"bar-separator"){JsonObject e;Put(e,L"id",found->id);Put(e,L"passive",true);dragEntries.push_back(e);dragRects.push_back(found->rect);left=found->rect.left;right=found->rect.right;}
        if(dragEntries.empty())return false;
        // T39: only a whole-block drag of the tray module folds its icons into the
        // button; a single tray icon keeps the plain ghost. The target, the grab and
        // the order stay frozen below, so nothing about the swap arithmetic changes.
        trayCollapseActive=!dock&&!trayRow&&std::any_of(dragEntries.begin(),dragEntries.end(),[](JsonObject const &e){return TrayRoot(e);});
        trayCollapsedGhosts.clear();
        dragOrder.clear();for(auto section:{L"left",L"center",L"right"}){if(std::wstring(section)==L"center")dragOrder.push_back(L"@boundary:left");for(auto const &v:data.GetNamedObject(dock?L"dock":L"toolbar").GetNamedArray(section)){auto item=v.GetObject();if(Str(item,L"type")!=L"Tray"&&Str(item,L"type")!=L"Uac"&&Str(item,L"type")!=L"Recovery"&&Str(item,L"key")!=L"@magic/recovery-separator")dragOrder.push_back(dock?Str(item,L"key"):Str(item,L"id"));}if(std::wstring(section)==L"center")dragOrder.push_back(L"@boundary:right");}
        dragOriginalOrder=dragOrder;
        // The icons keep the order the adapter published, so the drag starts from the
        // arrangement on screen rather than from a remembered list.
        if(trayRow)for(auto section:{L"left",L"center",L"right"})for(auto const &v:data.GetNamedObject(dock?L"dock":L"toolbar").GetNamedArray(section)){auto item=v.GetObject();if(Str(item,L"type")==L"Tray"&&Str(item,L"module")==dragTrayOwner)dragTrayOrder.push_back(Str(item,L"id"));}
        dragTrayOriginal=dragTrayOrder;
        // T35: slots are rebuilt after every reflow (RefreshDragTargets), so the next
        // swap measures the layout the user is actually looking at.
        dragTarget=dragId;RefreshDragTargets();
        for(auto &r:dragRects){if(Vertical()){r.top-=left;r.bottom-=left;}else{r.left-=left;r.right-=left;}}
        dragEntry=entries[i];dragWidth=right-left;dragGrab=(Vertical()?downPoint.y:downPoint.x)*96/dpi-(Vertical()?contentY:contentX)-left;
        {JsonObject detail;Put(detail,L"surface",std::wstring(dock?L"dock":L"toolbar"));Put(detail,L"dpi",static_cast<double>(dpi));Put(detail,L"downPx",static_cast<double>(downPoint.x));Put(detail,L"downDip",static_cast<double>(downPoint.x*96/dpi));Put(detail,L"groupLeft",static_cast<double>(left));Put(detail,L"groupRight",static_cast<double>(right));Put(detail,L"dragGrab",static_cast<double>(dragGrab));Put(detail,L"contentX",static_cast<double>(contentX));Put(detail,L"fullLeft",static_cast<double>(full.left));Put(detail,L"width",static_cast<double>(width));if(hwnd)Put(detail,L"windowDpi",static_cast<double>(GetDpiForWindow(hwnd)));detail.Insert(L"tray",JsonValue::CreateBooleanValue(dragTray));app.Telemetry(L"drag-ghost-start",detail);dragGhostTraceX=FLT_MAX;dragSwapTraceX=FLT_MAX;dragSwapTraceCenter=FLT_MAX;dragSwapTraceBest=FLT_MAX;dragSwapTraceCurrent=FLT_MAX;dragSwapTraceScroll=FLT_MAX;dragSwapTraceTargetScroll=FLT_MAX;dragSwapTraceTarget.clear();dragSwapTraceDragTarget.clear();dragSwapTraceTray=false;dragSwapTraceSlots=0;dragSwapTraceSelf=false;dragSwapTraceSelfValid=false;dragSwapTraceSelfLeft=FLT_MAX;dragSwapTraceSelfRight=FLT_MAX;lastSwapCursor=FLT_MAX;}
        dragSequential=toolbarSequential;barDrag=true;LockDockReorderHover();previousFocus=GetForegroundWindow();SetForegroundWindow(hwnd);SetFocus(hwnd);KillTimer(hwnd,92);KillTimer(hwnd,98);hidePending=false;
        KillTimer(hwnd,94);shell.HidePreview(this);HideBarTooltip();if(tooltip)SendMessageW(tooltip,TTM_POP,0,0);
    }
    // Restrict to the horizontal axis, as dnd-kit does; vertical pointer motion
    // must not cancel a valid captured drag.
    if(Vertical())x=y;dragPointer=x;
    if(dock||toolbarSequential){if(x<24)barScroll=std::max(0.f,barScroll-12);else if(x>(Vertical()?height:width)-24)barScroll=std::min(std::max(0.f,contentLength-(Vertical()?height:width)),barScroll+12);}
    bool slotsRefreshed=false;if(dragSlotsDirty){RefreshDragTargets();slotsRefreshed=true;}
    // T35: the pointer, not the block centre, picks the slot. Slots were measured in the
    // dragTargetScroll frame, so map the pointer into that frame before comparing.
    float cursorContent=x+barScroll-dragTargetScroll,best=FLT_MAX,currentDistance=FLT_MAX,targetLeft=0,targetRight=0;std::wstring target;
    // T37: the dragged row's own slot is the yardstick, never a destination. Taking it as
    // a target rebuilt the original order (the row "swapped" with itself), which teleported
    // the row back to its start slot while the pointer kept moving, so the next nearest-slot
    // pick could land on a distant neighbour. currentDistance keys on the row's own slot now;
    // keying it on the last swap target disabled the "clearly closer" guard right after a swap.
    for(size_t i=0;i<dragTargets.size();++i){auto const &[id,slot]=dragTargets[i];float distance=std::abs(cursorContent-slot);if(id==dragId){currentDistance=std::min(currentDistance,distance);continue;}if(distance<best){best=distance;target=id;float stepLeft=i?slot-dragTargets[i-1].second:(i+1<dragTargets.size()?dragTargets[i+1].second-slot:0),stepRight=i+1<dragTargets.size()?dragTargets[i+1].second-slot:(i?slot-dragTargets[i-1].second:0);targetLeft=slot-stepLeft/2;targetRight=slot+stepRight/2;}}
    if(currentDistance==FLT_MAX&&dragSelfRectValid)currentDistance=std::abs(cursorContent-(Vertical()?(dragSelfRect.top+dragSelfRect.bottom)/2:(dragSelfRect.left+dragSelfRect.right)/2));
    // T36: a 296 DIP row grabbed 273.5 from its left edge keeps the pointer far from
    // its own slot centre, so centre distance calls a row that still sits under the
    // pointer "not mine". On the dragged row's own rect, containment wins: the target
    // is the row itself and no swap is considered. The 4 DIP halo also covers the
    // Voronoi edge, which for a 296 wide row lies 2.5 DIP outside the rect.
    bool selfHit=false;if(dragSelfRectValid){float selfStart=Vertical()?dragSelfRect.top:dragSelfRect.left,selfEnd=Vertical()?dragSelfRect.bottom:dragSelfRect.right;selfHit=cursorContent>=selfStart-4&&cursorContent<=selfEnd+4;}
    if(selfHit){target=dragId;best=0;targetLeft=Vertical()?dragSelfRect.top:dragSelfRect.left;targetRight=Vertical()?dragSelfRect.bottom:dragSelfRect.right;}
    if(!selfHit&&!target.empty()&&target!=dragTarget&&best+4<currentDistance&&std::abs(cursorContent-lastSwapCursor)>=4){lastSwapCursor=cursorContent;
        if(dragTray){
            auto next=dragTrayOriginal;auto from=std::find(next.begin(),next.end(),dragId),to=std::find(next.begin(),next.end(),target);
            if(from!=next.end()&&to!=next.end()){auto index=static_cast<size_t>(to-next.begin());auto moving=*from;next.erase(from);next.insert(next.begin()+index,std::move(moving));dragTarget=target;
                if(next!=dragTrayOrder){dragTrayOrder=std::move(next);changed=true;SetTimer(hwnd,102,300,nullptr);}}
        }else{
            auto next=dragOriginalOrder;auto from=std::find(next.begin(),next.end(),dragId),to=std::find(next.begin(),next.end(),target);
            if(from!=next.end()&&to!=next.end()){auto index=static_cast<size_t>(to-next.begin());auto moving=*from;next.erase(from);next.insert(next.begin()+index,std::move(moving));dragTarget=target;
                if(next!=dragOrder){dragOrder=std::move(next);changed=true;SetTimer(hwnd,102,300,nullptr);}}
        }
    }
    // T34: report the swap decision only when one of its inputs changes; no polling.
    if(barDrag){
        float selfLeft=dragSelfRectValid?(Vertical()?dragSelfRect.top:dragSelfRect.left):0,selfRight=dragSelfRectValid?(Vertical()?dragSelfRect.bottom:dragSelfRect.right):0;
        bool trace=std::abs(x-dragSwapTraceX)>=.01f||std::abs(cursorContent-dragSwapTraceCenter)>=.01f||std::abs(best-dragSwapTraceBest)>=.01f||std::abs(currentDistance-dragSwapTraceCurrent)>=.01f||std::abs(barScroll-dragSwapTraceScroll)>=.01f||std::abs(dragTargetScroll-dragSwapTraceTargetScroll)>=.01f||target!=dragSwapTraceTarget||dragTarget!=dragSwapTraceDragTarget||dragTray!=dragSwapTraceTray||dragTargets.size()!=dragSwapTraceSlots||selfHit!=dragSwapTraceSelf||dragSelfRectValid!=dragSwapTraceSelfValid||std::abs(selfLeft-dragSwapTraceSelfLeft)>=.01f||std::abs(selfRight-dragSwapTraceSelfRight)>=.01f||slotsRefreshed;
        if(trace){
            dragSwapTraceX=x;dragSwapTraceCenter=cursorContent;dragSwapTraceBest=best;dragSwapTraceCurrent=currentDistance;dragSwapTraceScroll=barScroll;dragSwapTraceTargetScroll=dragTargetScroll;dragSwapTraceTarget=target;dragSwapTraceDragTarget=dragTarget;dragSwapTraceTray=dragTray;dragSwapTraceSlots=dragTargets.size();dragSwapTraceSelf=selfHit;dragSwapTraceSelfValid=dragSelfRectValid;dragSwapTraceSelfLeft=selfLeft;dragSwapTraceSelfRight=selfRight;
            JsonObject detail;Put(detail,L"surface",std::wstring(dock?L"dock":L"toolbar"));Put(detail,L"x",static_cast<double>(x));Put(detail,L"dragPointer",static_cast<double>(dragPointer));Put(detail,L"dragGrab",static_cast<double>(dragGrab));Put(detail,L"dragWidth",static_cast<double>(dragWidth));Put(detail,L"center",static_cast<double>(cursorContent));Put(detail,L"cursorContent",static_cast<double>(cursorContent));Put(detail,L"best",static_cast<double>(best));Put(detail,L"currentDistance",static_cast<double>(currentDistance));Put(detail,L"dragTarget",dragTarget);Put(detail,L"target",target);Put(detail,L"slotCount",static_cast<double>(dragTargets.size()));detail.Insert(L"selfHit",JsonValue::CreateBooleanValue(selfHit));detail.Insert(L"selfRectValid",JsonValue::CreateBooleanValue(dragSelfRectValid));Put(detail,L"selfRectLeft",static_cast<double>(selfLeft));Put(detail,L"selfRectRight",static_cast<double>(selfRight));Put(detail,L"lastSwapCursor",static_cast<double>(lastSwapCursor));
            JsonArray slots;for(size_t i=0;i<dragTargets.size()&&i<16;++i){JsonObject slot;Put(slot,L"id",dragTargets[i].first);Put(slot,L"slot",static_cast<double>(dragTargets[i].second));slots.Append(slot);}detail.Insert(L"slots",slots);
            Put(detail,L"barScroll",static_cast<double>(barScroll));Put(detail,L"dragTargetScroll",static_cast<double>(dragTargetScroll));detail.Insert(L"tray",JsonValue::CreateBooleanValue(dragTray));detail.Insert(L"changed",JsonValue::CreateBooleanValue(changed));{JsonArray interval;interval.Append(JsonValue::CreateNumberValue(static_cast<double>(targetLeft)));interval.Append(JsonValue::CreateNumberValue(static_cast<double>(targetRight)));detail.Insert(L"targetInterval",interval);}detail.Insert(L"slotsRefreshed",JsonValue::CreateBooleanValue(slotsRefreshed));app.Telemetry(L"drag-swap",detail);
        }
    }

    if(!started&&!changed&&barScroll==oldScroll){
        size_t n=0;for(auto &row:rows)if(row.control==L"drag-ghost"&&n<dragRects.size()){
            auto original=dragRects[n++];float start=dragPointer-dragGrab+(Vertical()?original.top:original.left);if(Vertical()){row.rect.top=start;row.rect.bottom=start+original.bottom-original.top;}else{row.rect.left=start;row.rect.right=start+original.right-original.left;}
            float extra=dock?8.f:CompoundOwner(dragEntries[n-1]).empty()?0.f:2.f;
            if(row.id==dragId)TraceDragGhost(x,start,original.left,extra);
            auto found=toolbarVisuals.find(L"@drag:"+row.id);if(found!=toolbarVisuals.end()&&found->second.group)found->second.group.Offset({(row.rect.left-extra)*dpi/96,(row.rect.top-extra)*dpi/96,0});
        }return false;
    }
    Layout();return true;
}
inline void NativeBar::EndToolbarDrag(bool commit)
{
    if(!barDrag)return;
    // Foreground handoff, positioning and capture release may dispatch nested
    // messages. Keep all monitor snapshots stable through this transaction,
    // including the interval before/after Layout's narrower render scope.
    NativeRenderScope releasing(app.relay);
    JsonObject trace;Put(trace,L"stage",std::wstring(L"release"));Put(trace,L"surface",std::wstring(dock?L"dock":L"toolbar"));
    trace.Insert(L"commit",JsonValue::CreateBooleanValue(commit));trace.Insert(L"changed",JsonValue::CreateBooleanValue(dragTray?dragTrayOrder!=dragTrayOriginal:dragOrder!=dragOriginalOrder));app.Telemetry(L"bar-reorder",trace);
    KillTimer(hwnd,102);auto id=dragId;
    dropRows.clear();for(auto const &e:dragEntries){auto key=Str(e,L"visualId");if(key.empty())key=Str(e,L"id");if(key.empty())key=Str(e,L"key");dropRows.insert(key);}
    barDrag=false;bool motion=NativeMotion(app.model);dropId=motion?id:L"";dropUntil=motion?GetTickCount64()+250:0;if(motion)SetTimer(hwnd,97,260,nullptr);dragEntry=nullptr;dragId.clear();downId.clear();down=-1;
    trayCollapseActive=false;
    bool trayChanged=dragTray&&dragTrayOrder!=dragTrayOriginal;
    // Finish HWND bookkeeping before a model update can remove this monitor.
    if(dock&&hwnd){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);POINT point{};if(GetCursorPos(&point))DockPointerLeft(!PtInRect(&full,point));}
    if(GetForegroundWindow()==hwnd&&IsWindow(previousFocus))SetForegroundWindow(previousFocus);
    // ReleaseCapture sends WM_CAPTURECHANGED synchronously. Paint its final order
    // only after committing, so the old slot never flashes back during release.
    HWND window=hwnd;bool captured=window&&GetCapture()==window;
    if(commit&&trayChanged)shell.ReorderTraySequence(*this,dragTrayOrder,dragTraySection);
    else if(commit&&dragOrder!=dragOriginalOrder)shell.ReorderToolbarSequence(*this,dragOrder);
    else {shell.provisionalOrder.clear();shell.provisionalOwner=nullptr;shell.provisionalTray.clear();if(shell.authoritative)shell.Update(shell.authoritative);else {Layout();Draw();}}
    dragTray=false;dragTrayOrder.clear();dragTrayOriginal.clear();
    if(captured&&GetCapture()==window)ReleaseCapture();
}
inline void NativeBar::PaintPreview70(ID2D1DeviceContext *c)
{
    NativePaint p(c,app.graphics,dark);
    float radius=dock?(Num(settings,L"margin")>0||Str(settings,L"mode")!=L"FullWidth"?16.f:0.f):Num(settings,L"margin")>0?height*.3f:0.f;
    if(dock)p.Material({0,0,width,height},NativeMaterial::Dock,Glass(),radius);
    // Solid black rather than a dark glass tint: the point is to stop reading the
    // application underneath, and any translucency brings its edges back.
    else if(paintedBlackBar)p.Fill({0,0,width,height},D2D1::ColorF(0x000000),radius);
    // "Keep transparent" under a window used to fall through to the menu material -
    // TransparentToolbar() is false while covered - so the bar turned pale blue the
    // moment a window went fullscreen. Nothing is painted now; the surface is cleared.
    else if(paintedCoveredClear){}
    // "Acrylic" is the blur, not the panel tint: the tint is what made it look exactly
    // like the opaque bar. A thin scrim keeps the icons legible and lets the backdrop
    // through; without a blur to sit on it would be unreadable, so fall back to a panel.
    // A 12% veil on top of the blur was indistinguishable from painting nothing at all:
    // the blur alone barely shows through a bar this thin. The veil is what makes the
    // acrylic pick read as frosted glass next to the plain transparent one.
    else if(paintedCoveredGlass&&toolbarGlass)p.Fill({0,0,width,height},D2D1::ColorF(dark?0x000000:0xFFFFFF,dark?.34f:.30f),radius);
    else if(!TransparentToolbar())p.Material({0,0,width,height},paintedCoveredGlass?NativeMaterial::Panel:NativeMaterial::Menu,Glass(),radius);
    if(TransparentToolbar()&&Bool(app.model.preferences,L"transparentToolbarMask")){
        float tint=Bool(data,L"dark",Preview70Theme::systemDark)?1.f:0.f;
        D2D1_GRADIENT_STOP stops[]={{0,{tint,tint,tint,.22f}},{.35f,{tint,tint,tint,.16f}},{.7f,{tint,tint,tint,.06f}},{1,{tint,tint,tint,0}}};
        com_ptr<ID2D1GradientStopCollection> colors;com_ptr<ID2D1LinearGradientBrush> brush;
        check_hresult(c->CreateGradientStopCollection(stops,4,D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,colors.put()));
        check_hresult(c->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0,0},{0,height}),colors.get(),brush.put()));
        c->FillRectangle({0,0,width,height},brush.get());
    }
    // Corner commands keep their hit targets without a colored hover overlay.
    if(dock&&rows.empty()){
        com_ptr<IDWriteTextFormat> format;check_hresult(app.graphics.text->CreateTextFormat(app.graphics.fonts.family.c_str(),app.graphics.fonts.collection.get(),DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_ITALIC,DWRITE_FONT_STRETCH_NORMAL,12.8f,L"",format.put()));
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);auto text=BarText(L"weg.empty");c->DrawTextW(text.c_str(),static_cast<UINT32>(text.size()),format.get(),{padding,0,width-padding,height},p.Color(p.theme.Muted()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}
inline void NativeBar::Draw()
{
    NativeRenderScope rendering(app.relay);if(!rendering){if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);return;}
    if (gridSuppressed || !visible || edge || !hwnd)
        return;
    // A bar painted out in black is light-on-dark regardless of the system theme; over
    // the wallpaper the ink follows the theme, so the same row stays legible in both states.
    paintedTransparentToolbar=TransparentToolbar();paintedBlackBar=BlackBar();
    paintedCoveredClear=CoveredClear();paintedCoveredGlass=CoveredGlass();
    dark=paintedBlackBar?true:Bool(data,L"dark",Preview70Theme::systemDark);
    if(dock&&dockBackdropDirty){if(materialVisual){materialVisual.Brush(nullptr);materialVisual.Comment(L"");}dockBackdropDirty=false;}
    if(dock)SyncNativeGlass(app.graphics,hwnd,toolbarRoot,materialVisual,{0,0,width,height},dpi/96,(Num(settings,L"margin")>0||Str(settings,L"mode")!=L"FullWidth"?16.f:0.f),NativeMaterial::Dock,!NativeLowPower(app.model),visual);
    else toolbarGlass=SyncNativeGlass(app.graphics,hwnd,toolbarRoot,materialVisual,{0,0,width,height},dpi/96,Num(settings,L"margin")>0?height*.3f:0.f,paintedCoveredGlass?NativeMaterial::Panel:NativeMaterial::Menu,!paintedTransparentToolbar&&!paintedBlackBar&&!paintedCoveredClear&&!NativeLowPower(app.model),visual);
    if(!dock){std::wstring trace=paintedTransparentToolbar?L"bare":paintedBlackBar?L"black":paintedCoveredClear?L"clear":paintedCoveredGlass?(toolbarGlass?L"glass":L"panel"):L"material";
        trace+=L":"+CoveredBackdrop()+L":"+std::to_wstring(toolbarGlass)+L":"+std::to_wstring(toolbarCoveredByApplication)+L":"+std::to_wstring(NativeGlassAvailable());
        if(trace!=backdropTraceKey){backdropTraceKey=trace;JsonObject detail;Put(detail,L"branch",trace);Put(detail,L"backdrop",CoveredBackdrop());detail.Insert(L"covered",JsonValue::CreateBooleanValue(toolbarCoveredByApplication));detail.Insert(L"glass",JsonValue::CreateBooleanValue(toolbarGlass));detail.Insert(L"glassAvailable",JsonValue::CreateBooleanValue(NativeGlassAvailable()));Put(detail,L"reason",NativeGlassStates()[hwnd].reason);Put(detail,L"transparentToolbar",static_cast<double>(Bool(app.model.preferences,L"transparentToolbar")));app.Telemetry(L"toolbar-backdrop",detail);}}
    {SyncToolbarVisuals();auto key=std::to_wstring(width)+L":"+std::to_wstring(height)+L":"+std::to_wstring(dpi)+L":"+std::to_wstring(dark)+L":"+std::to_wstring(-1)+L":"+std::to_wstring(!downId.empty())+L":"+std::to_wstring(rows.empty())+L":"+std::to_wstring(Preview70Theme::revision)+L":"+std::to_wstring(Glass())+L":"+std::to_wstring(paintedTransparentToolbar)+L":"+std::to_wstring(paintedBlackBar)+L":"+CoveredBackdrop()+L":"+std::to_wstring(Bool(app.model.preferences,L"transparentToolbarMask"))+L":"+std::to_wstring(paintedCoveredClear)+L":"+std::to_wstring(toolbarGlass);if(key==toolbarBackgroundKey&&surface){ApplyDockReflow();return;}toolbarBackgroundKey=key;}
    RECT rect{};
    GetClientRect(hwnd, &rect);
    int w = dock?static_cast<int>(std::ceil(width*dpi/96)):rect.right, h = dock?static_cast<int>(std::ceil(height*dpi/96)):rect.bottom;
    if (w < 1 || h < 1 || static_cast<size_t>(w) * h * 4 > 8 * 1024 * 1024)
        return;
    visual.Size({static_cast<float>(w), static_cast<float>(h)});
    if (!surface || surfaceBytes != static_cast<size_t>(w) * h * 4)
    {
        surface = app.graphics.composition.CreateDrawingSurface(
            {static_cast<float>(w), static_cast<float>(h)},
            Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        visual.Brush(app.graphics.compositor.CreateSurfaceBrush(surface));
        surfaceBytes = static_cast<size_t>(w) * h * 4;
    }
    auto interop = surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();
    com_ptr<ID2D1DeviceContext> c;
    POINT offset{};
    check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), c.put_void(), &offset));
    bool ended = false;
    try
    {
        c->SetDpi(dpi, dpi);
        c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x * 96 / dpi, offset.y * 96 / dpi));
        c->Clear(D2D1::ColorF(0,0));
        PaintPreview70(c.get());
        check_hresult(interop->EndDraw());
        ended = true;
        ++paints;
        ++redraws;
        ApplyDockReflow();
    }
    catch (...)
    {
        if (!ended)
            interop->EndDraw();
        throw;
    }
}
inline void NativeBar::Send(int index, std::wstring verb, long long target, std::wstring destination)
{
    AllowSetForegroundWindow(static_cast<DWORD>(Num(data, L"hostPid")));
    app.inputEpoch++;
    if (index < 0 || index >= static_cast<int>(rows.size()) || !rows[index].enabled)
        return;
    if(rows[index].id==L"@magic/pages"){
        if(auto board=PageBoard()){HMENU menu=CreatePopupMenu();int id=1;for(auto const &page:board->pages){auto title=L"第 "+std::to_wstring(id)+L" 页";AppendMenuW(menu,MF_STRING|(page==board->pageId?MF_CHECKED:0),id++,title.c_str());}POINT point{};GetCursorPos(&point);SetForegroundWindow(hwnd);int choice=TrackPopupMenu(menu,TPM_RETURNCMD,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);if(choice>0&&choice<=static_cast<int>(board->pages.size())){JsonObject request;Put(request,L"op",std::wstring(L"page"));Put(request,L"board",board->id);Put(request,L"page",board->pages[choice-1]);app.Request(request);}}
        return;
    }
    JsonObject command;
    Put(command,L"id",!Str(entries[index],L"commandId").empty()?Str(entries[index],L"commandId"):dock?Str(entries[index],L"key"):rows[index].id);
    Put(command, L"verb", verb);
    if(Str(entries[index],L"type")==L"Extension"&&(verb==L"activate"||verb==L"extension-action")){
        POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);auto action=verb==L"extension-action"?destination:ExtensionActionAt(index,point.x*96/dpi-contentX-rows[index].rect.left);
        auto session=Str(entries[index],L"extensionSession"),revision=Str(entries[index],L"extensionRevision");
        if(verb==L"activate"&&extensionPressId==rows[index].id){action=extensionPressAction;session=extensionPressSession;revision=extensionPressRevision;}
        if(action.empty())return;Put(command,L"verb",std::wstring(L"extension-action"));Put(command,L"extensionAction",action);Put(command,L"extensionSession",session);Put(command,L"extensionRevision",revision);
    }
    Put(command,L"sentAtTick",static_cast<double>(GetTickCount64()));
    if(!target&&dock&&entries[index].HasKey(L"windows")&&entries[index].GetNamedArray(L"windows").Size()==1)target=static_cast<long long>(Num(entries[index].GetNamedArray(L"windows").GetAt(0).GetObject(),L"hwnd"));
    Put(command, L"target", static_cast<double>(target));
    Put(command, L"destination", destination);
    auto rect = rows[index].rect;auto compound=CompoundOwner(entries[index]);if(!dock&&!compound.empty())for(auto const &r:rows)if(r.id==compound+L":parent"){rect=r.rect;break;}
    Put(command, L"x", static_cast<double>(full.left + (dock?rect.right:(rect.left+rect.right)/2) * dpi / 96));
    auto side=Str(settings,L"position");float margin=static_cast<float>(Num(settings,L"margin"))*dpi/96;
    Put(command,L"x",static_cast<double>(std::lround(full.left+(rect.left+rect.right)*dpi/192)));
    Put(command,L"y",static_cast<double>(std::lround(side==L"Bottom"?full.top-margin:full.bottom+margin)));
    if(dock&&Vertical()){Put(command,L"x",static_cast<double>(std::lround(side==L"Left"?full.right+margin:full.left-margin)));Put(command,L"y",static_cast<double>(std::lround(full.top+(rect.top+rect.bottom)*dpi/192)));}
    auto entry = entries[index];
    auto focused = reinterpret_cast<UINT_PTR>(GetForegroundWindow());
    bool was = false;
    if (entry.HasKey(L"windows"))
        for (auto const &value : entry.GetNamedArray(L"windows"))
            if (static_cast<UINT_PTR>(Num(value.GetObject(), L"hwnd")) == focused)
                was = true;
    command.Insert(L"wasFocused", JsonValue::CreateBooleanValue(was));
    JsonObject request;
    Put(request, L"op", std::wstring(L"shell-command"));
    Put(request, L"window", static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));
    request.Insert(L"command", command);
    auto state=life;
    app.Request(request,[state](JsonObject const &response){if(!state->owner)return;JsonObject detail;detail.Insert(L"ok",JsonValue::CreateBooleanValue(Bool(response,L"ok")));Put(detail,L"kind",state->owner->kind);if(!Bool(response,L"ok"))Put(detail,L"error",Str(response,L"error"));state->owner->app.Telemetry(L"shell-command-result",detail);});
}
inline void NativeBar::Activate(int index)
{
    if(index>=0&&index<static_cast<int>(entries.size())&&Str(entries[index],L"type")==L"Recovery"){OpenDesktopRecovery(app,hwnd);return;}
    if(index>=0&&index<static_cast<int>(entries.size())&&Str(entries[index],L"type")==L"Music"){if(NativeMusicLayout::Enabled(entries[index],1))Send(index,L"play");return;}
    if(index>=0&&index<static_cast<int>(entries.size())&&Str(entries[index],L"type")==L"Media"){if(Bool(entries[index],L"hasPlayer"))Send(index,L"open-source");return;}
    if(index>=0&&index<static_cast<int>(entries.size())&&Bool(entries[index],L"corner")){SendBar(L"show-desktop");return;}
    if(index>=0&&index<static_cast<int>(entries.size())&&Str(entries[index],L"type")==L"Separator")return;
    if (dock && index >= 0 && index < static_cast<int>(entries.size()) && entries[index].HasKey(L"windows") &&
        entries[index].GetNamedArray(L"windows").Size() > 1)
    {
        KillTimer(hwnd,94);
        if(shell.PreviewFor(*this,index))shell.HidePreview(this);else shell.ShowPreview(*this,index);
        return;
    }
    if(!dock&&index>=0&&index<static_cast<int>(entries.size())){
        auto module=Str(entries[index],L"nativeModule");
        if(module==L"magic.focused-app"||module==L"magic.focused-title"){Menu(index,false);return;}
    }
    if(!dock&&index>=0&&index<static_cast<int>(entries.size())&&Bool(entries[index],L"passive"))return;
    if(dock)shell.HidePreview(this);
    Send(index);
}
inline void NativeBar::SendBar(std::wstring const &verb,std::wstring const &destination,int item)
{
    if(item>=0){Send(item,verb,0,destination);return;}
    AllowSetForegroundWindow(static_cast<DWORD>(Num(data,L"hostPid")));app.inputEpoch++;
    JsonObject command,request;Put(command,L"id",std::wstring(dock?L"bar:dock":L"bar:toolbar"));Put(command,L"verb",verb);Put(command,L"destination",destination);
    Put(request,L"op",std::wstring(L"shell-command"));Put(request,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));request.Insert(L"command",command);app.Request(request);
}
inline void NativeBar::Menu(int index,bool refresh)
{
    if(index>=0&&index<static_cast<int>(entries.size())&&(Str(entries[index],L"type")==L"Recovery"||Str(entries[index],L"key")==L"@magic/recovery-separator"))return;
    ClearDockHover();
    bool valid=index>=0&&index<static_cast<int>(entries.size());
    if(valid&&!dock&&(rows[index].control==L"corner"||rows[index].control==L"bar-separator"))valid=false;
    if(valid&&!dock&&Str(entries[index],L"type")==L"Tray"){
        auto rect=rows[index].rect;auto side=Str(settings,L"position");bool above=side==L"Bottom";
        POINT trigger{static_cast<LONG>(std::lround(full.left+(rect.left+rect.right)*dpi/192)),above?full.top:full.bottom};
        TrayMenuAnchor().Arm(reinterpret_cast<HWND>(static_cast<UINT_PTR>(Num(entries[index],L"ownerHwnd"))),trigger,above,full);
        Send(index,L"context");return;
    }
    auto item=valid?entries[index]:JsonObject{};
    if(valid&&!dock&&Str(item,L"type")==L"Extension"&&Bool(item,L"extensionGranted")){
        POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);auto action=ExtensionActionAt(index,(point.x*96/dpi)-contentX-rows[index].rect.left,true);
        if(!action.empty()){Send(index,L"extension-action",0,action);return;}
    }
    struct Command {std::wstring verb,destination;int item;};
    std::map<std::wstring,Command> actions;std::map<std::wstring,std::wstring> menuIcons;
    JsonArray values;
    unsigned next=0;
    auto add=[&](JsonArray const &parent,std::wstring label,std::wstring verb,std::wstring destination=L"",bool local=false,std::wstring control=L"",std::wstring detail=L""){
        JsonObject row;auto id=L"bar-menu-"+std::to_wstring(++next);
        Put(row,L"id",id);Put(row,L"label",label);Put(row,L"control",control);Put(row,L"detail",detail);
        row.Insert(L"enabled",JsonValue::CreateBooleanValue(true));parent.Append(row);
        // Picking a mark, a wordmark or a layout is a run of small tries, not one
        // decision, so those rows keep the menu standing. Typing custom text is the
        // exception: it leaves the menu for a dialog.
        if(verb==L"set-brand-icon"||verb==L"set-brand-style"||verb==L"set-brand-device"||
           verb==L"set-brand-icon-hide"||verb==L"set-menu-layout"||verb==L"set-focused-separator"||verb==L"set-toolbar-drag-button-only"||
           (verb==L"set-brand-label"&&destination!=L"custom"))
            row.Insert(L"keepOpen",JsonValue::CreateBooleanValue(true));
        actions[id]={verb,destination,local?index:-1};menuIcons[id]=detail;return row;
    };
    auto separator=[&](JsonArray const &parent){JsonObject row;Put(row,L"control",std::wstring(L"separator"));parent.Append(row);};
    std::wstring headerId;
    if(valid&&dock&&Str(item,L"type")==L"AppOrFile"){
        if(!refresh)Send(index,L"jump-prefetch");
        auto title=add(values,Str(item,L"label"),L"launch",L"",true,L"header",L"open");headerId=Str(title,L"id");
        auto addJumpGroup=[&](wchar_t const* key,wchar_t const* label,wchar_t const* icon){
            auto entries=item.GetNamedArray(key,JsonArray{});if(!entries.Size())return;
            auto heading=add(values,label,L"",L"",false,L"header");heading.Insert(L"enabled",JsonValue::CreateBooleanValue(false));
            for(auto const &entry:entries){auto row=entry.GetObject();add(values,Str(row,L"label"),L"jump-open",Str(row,L"id"),true,L"",icon);}
            separator(values);
        };
        addJumpGroup(L"jumpPinned",L"已固定",L"folder");
        addJumpGroup(L"jumpFrequent",L"常用",L"folder");
        addJumpGroup(L"jumpRecent",L"最近",L"file");
        addJumpGroup(L"jumpTasks",L"任务",L"open");
        if(!Bool(item,L"preventPinning")){
            add(values,BarText(Bool(item,L"pinned")?L"app_menu.unpin":L"app_menu.pin"),Bool(item,L"pinned")?L"unpin":L"pin",L"",true,L"",Bool(item,L"pinned")?L"RiUnpinLine":L"RiPushpinLine");separator(values);
        }
        add(values,BarText(L"app_menu.open_file_location"),L"open-location",L"",true,L"",L"MdOutlineMyLocation");
        add(values,BarText(L"app_menu.run_as"),L"runas",L"",true,L"",L"MdOutlineAdminPanelSettings");
        add(values,BarText(L"app_menu.edit_app_icon"),L"edit-icon",L"",true,L"",L"RiEditBoxLine");
        auto windows=item.GetNamedArray(L"windows",JsonArray{});
        if(windows.Size()){separator(values);if(Bool(data,L"devTools"))add(values,BarText(L"app_menu.copy_handles"),L"copy-handles",L"",true,L"",L"AiOutlineCopy");add(values,BarText(windows.Size()>1?L"app_menu.close_multiple":L"app_menu.close"),L"close",L"",true,L"danger",L"BiWindowClose");if(Bool(settings,L"showEndTask"))add(values,BarText(windows.Size()>1?L"app_menu.kill_multiple":L"app_menu.kill"),L"kill",L"",true,L"danger",L"MdOutlineDangerous");}
    }else if(valid&&dock&&(Str(item,L"type")==L"Media"||Str(item,L"type")==L"Plugin"||Str(item,L"type")==L"Separator")&&!rows[index].id.starts_with(L"@boundary:")){
        if(Str(item,L"plugin")==L"@default/weg-trash-bin"){add(values,BarText(L"trash_bin.empty_bin"),L"empty-bin",L"",true,L"",L"FaRegTrashAlt");separator(values);}
        if(Str(item,L"type")==L"Plugin"&&(Str(item,L"plugin")==L"@default/weg-start-menu"||Str(item,L"plugin")==L"@default/weg-show-desktop"||Str(item,L"plugin")==L"@default/weg-trash-bin")){add(values,BarText(L"context_menu.edit_icon"),L"edit-custom-icon",L"",true,L"",L"RiEditBoxLine");separator(values);}
        add(values,BarText(Str(item,L"type")==L"Separator"?L"context_menu.remove_separator":L"context_menu.remove_module"),L"remove",L"",true,L"",L"CgExtensionRemove");
    }else if(valid&&!dock&&(Str(item,L"nativeModule")==L"magic.focused-app"||Str(item,L"nativeModule")==L"magic.focused-title")){
        // The adapter has nothing to run for these rows, which used to surface as a
        // "not finished" card. What the row does own is the mark in front of the name,
        // so the click opens that choice instead.
        JsonArray separators;auto choice=Str(app.model.preferences,L"focusedSeparator");if(choice.empty())choice=L"desktop";
        struct SeparatorOption{wchar_t const *verb,*zh,*en;};
        SeparatorOption options[]={{L"desktop",L"笔记本图案",L"Laptop mark"},{L"bar",L"竖杠",L"Bar"},{L"none",L"不显示",L"None"}};
        for(auto const &option:options){
            auto row=add(separators,Ui(option.zh,option.en),L"set-focused-separator",option.verb,true,L"",L"");
            row.Insert(L"checked",JsonValue::CreateBooleanValue(choice==option.verb));
        }
        auto separatorMenu=add(values,Ui(L"分隔符",L"Separator"),L"",L"",false,L"submenu",L"");
        separatorMenu.Insert(L"children",separators);
        separator(values);
        add(values,BarText(L"context_menu.remove"),L"remove",L"",true,L"",L"CgExtensionRemove");
    }else if(valid&&!dock&&Str(item,L"nativeModule")!=L"magic.user"){
        // Every other toolbar row is a plain module: no mark to swap, no wordmark to
        // edit, no device shape to pick. Removal is the whole menu. The system tray
        // block is the one exception: its whole-block drag can fold its icons, so the
        // switch for that lives here, on the button that starts the gesture.
        if(Str(item,L"nativeModule")==L"magic.tray"){
            bool animation=Bool(app.model.preferences,L"toolbarDragButtonOnly",true);
            auto row=add(values,Ui(L"拖拽仅显示按钮",L"Show button only while dragging"),L"set-toolbar-drag-button-only",animation?L"off":L"on",true,L"",L"");
            row.Insert(L"checked",JsonValue::CreateBooleanValue(animation));
            row.Insert(L"toggle",JsonValue::CreateBooleanValue(true));
        }
        add(values,BarText(L"context_menu.remove"),L"remove",L"",true,L"",L"CgExtensionRemove");
    }else if(valid&&!dock){
        // Only the brand cell (magic.user) is a lockup of mark plus wordmark, so every
        // editor below is keyed to it: the brand list, the device shape, the wordmark
        // mode and the hide switch mean nothing on a CPU gauge or a clock.
        // Bar items carry user images too, not just dock pins.
        add(values,Ui(L"导入自定义图标",L"Import custom icon"),L"edit-custom-icon",L"",true,L"",L"RiEditBoxLine");
        {
            // Two sources for the mark, one per branch of the menu: the bundled
            // device shapes, and the vendor marks grouped by initial.
            struct BrandEntry{std::wstring slug,label;bool color=false;};
            std::vector<BrandEntry> entries;
            for(auto const &asset:BrandSvgAssets){
                std::wstring name(asset.name);
                auto first=name.find(L':'),second=name.find(L':',first+1);
                if(first==std::wstring::npos||second==std::wstring::npos)continue;
                auto slug=name.substr(first+1,second-first-1),style=name.substr(second+1);
                auto found=std::find_if(entries.begin(),entries.end(),[&](BrandEntry const &e){return e.slug==slug;});
                if(found==entries.end()){entries.push_back({slug,std::wstring(asset.label),false});found=entries.end()-1;}
                if(style==L"color")found->color=true;
            }
            // The baked table follows the fetch order, so sort before grouping; a
            // repeated letter heading is the tell that this is missing.
            // Group and order by the slug, never by the label: a vendor whose display
            // name is Chinese (机械革命) belongs under its Latin initial (M), not under
            // a one-off heading made of its first character.
            std::stable_sort(entries.begin(),entries.end(),[](BrandEntry const &a,BrandEntry const &b){
                return CompareStringEx(LOCALE_NAME_INVARIANT,0,a.slug.c_str(),static_cast<int>(a.slug.size()),b.slug.c_str(),static_cast<int>(b.slug.size()),nullptr,nullptr,0)==CSTR_LESS_THAN;});
            JsonArray brands;std::wstring letter;
            bool color=Str(item,L"brandStyle")==L"color",chosen=!Str(item,L"brandIcon").empty();
            std::wstring device=Str(item,L"brandDevice");
            // How the menu itself is laid out belongs next to the list it distorts: 39
            // vendors grouped by initial is the case that runs past one column, and the
            // mark picker is where the reader notices it.
            {
                bool columns=Str(app.model.preferences,L"contextMenuLayout")==L"columns";
                JsonArray layouts;
                auto scroll=add(layouts,Ui(L"竖列滚动",L"Single column"),L"set-menu-layout",L"scroll",true,L"",L"");
                scroll.Insert(L"checked",JsonValue::CreateBooleanValue(!columns));
                auto grid=add(layouts,Ui(L"多列平铺",L"Columns"),L"set-menu-layout",L"columns",true,L"",L"");
                grid.Insert(L"checked",JsonValue::CreateBooleanValue(columns));
                auto layout=add(brands,Ui(L"菜单布局",L"Menu layout"),L"",L"",false,L"submenu",L"");
                layout.Insert(L"children",layouts);
                separator(brands);
            }
            // Colour lockup versus single-path glyph is a property of the whole cell, not
            // of one vendor, so it sits above the list instead of tripling it. It stays
            // visible without a vendor chosen: hiding it is what made the row look lost.
            auto style=add(brands,color?Ui(L"改用单色图形",L"Switch to glyph"):Ui(L"改用彩色徽标",L"Switch to colour"),
                L"set-brand-style",color?L"mono":L"color",true,L"",L"");
            // The switch shares its level with the vendor radio list, so it must not
            // take part in that group: picking it leaves the chosen vendor ticked.
            style.Insert(L"checked",JsonValue::CreateBooleanValue(color));
            style.Insert(L"toggle",JsonValue::CreateBooleanValue(true));
            style.Insert(L"independent",JsonValue::CreateBooleanValue(true));
            separator(brands);
            for(auto const &entry:entries){
                auto label=entry.label.empty()?entry.slug:entry.label;
                auto initial=std::wstring(1,static_cast<wchar_t>(towupper(entry.slug.empty()?L'#':entry.slug[0])));
                if(initial!=letter){
                    letter=initial;
                    // A drawn band, not a bare caption: the eye needs a hard marker to
                    // find the first letter it wants in a 36-row list.
                    auto heading=add(brands,letter,L"",L"",false,L"group");
                    heading.Insert(L"enabled",JsonValue::CreateBooleanValue(false));
                }
                auto row=add(brands,label,L"set-brand-icon",entry.slug,true,L"",L"");
                // Every alternative states its state, so the tick can move when the
                // choice changes instead of only ever appearing once.
                row.Insert(L"checked",JsonValue::CreateBooleanValue(Str(item,L"brandIcon")==entry.slug));
            }
            JsonArray icons;
            if(brands.Size()){
                auto preset=add(icons,Ui(L"预设品牌",L"Preset brands"),L"",L"",false,L"submenu",L"");
                preset.Insert(L"children",brands);
            }
            {
                // The default branch is a family, not one glyph: laptop or desktop.
                JsonArray defaults;
                struct DeviceOption{wchar_t const *verb,*zh,*en;};
                DeviceOption devices[]={{L"laptop",L"笔记本",L"Laptop"},{L"desktop",L"台式电脑",L"Desktop"}};
                for(auto const &option:devices){
                    auto row=add(defaults,Ui(option.zh,option.en),L"set-brand-device",option.verb,true,L"",L"");
                    bool desktop=std::wstring(option.verb)==L"desktop";
                    row.Insert(L"checked",JsonValue::CreateBooleanValue(!chosen&&(device==L"desktop")==desktop));
                }
                auto plain=add(icons,Ui(L"默认图标",L"Default icon"),L"",L"",false,L"submenu",L"");
                plain.Insert(L"children",defaults);
            }
            if(icons.Size()){
                auto iconMenu=add(values,Ui(L"图标",L"Mark"),L"",L"",false,L"submenu",L"");
                iconMenu.Insert(L"children",icons);
            }
        }
        {
            // The wordmark is its own component, independent of the mark.
            JsonArray labels;auto mode=Str(item,L"brandLabel");if(mode.empty())mode=L"brand";
            struct LabelOption{wchar_t const *verb,*zh,*en;};
            LabelOption options[]={{L"brand",L"品牌名",L"Vendor name"},{L"host",L"主机名",L"PC name"},
                                   {L"none",L"不显示字标",L"No wordmark"}};
            for(auto const &option:options){
                auto row=add(labels,Ui(option.zh,option.en),L"set-brand-label",option.verb,true,L"",L"");
                row.Insert(L"checked",JsonValue::CreateBooleanValue(mode==option.verb));
            }
            auto custom=add(labels,Ui(L"自定义文字…",L"Custom text…"),L"set-brand-label",std::wstring(L"custom"),true,L"",L"");
            custom.Insert(L"checked",JsonValue::CreateBooleanValue(mode==L"custom"));
            auto labelMenu=add(values,Ui(L"字标",L"Wordmark"),L"",L"",false,L"submenu",L"");
            labelMenu.Insert(L"children",labels);
        }
        {
            bool hidden=Str(item,L"brandIconHide")==L"1";
            add(values,hidden?Ui(L"显示图标",L"Show mark"):Ui(L"隐藏图标",L"Hide mark"),
                L"set-brand-icon-hide",hidden?L"0":L"1",true,L"",hidden?L"Select":L"Cancel");
        }
        if(Bool(item,L"customIcon")||!Str(item,L"icon").empty()||!Str(item,L"brandIcon").empty()||Str(item,L"brandDevice")==L"desktop")add(values,Ui(L"恢复默认图标",L"Reset icon"),L"reset-icon",L"",true,L"",L"TbRestore");
        separator(values);
        add(values,BarText(L"context_menu.remove"),L"remove",L"",true,L"",L"CgExtensionRemove");
    }else{
        JsonArray modules;
        if(!dock){add(modules,BarText(L"context_menu.restore"),L"restore",L"",false,L"",L"TbRestore");separator(modules);}
        auto moduleValues=data.TryLookup(dock?L"dockModules":L"toolbarModules");
        std::vector<JsonObject> sortedModules;if(moduleValues&&moduleValues.ValueType()==JsonValueType::Array)for(auto const &v:moduleValues.GetArray())sortedModules.push_back(v.GetObject());
        std::stable_sort(sortedModules.begin(),sortedModules.end(),[](auto const &a,auto const &b){auto x=Str(a,L"label"),y=Str(b,L"label");return CompareStringEx(LOCALE_NAME_USER_DEFAULT,SORT_DIGITSASNUMBERS,x.c_str(),static_cast<int>(x.size()),y.c_str(),static_cast<int>(y.size()),nullptr,nullptr,0)==CSTR_LESS_THAN;});
        for(auto const &v:sortedModules){
            auto module=v;bool checked=false;auto groups=data.GetNamedObject(dock?L"dock":L"toolbar");
            for(auto const *g:{L"left",L"center",L"right"})for(auto const &entry:groups.GetNamedArray(g)){
                auto row=entry.GetObject();checked|=dock?(Str(module,L"id")==L"@magic/media"?Str(row,L"type")==L"Media":(Str(row,L"nativeModule")==Str(module,L"id")||Str(row,L"plugin")==Str(module,L"id"))):(Str(row,L"id")==L"toolbar:"+Str(module,L"id")||Str(row,L"nativeModule")==Str(module,L"id"));
            }
            auto row=add(modules,Str(module,L"label"),module.HasKey(L"extensionId")?L"toggle-extension":Str(module,L"id")==L"@magic/media"?L"toggle-media":L"toggle-module",module.HasKey(L"extensionId")?Str(module,L"extensionId"):Str(module,L"id"),false,L"",Str(module,L"icon"));if(module.HasKey(L"extensionId"))checked=Bool(module,L"extensionEnabled");row.Insert(L"checked",JsonValue::CreateBooleanValue(checked));
        }
        auto module=add(values,BarText(dock?L"taskbar_menu.modules":L"context_menu.modules"),L"",L"",false,L"submenu",L"CgExtensionAdd");module.Insert(L"children",modules);
        if(dock){
            separator(values);add(values,BarText(L"taskbar_menu.add_file"),L"pin-file",L"",false,L"",L"RiFileAddLine");
            POINT cursor{};GetCursorPos(&cursor);ScreenToClient(hwnd,&cursor);float axis=(Vertical()?cursor.y:cursor.x)*96/dpi-(Vertical()?contentY:contentX),best=FLT_MAX;std::wstring destination;
            for(size_t i=0;i<rows.size();++i){auto const &r=rows[i];if(r.control==L"drag-ghost"||r.offscreen||Str(entries[i],L"type")==L"Uac")continue;float center=Vertical()?(r.rect.top+r.rect.bottom)/2:(r.rect.left+r.rect.right)/2,distance=std::abs(axis-center);
                if(distance<best){best=distance;auto key=Str(entries[i],L"commandId");if(key.empty())key=Str(entries[i],L"key");destination=(axis>center?L"after:":L"before:")+key;}}
            add(values,BarText(L"taskbar_menu.add_separator"),L"separator",destination,false,L"",Vertical()?L"LuSeparatorHorizontal":L"LuSeparatorVertical");separator(values);
        }
        if(!dock)separator(values);
        bool locked=!Bool(data,dock?L"reorderDock":L"reorderToolbar");
        auto lock=add(values,BarText(locked?L"context_menu.reorder_enable":L"context_menu.reorder_disable"),L"lock",L"",false,L"",dock?(locked?L"CgLockUnlock":L"CgLock"):(locked?L"VscUnlock":L"VscLock"));
        add(values,BarText(dock?L"taskbar_menu.task_manager":L"context_menu.task_manager"),L"task-manager",L"",false,L"",L"PiChartLineFill");
        if(Str(data,L"providerProtocol")==L"Magic.System.Native/1")add(values,L"Windows 授权入口",L"windows-authorization",L"",false,L"",L"MdOutlineAdminPanelSettings");
        for(auto const &entry:NativeBarSettingsEntries)add(values,entry.label,entry.verb,L"",false,L"",entry.icon);

    }
    HideBarTooltip();KillTimer(hwnd,94);shell.HidePreview(this);
    POINT point{};GetCursorPos(&point);auto state=life;auto &menus=Menus(app);
    auto itemKey=valid?rows[index].id:L"";
    auto choose=[state,actions,itemKey](size_t depth,std::wstring const &id){
        if(!state->owner||!actions.contains(id))return;
        auto &bar=*static_cast<NativeBar*>(state->owner);auto action=actions.at(id);
        // A row that carries keepOpen is one step in a run: the menu stays up so the
        // next step is one click away, and the bar is not asked to hide underneath it.
        bool persistent=Menus(bar.app).Persistent(depth,id);
        if(!persistent){Menus(bar.app).Close(false);bar.menuOpen=false;}
        if(action.item>=0){
            auto found=std::find_if(bar.rows.begin(),bar.rows.end(),[&](auto const &row){return row.id==itemKey;});
            if(found==bar.rows.end()){bar.ScheduleHide();return;}
            action.item=static_cast<int>(found-bar.rows.begin());
        }
        if(action.verb==L"focus")bar.Send(action.item,action.verb,_wcstoi64(action.destination.c_str(),nullptr,10));
        else bar.SendBar(action.verb,action.destination,action.item);
        if(!persistent)bar.ScheduleHide();
    };
    if(refresh)menus.choose=choose;
    else menus.Begin(point,L"toolbar",choose,[state](){if(state->owner){auto bar=static_cast<NativeBar*>(state->owner);bar->menuOpen=false;bar->menuItemKey.clear();bar->ScheduleHide();}});
    menuItemKey=valid&&dock&&Str(item,L"type")==L"AppOrFile"?itemKey:L"";menuJumpStamp=menuItemKey.empty()?L"":JumpStamp(item);
    auto key=valid?Str(item,L"key"):L"";
    menus.icon=[state,key,headerId,menuIcons](ID2D1DeviceContext *c,std::wstring const &id,D2D1_RECT_F rect){
        if(!state->owner)return false;
        auto bar=static_cast<NativeBar*>(state->owner);
        if(auto glyph=menuIcons.find(id);glyph!=menuIcons.end()&&!glyph->second.empty()&&bar->toolbarSvg.Draw(c,glyph->second,rect,Preview70Theme{bar->dark}.Muted()))return true;
        if(id!=headerId||headerId.empty())return false;
        auto &icons=static_cast<NativeBar*>(state->owner)->shell.icons;auto found=icons.find(key);if(found==icons.end())return false;
        c->DrawBitmap(found->second.bitmap.get(),rect,1,D2D1_INTERPOLATION_MODE_LINEAR);return true;
    };
    if(!dock){menus.rootAlignX=L"Center";menus.rootAlignY=Str(settings,L"position")==L"Bottom"?L"End":L"Start";}
    if(dock){auto side=Str(settings,L"position");menus.rootAlignX=side==L"Left"?L"Start":side==L"Right"?L"End":L"Center";menus.rootAlignY=side==L"Top"?L"Start":side==L"Bottom"?L"End":L"Center";
        if(side==L"Top")menus.origin.y=full.bottom;
        else if(side==L"Left")menus.origin.x=full.right;
        else if(side==L"Right"){menus.origin.x=full.left;menus.rootAlignX=L"End";}
        else {menus.origin.y=full.top;menus.rootAlignY=L"End";}
    }
    if(app.popup)app.popup->Hide();menuOpen=true;if(refresh)menus.ReplaceRoot(values);else menus.Show(0,values);
    // Stamped last: a rebuild that throws must not claim the data it failed to show.
    menus.itemKey=itemKey;menus.itemBar=hwnd;menus.itemStamp=MenuStamp(item);
}
inline void NativeBar::ShowBarTooltip(){
    if(edge||!visible||barDrag||menuOpen||focus<0||focus>=static_cast<int>(entries.size()))return;
    if(dock&&Str(entries[focus],L"type")==L"AppOrFile")Send(focus,L"jump-prefetch");
    bool oneLine=dock||Bool(entries[focus],L"corner");
    auto text=Str(entries[focus],L"tooltip");if(text.empty()&&dock)text=rows[focus].label;if(text.empty())return;if(oneLine)text=NativeDockTooltipText(text);
    POINT cursor{};GetCursorPos(&cursor);float scale=dpi/96;auto side=Str(settings,L"position");int margin=static_cast<int>(Num(settings,L"margin")*scale);
    int x=cursor.x,y=side==L"Bottom"?full.top-margin:full.bottom+margin;
    if(dock&&side==L"Left"){x=full.right+margin;y=cursor.y;}else if(dock&&side==L"Right"){x=full.left-margin;y=cursor.y;}
    JsonObject packet,payload,data,position;Put(packet,L"kind",std::wstring(L"@magic/bar-tooltip"));Put(packet,L"token",std::wstring(L"local-tooltip"));Put(position,L"x",x);Put(position,L"y",y);payload.Insert(L"desiredPosition",position);
    Put(payload,L"alignX",dock&&side==L"Left"?std::wstring(L"Start"):dock&&side==L"Right"?std::wstring(L"End"):std::wstring(L"Center"));Put(payload,L"alignY",dock&&Vertical()?std::wstring(L"Center"):side==L"Bottom"?std::wstring(L"End"):std::wstring(L"Start"));
    Put(data,L"text",text);data.Insert(L"oneLine",JsonValue::CreateBooleanValue(oneLine));data.Insert(L"dark",JsonValue::CreateBooleanValue(dark));data.Insert(L"rows",JsonArray{});packet.Insert(L"payload",payload);packet.Insert(L"data",data);packet.Insert(L"account",JsonObject{});
    if(!nativeTooltip)nativeTooltip=std::make_unique<NativePopup>(app);if(nativeTooltip->visible)nativeTooltip->Hide(false);nativeTooltip->Show(packet);
}
inline bool NativeBar::SuppressFullscreen(){
    if(!dock)return false;
    if(!ForegroundFullscreen()){fullscreenSuppressed=false;return false;}
    fullscreenSuppressed=true;
    if(barDrag)EndToolbarDrag(false);
    for(UINT_PTR timer:std::initializer_list<UINT_PTR>{91,92,93,94,98,99,100,101,102,103,105,DockPreviewTimer,DockBodyTimer})KillTimer(hwnd,timer);
    showPending=hidePending=hidingToolbar=edgePointerWatch=false;down=-1;downId.clear();pointerId=0;touchPrimary=false;
    if(GetCapture()==hwnd)ReleaseCapture();
    if(menuOpen){Menus(app).Close(false);menuOpen=false;}
    HideBarTooltip();ClearDockHover();shell.HidePreview(this);dockDissolve.Reset(barHost,toolbarRoot);ResetDockBodyMotion();
    edge=true;visible=false;ShowWindow(hwnd,SW_HIDE);Release();return true;
}
inline void NativeBar::ScheduleHide()
{
    if(dock&&SuppressFullscreen())return;
    if (menuOpen || touchPrimary || barDrag || GetCapture()==hwnd || attentionReveal || Str(settings, L"hideMode") == L"Never"){
        if(hwnd)KillTimer(hwnd,92);hidePending=false;
        if(Str(settings,L"hideMode")==L"Never")Reveal();return;
    }
    bool overlap = Str(settings, L"hideMode") == L"Always";
    if(Bool(data,L"workspaceSwitching")){KillTimer(hwnd,92);hidePending=false;return;}
    if(dock&&std::any_of(entries.begin(),entries.end(),[](auto const &e){return Str(e,L"type")==L"Uac";})){KillTimer(hwnd,92);hidePending=false;Reveal();return;}
    auto focused = data.GetNamedObject(L"focused");
    auto value = focused.TryLookup(L"rect");
    DWORD pid=0;HWND active=reinterpret_cast<HWND>(static_cast<INT_PTR>(Num(focused,L"hwnd")));GetWindowThreadProcessId(active,&pid);wchar_t windowClass[80]{};GetClassNameW(active,windowClass,80);
    bool application=pid&&pid!=GetCurrentProcessId()&&pid!=static_cast<DWORD>(Num(data,L"hostPid"))&&wcscmp(windowClass,L"Progman")&&wcscmp(windowClass,L"WorkerW")&&wcscmp(windowClass,L"Shell_TrayWnd");
    if (!dock&&application&&value && value.ValueType() == JsonValueType::Object)
    {
        auto rect = value.GetObject();
        overlap = overlap || (Num(rect, L"left") < full.right && Num(rect, L"right") > full.left &&
                              Num(rect, L"top") < full.bottom && Num(rect, L"bottom") > full.top);
    }
    if(dock&&application&&Str(settings,L"hideMode")!=L"Always"){
        bool known=false;auto apps=data.GetNamedArray(L"appWindows",JsonArray{});for(auto const &v:apps)known|=Num(v.GetObject(),L"hwnd")==Num(focused,L"hwnd");
        if(known)for(auto const &v:apps){auto w=v.GetObject();if(Str(w,L"monitor")!=monitor||Bool(w,L"isIconic"))continue;auto rect=ShellObject(w,L"rect");overlap|=Num(rect,L"left")<full.right&&Num(rect,L"right")>full.left&&Num(rect,L"top")<full.bottom&&Num(rect,L"bottom")>full.top;}
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    WatchEdgePointer(cursor);
    if (overlap && !PointerKeepsOpen(cursor)){
        if(!edge&&!hidePending)hidePending=SetTimer(hwnd,92,static_cast<UINT>(std::clamp(Num(settings,L"delayToHide",800),1.0,5000.0)),nullptr)!=0;
    }else {KillTimer(hwnd,92);hidePending=false;if(!overlap)Reveal();}
}
inline void NativeBar::FlushDragInput()
{
    if(replayingDragInput||NativeRenderScope::busy||deferredDragInput.empty())return;
    HWND window=hwnd;replayingDragInput=true;
    auto batch=std::exchange(deferredDragInput,{});
    for(auto const &input:batch){
        HandleMessage(window,input.message,input.w,input.l,input.contact);
        if(reinterpret_cast<NativeBar*>(GetWindowLongPtrW(window,GWLP_USERDATA))!=this)return;
    }
    replayingDragInput=false;
}
inline LRESULT CALLBACK NativeBar::Proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return HandleMessage(window,message,w,l,{});
}
inline LRESULT NativeBar::HandleMessage(HWND window,UINT message,WPARAM w,LPARAM l,std::optional<PointerContact> contact)
{
    auto self = reinterpret_cast<NativeBar *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<NativeBar *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = window;
    }
    if (!self)
        return DefWindowProcW(window, message, w, l);
    try
    {
        // WM_POINTER data expires when its dispatch returns. Capture only the
        // fields needed for replay and discard Windows' duplicate promoted mouse.
        if(IsPointerContact(message)&&!contact){
            POINTER_INFO info{};
            if(!GetPointerInfo(GET_POINTERID_WPARAM(w),&info)||info.pointerType==PT_MOUSE)return DefWindowProcW(window,message,w,l);
            contact=PointerContact{info.pointerType,info.ptPixelLocation};
        }
        bool mouseContact=message==WM_MOUSEMOVE||message==WM_LBUTTONUP||message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK;
        if(mouseContact&&!self->pointerDispatch&&!self->replayingDragInput&&(static_cast<DWORD>(GetMessageExtraInfo())&0xffffff00)==0xff515700)return 0;
        if(NativeRenderScope::busy){
            // Timers remain registered until handled. A hit test must not hide
            // the Dock and release the visuals that the outer frame is painting.
            if(message==WM_TIMER)return 0;
            if(message==WM_NCHITTEST)return self->gridSuppressed||self->toolbarOccluded?HTTRANSPARENT:HTCLIENT;
            if(IsDragMessage(message,w)){
                self->DeferDragInput({message,w,l,contact});
                return 0;
            }
        }else if(IsDragMessage(message,w)&&!self->replayingDragInput&&!self->deferredDragInput.empty()){
            self->FlushDragInput();
            if(reinterpret_cast<NativeBar*>(GetWindowLongPtrW(window,GWLP_USERDATA))!=self)return 0;
        }
        if(message==WM_KEYDOWN)self->extensionPressId.clear();
        bool pointerInput=(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||message==WM_POINTERDOWN||message==WM_POINTERUPDATE||message==WM_POINTERUP;
        if(self->dock&&(pointerInput||message==WM_NCHITTEST||message==WM_TIMER)&&self->SuppressFullscreen())return message==WM_NCHITTEST?HTTRANSPARENT:0;
        if(self->gridSuppressed&&message==WM_NCHITTEST)return HTTRANSPARENT;
        if(self->gridSuppressed&&((message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||message==WM_POINTERDOWN||message==WM_POINTERUPDATE||message==WM_POINTERUP))return 0;
        switch (message)
        {
        case WM_NCHITTEST:
            if(self->toolbarOccluded)return HTTRANSPARENT;
            if(self->dock&&!self->edge){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(window,&point);float x=point.x*96/self->dpi,y=point.y*96/self->dpi;
                if(!Inside({self->contentX,self->contentY,self->contentX+self->width,self->contentY+self->height},x,y)&&self->Hit(x,y)<0)return HTTRANSPARENT;return HTCLIENT;}break;
        case WM_APP+21:
            if(w==ABN_POSCHANGED&&!self->positioningBar){self->reservationDirty=true;self->shell.ScheduleGeometry();}
            else if(w==ABN_FULLSCREENAPP)self->shell.ScheduleGeometry();
            return 0;
        case WM_WINDOWPOSCHANGED:
            if(self->reserved&&!self->positioningBar){RECT rect{};GetWindowRect(window,&rect);if(memcmp(&rect,&self->notifiedBar,sizeof(rect))!=0){self->notifiedBar=rect;APPBARDATA bar{sizeof(bar)};bar.hWnd=window;SHAppBarMessage(ABM_WINDOWPOSCHANGED,&bar);}}
            break;
        case WM_ACTIVATE:
            if(self->reserved){APPBARDATA bar{sizeof(bar)};bar.hWnd=window;bar.lParam=LOWORD(w)!=WA_INACTIVE;SHAppBarMessage(ABM_ACTIVATE,&bar);}break;
        case WM_DISPLAYCHANGE:
            self->reservationDirty=true;self->shell.ScheduleGeometry();return 0;
        case WM_SHOWWINDOW:
            if(w&&self->dock){self->InvalidateDockBackdrop();InvalidateRect(window,nullptr,FALSE);}return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT p{};
            BeginPaint(window, &p);
            EndPaint(window, &p);
            if(self->themeRefreshPending){self->themeRefreshPending=false;Preview70Theme::Refresh();self->dark=Preview70Theme::systemDark;self->barInteraction.Release(self->toolbarRoot);if(self->hidingToolbar)self->FinishToolbarHide();}
            self->Draw();
            return 0;
        }
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP: {
            POINT point=contact->screen;ScreenToClient(window,&point);self->pointerDispatch=true;
            auto id=GET_POINTERID_WPARAM(w);
            if(message==WM_POINTERDOWN){self->pointerId=id;self->pointerKind=contact->type;self->contactPoint=point;self->pointerDownAt=GetTickCount64();self->pointerCancelled=false;self->touchPrimary=contact->type==PT_TOUCH;self->Reveal();Proc(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));SetTimer(window,98,contact->type==PT_TOUCH?250:200,nullptr);}
            else if(id==self->pointerId){if(message==WM_POINTERUP){KillTimer(window,98);self->pointerId=0;Proc(window,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));}else {self->contactPoint=point;Proc(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x,point.y));}}
            if(reinterpret_cast<NativeBar*>(GetWindowLongPtrW(window,GWLP_USERDATA))==self)self->pointerDispatch=false;
            return 0;
        }
        case WM_POINTERCAPTURECHANGED:
            if(GET_POINTERID_WPARAM(w)==self->pointerId){self->pointerId=0;self->pointerCancelled=true;KillTimer(window,98);if(self->barDrag)self->EndToolbarDrag(false);}return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if(!self->pointerDispatch){self->pointerKind=PT_MOUSE;self->touchPrimary=false;}
        if(self->pointerKind!=PT_MOUSE&&!self->barDrag){float distance=std::hypot((GET_X_LPARAM(l)-self->downPoint.x)*96/self->dpi,(GET_Y_LPARAM(l)-self->downPoint.y)*96/self->dpi);auto delay=self->pointerKind==PT_TOUCH?250:200;
            if(GetTickCount64()-self->pointerDownAt<delay&&distance>(self->pointerKind==PT_TOUCH?5:10)){self->pointerCancelled=true;KillTimer(window,98);}
        }
        if(!self->pointerCancelled&&(self->pointerKind==PT_MOUSE||GetTickCount64()-self->pointerDownAt>=(self->pointerKind==PT_TOUCH?250:200))&&!self->downId.empty()&&(self->barDrag||std::hypot((GET_X_LPARAM(l)-self->downPoint.x)*96/self->dpi,(GET_Y_LPARAM(l)-self->downPoint.y)*96/self->dpi)>=5)){self->ToolbarDrag(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);if(self->barDrag)return 0;}
        if(!self->dock&&!self->lastPage.empty()){bool restore=self->pageAlpha<100;self->pageAlpha=100;self->pageWaiting=true;SetTimer(window,96,2000,nullptr);if(restore)self->Draw();} {
            KillTimer(window, 92);
            self->hidePending=false;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            if(self->dockDissolve.Active())self->Reveal();
            if (self->edge)
            {
                if(!self->showPending)self->showPending=SetTimer(window,93,
                    static_cast<UINT>(std::clamp(Num(self->settings,L"delayToShow",100),1.0,5000.0)),nullptr)!=0;
                return 0;
            }
            self->DockPointer(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);
            int next = self->Hit(GET_X_LPARAM(l) * 96 / self->dpi, GET_Y_LPARAM(l) * 96 / self->dpi);
            self->SyncBarInteraction(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi,next);
            int media=self->MediaButtonAt(next,GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);
            if (next != self->focus||media!=self->mediaHover)
            {
                self->focus = next;self->mediaHover=media;
                // Crossing the icon edge towards its preview is not dismissal.
                POINT screenPoint{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(window,&screenPoint);
                if(next>=0&&!self->shell.PreviewFor(*self,next)&&!self->shell.PreviewHasPoint(screenPoint))self->shell.HidePreview();
                if (self->tooltip)
                    SendMessageW(self->tooltip, TTM_POP, 0, 0);

                self->HideBarTooltip();if(next>=0&&!self->menuOpen)SetTimer(window,100,400,nullptr);self->Draw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            {POINT point{};bool outside=GetCursorPos(&point)&&!PtInRect(&self->full,point);self->DockPointerLeft(outside);}
            self->SyncBarInteraction(0,0,-1);self->HideBarTooltip();
            if(!self->dock&&!self->lastPage.empty()){self->pageWaiting=true;SetTimer(window,96,2000,nullptr);}
            KillTimer(window, 94);
            KillTimer(window, 93);
            self->showPending=false;
            self->focus=-1;self->mediaHover=-1;
            self->Draw();
            if(self->dock&&self->menuOpen)SetTimer(window,DockPreviewTimer,250,nullptr);
            self->ScheduleHide();
            return 0;
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONDOWN:
            if(!self->pointerDispatch){self->pointerKind=PT_MOUSE;self->pointerCancelled=false;}self->HideBarTooltip();if(self->dockDissolve.Active())self->Reveal();
            self->down = self->Hit(GET_X_LPARAM(l) * 96 / self->dpi, GET_Y_LPARAM(l) * 96 / self->dpi);
            self->downId=self->down>=0?self->rows[self->down].id:L"";
            self->extensionPressId.clear();
            if(self->down>=0&&Str(self->entries[self->down],L"type")==L"Extension"){auto item=self->entries[self->down];self->extensionPressId=self->rows[self->down].id;self->extensionPressSession=Str(item,L"extensionSession");self->extensionPressRevision=Str(item,L"extensionRevision");self->extensionPressAction=self->ExtensionActionAt(self->down,GET_X_LPARAM(l)*96/self->dpi-self->contentX-self->rows[self->down].rect.left);}
            self->downPoint = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};self->mediaDown=self->MediaButtonAt(self->down,GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);
            SetCapture(window);if(self->tooltip)SendMessageW(self->tooltip,TTM_POP,0,0);self->Draw();
            if(self->dock&&self->pointerKind==PT_MOUSE&&!self->downId.empty()&&Bool(self->data,L"reorderDock"))SetTimer(window,98,250,nullptr);
            return 0;
        case WM_LBUTTONUP: {
            KillTimer(window,98);
            if(self->barDrag){self->EndToolbarDrag(true);return 0;}
            auto pressedId=std::exchange(self->downId,L"");
            auto pressPoint=self->downPoint;
            ReleaseCapture();
            auto source=std::find_if(self->rows.begin(),self->rows.end(),[&](auto const&r){return !pressedId.empty()&&r.id==pressedId;});self->down=source==self->rows.end()?-1:static_cast<int>(source-self->rows.begin());
            int hit = self->Hit(GET_X_LPARAM(l) * 96 / self->dpi, GET_Y_LPARAM(l) * 96 / self->dpi);
            bool moved=std::hypot((GET_X_LPARAM(l)-pressPoint.x)*96/self->dpi,(GET_Y_LPARAM(l)-pressPoint.y)*96/self->dpi)>=5;
            JsonObject input;Put(input,L"kind",self->kind);input.Insert(L"pressed",JsonValue::CreateBooleanValue(self->down>=0));input.Insert(L"hit",JsonValue::CreateBooleanValue(hit>=0));input.Insert(L"moved",JsonValue::CreateBooleanValue(moved));self->app.Telemetry(L"shell-left-input",input);
            if (!moved&&hit == self->down){
                if(hit>=0&&(Str(self->entries[hit],L"type")==L"Media"||Str(self->entries[hit],L"type")==L"Music")){int button=self->MediaButtonAt(hit,GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);if(button>=0&&button==self->mediaDown)self->Send(hit,button==0?L"prev":button==1?L"play":L"next");else if(button<0&&self->mediaDown<0&&Str(self->entries[hit],L"type")==L"Media")self->Activate(hit);}
                else self->Activate(hit);
            }
            self->extensionPressId.clear();self->down=-1;self->mediaDown=-1;self->Draw();
            return 0;
        }
        case WM_CAPTURECHANGED:
            KillTimer(window,98);if(self->barDrag){self->EndToolbarDrag(false);return 0;}self->downId.clear();self->down=-1;self->Draw();return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(window,&point);int hit=self->Hit(point.x*96/self->dpi,point.y*96/self->dpi);
            if(hit>=0&&((!self->dock&&(Str(self->entries[hit],L"nativeModule")==L"magic.audio"||self->rows[hit].id==L"toolbar:@seelen/tb-media-popup"))||(self->dock&&CanonicalNativeBarPlugin(Str(self->entries[hit],L"plugin"))==L"@magic/dock-media-popup"))){if(GET_WHEEL_DELTA_WPARAM(w))self->Send(hit,GET_WHEEL_DELTA_WPARAM(w)>0?L"volume-up":L"volume-down");return 0;}
            if(self->dock||self->toolbarSequential){self->barScroll-=GET_WHEEL_DELTA_WPARAM(w)/120.f*(self->iconSize+self->gap)*2;self->Layout();self->Draw();self->shell.RequestIcons();}return 0;}
        case WM_MBUTTONUP: {
            int hit=self->Hit(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);
            if(self->dock&&hit>=0&&Str(self->entries[hit],L"type")==L"AppOrFile")self->Send(hit,Str(self->settings,L"middleClickAction")==L"CloseApp"?L"close-one":L"launch");
            else if(!self->dock&&hit>=0&&Str(self->entries[hit],L"type")==L"Tray")self->Send(hit,L"middle");
            return 0;}
        case WM_CONTEXTMENU:
            if(GET_X_LPARAM(l)==-1&&GET_Y_LPARAM(l)==-1)self->Menu(self->focus);return 0;
        case WM_RBUTTONUP:
            self->Menu(self->Hit(GET_X_LPARAM(l) * 96 / self->dpi, GET_Y_LPARAM(l) * 96 / self->dpi));
            return 0;
        case WM_KEYDOWN:
            if(w==VK_ESCAPE){self->pointerCancelled=true;KillTimer(window,98);if(self->barDrag){self->EndToolbarDrag(false);return 0;}self->downId.clear();self->down=-1;if(GetCapture()==window)ReleaseCapture();return 0;}
            if (w != VK_ESCAPE)
                self->Key(static_cast<UINT>(w));
            return 0;
        case WM_TIMER:
            if(w==105){POINT point{};GetCursorPos(&point);bool keep=self->PointerKeepsOpen(point);self->WatchEdgePointer(point);if(!keep)self->ScheduleHide();return 0;}
            if(w==DockPreviewTimer){KillTimer(window,DockPreviewTimer);if(self->menuOpen){if(self->shell.PreviewHasCursor())SetTimer(window,DockPreviewTimer,200,nullptr);else self->shell.HidePreview(self);}return 0;}
            if(w==DockBodyTimer){KillTimer(window,DockBodyTimer);self->ResetDockBodyMotion();if(!self->edge)self->PositionBar(SWP_NOACTIVATE);return 0;}
            if(w==103){KillTimer(window,103);self->SyncDockHover();return 0;}
            if(w==102){KillTimer(window,102);if(self->barDrag)self->shell.PreviewToolbarOrder(*self,self->dragOrder,self->dragTray?&self->dragTrayOrder:nullptr);return 0;}
            if(w==100){KillTimer(window,100);self->ShowBarTooltip();return 0;}
            if(w==98){KillTimer(window,98);
                if(self->dock&&self->pointerKind==PT_MOUSE&&!self->pointerCancelled&&!self->downId.empty()&&GetCapture()==window&&(GetAsyncKeyState(VK_LBUTTON)&0x8000)){
                    POINT p=self->downPoint,screen{};if(GetCursorPos(&screen)&&ScreenToClient(window,&screen))p=screen;self->ToolbarDrag(p.x*96/self->dpi,p.y*96/self->dpi);
                }else if(self->pointerId&&!self->pointerCancelled&&!self->downId.empty()){POINT p=self->contactPoint;float distance=std::hypot((p.x-self->downPoint.x)*96/self->dpi,(p.y-self->downPoint.y)*96/self->dpi);if(self->pointerKind==PT_TOUCH||distance>=5)self->ToolbarDrag(p.x*96/self->dpi,p.y*96/self->dpi);}return 0;}
            if(w==99){KillTimer(window,99);if(self->occlusionAnimating)self->FinishToolbarOcclusion();else self->FinishToolbarHide();return 0;}
            if(w==97){KillTimer(window,97);if(GetTickCount64()>=self->dropUntil){self->dropId.clear();self->dropRows.clear();self->dropUntil=0;}self->Draw();return 0;}
            if(w==96){if(self->focus>=0&&self->focus<static_cast<int>(self->rows.size())&&self->rows[self->focus].id==L"@magic/pages"){self->pageAlpha=100;self->Draw();KillTimer(window,96);return 0;}if(!NativeMotion(self->app.model)){self->pageAlpha=18;self->Draw();KillTimer(window,96);return 0;}if(!Bool(self->app.model.preferences,L"fadePages",true)){KillTimer(window,96);return 0;}if(self->pageWaiting){self->pageWaiting=false;SetTimer(window,96,16,nullptr);}self->pageAlpha=std::max(18,self->pageAlpha-12);self->Draw();if(self->pageAlpha==18)KillTimer(window,96);return 0;}
            if (w == 94)
            {
                KillTimer(window, 94);
                if (!self->barDrag&&self->focus >= 0)
                    self->shell.ShowPreview(*self, self->focus);
            }
            else if (w == 91)
            {
                KillTimer(window, 91);
                if(!self->visible||self->edge)return 0;
                SYSTEMTIME now{};
                GetLocalTime(&now);
                if(!self->dock&&self->clockSeconds){
                    NativeRenderScope rendering(self->app.relay);
                    if(rendering){for(size_t i=0;i<self->entries.size()&&i<self->rows.size();++i)if(Bool(self->entries[i],L"clock"))self->rows[i].label=ToolbarClock(self->data);self->SyncToolbarVisuals();}
                }else{self->Layout();self->Draw();}
                self->ScheduleClock();
            }
            else if (w == 92)
            {
                KillTimer(window, 92);
                self->hidePending=false;
                POINT point{};GetCursorPos(&point);
                if (!self->menuOpen && !self->shell.PreviewHasCursor()){
                    if(self->PointerKeepsOpen(point))self->WatchEdgePointer(point);
                    else self->Hide();
                }
            }
            else if (w == 93)
            {
                KillTimer(window, 93);
                self->showPending=false;
                POINT cursor{};RECT edgeRect{};GetCursorPos(&cursor);GetWindowRect(window,&edgeRect);
                if(PtInRect(&edgeRect,cursor)){self->Reveal();self->shell.RequestIcons();}
            }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_LOCK)
            {
                KillTimer(window, 91);
                ShowWindow(window, SW_HIDE);
                self->visible = false;
                self->HideBarTooltip();if(self->barDrag){self->barDrag=false;self->downId.clear();self->dragId.clear();self->dragTray=false;self->dragTrayOrder.clear();self->dragTrayOriginal.clear();self->shell.provisionalOrder.clear();self->shell.provisionalOwner=nullptr;self->shell.provisionalTray.clear();if(GetCapture()==window)ReleaseCapture();}self->Release();
            }
            else if (w == WTS_SESSION_UNLOCK)
            {
                self->visible = true;
                ShowWindow(window, SW_SHOWNOACTIVATE);
                self->Draw();
                if (!self->dock)
                {
                    SYSTEMTIME now{};
                    GetLocalTime(&now);
                    self->ScheduleClock();
                }
            }
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR *>(l)->code == TTN_GETDISPINFOW)
            {
                auto info = reinterpret_cast<NMTTDISPINFOW *>(l);
                self->tooltipText = self->focus >= 0 ? (Str(self->entries[self->focus],L"tooltip").empty()?self->rows[self->focus].label:Str(self->entries[self->focus],L"tooltip")) : L"";
                info->lpszText = self->tooltipText.data();
                return 0;
            }
            break;
        case WM_GETOBJECT:
            if (static_cast<LONG>(l) == UiaRootObjectId && self->life)
            {
                auto provider = PopupProvider(self->life);
                auto result = UiaReturnRawElementProvider(window, w, l, provider);
                provider->Release();
                return result;
            }
            break;
        case WM_TIMECHANGE:self->Layout();self->ScheduleClock();self->Draw();return 0;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:InvalidateNativeClockSeconds();self->clockConfiguration.clear();self->Layout();self->ScheduleClock();self->themeRefreshPending=true;InvalidateRect(window,nullptr,FALSE);if(message==WM_SETTINGCHANGE&&w==SPI_SETWORKAREA&&!self->positioningBar){self->reservationDirty=true;self->shell.ScheduleGeometry();}return 0;
        case WM_POWERBROADCAST:self->barInteraction.Release(self->toolbarRoot);if(self->hidingToolbar)self->FinishToolbarHide();self->ResetDockBodyMotion();if(!self->edge)self->PositionBar(SWP_NOACTIVATE);self->ClearDockHover();self->Draw();return TRUE;
        case WM_DPICHANGED:
            self->pendingDpi=HIWORD(w);if(!self->dpiRefreshPending){self->dpiRefreshPending=true;self->app.QueueRenderWork();}
            self->reservationDirty=true;self->shell.ScheduleGeometry();
            return 0;
        case WM_DESTROY:
            self->ReportExtensions(true);WTSUnRegisterSessionNotification(window);
            KillTimer(window, 91);
            KillTimer(window, 92);
            KillTimer(window, 93);
            if (self->life)
                self->life->owner = nullptr;
            self->Release();
            self->target = nullptr;
            self->visual = nullptr;
            self->hwnd = nullptr;
            return 0;
        }
    }
    catch (hresult_error const &error)
    {
        JsonObject detail;Put(detail,L"error",std::wstring(error.message()));Put(detail,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));Put(detail,L"message",static_cast<double>(message));
        self->app.Telemetry(L"shell-error",detail);self->Release();
    }
    catch (...)
    {
        self->app.Telemetry(L"shell-error");
        self->Release();
    }
    return DefWindowProcW(window, message, w, l);
}

#include "native-dock-preview.h"
inline NativeShell::NativeShell(App &app,bool observeWindows) : app(app)
{
    if(!observeWindows)return;
    clockWatch.Start(app.relay);
    regionalClockWatch.Start(app.relay,L"Control Panel\\International");
    InvalidateNativeClockSeconds();
    eventOwner=this;
    for(auto event:{EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_MOVESIZESTART,EVENT_SYSTEM_MOVESIZEEND,EVENT_SYSTEM_MINIMIZESTART,EVENT_SYSTEM_MINIMIZEEND,EVENT_OBJECT_LOCATIONCHANGE}){
        auto hook=SetWinEventHook(event,event,nullptr,WindowEvent,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);if(hook)windowHooks.push_back(hook);
    }
}
inline NativeShell::~NativeShell()
{
    TrayMenuAnchor().Reset();
    stopping = true;if(eventOwner==this)eventOwner=nullptr;
    for(auto hook:windowHooks)UnhookWinEvent(hook);windowHooks.clear();
    if(app.relay)KillTimer(app.relay,76);
    preview.reset();
    bars.clear();
}
inline void NativeShell::ShowPreview(NativeBar &bar, int index)
{
    if(PreviewFor(bar,index))return;
    if (!preview)
        preview = std::make_unique<NativePreview>(*this);
    preview->Show(bar, index);
}
inline void NativeShell::HidePreview(NativeBar *owner)
{
    if (preview && (!owner || preview->owner == owner))
        preview->Hide();
}
inline void NativeShell::RefreshPreview(){if(preview)preview->Refresh();}
inline bool NativeShell::PreviewFor(NativeBar const &bar,int index){return preview&&preview->visible&&!preview->dismissing&&preview->owner==&bar&&index>=0&&index<static_cast<int>(bar.rows.size())&&preview->itemId==bar.rows[index].id;}
inline bool NativeShell::PreviewHasPoint(POINT point){return preview&&preview->ContainsPoint(point);}
inline bool NativeShell::PreviewHasCursor()
{
    return preview && preview->ContainsCursor();
}
// OLE hover activation preserves Windows' real target-window drop semantics.
// No file path is read and no idle hook/timer is installed.
struct NativeBarDrop final : IDropTarget
{
    ULONG refs = 1;
    NativeBar &bar;
    int hover = -1;
    ULONGLONG entered = 0;
    bool activated = false;
    NativeBarDrop(NativeBar &bar) : bar(bar)
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget)
        {
            *out = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL point, DWORD *effect) override
    {
        hover = -1;
        activated = false;
        return DragOver(0, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD *effect) override
    {
        if (effect)
            *effect = DROPEFFECT_NONE;
        bar.ClearDockHover();POINT p{point.x, point.y};
        ScreenToClient(bar.hwnd, &p);
        if (bar.edge)
            bar.Reveal();
        int index = bar.Hit(p.x * 96 / bar.dpi, p.y * 96 / bar.dpi);
        if (index != hover)
        {
            hover = index;
            entered = GetTickCount64();
            activated = false;
        }
        if (index >= 0 && bar.entries[index].HasKey(L"windows") &&
            bar.entries[index].GetNamedArray(L"windows").Size() && !activated && GetTickCount64() - entered >= 600)
        {
            activated = true;
            bar.Send(index, L"focus");
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        hover = -1;
        bar.ScheduleHide();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *effect) override
    {
        if (effect)
            *effect = DROPEFFECT_NONE;
        bar.ScheduleHide();
        return S_OK;
    }
};
inline void InstallNativeBarDrop(NativeBar &bar)
{
    auto target = new NativeBarDrop(bar);
    RegisterDragDrop(bar.hwnd, target);
    target->Release();
}
