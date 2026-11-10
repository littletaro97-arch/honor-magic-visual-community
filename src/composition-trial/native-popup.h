#pragma once
#include "native-appearance.h"
#include "native-brand.h"
#include "native-tooltip-layout.h"
#include "native-popup-placement.h"
#include "native-context-motion.h"
#include "native-marquee.h"
#include "native-preview-motion.h"
#include "native-toolbar-assets.h"
#include "native-tray-menu-anchor.h"
#include "native-official-panels.h"
#include <dwmapi.h>
struct NativePopup;
struct NativePopupResize;
struct PopupLife
{
    NativePopup *owner = nullptr;
};
IRawElementProviderSimple *PopupProvider(std::shared_ptr<PopupLife> const &, std::wstring const &key = L"");
struct PopupRow
{
    std::wstring key, label, detail, action, id, handle;
    D2D1_RECT_F rect{};
    bool enabled = true, active = false, offscreen = false, dimmed = false;
    std::wstring control;
    double value = 0;
};
// One event-driven popup shared by the calendar, input selector and user menu.
// Hidden popups own no drawing surface or timer. Graphics/fonts share App.
struct NativePopup
{
    App &app;
    HWND hwnd = nullptr, previousFocus = nullptr;
    // The card's tuning panel is a separate window in the adapter process. While it
    // is open the card must not read its own deactivation as "the reader left": this
    // is the one window that keeps the card standing.
    HWND holdWindow = nullptr;
    // The panel's process: WPF may recreate the panel's HWND between EnsureHandle and
    // Show, so a handle that no longer matches must not dismiss the card while the
    // reader is still in the panel. Only this one process's windows are exempt.
    DWORD holdPid = 0;
    float dpi = 96, width = 360, height = 440;
    std::wstring token, kind, caption, status, armed;
    ULONGLONG armedUntil = 0;
    int ConfirmationSeconds(ULONGLONG now=GetTickCount64())const{return armed.empty()||now>=armedUntil?0:static_cast<int>((armedUntil-now+999)/1000);}
    D2D1_RECT_F ConfirmationBounds(PopupRow const &row)const{return {row.rect.right-150,row.rect.top,row.rect.right-8,row.rect.bottom};}
    JsonObject packet{nullptr};
    bool visible = false, closing = false, english = false, dark = false, yearMode = false;
    int year = 2026, month = 1, selectedDay = 1, selectedYear = 2026, selectedMonth = 1, firstWeekday = 1, scroll = 0,
        focus = -1;
    int visibleRows = 8, hover = -1,pressedTray=-1;
    float appearanceOverflow=0,appearanceTop=0,appearanceView=0,appearanceOffset=0;
    // Content bottom of the account card as laid out, before the card's own scroll
    // shifts it. The height contract reads this; other kinds leave it at zero.
    float userMenuBottom=0;
    POINT trayPress{};bool trayDragging=false;
    Preview70Icons appearanceIcons;
    NativeTooltipLayout tooltipLayout;
    std::set<std::wstring> iconRequests;
    std::map<std::wstring,std::wstring> imageSources;
    void RequestAppearanceIcons();
    float CalendarHeight()const{
        using namespace std::chrono;if(yearMode)return 82+4*58.f+68;
        auto first=year_month_day{std::chrono::year{year},std::chrono::month{static_cast<unsigned>(month)},day{1}};
        int lead=(static_cast<int>(weekday{sys_days{first}}.c_encoding())-firstWeekday+7)%7;
        int days=static_cast<int>(static_cast<unsigned>((std::chrono::year{year}/std::chrono::month{static_cast<unsigned>(month)}/last).day()));
        return 88+((lead+days+6)/7)*((width-36)/7)+68;
    }
    bool ToolbarDropdown()const{return kind==L"@magic/user-menu"||kind==L"@magic/calendar-popup"||kind==L"@magic/keyboard-selector"||kind==L"@magic/bluetooth-popup"||kind==L"@magic/network-popup"||kind==L"@magic/media-popup"||kind==L"@magic/system-tray"||kind==L"@magic/quick-settings"||kind==L"@magic/notifications";}
    // Tray/apps pixels belong entirely to Composition. Apps text uses a small
    // owned opaque input host; other kinds retain their existing child EDITs.
    DWORD HostStyle()const{return WS_EX_TOOLWINDOW|WS_EX_TOPMOST|(kind==L"@magic/system-tray"||kind==L"@magic/apps-menu"?WS_EX_NOREDIRECTIONBITMAP:0);}
    void ResetIncompatibleHost(){
        // Windows does not permit toggling NOREDIRECTIONBITMAP on an existing
        // HWND. Recreate only when crossing tray/input modes, after FinishHide.
        if(hwnd&&bool(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_NOREDIRECTIONBITMAP)!=bool(HostStyle()&WS_EX_NOREDIRECTIONBITMAP))DestroyHost();
    }
    int DropdownTop(int anchor,float scale)const{return anchor+static_cast<int>(std::lround((NativeMetrics::ToolbarPopupGap-NativeMetrics::PopupInset)*scale));}
    int DropdownClampTop(int y,RECT const &work,int h)const{
        return std::clamp(y,static_cast<int>(work.top),std::max(static_cast<int>(work.top),static_cast<int>(work.bottom)-h));
    }
    int UpdatedPanelTop(RECT const &before,RECT const &work,int h,float unit)const{
        int y=before.top;auto value=packet?packet.TryLookup(L"payload"):nullptr;
        if(value&&value.ValueType()==JsonValueType::Object){auto payload=value.GetObject();auto position=payload.TryLookup(L"desiredPosition");
            if(position&&position.ValueType()==JsonValueType::Object){int anchor=static_cast<int>(Num(position.GetObject(),L"y"));auto align=Str(payload,L"alignY");
                if(align==L"Start")y=DropdownTop(anchor,unit);else if(align==L"End")y=anchor-h;else if(align==L"Center")y=anchor-h/2;
            }
        }return DropdownClampTop(y,work,h);
    }
    void ResizeCalendar(){
        if(!visible||!hwnd)return;auto payload=packet.GetNamedObject(L"payload"),anchor=payload.GetNamedObject(L"desiredPosition");
        int ax=static_cast<int>(Num(anchor,L"x")),ay=static_cast<int>(Num(anchor,L"y"));float scale=dpi/96;int w=static_cast<int>(std::ceil(width*scale)),h=static_cast<int>(std::ceil(height*scale));
        MONITORINFO m{sizeof(m)};GetMonitorInfoW(MonitorFromPoint({ax,ay},MONITOR_DEFAULTTONEAREST),&m);
        bool horizontal=Str(payload,L"alignY")==L"Start"||Str(payload,L"alignY")==L"End";
        auto placement=NativePlacePopup(ax,w,m.rcWork,Str(payload,L"alignX"),scale);int x=placement.x;
        if(!horizontal){int origin=Str(payload,L"alignX")==L"End"?w:Str(payload,L"alignX")==L"Center"?w/2:0;x=std::clamp(ax-origin,static_cast<int>(m.rcWork.left),std::max(static_cast<int>(m.rcWork.left),static_cast<int>(m.rcWork.right)-w));}
        motionAnchorX=horizontal?placement.origin:width/2;centeredAtTrigger=horizontal&&placement.centered;
        int y=DropdownClampTop(Str(payload,L"alignY")==L"Start"?DropdownTop(ay,scale):ay-(Str(payload,L"alignY")==L"End"?h:h/2),m.rcWork,h);
        NativeLayerPosition(hwnd,x,y,w,h,SWP_NOACTIVATE);auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(2*MaterialRadius()*scale),static_cast<int>(2*MaterialRadius()*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
    }
    bool dismissing=false,opening=false,centeredAtTrigger=false;
    float motionAnchorX=-1;
    bool DesktopContext()const{return packet&&LocalContext()&&Str(Data(),L"menuStyle")==L"desktop";}
    bool AnimatedContext()const{return packet&&LocalContext()&&(Str(Data(),L"menuStyle")==L"desktop"||Str(Data(),L"menuStyle")==L"toolbar");}
    bool contextChild=false,contextFlipped=false;float2 contextOrigin{};
    int PanelCloseMs()const{return LocalDockPreview()?NativeInteractionMotion::CloseMs:(AnimatedContext()||ToolbarDropdown()||kind==L"@magic/power-menu")?NativeContextMotion::CloseMs:NativeInteractionMotion::CloseMs;}
    RECT ContentScreenRect()const{RECT rect{};if(hwnd)GetWindowRect(hwnd,&rect);float unit=dpi/96;rect.left+=static_cast<LONG>(contentX*unit);rect.top+=static_cast<LONG>(contentY*unit);rect.right=rect.left+static_cast<LONG>(std::ceil(width*unit));rect.bottom=rect.top+static_cast<LONG>(std::ceil(height*unit));return rect;}
    bool MotionPanel()const{return LocalDockPreview()||AnimatedContext()||Flyout()|| LocalPicker()||ToolbarDropdown()||Modal()||kind==L"@magic/apps-menu";}
    void SetMotionInputs(bool show){
        for(auto const &[id,edit]:edits){auto row=std::find_if(rows.begin(),rows.end(),[&](auto const&r){return r.id==id;});
            bool showEdit=show&&!resizeInputsBlocked&&!dismissing&&row!=rows.end()&&!row->offscreen&&(row->rect.top>=58||id==L"search");ShowInput(id,showEdit);}
    }
    void FocusReadyInput(){if(kind==L"@magic/apps-menu"&&(GetForegroundWindow()==hwnd||InputFamily(GetForegroundWindow()))&&edits.contains(L"search")&&IsWindowVisible(edits[L"search"]))SetFocus(edits[L"search"]);}
    void ResetPanelMotion(bool restoreContent=true){
        if(hwnd){KillTimer(hwnd,88);KillTimer(hwnd,89);}previewEnterAt=0;bool wasOpening=std::exchange(opening,false);
        if(popupRoot){popupRoot.StopAnimation(L"Scale");popupRoot.StopAnimation(L"Offset");popupRoot.StopAnimation(L"Opacity");popupRoot.Scale({1,1,1});popupRoot.Offset({0,0,0});popupRoot.Opacity(1);}
        if(shade){shade.StopAnimation(L"Opacity");shade.Opacity(1);}
        if(restoreContent&&wasOpening&&visible&&!dismissing){SetMotionInputs(true);UpdateTaskPreviews();}
    }
    bool PanelAbove()const{return kind==L"@magic/apps-menu"||Str(packet.GetNamedObject(L"payload"),L"alignY")==L"End";}
    void AnimatePanel(bool close){
        if(!popupRoot)return;
        if(LocalDockPreview()){AnimatePreview(close);return;}
        if(AnimatedContext()){
            if(close){opening=false;if(hwnd)KillTimer(hwnd,89);NativeContextMotion::Exit(app.graphics.compositor,popupRoot);return;}
            ResetPanelMotion(false);opening=true;auto &c=app.graphics.compositor;float unit=dpi/96,start=contextChild?.9f:.78f;
            popupRoot.CenterPoint({(contentX+contextOrigin.x)*unit,(contentY+contextOrigin.y)*unit,0});popupRoot.Scale({start,start,1});popupRoot.Opacity(0);
            float shift=contextChild?(contextFlipped?6.f:-6.f)*unit:0;popupRoot.Offset({shift,0,0});NativeContextMotion::Enter(c,popupRoot,contextChild);
            if(contextChild){auto move=NativeInteractionMotion::DockSpringVector(c,{shift,0,0},{0,0,0});move.Duration(std::chrono::milliseconds(NativeContextMotion::SubMs));popupRoot.StartAnimation(L"Offset",move);}
            if(!hwnd||!SetTimer(hwnd,89,(contextChild?NativeContextMotion::SubMs:NativeContextMotion::RootMs)+16,nullptr))ResetPanelMotion();return;
        }
        if(ToolbarDropdown()||kind==L"@magic/power-menu"){
            if(close){opening=false;if(hwnd)KillTimer(hwnd,89);NativeContextMotion::Exit(app.graphics.compositor,popupRoot);if(Modal()&&shade)NativeInteractionMotion::Opacity(app.graphics.compositor,shade,0,NativeContextMotion::CloseMs);return;}
            ResetPanelMotion(false);opening=true;auto &c=app.graphics.compositor;
            auto pose=NativeInteractionMotion::Panel(width,height,false,PanelAbove(),1,Modal());
            popupRoot.CenterPoint({(contentX+(!Modal()&&motionAnchorX>=0?motionAnchorX:pose.origin.x))*dpi/96,(contentY+pose.origin.y)*dpi/96,0});popupRoot.Scale({.78f,.78f,1});popupRoot.Opacity(0);
            SetMotionInputs(false);NativeContextMotion::EnterPanel(c,popupRoot,std::max(width,height));if(Modal()&&shade){shade.Opacity(0);NativeInteractionMotion::Opacity(c,shade,1,120);}
            if(!hwnd||!SetTimer(hwnd,89,NativeContextMotion::RootMs+16,nullptr))ResetPanelMotion();return;
        }
        if(Flyout()){
            ResetPanelMotion(false);auto &c=app.graphics.compositor;popupRoot.CenterPoint({width*dpi/192,0,0});
            if(close){NativeInteractionMotion::Vector(c,popupRoot,L"Scale",{.93f,.93f,1},NativeInteractionMotion::CloseMs);NativeInteractionMotion::Vector(c,popupRoot,L"Offset",{0,-12*dpi/96,0},NativeInteractionMotion::CloseMs);NativeInteractionMotion::Opacity(c,popupRoot,0,NativeInteractionMotion::CloseMs);}
            else{opening=true;popupRoot.Scale({.62f,.62f,1});popupRoot.Offset({0,-14*dpi/96,0});popupRoot.Opacity(0);auto spring=NativeInteractionMotion::DockSpringVector(c,{.62f,.62f,1},{1,1,1});spring.Duration(std::chrono::milliseconds(520));popupRoot.StartAnimation(L"Scale",spring);auto offset=NativeInteractionMotion::DockSpringVector(c,{0,-14*dpi/96,0},{0,0,0});offset.Duration(std::chrono::milliseconds(520));popupRoot.StartAnimation(L"Offset",offset);NativeInteractionMotion::Opacity(c,popupRoot,1,220);SetTimer(hwnd,89,536,nullptr);}return;
        }
        bool picker=LocalPicker();auto payload=packet.GetNamedObject(L"payload");bool above=PanelAbove();
        auto pose=NativeInteractionMotion::Panel(width,height,picker,above,1,Modal());float factor=pose.scale,shift=pose.offset.y*dpi/96;
        popupRoot.CenterPoint({(contentX+(!Modal()&&motionAnchorX>=0?motionAnchorX:pose.origin.x))*dpi/96,(contentY+pose.origin.y)*dpi/96,0});
        if(!close){ResetPanelMotion(false);opening=true;popupRoot.Scale({factor,factor,1});popupRoot.Offset({0,shift,0});popupRoot.Opacity(0);if(Modal()&&shade)shade.Opacity(0);}
        else {opening=false;KillTimer(hwnd,89);}
        SetMotionInputs(false);for(auto thumbnail:taskThumbnails)DwmUnregisterThumbnail(thumbnail);taskThumbnails.clear();taskPreviewBytes=0;
        int ms=close?NativeInteractionMotion::CloseMs:NativeInteractionMotion::OpenMs;auto &c=app.graphics.compositor;
        NativeInteractionMotion::Vector(c,popupRoot,L"Scale",close?float3{factor,factor,1}:float3{1,1,1},ms);
        NativeInteractionMotion::Vector(c,popupRoot,L"Offset",close?float3{0,shift,0}:float3{},ms);
        NativeInteractionMotion::Opacity(c,popupRoot,close?0.f:1.f,ms);
        if(Modal()&&shade)NativeInteractionMotion::Opacity(c,shade,close?0.f:1.f,ms);
        if(!close&&!SetTimer(hwnd,89,NativeInteractionMotion::OpenMs+16,nullptr))ResetPanelMotion();
    }
    void FadeIn(){if(MotionPanel()){if(NativeMotion(app.model)&&(!LocalPicker()||NativeMotion(app.model,true)))AnimatePanel(false);else ResetPanelMotion();return;}if(!visual)return;visual.StopAnimation(L"Opacity");visual.Opacity(1);BOOL enabled=TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);if(!enabled||!NativeMotion(app.model))return;
        auto animation=app.graphics.compositor.CreateScalarKeyFrameAnimation();animation.InsertKeyFrame(0,0);animation.InsertKeyFrame(1,1);
        animation.Duration(std::chrono::milliseconds(150));visual.StartAnimation(L"Opacity",animation);
    }
    std::vector<HTHUMBNAIL> taskThumbnails;
    size_t taskPreviewBytes=0;
    bool iconReady=false, contextMenuOpen=false;
    void UpdateTaskPreviews();void BuildDockPreviewRows();void PaintDockPreview(ID2D1DeviceContext *);void UpdateDockPreviewThumbnails();
    NativeToolbarSvg previewSvg;float previewScroll=0;
    NativePreviewMotion::Options previewMotion;std::wstring previewSide=L"Bottom";float2 previewOrigin{};ULONGLONG previewEnterAt=0;bool previewPreparing=false,previewMounted=false;
    std::map<HTHUMBNAIL,DWM_THUMBNAIL_PROPERTIES> previewThumbnailProps;
    void AnimatePreview(bool close);void TickPreviewMotion();
    SpriteVisual refreshVisual{nullptr};CompositionDrawingSurface refreshSurface{nullptr};std::wstring refreshId;ULONGLONG refreshUntil=0;size_t refreshBytes=0;
    bool StartRefreshFeedback(PopupRow const &);void SyncRefreshFeedback();void StopRefreshFeedback();
    std::map<std::wstring,NativeMarquee> mediaMarquees;size_t marqueeBytes=0;
    void SyncMediaMarquees();void ClearMediaMarquees(){for(auto &[_,m]:mediaMarquees)m.Release();mediaMarquees.clear();marqueeBytes=0;}
    bool LocalDockPreview()const{return kind==L"@magic/dock-preview";}
    virtual void MiddleActivate(int){}
    HWND tooltip=nullptr;
    std::wstring tooltipText;
    HWND changedEdit=nullptr;
    HFONT inputFont=nullptr;
    HBRUSH inputBackground=nullptr;
    std::wstring pickerType,pickerBoard;
    POINT pickerCell{};bool pickerHasCell=false;
    int pickerSize=0;bool pickerConfirm=false;
    long long pickerRevision=0;
    JsonArray pickerCatalog{};
    JsonArray pickerDefinitions{nullptr};
    JsonArray PickerDefinitions()const;
    int PickerColumns()const{return std::clamp(static_cast<int>((width*.68f-36)/120),1,4);}
    int PickerInstances()const;
    void LoadPickerCatalog();void PickerMenu(int index,POINT point);
    bool LocalPicker()const{return kind==L"@magic/card-picker";}
    bool LocalTooltip()const{return kind==L"@magic/bar-tooltip";}
    bool LocalContext()const{return kind==L"@magic/context-menu";}
    std::function<void(std::wstring const &)> contextAction;
    std::function<void()> contextDismiss;
    std::function<void(std::wstring const &)> contextHover,contextPrepare;
    std::function<void()> contextBack;
    std::function<bool(HWND)> contextFamily;
    std::function<bool(ID2D1DeviceContext*,std::wstring const&,D2D1_RECT_F)> contextIcon;
    float ContextPadding()const{return Str(Data(),L"menuStyle")==L"toolbar"?8.f:20.f;}
    float ContextViewport()const{return std::max(1.f,height-ContextPadding());}
    // v0.4.9 lays a long menu out in columns, which runs the rightmost one past the
    // monitor edge on a list like the 39-vendor mark picker. The scrolling column is
    // the default here and the column layout stays available from the bar's own menu.
    bool ContextScrolls()const{return Str(app.model.preferences,L"contextMenuLayout")!=L"columns";}
    bool ContextOverflows()const{return ContextScrolls()&&contextTotal>ContextViewport()+.5f;}
    float contextColumnWidth=0;
    int contextColumns=1;
    struct ContextLayout {float width,height,columnWidth;int columns;};
    static ContextLayout MeasureContext(JsonArray const &values,std::wstring const &style,float maximumWidth,float maximumHeight,float preferredWidth,bool scrolls){
        float padding=style==L"toolbar"?8.f:20.f;
        auto rowHeight=[&](auto const &v){return Str(v.GetObject(),L"control")==L"separator"?9.f:(style==L"shell"||style==L"toolbar")?40.f:32.f;};
        float content=padding;for(auto const &v:values)content+=rowHeight(v);
        float h=std::min(content,maximumHeight);
        if(scrolls){float column=std::min(preferredWidth,maximumWidth);return {column,h,column,1};}
        float viewport=std::max(1.f,h-padding),used=0;int columns=1;
        for(auto const &v:values){float row=rowHeight(v);if(used>0&&used+row>viewport+.1f){++columns;used=0;}used+=row;}
        float column=std::min(preferredWidth,maximumWidth/columns);return {column*columns,h,column,columns};
    }
    void LayoutContext(float maximumWidth,float maximumHeight,float preferredWidth){
        bool scrolls=ContextScrolls();
        auto layout=MeasureContext(Data().GetNamedArray(L"rows"),Str(Data(),L"menuStyle"),maximumWidth,maximumHeight,preferredWidth,scrolls);
        width=layout.width;height=layout.height;contextColumnWidth=layout.columnWidth;contextColumns=layout.columns;contextScroll=0;
        // One line per measurement: the switch is a preference, so "it did nothing" has to
        // be distinguishable from "it measured one column anyway" without a debugger.
        JsonObject trace;trace.Insert(L"scrolls",JsonValue::CreateBooleanValue(scrolls));Put(trace,L"columns",static_cast<double>(contextColumns));Put(trace,L"width",static_cast<double>(width));Put(trace,L"height",static_cast<double>(height));Put(trace,L"columnWidth",static_cast<double>(contextColumnWidth));Put(trace,L"rows",static_cast<double>(Data().GetNamedArray(L"rows").Size()));Put(trace,L"maxHeight",static_cast<double>(maximumHeight));Put(trace,L"preferredWidth",static_cast<double>(preferredWidth));Put(trace,L"style",Str(Data(),L"menuStyle"));app.Telemetry(L"context-layout",trace);
    }
    // The level's base width: the same number Show() feeds MeasureContext, kept so an
    // in-place re-layout measures with the same input instead of its own output.
    float contextPreferredWidth=300;
    struct ContextFrame{int x=0,y=0,width=0,height=0;};
    // One geometry for a cascade level: measure from the level's own packet, then place
    // it from the anchor that opened it, with the same transparent gutter Show() adds.
    // Show() and SetContextRows() both call this, so re-laying in place lands exactly
    // where reopening the level does.
    ContextFrame MeasureAndPlaceContext(RECT const &work){
        float scale=dpi/96;
        float maxWidth=(work.right-work.left)/scale-12,maxHeight=(work.bottom-work.top)/scale-12;
        LayoutContext(maxWidth,maxHeight,contextPreferredWidth);
        width=std::min(width,(work.right-work.left)/scale-20);height=std::min(height,(work.bottom-work.top)/scale-20);
        // Shrinking the finished frame to the 8 MiB surface bound left it narrower
        // than columns*columnWidth, so the last column and its tick fell outside.
        // Feed the tighter width back into the same measurement instead: the
        // columns narrow with the frame and every column stays inside it. A single
        // column has none to give back and keeps the minimal scale below.
        for(int attempt=0;attempt<8&&contextColumns>1&&width*height*scale*scale*4>8*1024*1024.f;++attempt){
            float budget=8*1024*1024.f/(height*scale*scale*4)*.998f;
            if(budget>=width)break;
            LayoutContext(budget,maxHeight,contextPreferredWidth);
            width=std::min(width,(work.right-work.left)/scale-20);height=std::min(height,(work.bottom-work.top)/scale-20);
        }
        float fit=std::min(1.f,std::sqrt((8*1024*1024.f)/(width*height*scale*scale*4)));if(fit<1){width*=fit*.998f;height*=fit*.998f;}
        auto payload=packet.GetNamedObject(L"payload"),point=payload.GetNamedObject(L"desiredPosition");
        int ax=static_cast<int>(Num(point,L"x")),ay=static_cast<int>(Num(point,L"y"));
        int pw=static_cast<int>(std::ceil(width*scale)),ph=static_cast<int>(std::ceil(height*scale));
        auto offset=[](std::wstring const &align,int size){return align==L"Center"?size/2:align==L"End"?size:0;};
        int x=ax-offset(Str(payload,L"alignX"),pw),y=ay-offset(Str(payload,L"alignY"),ph);
        if(Num(payload,L"contextDepth")>0&&payload.HasKey(L"contextParentLeft")){
            auto side=NativeContextMotion::ChildX(static_cast<int>(Num(payload,L"contextParentLeft")),static_cast<int>(Num(payload,L"contextParentRight")),pw,work,scale);
            ax=x=side.x;payload.Insert(L"contextFlipped",JsonValue::CreateBooleanValue(side.flipped));
        }
        x=std::clamp(x,static_cast<int>(work.left),static_cast<int>(work.right)-pw);
        y=DropdownClampTop(y,work,ph);
        centeredAtTrigger=false;motionAnchorX=-1;
        previewOrigin={static_cast<float>((Num(payload,L"previewAnchorX",ax)-x)/scale),Str(payload,L"alignY")==L"End"?height:Str(payload,L"alignY")==L"Start"?0.f:static_cast<float>((Num(payload,L"previewAnchorY",ay)-y)/scale)};
        contentX=contentY=0;
        if(AnimatedContext()){
            contextChild=Num(payload,L"contextDepth")>0;contextFlipped=Bool(payload,L"contextFlipped");
            auto placement=NativeContextMotion::Place({ax,ay},work,pw,ph,scale,contextChild,contextFlipped,static_cast<float>(Num(payload,L"contextAnchorY",ay+22*scale)));
            if(!DesktopContext()&&!contextChild){
                // Keep the bar's existing centered/edge-clamped placement.
                contextOrigin={std::clamp((ax-x)/scale,4.f,width-4),std::clamp((ay-y)/scale,4.f,height-4)};
            }else{x=placement.x;y=placement.y;contextOrigin=placement.origin;}
            // Transparent overshoot allowance is not a second material/background.
            contentX=NativeContextMotion::Gutter(width);contentY=NativeContextMotion::Gutter(height);
            x-=static_cast<int>(contentX*scale);y-=static_cast<int>(contentY*scale);pw+=static_cast<int>(2*contentX*scale);ph+=static_cast<int>(2*contentY*scale);
        }
        return {x,y,pw,ph};
    }
    float contextScroll=0,contextTotal=0,contextDragY=0,contextDragStart=0;
    bool contextDragging=false,contextScrollbarHover=false;
    float ContextRowHeight(JsonObject const &row)const{return Str(row,L"control")==L"separator"?9.f:(Str(Data(),L"menuStyle")==L"shell"||Str(Data(),L"menuStyle")==L"toolbar")?40.f:32.f;}
    void BuildContextRows();void PaintContext(ID2D1DeviceContext *c);
    void SetContextRows(JsonArray const &values){
        Data().Insert(L"rows",values);scroll=0;focus=-1;hover=-1;contextScroll=0;
        if(hwnd&&packet){MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);float scale=dpi/96;
            // Same measurement and anchor as Show(): the level must land where reopening
            // it would, and the window must carry the paint's transparent gutter or the
            // region clips the right and bottom edges away.
            auto frame=MeasureAndPlaceContext(monitor.rcWork);visibleRows=std::max(1,static_cast<int>((height-20)/32));
            if(popupRoot)popupRoot.Size({static_cast<float>(frame.width),static_cast<float>(frame.height)});if(popupScene)popupScene.Size({static_cast<float>(frame.width),static_cast<float>(frame.height)});
            if(visual)visual.Offset({contentX*scale,contentY*scale,0});
            if(AnimatedContext())SetWindowRgn(hwnd,nullptr,FALSE);
            else{auto region=CreateRoundRectRgn(0,0,frame.width+1,frame.height+1,static_cast<int>(2*MaterialRadius()*scale),static_cast<int>(2*MaterialRadius()*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);}
            NativeLayerPosition(hwnd,frame.x,frame.y,frame.width,frame.height,SWP_NOACTIVATE);Release();
        }BuildRows();Draw();
    }
    // A rebuild regenerates a level's rows in place. The cascade saves this value before
    // SetContextRows and puts it back afterwards, clamped to the new scroll range.
    float ContextScrollOffset()const{return contextScroll;}
    void SetContextScrollOffset(float value){
        contextScroll=std::clamp(value,0.f,std::max(0.f,contextTotal-ContextViewport()));
        if(hwnd&&packet){BuildRows();Draw();}
    }
    JsonObject PickerDefinition()const;
    void BuildPickerRows();void PaintPicker(ID2D1DeviceContext *c);void PickerActivate(PopupRow const &row);
    bool PowerView() const;
    bool DisplayView()const{return packet&&Bool(Data(),L"nativeDisplayView");}
    bool CenterPanel()const{return kind==L"@magic/notifications"||kind==L"@magic/quick-settings"&&!PowerView()&&!DisplayView();}
    bool FunctionalPanel()const{return kind==L"@magic/quick-settings"||kind==L"@magic/network-popup"||kind==L"@magic/bluetooth-popup"||kind==L"@magic/media-popup"||kind==L"@magic/notifications";}
    bool quickDevices=false,audioAdvanced=false,resizeDisclosure=false;
    void RelayoutDisclosure();
    void LayoutCenterPanel();void PaintCenterPanel(ID2D1DeviceContext *);
    std::shared_ptr<NativePopupResize> resizeMotion;
    JsonObject resizePending{nullptr};bool resizePreparing=false,resizePainting=false,resizeInputsBlocked=false,resizeImmediate=false;
    bool TryResizeUpdate(JsonObject const &);void FinishResize(bool redraw=true,bool flush=true,bool animatePending=false);size_t ResizeBytes()const;
    float FunctionalHeight(RECT const &work,float unit)const{
        bool tray=kind==L"@magic/system-tray";float minimum=tray?130.f:220.f,content=tray?70+std::ceil(rows.size()/5.f)*56:functionalExtent+((kind==L"@magic/bluetooth-popup"||kind==L"@magic/network-popup")?18:66);
        float next=std::min(std::max(minimum,content),(work.bottom-work.top)/unit-20);next=std::min(next,(8*1024*1024.f)/(width*unit*unit*4)*.998f);
        float limit=NativeGlassHeightLimit(width,unit,MaterialTier());return limit>=minimum?std::min(next,limit):next;
    }
    void LayoutFunctionalPanel();void PaintFunctionalPanel(ID2D1DeviceContext *);
    struct FunctionalFrame{D2D1_RECT_F rect;std::wstring label;};
    std::vector<FunctionalFrame> functionalFrames,functionalSections;
    std::set<std::wstring> functionalPinned;
    float functionalTop=58,functionalBottom=0,functionalOverflow=0,functionalExtent=0;
    NativeOfficialPanelSvg officialPanelIcons;
    D2D1_RECT_F RangeTrack(PopupRow const &row)const{auto r=row.rect;if(kind==L"@magic/media-popup"&&row.id.starts_with(L"volume:")&&row.id!=L"volume:microphone")return {r.left+46,r.top+6,r.right-16,r.bottom-6};bool compact=!Flyout()&&(kind==L"@magic/media-popup"||kind==L"@magic/quick-settings"&&!PowerView()&&!DisplayView());return {r.left+(compact?52:12),r.bottom-34,r.right-(compact?54:12),r.bottom-6};}
    // The account card's rows begin below the brand header; while the card scrolls,
    // that boundary is also the clip, or a half-scrolled row would paint over it.
    float ContentTop()const{return kind==L"@magic/quick-settings"&&!PowerView()&&!DisplayView()?14.f:kind==L"@magic/apps-menu"?18.f:kind==L"@magic/user-menu"?76.f:54.f;}
    void LayoutPreview70();
    void PaintPreview70(ID2D1DeviceContext *c);
    long long shows = 0, hides = 0, redraws = 0;
    size_t surfaceBytes = 0;
    std::map<std::wstring, HWND> edits;
    std::map<std::wstring, HWND> inputHosts;
    std::set<HWND> composingInputs;
    HWND EnsureInput(PopupRow const &row);
    void PlaceInput(std::wstring const &,int,int,int,int,bool);
    void ShowInput(std::wstring const &,bool);
    bool InputFamily(HWND window)const;
    static LRESULT CALLBACK InputHostProc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK InputEditProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    void CloseInputs(){
        changedEdit=nullptr;if(hwnd)KillTimer(hwnd,83);
        for(auto const &[_,edit]:edits){ShowWindow(edit,SW_HIDE);DestroyWindow(edit);}edits.clear();
        for(auto const &[_,host]:inputHosts){SetWindowLongPtrW(host,GWLP_USERDATA,0);ShowWindow(host,SW_HIDE);DestroyWindow(host);}inputHosts.clear();composingInputs.clear();
    }
    std::wstring networkExpanded;
    std::wstring bluetoothExpanded; // Local disclosure only; device commands remain authenticated.
    bool generic = false, passive = false, measurement = false;
    std::function<void()> passiveClick;
    bool Flyout()const{return kind==L"@magic/flyouts";}
    bool flyoutExpanded=false,flyoutOverflow=false,flyoutDragging=false,flyoutInFlight=false,flyoutPending=false;
    float flyoutFullHeight=0,flyoutScroll=0,flyoutBodyLimit=240;D2D1_RECT_F flyoutBody{};
    std::function<void(double,bool,std::function<void(bool,double)>)> flyoutAdjust;
    double flyoutStartValue=0;bool flyoutFeedback=false;
    bool flyoutScheduled=false,flyoutFinalPending=false;ULONGLONG flyoutLastSent=0,flyoutEchoUntil=0;
    std::shared_ptr<NativeFlyoutResize> flyoutResize;bool flyoutResizePainting=false;
    void FinishFlyoutResize(bool redraw=true);size_t FlyoutResizeBytes()const;
    void BuildFlyout();void ResizeFlyout(bool animate=false);void PaintFlyout(ID2D1DeviceContext *);
    void SetFlyoutValue(double);void SendFlyoutValue(bool final=false);bool FlyoutPointer(UINT,WPARAM,LPARAM);void ActivateFlyout(PopupRow const &);
    struct RangeEdit {double value=0,confirmed=0;bool pending=false,inFlight=false,feedback=false;ULONGLONG holdUntil=0;};
    std::map<std::wstring,RangeEdit> rangeEdits;
    double rangeStartValue=0;std::wstring rangeDragging;bool rangeInFlight=false,rangeScheduled=false,rangeFinalPending=false;ULONGLONG rangeLastSent=0;
    bool AdjustableRange(PopupRow const &row)const{return generic&&!Flyout()&&row.enabled&&row.control==L"range"&&(row.id.starts_with(L"brightness:")||row.id.starts_with(L"volume:")||row.id.starts_with(L"session:"));}
    void SetRangeValue(std::wstring const &,double);void SendRangeValue(bool final=false);bool RangePointer(UINT,WPARAM,LPARAM);
    double RangeValue(std::wstring const &,double);
    std::vector<PopupRow> rows;
    std::shared_ptr<PopupLife> life;
    Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
    ContainerVisual popupRoot{nullptr},popupScene{nullptr};
    SpriteVisual shade{nullptr};
    float contentX=0,contentY=0;
    bool Modal()const{return kind==L"@magic/power-menu"||kind==L"@magic/task-switcher";}
    SpriteVisual visual{nullptr};
    CompositionDrawingSurface surface{nullptr};
    SpriteVisual materialVisual{nullptr};
    bool materialPreview=false;
    NativeGlassSheet glassSheet;
    NativeHoverVisual interactionVisual;D2D1_POINT_2F interactionPoint{};
    bool Glass()const{return !NativeLowPower(app.model)&&(materialVisual||glassSheet.root||materialPreview);}
    NativeMaterial MaterialTier()const{return LocalContext()||LocalTooltip()||Flyout()||ToolbarDropdown()||kind==L"@magic/power-menu"?NativeMaterial::Menu:NativeMaterial::Panel;}
    float MaterialRadius()const{
        if(Flyout())return 28.f;
        if(LocalTooltip()||LocalDockPreview())return 10.f;
        if(LocalContext()&&Str(Data(),L"menuStyle")==L"toolbar")return 10.f;
        // The account card carries its own corner; the window region reads from here
        // too, so changing only the paint would be clipped away.
        if(kind==L"@magic/user-menu"){double r=Num(app.model.preferences,L"accountCardRadius");if(r>=8&&r<=32)return static_cast<float>(r);}
        return NativeMetrics::FloatingRadius;
    }
    D2D1_RECT_F MaterialBounds()const{return LocalContext()?D2D1::RectF(4,4,width-4,height-4):LocalTooltip()||LocalDockPreview()?D2D1::RectF(0,0,width,height):D2D1::RectF(10,10,width-10,height-10);}
    struct PickerTile {SpriteVisual visual{nullptr};CompositionDrawingSurface surface{nullptr};int width=0,height=0;};
    std::vector<PickerTile> pickerTiles;
    com_ptr<ID2D1SolidColorBrush> brush;
    NativePopup(App &value) : app(value)
    {
    }
    virtual ~NativePopup()
    {
        contextAction={};contextDismiss={};contextFamily={};contextIcon={};contextPrepare={};
        if(life)life->owner=nullptr;
        DestroyHost();
        if(inputFont)DeleteObject(inputFont);
        if(inputBackground)DeleteObject(inputBackground);
    }
    void DestroyHost(){
        if(!hwnd)return;auto old=hwnd;WTSUnRegisterSessionNotification(old);SetWindowLongPtrW(old,GWLP_USERDATA,0);
        Release();CloseInputs();if(tooltip){DestroyWindow(tooltip);tooltip=nullptr;}NativeGlassStates().erase(old);
        if(target){try{target.Root(nullptr);}catch(...){}}target=nullptr;visual=nullptr;popupRoot=nullptr;popupScene=nullptr;shade=nullptr;
        hwnd=nullptr;DestroyWindow(old);
    }
    std::wstring Ui(wchar_t const *zh, wchar_t const *en) const
    {
        return english ? en : zh;
    }
    JsonObject Data() const
    {
        return packet ? packet.GetNamedObject(L"data") : JsonObject{};
    }
    JsonObject Account() const
    {
        return packet.GetNamedObject(L"account");
    }
    // Account-card row metrics. LayoutPreview70 applies the same range gates and the
    // same fallbacks; these are the named values this side reads, so the estimate and
    // the renderer cannot drift by a typed-in number.
    static constexpr float AccountRowHeightMin=28,AccountRowHeightMax=52,AccountRowHeightDefault=38;
    static constexpr float AccountGapMin=0,AccountGapMax=16,AccountGapDefault=0;
    // The account card's content bottom, derived from the packet's rows with the
    // same accumulation LayoutPreview70 runs, so the card height is final before
    // BuildRows lays anything out.
    float UserMenuContentBottom() const
    {
        auto entries = Data().GetNamedArray(L"rows");
        int count = std::min(256, static_cast<int>(entries.Size()));
        double wantedRow = Num(app.model.preferences, L"accountCardRowHeight");
        double wantedGap = Num(app.model.preferences, L"accountCardGap");
        float rowHeight = (wantedRow >= AccountRowHeightMin && wantedRow <= AccountRowHeightMax)
                              ? static_cast<float>(wantedRow)
                              : AccountRowHeightDefault;
        float rowGap = (wantedGap >= AccountGapMin && wantedGap <= AccountGapMax)
                           ? static_cast<float>(wantedGap)
                           : AccountGapDefault;
        float y = 76, bottom = 76;
        for (int i = 0; i < count; i++)
        {
            auto item = entries.GetAt(i).GetObject();
            if (Str(item, L"control") == L"label") { bottom = y + 6; y += 13 + rowGap; }
            else { bottom = y + rowHeight; y += rowHeight + rowGap; }
        }
        return bottom;
    }
    // The smallest card still shows the brand header and one full row: rows begin at
    // 76 DIP and the scroll viewport ends at height-18, so the floor is 94 + the
    // reader's row height (122 at 28 DIP, 146 at 52 DIP).
    float UserMenuMinimumHeight() const
    {
        double wantedRow = Num(app.model.preferences, L"accountCardRowHeight");
        float rowHeight = (wantedRow >= AccountRowHeightMin && wantedRow <= AccountRowHeightMax)
                              ? static_cast<float>(wantedRow)
                              : AccountRowHeightDefault;
        return 76.f + rowHeight + 18.f;
    }
    // The card's height contract: the reader's height is the card's height. Content
    // that does not fit scrolls inside it; content shorter than the card leaves the
    // chosen height standing. The work area is the ceiling.
    float UserMenuHeight(float workHeight, float scale) const
    {
        double cardHeight = Num(app.model.preferences, L"accountCardHeight");
        float preferred = (cardHeight >= 320 && cardHeight <= 780) ? static_cast<float>(cardHeight) : 528.f;
        float limit = std::max(130.f, workHeight / scale - 20);
        return std::min(std::max(preferred, UserMenuMinimumHeight()), limit);
    }
    // One re-frame for the standing card: settle width/height from the same contract
    // Show() uses, then resize around the reader's current top-left. A slider must
    // change the card, never move it.
    void ReframeCard(){
        if(!hwnd)return;
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);float scale=dpi/96;
        double cardWidth=Num(app.model.preferences,L"accountCardWidth");
        if(cardWidth>=260&&cardWidth<=560)width=static_cast<float>(cardWidth);
        height=UserMenuHeight(static_cast<float>(monitor.rcWork.bottom-monitor.rcWork.top),scale);
        width=std::min(width,(monitor.rcWork.right-monitor.rcWork.left)/scale-20);height=std::min(height,(monitor.rcWork.bottom-monitor.rcWork.top)/scale-20);
        float fit=std::min(1.f,std::sqrt((8*1024*1024.f)/(width*height*scale*scale*4)));if(fit<1){width*=fit*.998f;height*=fit*.998f;}
        RECT bounds{};GetWindowRect(hwnd,&bounds);
        int w=static_cast<int>(std::ceil(width*scale)),h=static_cast<int>(std::ceil(height*scale));
        int x=std::clamp(static_cast<int>(bounds.left),static_cast<int>(monitor.rcWork.left),std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-w));
        int y=DropdownClampTop(bounds.top,monitor.rcWork,h);
        if(w==bounds.right-bounds.left&&h==bounds.bottom-bounds.top&&x==bounds.left&&y==bounds.top)return;
        if(popupRoot)popupRoot.Size({static_cast<float>(w),static_cast<float>(h)});if(popupScene)popupScene.Size({static_cast<float>(w),static_cast<float>(h)});
        if(visual)visual.Offset({contentX*scale,contentY*scale,0});
        auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(2*MaterialRadius()*scale),static_cast<int>(2*MaterialRadius()*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
        NativeLayerPosition(hwnd,x,y,w,h,SWP_NOACTIVATE);
    }
    std::wstring Kind() const
    {
        return kind.substr(kind.find_last_of(L'/') + 1);
    }
    void Emit(std::wstring const &action, JsonObject result = JsonObject{})
    {
        Put(result, L"action", action);
        JsonObject request;
        Put(request, L"op", std::wstring(L"popup-result"));
        Put(request, L"token", token);
        request.Insert(L"result", result);
        app.Request(request);
    }
    void Metrics(std::wstring const &phase)
    {
        JsonObject detail;
        Put(detail, L"kind", Kind());
        Put(detail,L"view",kind==L"@magic/bluetooth-popup"?L"bluetooth-compact-v1":FunctionalPanel()?L"preview70-functional":L"native");
        Put(detail, L"shows", static_cast<double>(shows));
        Put(detail, L"hides", static_cast<double>(hides));
        Put(detail, L"redraws", static_cast<double>(redraws));
        Put(detail, L"surfaceBytes", static_cast<double>(surfaceBytes));
        Put(detail,L"mediaMarqueeBytes",static_cast<double>(marqueeBytes));Put(detail,L"refreshFeedbackBytes",static_cast<double>(refreshBytes));
        if(auto it=NativeGlassStates().find(hwnd);it!=NativeGlassStates().end()){Put(detail,L"material",it->second.reason);Put(detail,L"materialHresult",static_cast<double>(it->second.error));}
        Put(detail, L"dpi", dpi);detail.Insert(L"centeredAtTrigger",JsonValue::CreateBooleanValue(centeredAtTrigger));
        app.Telemetry(phase, detail);
    }
    virtual void Hide(bool notify = true, bool restore = false)
    {
        if(!visible||dismissing)return;
        if(kind==L"@magic/apps-menu")CloseAppMenu();
        FinishResize(true,false);FinishFlyoutResize();
        if(hwnd)KillTimer(hwnd,95);if(refreshVisual)refreshVisual.StopAnimation(L"RotationAngleInDegrees");for(auto &[_,m]:mediaMarquees)if(m.text)m.text.StopAnimation(L"Offset");
        if(MotionPanel()&&NativeMotion(app.model)&&(!LocalPicker()||NativeMotion(app.model,true))&&!app.stopping&&!measurement&&!app.popupCycles){
            dismissing=closing=true;if(GetCapture()==hwnd)ReleaseCapture();pressedTray=-1;trayDragging=false;
            if(tooltip){DestroyWindow(tooltip);tooltip=nullptr;}
            // Hidden child HWNDs can retain a redirected white input surface.
            // End their lifetime before the visual-only closing animation.
            CloseInputs();RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            for(int timer:{81,82,83,84,85,90,96})KillTimer(hwnd,timer);
            interactionVisual.Sync(app.graphics.compositor,popupRoot,{}, {},dpi/96,false,dark);
            AnimatePanel(true);if(!SetTimer(hwnd,88,PanelCloseMs()+16,nullptr)){FinishHide(notify,restore);return;}
            // Complete the logical dismissal immediately; only the pixels linger.
            if(notify&&!passive&&!LocalPicker()&&!LocalContext())Emit(L"cancel");
            if(restore&&IsWindow(previousFocus))SetForegroundWindow(previousFocus);return;
        }
        FinishHide(notify,restore);
    }
    void FinishHide(bool notify=false,bool restore=false)
    {
        if (!visible)
            return;
        visible = false;dismissing=false;closing=true;holdWindow=nullptr;holdPid=0;ShowWindow(hwnd,SW_HIDE);ResetPanelMotion();
        auto dismiss=std::move(contextDismiss);contextAction={};if(dismiss)dismiss();
        closing = true;
        if(GetCapture()==hwnd)ReleaseCapture();pressedTray=-1;trayDragging=false;
        KillTimer(hwnd, 81);
        KillTimer(hwnd, 82);
        KillTimer(hwnd, 83);
        KillTimer(hwnd, 84);
        KillTimer(hwnd, 85);KillTimer(hwnd,90);KillTimer(hwnd,96);flyoutDragging=flyoutPending=flyoutInFlight=flyoutFeedback=false;flyoutAdjust={};
        rangeEdits.clear();rangeDragging.clear();rangeInFlight=rangeScheduled=rangeFinalPending=false;rangeLastSent=0;
        flyoutScheduled=flyoutFinalPending=false;flyoutLastSent=flyoutEchoUntil=0;
        SetTimer(app.relay, 74, 2000, nullptr);
        if(tooltip){DestroyWindow(tooltip);tooltip=nullptr;}
        officialPanelIcons.documents.clear();functionalFrames.clear();functionalSections.clear();functionalPinned.clear();
        appearanceIcons.Clear();tooltipLayout.text=nullptr;iconRequests.clear();imageSources.clear();iconReady=false;
        pickerConfirm=false;pickerCatalog=JsonArray{};pickerDefinitions=nullptr;pickerRevision=0;
        armed.clear();armedUntil=0;
        if (life)
            life->owner = nullptr;
        ShowWindow(hwnd, SW_HIDE);
        if(shade)shade.IsVisible(false);
        Release();
        CloseInputs();
        rows.clear();
        packet = nullptr;
        status.clear();
        hides++;
        if (notify && !passive&&!LocalPicker()&&!LocalContext())
            Emit(L"cancel");
        if (restore && IsWindow(previousFocus))
            SetForegroundWindow(previousFocus);
        closing = false;
        Metrics(L"popup-hidden");
    }
    void Release(bool keepFeedback=false)
    {
        if(!keepFeedback)StopRefreshFeedback();ClearMediaMarquees();
        FinishResize(false,false);FinishFlyoutResize(false);ResetPanelMotion(false);interactionVisual.Release(popupRoot);glassSheet.Release(popupRoot);
        for(auto const &tile:pickerTiles)if(popupRoot)popupRoot.Children().Remove(tile.visual);
        pickerTiles.clear();
        for(auto thumbnail:taskThumbnails)DwmUnregisterThumbnail(thumbnail);
        taskThumbnails.clear();taskPreviewBytes=0;previewThumbnailProps.clear();previewMounted=false;previewPreparing=false;previewSvg.documents.clear();officialPanelIcons.documents.clear();
        if (visual){visual.StopAnimation(L"Opacity");visual.Opacity(1);visual.Brush(nullptr);}

        if(popupRoot)ReleaseNativeGlass(popupRoot,materialVisual);
        surface = nullptr;
        brush = nullptr;
        surfaceBytes = 0;
    }
    void BuildRows()
    {
        kind=CanonicalNativePopupId(kind);
        rows.clear();if(Flyout()){BuildFlyout();return;}if(LocalTooltip())return;if(LocalDockPreview()){BuildDockPreviewRows();return;}
        auto data = Data();
        auto add = [&](std::wstring key, std::wstring label, std::wstring action, D2D1_RECT_F r,
                       std::wstring detail = L"", bool enabled = true, bool active = false) {
            rows.push_back({key, label, detail, action, L"", L"", r, enabled, active, false});
        };
        generic = data.HasKey(L"rows");
        if(LocalContext()){generic=true;BuildContextRows();}
        else if(LocalPicker()){generic=true;BuildPickerRows();}
        else if (generic)
        {
            auto entries = data.GetNamedArray(L"rows");
            int count = std::min(256, static_cast<int>(entries.Size()));
            if(!rangeEdits.empty()){std::set<std::wstring> offeredRanges;for(int i=0;i<count;++i){auto item=entries.GetAt(i).GetObject();if(Str(item,L"control")==L"range")offeredRanges.insert(Str(item,L"id"));}
                std::erase_if(rangeEdits,[&](auto const &edit){return !offeredRanges.contains(edit.first);});}
            if(!FunctionalPanel()&&kind!=L"@magic/user-menu")scroll = std::clamp(scroll, 0, std::max(0, count - visibleRows));
            for (int i = 0; i < count; i++)
            {
                auto item = entries.GetAt(i).GetObject();
                float y = 58 + (i - scroll) * 56.0f;
                auto label = Str(item, L"label");
                if (english) { auto translated = Str(item, L"labelEn"); if (!translated.empty()) label = translated; }
                add(Str(item, L"id"), label, L"native-command", {14, y, width - 14, y + 52},
                    Str(item, L"detail"), Bool(item, L"enabled", true), Bool(item, L"active"));
                auto &row = rows.back();
                row.id = row.key;
                row.control = Str(item, L"control");
                if(kind==L"@magic/power-menu"&&(row.id==L"lock"||row.id==L"sleep"||row.id==L"restart"||row.id==L"shutdown"||row.id==L"settings"||row.id==L"exit"))row.action=row.id;
                // The account card offers the same session actions, but its rows carry
                // no command table entry: the name itself is the receipt, checked
                // against the session's user-menu whitelist. account:* and account-tune
                // keep native-command so the adapter still resolves their local action.
                if(kind==L"@magic/user-menu"&&(row.id==L"account"||row.id==L"settings"||row.id==L"update"||row.id==L"feedback"||row.id==L"about"||row.id==L"restart"||row.id==L"sleep"||row.id==L"shutdown"||row.id==L"lock"||row.id==L"exit"))row.action=row.id;
                row.value = row.control==L"range"?RangeValue(row.id,Num(item,L"value")):Num(item,L"value");
                // The card lays every row out from its own height, so the generic
                // row-window does not decide what is reachable there.
                row.offscreen = FunctionalPanel()?false:kind==L"@magic/user-menu"?false:i < scroll || i >= scroll + visibleRows;
                if (row.control == L"text" || row.control == L"password")
                {
                    if(!hwnd||!visible||dismissing)continue;
                    EnsureInput(row); // LayoutPreview70 places the field at its final bounds.
                }
            }
        }
        else if (kind == L"@magic/keyboard-selector")
        {
            auto layouts = data.GetNamedArray(L"layouts");
            int count = static_cast<int>(layouts.Size());
            scroll = std::clamp(scroll, 0, std::max(0, count - 6));
            for (int i = 0; i < count && i < 256; i++)
            {
                auto item = layouts.GetAt(i).GetObject();
                float y = 58 + (i - scroll) * 62.0f;
                add(L"layout:" + Str(item, L"id") + Str(item, L"handle"), Str(item, L"language"), L"keyboard-select",
                    {14, y, width - 14, y + 58}, Str(item, L"name"), Bool(data,L"inputTargetAvailable",true), Bool(item, L"active"));
                rows.back().id = Str(item, L"id");
                rows.back().handle = Str(item, L"handle");
                rows.back().offscreen = i < scroll || i >= scroll + 6;
            }
            add(L"settings", Ui(L"更多键盘设置", L"Keyboard settings"), L"keyboard-settings",
                {14, height - 47, width - 14, height - 11});
        }
        else if (kind == L"@magic/calendar-popup")
        {
            height=CalendarHeight();ResizeCalendar();
            add(L"previous", L"‹", L"previous", {16, 56, 52, 92});
            add(L"heading", std::to_wstring(year) + Ui(L"年 ", L" / ") + std::to_wstring(month) + Ui(L"月", L""),
                L"mode", {57, 56, width - 57, 92});
            add(L"next", L"›", L"next", {width - 52, 56, width - 16, 92});
            using namespace std::chrono;
            if (yearMode)
            {
                for (int m = 1; m <= 12; m++)
                {
                    float x = 17 + ((m - 1) % 3) * (width - 34) / 3, y = 110 + ((m - 1) / 3) * 62.0f;
                    add(L"month:" + std::to_wstring(m), std::to_wstring(m) + Ui(L"月", L""), L"month",
                        {x, y, x + (width - 34) / 3 - 4, y + 54});
                    rows.back().id = std::to_wstring(m);
                }
            }
            else
            {
                auto first =
                    year_month_day{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)}, day{1}};
                int lead = (static_cast<int>(weekday{sys_days{first}}.c_encoding()) - firstWeekday + 7) % 7;
                int days = static_cast<int>(static_cast<unsigned>(
                    (std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} / last).day()));
                float cell = (width - 32) / 7;
                for (int d = 1; d <= days; d++)
                {
                    int i = lead + d - 1;
                    float x = 16 + (i % 7) * cell, y = 132 + (i / 7) * 42.0f;
                    add(L"day:" + std::to_wstring(year) + L"-" + std::to_wstring(month) + L"-" + std::to_wstring(d),
                        std::to_wstring(d), L"day", {x, y, x + cell - 3, y + 38}, L"", true,
                        d == selectedDay && year == selectedYear && month == selectedMonth);
                    rows.back().id = std::to_wstring(d);
                }
            }
            add(L"today", Ui(L"今天", L"Today"), L"today", {16, 401, 104, 436});
            add(L"settings", Ui(L"日期和时间设置", L"Date and time settings"), L"date-settings",
                {112, 401, width - 16, 436});
        }
        else
        {
            struct Item
            {
                wchar_t const *key, *zh, *en;
                bool divider;
            };
            // Same grouping as the brand menu: account, application, session,
            // exit. Divider rows carry no action so they stay inert.
            Item items[] = {{L"account", L"荣耀账号", L"HONOR account", false},
                            {L"", L"", L"", true},
                            {L"settings", L"设置", L"Settings", false},
                            {L"update", L"检查更新", L"Check for updates", false},
                            {L"feedback", L"问题反馈", L"Feedback", false},
                            {L"about", L"关于", L"About", false},
                            {L"", L"", L"", true},
                            {L"restart", L"重新启动", L"Restart", false},
                            {L"sleep", L"睡眠", L"Sleep", false},
                            {L"shutdown", L"关机", L"Shut down", false},
                            {L"lock", L"锁定", L"Lock", false},
                            {L"", L"", L"", true},
                            {L"exit", L"退出社区版", L"Exit Magic Community", false}};
            float y = 76;
            int divider = 0;
            for (auto const &item : items)
            {
                if (item.divider)
                {
                    add(L"@divider:" + std::to_wstring(++divider), L"", L"", {14, y, width - 14, y + 13}, L"", false);
                    rows.back().control = L"label";
                    y += 13;
                    continue;
                }
                bool enabled = (std::wstring(item.key) != L"account" || Bool(Account(), L"accountAvailable"));
                add(item.key, Ui(item.zh, item.en), item.key, {14, y, width - 14, y + 38}, L"", enabled);
                y += 38;
            }
        }
        LayoutPreview70();
        RequestAppearanceIcons();
        UpdateTaskPreviews();
        for (auto it = edits.begin(); it != edits.end();)
        {
            if (std::none_of(rows.begin(), rows.end(), [&](auto const &row) {
                    return row.id == it->first && (row.control == L"text" || row.control == L"password");
                }))
            {
                SetWindowTextW(it->second, L"");
                DestroyWindow(it->second);
                if(auto host=inputHosts.find(it->first);host!=inputHosts.end()){SetWindowLongPtrW(host->second,GWLP_USERDATA,0);DestroyWindow(host->second);inputHosts.erase(host);}
                it = edits.erase(it);
            }
            else
                ++it;
        }
        if (focus >= static_cast<int>(rows.size()))
            focus = -1;
        if (life && UiaClientsAreListening())
        {
            auto provider = PopupProvider(life);
            UiaRaiseStructureChangedEvent(provider, StructureChangeType_ChildrenInvalidated, nullptr, 0);
            provider->Release();
        }
    }
    void Show(JsonObject const &value)
    {
        auto nextKind = CanonicalNativePopupId(Str(value, L"kind")), nextToken = Str(value, L"token");
        if (visible && kind == nextKind && kind == L"@magic/task-switcher")
        {
            token = nextToken;
            auto args = value.GetNamedObject(L"data").GetNamedObject(L"taskArgs");
            packet = value;
            BuildRows();
            int delta = Str(args, L"direction") == L"previous" ? -1 : 1;
            Key(delta < 0 ? VK_UP : VK_DOWN);
            JsonObject ready;
            Put(ready, L"op", std::wstring(L"popup-shown"));
            Put(ready, L"token", token);
            Put(ready, L"window", static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));
            ready.Insert(L"shown", JsonValue::CreateBooleanValue(true));
            app.Request(ready);
            return;
        }
        // The session adapter owns toggling and receipt completion. A new
        // packet always represents a display request, never a second toggle.
        FinishHide(!dismissing);
        packet = value;
        token = nextToken;
        kind = nextKind;
        // A card opened while the tuning panel already stands inherits the hold with
        // its first packet, before the panel can take focus again. The preferences
        // block is the panel's unsaved copy, so the fresh card is shaped from it.
        if(value.HasKey(L"holdWindow"))holdWindow=reinterpret_cast<HWND>(static_cast<UINT_PTR>(Num(value,L"holdWindow")));
        if(value.HasKey(L"holdPid"))holdPid=static_cast<DWORD>(Num(value,L"holdPid"));
        if(auto preferred=value.TryLookup(L"preferences");preferred&&preferred.ValueType()==JsonValueType::Object)
            app.model.preferences=preferred.GetObject();
        status.clear();
        previewPreparing=LocalDockPreview();previewMounted=false;previewEnterAt=0;
        bluetoothExpanded.clear();networkExpanded.clear();quickDevices=audioAdvanced=false;flyoutExpanded=flyoutDragging=flyoutInFlight=flyoutPending=false;flyoutScroll=0;
        focus = -1;hover=-1;
        scroll = 0;
        yearMode = false;
        auto data = Data();
        passive = kind == L"@magic/flyouts"||LocalTooltip();
        measurement = Bool(data, L"measurement");
        english = Str(data, L"language").rfind(L"en", 0) == 0;
        dark = Bool(data, L"dark",Preview70Theme::systemDark);
        auto week = Str(data, L"startOfWeek");
        firstWeekday = week == L"Sunday" ? 0 : week == L"Saturday" ? 6 : 1;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        year = selectedYear = now.wYear;
        month = selectedMonth = now.wMonth;
        selectedDay = now.wDay;
        generic = data.HasKey(L"rows");
        width = Flyout()?380:FunctionalPanel()?360:LocalContext()?300:LocalPicker()?960:kind==L"@magic/apps-menu"?760:kind==L"@magic/task-switcher"?800:kind==L"@magic/user-menu"?370:kind==L"@magic/quick-settings"?360:kind==L"@magic/power-menu"?380:320;
        height = generic                             ? 74 + std::min(8u, data.GetNamedArray(L"rows").Size()) * 56.0f
                 : kind == L"@magic/calendar-popup" ? CalendarHeight()
                 : kind == L"@magic/user-menu"      ? 528
                                                : 110 + std::min(6u, data.GetNamedArray(L"layouts").Size()) * 62.0f;
        if(kind==L"@magic/user-menu"){
            // Width still travels with the card's own number; its gate is the slider's
            // own 260-560 range, so a value that takes effect inside the panel also
            // survives the next open.
            auto look=app.model.preferences;double cardWidth=Num(look,L"accountCardWidth");
            if(cardWidth>=260&&cardWidth<=560)width=static_cast<float>(cardWidth);
        }
        auto payload = value.GetNamedObject(L"payload"), point = payload.GetNamedObject(L"desiredPosition");
        int ax = static_cast<int>(Num(point, L"x")), ay = static_cast<int>(Num(point, L"y"));
        if (std::abs(static_cast<long long>(ax)) > 65536 || std::abs(static_cast<long long>(ay)) > 65536)
            throw std::runtime_error("Invalid popup anchor");
        previousFocus = GetForegroundWindow();
        if(kind==L"@magic/apps-menu"){MONITORINFO display{sizeof(display)};GetMonitorInfoW(MonitorFromWindow(previousFocus,MONITOR_DEFAULTTOPRIMARY),&display);ax=(display.rcWork.left+display.rcWork.right)/2;ay=(display.rcWork.top+display.rcWork.bottom)/2;}
        ResetIncompatibleHost();
        if (!hwnd)
        {
            WNDCLASSEXW type{sizeof(type)};
            type.hInstance = GetModuleHandleW(nullptr);
            type.style = CS_DBLCLKS; // Only the animated material owns the visible boundary.
            type.lpfnWndProc = Proc;
            type.lpszClassName = L"MagicCompositionPopupWindow";
            type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassExW(&type);
            hwnd = CreateWindowExW(HostStyle(), type.lpszClassName, L"Magic 视界 · 原生弹窗",
                                   WS_POPUP | WS_CLIPCHILDREN, ax, ay, 1, 1, nullptr, nullptr, type.hInstance, this);
            if (!hwnd)
                throw_last_error();
            check_hresult(app.graphics.compositor.as<abiComposition::Desktop::ICompositorDesktopInterop>()
                              ->CreateDesktopWindowTarget(
                                  hwnd, FALSE,
                                  reinterpret_cast<abiComposition::Desktop::IDesktopWindowTarget **>(put_abi(target))));
            popupScene=app.graphics.compositor.CreateContainerVisual();popupRoot=app.graphics.compositor.CreateContainerVisual();
            shade=app.graphics.compositor.CreateSpriteVisual();shade.Brush(app.graphics.compositor.CreateColorBrush({116,0,0,0}));shade.IsVisible(false);
            visual = app.graphics.compositor.CreateSpriteVisual();popupScene.Children().InsertAtBottom(shade);popupScene.Children().InsertAtTop(popupRoot);popupRoot.Children().InsertAtTop(visual);target.Root(popupScene);
            WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        }
        NativeLayerPosition(hwnd, ax, ay, 1, 1, SWP_NOACTIVATE);
        dpi = static_cast<float>(GetDpiForWindow(hwnd));
        float scale = dpi / 96;
        MONITORINFO available{sizeof(available)};
        GetMonitorInfoW(MonitorFromPoint({ax, ay}, MONITOR_DEFAULTTONEAREST), &available);
        if(LocalPicker()){
            float vw=(available.rcMonitor.right-available.rcMonitor.left)/scale,vh=(available.rcMonitor.bottom-available.rcMonitor.top)/scale;
            float top=vh*.14f,bottom=88;
            width=std::min(1440.f,vw*.88f);
            if(vw<=700){top=32;bottom=24;width=vw-24;}
            if(vh<=600){top=18;bottom=18;}
            height=std::max(100.f,vh-top-bottom);
            ax=(available.rcMonitor.left+available.rcMonitor.right)/2;
            ay=available.rcMonitor.top+static_cast<int>(top*scale);
            Put(payload,L"alignX",std::wstring(L"Center"));Put(payload,L"alignY",std::wstring(L"Start"));
        }
        else if(kind==L"@magic/apps-menu"){
            // A keyboard trigger uses the foreground display, never the pointer position.
            MONITORINFO foreground{sizeof(foreground)};GetMonitorInfoW(MonitorFromWindow(previousFocus,MONITOR_DEFAULTTOPRIMARY),&foreground);available=foreground;
            ax=(available.rcWork.left+available.rcWork.right)/2;ay=(available.rcWork.top+available.rcWork.bottom)/2;
            Put(payload,L"alignX",std::wstring(L"Center"));Put(payload,L"alignY",std::wstring(L"Center"));
            if(Bool(data,L"fullscreen")){width=(available.rcWork.right-available.rcWork.left)/scale-24;height=(available.rcWork.bottom-available.rcWork.top)/scale-24;}
        }

        else if(LocalDockPreview()){
            int columns=std::clamp(static_cast<int>(((available.rcWork.right-available.rcWork.left)/scale-16)/264),1,4);auto count=data.GetNamedArray(L"windows").Size();columns=std::min(columns,std::max(1,static_cast<int>(count)));
            width=columns*264.f+8;height=std::min(std::ceil(count/static_cast<float>(columns))*199+8,(available.rcWork.bottom-available.rcWork.top)/scale-20);height=std::min(height,8*1024*1024.f/(width*scale*scale*4));previewScroll=0;
        }
        else if(LocalTooltip()){
            appearanceIcons.Clear();auto preview=Str(data,L"previewPng");if(!preview.empty()&&preview.size()<22000)appearanceIcons.Load(app.graphics,L"attachment",preview);
            tooltipLayout.Build(app.graphics,Str(data,L"text"),(available.rcWork.right-available.rcWork.left)/scale-20,Bool(data,L"oneLine"),appearanceIcons.values.contains(L"attachment"));
            width=tooltipLayout.width;height=tooltipLayout.height;
        }
        else if(LocalContext()){
            contextPreferredWidth=Str(data,L"menuStyle")==L"shell"?240:300;
            if(Str(data,L"menuStyle")==L"toolbar"){contextPreferredWidth=200;for(auto const &v:data.GetNamedArray(L"rows")){auto label=Str(v.GetObject(),L"label");com_ptr<IDWriteTextLayout> layout;auto f=app.graphics.Format(12.8f,false,600);check_hresult(app.graphics.text->CreateTextLayout(label.c_str(),static_cast<UINT32>(label.size()),f,4096,100,layout.put()));DWRITE_TEXT_METRICS metrics{};layout->GetMetrics(&metrics);contextPreferredWidth=std::max(contextPreferredWidth,metrics.widthIncludingTrailingWhitespace+80);}}
            // Measuring and placing are one step now: MeasureAndPlaceContext owns both.
        }
        else if (generic)
        {
            visibleRows =
                passive
                    ? 1
                    : std::clamp(static_cast<int>(((available.rcWork.bottom - available.rcWork.top) / scale - 74) / 56),
                                 2, 8);
            height = passive?130:kind==L"@magic/task-switcher"?450:kind==L"@magic/apps-menu"?520:kind==L"@magic/power-menu"?380:74 + visibleRows * 56.0f;
            if(kind==L"@magic/quick-settings")height=std::min(620.f,(available.rcWork.bottom-available.rcWork.top)/scale-20);
            if(kind==L"@magic/system-tray")height=70+std::ceil(data.GetNamedArray(L"rows").Size()/5.f)*56;
        }
        // The card's rows are laid out from its height, so settle the reader's height
        // before BuildRows runs. Whatever does not fit is scrolled by the layout.
        if(kind==L"@magic/user-menu")
            height=UserMenuHeight(static_cast<float>(available.rcWork.bottom-available.rcWork.top),scale);
        if(generic&&!Flyout()&&!LocalDockPreview()&&!LocalTooltip()&&!LocalPicker()&&!LocalContext()&&kind!=L"@magic/apps-menu"&&!Modal()){
            BuildRows();float bottom=84;for(auto const &row:rows){if(kind==L"@magic/quick-settings"&&(row.id==L"power"||row.id==L"quick-back"||row.id==L"system-power"))continue;if(FunctionalPanel()&&functionalPinned.contains(row.id))continue;bottom=std::max(bottom,row.rect.bottom);}
            if(CenterPanel())bottom=std::max(bottom,functionalExtent);
            height=FunctionalPanel()?FunctionalHeight(available.rcWork,scale):std::min(std::max(130.f,bottom+(kind==L"@magic/quick-settings"?58:18)),(available.rcWork.bottom-available.rcWork.top)/scale-20);
        }
        if(Flyout()){width=std::min(380.f,(available.rcWork.right-available.rcWork.left)/scale-20);flyoutBodyLimit=std::max(40.f,std::min(280.f,(available.rcWork.bottom-available.rcWork.top)/scale-180));BuildFlyout();}
        if(FunctionalPanel()){
            float limit=NativeGlassHeightLimit(width,scale,MaterialTier());
            if(limit>=220)height=std::min(height,limit);
        }
        if(kind==L"@magic/user-menu"){
            // Close-out only: the height was settled from the same contract before
            // BuildRows, so this re-evaluates the identical expression and cannot
            // introduce a different number.
            height=UserMenuHeight(static_cast<float>(available.rcWork.bottom-available.rcWork.top),scale);
        }
        if(!LocalPicker()){
            width=std::min(width,(available.rcWork.right-available.rcWork.left)/scale-20);
            height=std::min(height,(available.rcWork.bottom-available.rcWork.top)/scale-20);
            float fit=std::min(1.f,std::sqrt((8*1024*1024.f)/(width*height*scale*scale*4)));
            if(fit<1){width*=fit*.998f;height*=fit*.998f;}
        }
        if(kind==L"@magic/apps-menu"){width=std::min(824.f,(available.rcWork.right-available.rcWork.left)/scale-24);height=std::min(510.f,(available.rcWork.bottom-available.rcWork.top)/scale-24);if(Bool(data,L"fullscreen")){width=(available.rcWork.right-available.rcWork.left)/scale-24;height=(available.rcWork.bottom-available.rcWork.top)/scale-24;}}
        if(kind==L"@magic/apps-menu"){float fit=std::min(1.f,std::sqrt((8*1024*1024.f)/(width*height*scale*scale*4)));if(fit<1){width*=fit*.998f;height*=fit*.998f;}}
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint({ax, ay}, MONITOR_DEFAULTTONEAREST), &monitor);
        int pw = 0, ph = 0, x = 0, y = 0;
        if(LocalContext()){
            // A cascade level's geometry belongs to one function, shared with the
            // in-place re-layout: measure from this level's own packet, place it from
            // the anchor that opened it, then widen the window by the same transparent
            // gutter the paint uses. Leaving the gutter out is what let the region clip
            // the right and bottom edges after an in-place re-layout.
            auto frame=MeasureAndPlaceContext(monitor.rcWork);
            x=frame.x;y=frame.y;pw=frame.width;ph=frame.height;
        }
        else{
        pw=static_cast<int>(std::ceil(width * scale));ph=static_cast<int>(std::ceil(height * scale));
        auto offset = [](std::wstring const &align, int size) {
            return align == L"Center" ? size / 2 : align == L"End" ? size : 0;
        };
        x = ax - offset(Str(payload, L"alignX"), pw); y = ay - offset(Str(payload, L"alignY"), ph);
        if (pw > monitor.rcWork.right - monitor.rcWork.left || ph > monitor.rcWork.bottom - monitor.rcWork.top)
            throw std::runtime_error("Popup exceeds monitor work area; use compatible popup");
        pw = std::min(pw, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        ph = std::min(ph, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        if(kind==L"@magic/user-menu"){
            // A screen coordinate wins when the reader set one; otherwise the card
            // follows the avatar plus whatever nudge is configured. The clamp below
            // still has the last word, so neither can leave the work area.
            double fixedX=Num(app.model.preferences,L"accountCardX",-1);
            double fixedY=Num(app.model.preferences,L"accountCardY",-1);
            if(fixedX>=0)x=static_cast<int>(fixedX*scale);
            else x+=static_cast<int>(Num(app.model.preferences,L"accountCardOffsetX")*scale);
            if(fixedY>=0)y=static_cast<int>(fixedY*scale);
            else y+=static_cast<int>(Num(app.model.preferences,L"accountCardOffsetY")*scale);
        }
        x = std::clamp(x, static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - pw);
        if(ToolbarDropdown()&&Str(payload,L"alignY")==L"Start")y=DropdownTop(ay,scale);
        y = DropdownClampTop(y,monitor.rcWork,ph);
        centeredAtTrigger=false;motionAnchorX=-1;
        if(ToolbarDropdown()&&(Str(payload,L"alignY")==L"Start"||Str(payload,L"alignY")==L"End")) {auto placement=NativePlacePopup(ax,pw,monitor.rcWork,Str(payload,L"alignX"),scale);x=placement.x;centeredAtTrigger=placement.centered;motionAnchorX=placement.origin;}
        previewOrigin={static_cast<float>((Num(payload,L"previewAnchorX",ax)-x)/scale),Str(payload,L"alignY")==L"End"?height:Str(payload,L"alignY")==L"Start"?0.f:static_cast<float>((Num(payload,L"previewAnchorY",ay)-y)/scale)};
        }
        if(!LocalContext())contentX=contentY=0;
        if(Modal()){
            int fullW=monitor.rcWork.right-monitor.rcWork.left,fullH=monitor.rcWork.bottom-monitor.rcWork.top;
            contentX=(fullW-pw)/2.f/scale;contentY=(fullH-ph)/2.f/scale;
            x=monitor.rcWork.left;y=monitor.rcWork.top;pw=fullW;ph=fullH;
        }
        if(popupRoot)popupRoot.Size({static_cast<float>(pw),static_cast<float>(ph)});if(popupScene)popupScene.Size({static_cast<float>(pw),static_cast<float>(ph)});
        if(shade){shade.Size({static_cast<float>(pw),static_cast<float>(ph)});shade.IsVisible(Modal());}
        visual.Offset({contentX*scale,contentY*scale,0});
        // User32 clips ordinary popups; modal scrims use a color visual instead
        // of allocating a full-screen bitmap.
        HRGN region =
            CreateRoundRectRgn(0, 0, pw + 1, ph + 1, static_cast<int>(2*MaterialRadius() * scale), static_cast<int>(2*MaterialRadius() * scale));
        if(Modal()||AnimatedContext()){DeleteObject(region);SetWindowRgn(hwnd,nullptr,FALSE);}
        else if (!SetWindowRgn(hwnd, region, FALSE))DeleteObject(region);
        NativeLayerPosition(hwnd, x, y, pw, ph, SWP_NOACTIVATE);
        life = std::make_shared<PopupLife>();
        life->owner = this;
        visible = true;
        caption = kind == L"@magic/calendar-popup"      ? Ui(L"日历", L"Calendar")
                  : kind == L"@magic/keyboard-selector" ? Ui(L"键盘布局", L"Keyboard layouts")
                                                         : Ui(L"用户菜单", L"User menu");
        if (!Str(data,L"title").empty())
            caption = Str(data, L"title");
        if(inputFont)DeleteObject(inputFont);
        inputFont=CreateFontW(-static_cast<int>(13*dpi/96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,NativeUiFont::Family().c_str());
        if(inputBackground)DeleteObject(inputBackground);
        auto bg=Preview70Theme{dark}.Raised();inputBackground=CreateSolidBrush(RGB(static_cast<int>(bg.r*255),static_cast<int>(bg.g*255),static_cast<int>(bg.b*255)));
        BuildRows();
        if (kind == L"@magic/task-switcher" && !rows.empty())
            focus = rows.size() > 1 ? 1 : 0;
        if(kind==L"@magic/system-tray"){
            tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(tooltip){TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=hwnd;tool.uId=reinterpret_cast<UINT_PTR>(hwnd);tool.lpszText=LPSTR_TEXTCALLBACKW;
                SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));SendMessageW(tooltip,WM_SETFONT,reinterpret_cast<WPARAM>(inputFont),FALSE);SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,300);}
        }
        if(LocalDockPreview()&&NativeMotion(app.model)&&Bool(data,L"previewAnimate",true))popupRoot.Opacity(0);
        Draw();
        // The card can open without motion; a panel that slides and fades under the
        // cursor is a taste, and the switch has to reach the one control that owns it.
        bool quietCard=kind==L"@magic/user-menu"&&!Bool(app.model.preferences,L"accountCardMotion",true);
        if(quietCard){ResetPanelMotion();if(popupRoot)popupRoot.Opacity(1);if(visual)visual.Opacity(1);}
        else if(!LocalDockPreview())FadeIn();
        // A fixture measures painting/lifetime without borrowing user focus.
        // Production still requires actual foreground activation before acceptance.
        ShowWindow(hwnd, (app.popupCycles || passive || measurement) ? SW_SHOWNOACTIVATE : SW_SHOW);
        if(LocalDockPreview()){previewPreparing=false;if(Bool(data,L"previewAnimate",true))FadeIn();else ResetPanelMotion(false);if(!opening)UpdateTaskPreviews();}
        InvalidateRect(hwnd,nullptr,FALSE); // Rebind hidden-created backdrop after DWM show.
        if (!app.popupCycles && !passive && !measurement)
        {
            SetForegroundWindow(hwnd);
            if(GetForegroundWindow()==hwnd)SetFocus(hwnd);
        }
        shows++;
        if (kind == L"@magic/calendar-popup")
            SetTimer(hwnd, 81, 60000 - now.wSecond * 1000 - now.wMilliseconds, nullptr);
        if (passive)
        {
            if(LocalTooltip())return;
            SetTimer(hwnd, 84, static_cast<UINT>(std::clamp(Num(data, L"duration", 4000), 1000.0, 10000.0)), nullptr);
            Metrics(L"flyout-shown");
            return;
        }
        if (kind == L"@magic/task-switcher" && Bool(data.GetNamedObject(L"taskArgs"), L"autoConfirm"))
            SetTimer(hwnd, 85, 50, nullptr);
        if(!app.popupCycles&&!measurement)FocusReadyInput();
        if(LocalDockPreview()){Metrics(L"dock-preview-shown");return;}
        if(LocalContext()){Metrics(L"context-menu-shown");return;}
        if(LocalPicker()){Metrics(L"card-picker-shown");LoadPickerCatalog();return;}
        JsonObject ready;
        Put(ready, L"op", std::wstring(L"popup-shown"));
        Put(ready, L"token", token);
        Put(ready, L"window", static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));
        HWND foreground=GetForegroundWindow();
        bool activated = app.popupCycles || passive || measurement || foreground == hwnd || InputFamily(foreground);
        ready.Insert(L"shown", JsonValue::CreateBooleanValue(activated));
        auto owner = life;
        app.Request(ready, [owner](JsonObject const &response) {
            if(owner->owner){if(!Bool(response,L"ok"))owner->owner->Hide();
                else{owner->owner->iconReady=true;owner->owner->RequestAppearanceIcons();}}
        });
        if (!activated)
        {
            Hide(false);
            return;
        }
        Metrics(L"popup-shown");
    }
    void Update(JsonObject const &value)
    {
        if((kind==L"@magic/system-tray"||kind==L"@magic/apps-menu")&&pressedTray>=0){if(GetCapture()==hwnd)ReleaseCapture();pressedTray=-1;trayDragging=false;}
        if (!visible || dismissing || Str(value, L"token") != token)
            return;
        // A hold names the window that must not dismiss this popup. It is set before
        // that window can take focus and cleared when the popup or the panel goes away.
        if(value.HasKey(L"holdWindow")||value.HasKey(L"holdPid")){
            if(value.HasKey(L"holdWindow"))holdWindow=reinterpret_cast<HWND>(static_cast<UINT_PTR>(Num(value,L"holdWindow")));
            if(value.HasKey(L"holdPid"))holdPid=static_cast<DWORD>(Num(value,L"holdPid"));
            if(!value.HasKey(L"data"))return;
        }
        // The tuning panel's unsaved copy rides with the card's own refresh, so the
        // card is measured from what the reader just set, not from the last file write.
        if(auto offered=value.TryLookup(L"preferences");offered&&offered.ValueType()==JsonValueType::Object)
            app.model.preferences=offered.GetObject();
        if(!resizePreparing&&TryResizeUpdate(value))return;
        bool previousPower=PowerView(),previousFullscreen=Bool(Data(),L"fullscreen");
        bool enteringAllApps=kind==L"@magic/apps-menu"&&Str(Data(),L"appsView")!=L"all"&&Str(value.GetNamedObject(L"data"),L"appsView")==L"all";
        auto data = value.GetNamedObject(L"data");
        if (!data.HasKey(L"taskArgs") && Data().HasKey(L"taskArgs"))
            data.Insert(L"taskArgs", Data().GetNamedValue(L"taskArgs"));
        packet.Insert(L"data", data);
        bool nextDark=Bool(data,L"dark",dark);if(nextDark!=dark){dark=nextDark;if(inputBackground)DeleteObject(inputBackground);auto bg=Preview70Theme{dark}.Raised();inputBackground=CreateSolidBrush(RGB(static_cast<int>(bg.r*255),static_cast<int>(bg.g*255),static_cast<int>(bg.b*255)));}
        if(!Str(data,L"title").empty())caption=Str(data,L"title");
        if(kind==L"@magic/apps-menu"&&previousFullscreen!=Bool(data,L"fullscreen")){
            MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);float scale=dpi/96;
            width=std::min(Bool(data,L"fullscreen")?100000.f:760.f,(monitor.rcWork.right-monitor.rcWork.left)/scale-20);height=std::min(Bool(data,L"fullscreen")?100000.f:520.f,(monitor.rcWork.bottom-monitor.rcWork.top)/scale-20);
            float fit=std::min(1.f,std::sqrt((8*1024*1024.f)/(width*height*scale*scale*4)));if(fit<1){width*=fit*.998f;height*=fit*.998f;}
            int w=static_cast<int>(std::ceil(width*scale)),h=static_cast<int>(std::ceil(height*scale));NativeLayerPosition(hwnd,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-w)/2,monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-h)/2,w,h,SWP_NOACTIVATE);auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(2*MaterialRadius()*scale),static_cast<int>(2*MaterialRadius()*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);Release();
        }
        // The tuning panel edits the card's shape while the card stands, so the same
        // height contract Show() runs has to run here too. Measure before BuildRows so
        // the row window matches the frame that is about to be painted.
        if(kind==L"@magic/user-menu"){
            MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);float scale=dpi/96;
            double cardWidth=Num(app.model.preferences,L"accountCardWidth");
            if(cardWidth>=260&&cardWidth<=560)width=static_cast<float>(cardWidth);
            height=UserMenuHeight(static_cast<float>(monitor.rcWork.bottom-monitor.rcWork.top),scale);
        }
        auto focused = focus >= 0 && focus < static_cast<int>(rows.size()) ? rows[focus].key : L"";
        BuildRows();
        if(FunctionalPanel()||kind==L"@magic/system-tray"){
            MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);float scale=dpi/96;
            float next=FunctionalHeight(monitor.rcWork,scale);
            if(std::abs(next-height)>.5f){height=next;RECT bounds{};GetWindowRect(hwnd,&bounds);int w=static_cast<int>(std::ceil(width*scale)),h=static_cast<int>(std::ceil(height*scale));
                int y=UpdatedPanelTop(bounds,monitor.rcWork,h,scale);NativeLayerPosition(hwnd,bounds.left,y,w,h,SWP_NOACTIVATE);
                if(popupRoot)popupRoot.Size({static_cast<float>(w),static_cast<float>(h)});if(popupScene)popupScene.Size({static_cast<float>(w),static_cast<float>(h)});auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(32*scale),static_cast<int>(32*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);Release(true);BuildRows();}
        }
        // Close-out with the laid-out content: the card's rows are laid out from its
        // height, so the frame follows the content it actually needed.
        if(kind==L"@magic/user-menu")ReframeCard();
        for (int i = 0; i < static_cast<int>(rows.size()); i++)
            if (rows[i].key == focused)
                focus = i;
        Draw();
        if(enteringAllApps){
            auto field=edits.find(L"search");
            if(field!=edits.end()&&GetWindowTextLengthW(field->second)>0)SetWindowTextW(field->second,L"");
            FocusReadyInput();
        }
        if(kind==L"@magic/quick-settings"&&previousPower!=PowerView())FadeIn();
    }
    JsonArray AppMenuRows(int index);
    void AppMenu(int index,POINT point);
    void CloseAppMenu();
    void Command(PopupRow const &row, double value = -1, int button = 0,std::wstring destination=L"")
    {
        if((kind==L"@magic/bluetooth-popup"&&row.id==L"bt-discover"||kind==L"@magic/network-popup"&&row.id.starts_with(L"scan:"))&&!StartRefreshFeedback(row))return;
        if(Flyout()&&row.control==L"range"){SetFlyoutValue(value<0?row.value:value);SendFlyoutValue();return;}
        if(AdjustableRange(row)){SetRangeValue(row.id,value<0?row.value:value);SendRangeValue(true);return;}
        if(row.control==L"toggle"&&value<0)value=row.value>.5?0:1;
        if(auto host=static_cast<DWORD>(Num(Data(),L"hostPid")))AllowSetForegroundWindow(host);
        if(kind==L"@magic/system-tray"&&button==2){
            for(auto const &v:Data().GetNamedArray(L"rows")){auto item=v.GetObject();if(Str(item,L"id")!=row.id)continue;
                POINT point{};GetCursorPos(&point);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
                bool above=point.y>(monitor.rcWork.top+monitor.rcWork.bottom)/2;
                TrayMenuAnchor().Arm(reinterpret_cast<HWND>(static_cast<UINT_PTR>(Num(item,L"ownerHwnd"))),point,above,{point.x,point.y,point.x,point.y});break;
            }
        }
        if(kind==L"@magic/apps-menu"&&(row.id==L"apps-back"||row.id==L"apps-next"||row.id==L"apps-previous"))scroll=0;
        if(kind==L"@magic/apps-menu"&&row.id==L"apps-view"){scroll=0;if(edits.contains(L"search")){closing=true;SetWindowTextW(edits[L"search"],L"");closing=false;KillTimer(hwnd,83);}}
        JsonObject result;
        Put(result, L"action", std::wstring(L"native-command"));
        Put(result, L"id", row.id);
        Put(result, L"value", value < 0 ? row.value : value);
        Put(result, L"button", static_cast<double>(button));
        Put(result,L"destination",destination);
        if(kind==L"@magic/apps-menu"&&button==0&&row.id.rfind(L"app:",0)==0){for(auto const &v:Data().GetNamedArray(L"rows")){auto value=v.GetObject();if(Str(value,L"id")==row.id&&Bool(value,L"folder")){scroll=0;break;}}}
        JsonObject fields;
        for (auto const &[id, edit] : edits)
        {
            auto separator = row.id.find(L':');
            bool notification = row.id.rfind(L"action:", 0) == 0 || row.id.rfind(L"notification:", 0) == 0;
            if (notification)
            {
                auto end = row.id.find(L':', separator + 1);
                auto number = row.id.substr(separator + 1, end == std::wstring::npos ? end : end - separator - 1);
                if (id.rfind(L"input:" + number + L":", 0) != 0)
                    continue;
            }
            else if (id != L"password" && id != L"username" && id != L"pin" && id != L"search" && id!=L"folder-name")
                continue;
            int n = std::min(1024, GetWindowTextLengthW(edit));
            std::wstring text(n + 1, L'\0');
            GetWindowTextW(edit, text.data(), n + 1);
            text.resize(n);
            Put(fields, id.c_str(), text);
        }
        result.Insert(L"fields", fields);
        JsonObject request;
        Put(request, L"op", std::wstring(L"popup-command"));
        Put(request, L"token", token);
        request.Insert(L"result", result);
        app.Request(request);
    }
    void Text(ID2D1DeviceContext *c, std::wstring const &value, D2D1_RECT_F r, float size, bool center = false,
              bool muted = false)
    {
        brush->SetColor(muted?Preview70Theme{dark}.Gray(700):Preview70Theme{dark}.Ink());
        c->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), app.graphics.Format(size, center), r,
                     brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    virtual void Draw()
    {
        if(flyoutResize||resizeMotion)return;
        NativeRenderScope rendering(app.relay);if(!rendering){if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);return;}
        if (!visible || dismissing || !hwnd)
            return;
        SyncRefreshFeedback();
        SyncMediaMarquees();
        if(interactionVisual.root&&interactionVisual.active)SyncInteraction(Hit(interactionPoint.x+contentX,interactionPoint.y+contentY));
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        int w=(Modal()||Flyout()||AnimatedContext())?static_cast<int>(std::ceil(width*dpi/96)):bounds.right,h=(Modal()||Flyout()||AnimatedContext())?static_cast<int>(std::ceil(height*dpi/96)):bounds.bottom;
        auto r=MaterialBounds();r.left+=contentX;r.right+=contentX;r.top+=contentY;r.bottom+=contentY;
        bool hadMaterial=static_cast<bool>(materialVisual);
        bool large=(LocalPicker()||LocalContext()||kind==L"@magic/apps-menu")&&static_cast<size_t>(w)*h*4>3*1024*1024;
        if(large){ReleaseNativeGlass(popupRoot,materialVisual);size_t content=static_cast<size_t>(w)*h*4;glassSheet.Sync(app.graphics,hwnd,popupRoot,r,dpi/96,MaterialRadius(),MaterialTier(),!NativeLowPower(app.model),visual,content<64*1024*1024?std::min<size_t>(32*1024*1024,64*1024*1024-content):0);}
        else{glassSheet.Release(popupRoot);SyncNativeGlass(app.graphics,hwnd,popupRoot,materialVisual,r,dpi/96,MaterialRadius(),MaterialTier(),!NativeLowPower(app.model),visual);}
        if(materialVisual&&!hadMaterial){auto opacity=app.graphics.compositor.CreateExpressionAnimation(L"content.Opacity");opacity.SetReferenceParameter(L"content",visual);materialVisual.StartAnimation(L"Opacity",opacity);}
        size_t bytes=static_cast<size_t>(w)*h*4;
        if((LocalPicker()||LocalContext())&&bytes>8*1024*1024){
            if(bytes>64*1024*1024)throw std::runtime_error("Transient sheet exceeds visible surface budget");
            visual.Brush(nullptr);surface=nullptr;
            int stripe=std::max(1,static_cast<int>((8*1024*1024)/(static_cast<size_t>(w)*4)));
            size_t count=(h+stripe-1)/stripe;
            while(pickerTiles.size()>count){popupRoot.Children().Remove(pickerTiles.back().visual);pickerTiles.pop_back();}
            while(pickerTiles.size()<count){PickerTile tile;tile.visual=app.graphics.compositor.CreateSpriteVisual();popupRoot.Children().InsertAtTop(tile.visual);pickerTiles.push_back(std::move(tile));}
            for(size_t i=0;i<count;i++){
                auto &tile=pickerTiles[i];int y=static_cast<int>(i)*stripe,th=std::min(stripe,h-y);
                tile.visual.Offset({contentX*dpi/96,contentY*dpi/96+y,0});tile.visual.Size({static_cast<float>(w),static_cast<float>(th)});
                if(!tile.surface||tile.width!=w||tile.height!=th){tile.surface=app.graphics.composition.CreateDrawingSurface({static_cast<float>(w),static_cast<float>(th)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);tile.visual.Brush(app.graphics.compositor.CreateSurfaceBrush(tile.surface));tile.width=w;tile.height=th;}
                auto interop=tile.surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};
                check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));
                try{c->SetDpi(dpi,dpi);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x*96/dpi,(offset.y-y)*96/dpi));c->Clear(D2D1::ColorF(0,0));PaintPreview70(c.get());}catch(...){interop->EndDraw();throw;}
                check_hresult(interop->EndDraw());
            }
            surfaceBytes=bytes;++redraws;return;
        }
        for(auto const &tile:pickerTiles)popupRoot.Children().Remove(tile.visual);pickerTiles.clear();
        if (w < 1 || h < 1 || bytes > 8 * 1024 * 1024)
            throw std::runtime_error("Popup surface exceeds 8 MiB bound");
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
            HRESULT hr = interop->EndDraw();
            ended = true;
            check_hresult(hr);
            ++redraws;
        }
        catch (...)
        {
            if (!ended)
                interop->EndDraw();
            throw;
        }
    }
    virtual void Activate(int index)
    {
        if(dismissing||LocalDockPreview()&&opening)return;
        if (app.popupCycles)
        {
            if (!app.windows.empty())
                app.windows[0]->CancelBenchmark();
            return;
        }
        if (index < 0 || index >= static_cast<int>(rows.size()) || !rows[index].enabled || rows[index].offscreen)
            return;
        if (Flyout()){ActivateFlyout(rows[index]);return;}
        if (passive)
        {
            if (passiveClick)
                passiveClick();
            Hide(false);
            return;
        }
        auto row = rows[index];
        auto action = row.action;
        if(LocalContext()){
            auto action=contextAction;if(action)action(row.id);return;
        }
        if(LocalPicker()){PickerActivate(row);return;}
        if(row.action==L"quick-devices"){
            quickDevices=!quickDevices;scroll=0;focus=hover=-1;interactionVisual.Release(popupRoot);RelayoutDisclosure();return;
        }
        if(kind==L"@magic/bluetooth-popup"&&row.id.rfind(L"device:",0)==0){
            bluetoothExpanded=bluetoothExpanded==row.id?L"":row.id;
            RelayoutDisclosure();return;
        }
        if(row.id.starts_with(L"wifi-group:")){networkExpanded=networkExpanded==row.id?L"":row.id;RelayoutDisclosure();return;}
        if(row.action==L"audio-advanced"){audioAdvanced=!audioAdvanced;RelayoutDisclosure();return;}
        // Only a row whose action is the opaque native-command token travels the
        // command table. power-menu and user-menu rows name their action directly and
        // fall through to the confirmation/emit path below.
        if (generic && action == L"native-command")
        {
            if (row.control != L"text" && row.control != L"password" && row.control != L"range")
                Command(row);
            return;
        }
        if (action == L"previous" || action == L"next")
        {
            int delta = action == L"next" ? 1 : -1;
            if (yearMode)
                year = std::clamp(year + delta, 1601, 9998);
            else
            {
                month += delta;
                if (month < 1)
                {
                    month = 12;
                    year--;
                }
                if (month > 12)
                {
                    month = 1;
                    year++;
                }
                year = std::clamp(year, 1601, 9998);
            }
            BuildRows();
            Draw();
            return;
        }
        if (action == L"mode")
        {
            yearMode = !yearMode;
            BuildRows();
            Draw();
            return;
        }
        if (action == L"month")
        {
            month = _wtoi(row.id.c_str());
            yearMode = false;
            BuildRows();
            Draw();
            return;
        }
        if (action == L"day")
        {
            selectedYear = year;
            selectedMonth = month;
            selectedDay = _wtoi(row.id.c_str());
            BuildRows();
            Draw();
            return;
        }
        if (action == L"today")
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            year = selectedYear = now.wYear;
            month = selectedMonth = now.wMonth;
            selectedDay = now.wDay;
            yearMode = false;
            BuildRows();
            Draw();
            return;
        }
        bool confirm = action == L"restart" || action == L"shutdown";
        if (confirm && (armed != action || !ConfirmationSeconds()))
        {
            armed = action;
            armedUntil = GetTickCount64() + 3000;
            SetTimer(hwnd, 82, 1000, nullptr);
            Draw();
            return;
        }
        JsonObject result;
        Put(result, L"id", row.id);
        Put(result, L"handle", row.handle);
        result.Insert(L"confirmed", JsonValue::CreateBooleanValue(confirm));
        auto currentToken = token;
        bool opensSettings=action==L"settings"||action==L"about"||action==L"update";
        if(opensSettings||action==L"keyboard-select")AllowSetForegroundWindow(app.parent);
        Hide(false, !opensSettings);
        token = currentToken;
        Emit(action, result);
    }
    virtual D2D1_RECT_F VisualRowBounds(int index)const{auto r=InteractionRegion(index).Visible();if(r.right<r.left||r.bottom<r.top)return {};return r;}
    NativeInteractionRegion InteractionRegion(int index)const{
        if(index<0||index>=static_cast<int>(rows.size()))return {};auto const &r=rows[index];
        D2D1_RECT_F viewport{6,6,width-6,height-6};
        if(LocalPicker()&&r.action==L"pick")viewport={18,100,width*.68f-12,height-44};
        else if(FunctionalPanel()&&!functionalPinned.contains(r.id))viewport={14,functionalTop,width-14,functionalBottom};
        else if(!Flyout()&&!LocalContext()&&!LocalPicker()&&!LocalDockPreview()&&generic&&(r.rect.top>=ContentTop()||kind==L"@magic/user-menu"))viewport={14,ContentTop(),width-14,height-16};
        bool enabled=r.enabled&&!r.offscreen&&r.control!=L"label"&&r.control!=L"separator";
        return NativeRegion(r.rect,viewport,r.action==L"pick"||r.control==L"tray"?NativeMetrics::PanelRadius:NativeMetrics::ControlRadius,enabled);
    }
    void SyncInteraction(int index){auto region=kind==L"@magic/system-tray"?NativeInteractionRegion{}:InteractionRegion(index);if(Modal()||AnimatedContext()){region.bounds.left+=contentX;region.bounds.right+=contentX;region.bounds.top+=contentY;region.bounds.bottom+=contentY;region.viewport.left+=contentX;region.viewport.right+=contentX;region.viewport.top+=contentY;region.viewport.bottom+=contentY;}
        interactionVisual.Sync(app.graphics.compositor,popupRoot,region,{interactionPoint.x+contentX,interactionPoint.y+contentY},dpi/96,NativeMotion(app.model),dark);
    }
    virtual int Hit(float x, float y) const
    {
        x-=contentX;y-=contentY;
        if(FunctionalPanel()){
            for(int i=static_cast<int>(rows.size())-1;i>=0;--i)if(!rows[i].offscreen&&InteractionRegion(i).Contains(x,y))return i;
            return -1;
        }
        for (int i=static_cast<int>(rows.size())-1;i>=0;i--)
            if ((!generic||Flyout()||LocalDockPreview()||LocalPicker()||LocalContext()||y>=ContentTop()&&y<=height-18||rows[i].id==L"search"||rows[i].id==L"power-refresh")&&(!LocalPicker()||rows[i].action!=L"pick"||y>=100&&y<=height-44)&&!rows[i].offscreen && InteractionRegion(i).Contains(x,y))
                return i;
        return -1;
    }
    void Key(UINT key)
    {
        if (app.popupCycles)
        {
            if (!app.windows.empty())
                app.windows[0]->CancelBenchmark();
            return;
        }
        if(Flyout()&&flyoutExpanded&&(key==VK_PRIOR||key==VK_NEXT)){flyoutScroll=std::clamp(flyoutScroll+(key==VK_PRIOR?-200.f:200.f),0.f,std::max(0.f,flyoutFullHeight-(flyoutBody.bottom-flyoutBody.top)));Draw();return;}
        if(Flyout()&&focus>=0&&focus<static_cast<int>(rows.size())&&rows[focus].control==L"range"&&(key==VK_LEFT||key==VK_RIGHT||key==VK_HOME||key==VK_END)){SetFlyoutValue(key==VK_HOME?0:key==VK_END?1:rows[focus].value+(key==VK_LEFT?-.01:.01));SendFlyoutValue();return;}
        if(focus>=0&&focus<static_cast<int>(rows.size())&&AdjustableRange(rows[focus])&&(key==VK_LEFT||key==VK_RIGHT||key==VK_HOME||key==VK_END)){auto row=rows[focus];SetRangeValue(row.id,key==VK_HOME?0:key==VK_END?1:row.value+(key==VK_LEFT?-.01:.01));return;}
        if(kind==L"@magic/system-tray"&&key=='P'&&(GetKeyState(VK_CONTROL)&0x8000)&&focus>=0&&focus<static_cast<int>(rows.size())){Command(rows[focus],-1,4);return;}
        if(LocalDockPreview()&&(key==VK_UP||key==VK_DOWN||key==VK_LEFT||key==VK_RIGHT||key==VK_TAB||key==VK_HOME||key==VK_END)){
            int count=static_cast<int>(rows.size());if(!count)return;int columns=std::max(1,static_cast<int>((width-8)/264));int step=key==VK_UP?-columns*2:key==VK_DOWN?columns*2:key==VK_LEFT?-2:key==VK_RIGHT?2:((GetKeyState(VK_SHIFT)&0x8000)?-1:1);
            focus=key==VK_HOME?0:key==VK_END?count-1:std::clamp(focus<0?0:focus+step,0,count-1);auto rect=rows[focus].rect;
            if(rect.top<8)previewScroll+=rect.top-8;else if(rect.bottom>height-8)previewScroll+=rect.bottom-height+8;BuildRows();Draw();return;
        }
        if(LocalContext()&&(key==VK_LEFT||key==VK_BACK)){
            auto back=contextBack;if(back)back();return;
        }
        if(LocalContext()&&key==VK_RIGHT){if(focus>=0&&focus<static_cast<int>(rows.size())&&rows[focus].control==L"submenu")Activate(focus);return;}
        if (key == VK_ESCAPE)
        {
            if(LocalContext()&&contextBack){auto back=contextBack;back();return;}
            Hide(true, true);
            return;
        }
        if(kind==L"@magic/apps-menu"&&key==VK_RETURN&&edits.contains(L"search")&&GetFocus()==edits[L"search"]){auto row=std::find_if(rows.begin(),rows.end(),[](auto const&r){return r.id==L"search";});if(row!=rows.end())Command(*row,-1,20);return;}
        if (key == VK_RETURN || key == VK_SPACE)
        {
            Activate(focus);
            return;
        }
        int step = key == VK_UP      ? -1
                   : key == VK_DOWN  ? 1
                   : key == VK_LEFT  ? -1
                   : key == VK_RIGHT ? 1
                   : key == VK_TAB   ? ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)
                                     : 0;
        if (!step && key != VK_HOME && key != VK_END)
            return;
        int count = static_cast<int>(rows.size());
        if (count == 0)
            return;
        int next = key == VK_HOME ? 0 : key == VK_END ? count - 1 : focus;
        if (next < 0)
            next = step < 0 ? 0 : -1;
        for (int n = 0; n < count; n++)
        {
            if (step)
                next = (next + step + count) % count;
            if (rows[next].enabled&&(!CenterPanel()||rows[next].rect.right>0))
            {
                focus = next;
                break;
            }
            if (!step)
                next = (next + (key == VK_END ? -1 : 1) + count) % count;
        }
        if(FunctionalPanel()&&focus>=0&&!functionalPinned.contains(rows[focus].id)){
            auto r=rows[focus].rect;if(r.top<functionalTop)scroll=std::max(0,scroll-static_cast<int>(std::ceil((functionalTop-r.top)/56)));
            else if(r.bottom>functionalBottom)scroll+=static_cast<int>(std::ceil((r.bottom-functionalBottom)/56));BuildRows();
        }else if(LocalContext()&&focus>=0){
            // Every row is visible in its column; keyboard focus never scrolls.
        }else if (!CenterPanel() && (generic || kind == L"@magic/keyboard-selector") && focus >= 0 &&
            (generic || rows[focus].action == L"keyboard-select"))
        {
            // The card's rows are 30-52 DIP tall, so its window is not visibleRows of
            // 56; LayoutPreview70 clamps scroll against the real content instead.
            if(kind!=L"@magic/user-menu"){
                if (focus < scroll)
                    scroll = focus;
                if (focus >= scroll + (generic ? visibleRows : 6))
                    scroll = focus - (generic ? visibleRows : 6) + 1;
            }else if(focus<scroll)scroll=focus;
            else if(rows[focus].rect.bottom>height-18)scroll=focus;
            BuildRows();
        }
        Draw();
        if (UiaClientsAreListening())
        {
            auto provider = PopupProvider(life, focus >= 0 ? rows[focus].key : L"");
            UiaRaiseAutomationEvent(provider, UIA_AutomationFocusChangedEventId);
            provider->Release();
        }
    }
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        auto self = reinterpret_cast<NativePopup *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<NativePopup *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
        }
        if (!self)
            return DefWindowProcW(window, message, w, l);
        try
        {
            if(self->dismissing&&((message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||message==WM_KEYDOWN||message==WM_CHAR))return 0;
            if((message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_KEYDOWN||message==WM_MOUSEWHEEL)&&self->MotionPanel()&&!self->LocalDockPreview()){
                bool entering=self->opening;self->ResetPanelMotion();
                if(entering&&message==WM_KEYDOWN){self->FocusReadyInput();auto edit=GetFocus();if(edit!=window&&(IsChild(window,edit)||self->InputFamily(edit))&&w!=VK_RETURN&&w!=VK_ESCAPE&&w!=VK_TAB){SendMessageW(edit,message,w,l);return 0;}}
            }
            if(self->flyoutResize&&(message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_KEYDOWN||message==WM_MOUSEWHEEL)){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(message!=WM_KEYDOWN&&message!=WM_MOUSEWHEEL)ClientToScreen(window,&point);self->FinishFlyoutResize();if(message!=WM_KEYDOWN&&message!=WM_MOUSEWHEEL){ScreenToClient(window,&point);l=MAKELPARAM(point.x,point.y);}}
            if(self->resizeMotion&&(message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_RBUTTONDOWN||message==WM_RBUTTONUP||message==WM_KEYDOWN||message==WM_MOUSEWHEEL)){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};bool client=message!=WM_KEYDOWN&&message!=WM_MOUSEWHEEL;if(client)ClientToScreen(window,&point);self->FinishResize();if(client){ScreenToClient(window,&point);l=MAKELPARAM(point.x,point.y);}}
            if(self->FlyoutPointer(message,w,l))return 0;
            if(self->RangePointer(message,w,l))return 0;
            switch (message)
            {
            case WM_NCHITTEST:if(self->dismissing||self->LocalDockPreview()&&self->opening)return HTTRANSPARENT;
                if(self->AnimatedContext()){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(window,&point);float x=point.x*96/self->dpi-self->contentX,y=point.y*96/self->dpi-self->contentY;if(!NativeContextMotion::Contains({x,y},{self->width,self->height},self->MaterialRadius()))return HTTRANSPARENT;}break;
            case WM_CHAR:{auto edit=GetFocus();if(edit!=window&&(IsChild(window,edit)||self->InputFamily(edit))){SendMessageW(edit,message,w,l);return 0;}break;}
            case WM_LBUTTONDOWN:
                if(self->LocalContext()&&self->ContextOverflows()&&GET_X_LPARAM(l)*96/self->dpi-self->contentX>=self->width-14){self->contextDragging=true;self->contextDragY=GET_Y_LPARAM(l)*96/self->dpi-self->contentY;self->contextDragStart=self->contextScroll;SetCapture(window);return 0;}
                self->app.inputEpoch++;
                if(self->kind==L"@magic/system-tray"||self->kind==L"@magic/apps-menu"){
                    self->pressedTray=self->Hit(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);self->trayPress={GET_X_LPARAM(l),GET_Y_LPARAM(l)};self->trayDragging=false;SetCapture(window);
                }
                break;
            case WM_CAPTURECHANGED:self->pressedTray=-1;self->trayDragging=false;self->contextDragging=false;return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                self->Draw();
                return 0;
            }
            case WM_KEYDOWN:
                self->Key(static_cast<UINT>(w));
                return 0;
            case WM_LBUTTONUP: {
                if(self->contextDragging){self->contextDragging=false;if(GetCapture()==window)ReleaseCapture();self->Draw();return 0;}
                int index = self->Hit(GET_X_LPARAM(l) * 96 / self->dpi, GET_Y_LPARAM(l) * 96 / self->dpi);
                if(self->kind==L"@magic/apps-menu"){
                    int from=self->pressedTray;bool dragged=self->trayDragging;self->pressedTray=-1;self->trayDragging=false;if(GetCapture()==window)ReleaseCapture();
                    if(from<0)return 0;
                    if(dragged)return 0;
                }
                if(self->kind==L"@magic/system-tray"){
                    int from=self->pressedTray;bool dragged=self->trayDragging;self->pressedTray=-1;self->trayDragging=false;if(GetCapture()==window)ReleaseCapture();
                    if(from<0)return 0;
                    if(from<0)return 0;
                    if(dragged){if(Str(self->Data(),L"appsView")!=L"all"&&from>=0&&index>=0&&from!=index&&from<static_cast<int>(self->rows.size())){
                        JsonObject r,command;Put(r,L"action",std::wstring(L"native-command"));Put(r,L"id",self->rows[from].id);Put(r,L"button",5.0);Put(r,L"destination",self->rows[index].id);
                        Put(command,L"op",std::wstring(L"popup-command"));Put(command,L"token",self->token);command.Insert(L"result",r);self->app.Request(command);
                    }return 0;}
                    if(index>=0){auto row=self->rows[index];float x=GET_X_LPARAM(l)*96/self->dpi,y=GET_Y_LPARAM(l)*96/self->dpi;
                        if(x>=row.rect.right-16&&y<row.rect.top+16){self->Command(row,-1,4);return 0;}}
                }
                if (self->generic && index >= 0 && self->rows[index].enabled && self->rows[index].control == L"range")
                {
                    auto row = self->rows[index];
                    double x = GET_X_LPARAM(l) * 96 / self->dpi;
                    auto track=self->RangeTrack(row);
                    self->Command(row,std::clamp((x-self->contentX-track.left)/(track.right-track.left),0.0,1.0));
                }
                else if(index<0&&self->Modal()&&!Inside({10,10,self->width-10,self->height-10},GET_X_LPARAM(l)*96/self->dpi-self->contentX,GET_Y_LPARAM(l)*96/self->dpi-self->contentY))self->Hide(true,true);
                else
                    self->Activate(index);
            }
                return 0;
            case WM_NOTIFY: {
                auto header=reinterpret_cast<NMHDR *>(l);
                if(header->hwndFrom==self->tooltip&&header->code==TTN_GETDISPINFOW){auto info=reinterpret_cast<NMTTDISPINFOW *>(l);
                    self->tooltipText=self->hover>=0&&self->hover<static_cast<int>(self->rows.size())?self->rows[self->hover].label:L"";
                    info->lpszText=self->tooltipText.data();return 0;}
                break;
            }
            case WM_MOUSEMOVE: {
                if(self->LocalContext()){
                    float y=GET_Y_LPARAM(l)*96/self->dpi-self->contentY;
                    bool hoverBand=GET_X_LPARAM(l)*96/self->dpi-self->contentX>=self->width-14;
                    if(hoverBand!=self->contextScrollbarHover){self->contextScrollbarHover=hoverBand;self->Draw();}
                    if(self->contextDragging){float view=self->ContextViewport(),thumb=std::max(24.f,view*view/self->contextTotal);self->contextScroll=self->contextDragStart+(y-self->contextDragY)*(self->contextTotal-view)/std::max(1.f,view-thumb);self->BuildRows();self->Draw();return 0;}
                }
                if(self->pressedTray>=0&&(w&MK_LBUTTON)&&
                    (std::abs(GET_X_LPARAM(l)-self->trayPress.x)>=GetSystemMetrics(SM_CXDRAG)||std::abs(GET_Y_LPARAM(l)-self->trayPress.y)>=GetSystemMetrics(SM_CYDRAG)))self->trayDragging=true;
                int next=self->Hit(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi);
                self->interactionPoint={GET_X_LPARAM(l)*96/self->dpi-self->contentX,GET_Y_LPARAM(l)*96/self->dpi-self->contentY};self->SyncInteraction(next);
                if(next!=self->hover){KillTimer(window,96);self->hover=next;if(self->LocalContext()&&next>=0&&self->contextPrepare&&self->rows[next].control==L"submenu")SetTimer(window,96,100,nullptr);if(self->tooltip)SendMessageW(self->tooltip,TTM_POP,0,0);self->Draw();if(self->LocalContext()&&next>=0&&self->contextHover){auto callback=self->contextHover;callback(self->rows[next].id);}}
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);return 0;
            }
            case WM_MOUSELEAVE:KillTimer(window,96);self->hover=-1;self->SyncInteraction(-1);self->contextScrollbarHover=false;self->Draw();return 0;
            case WM_CTLCOLOREDIT: {
                auto color=Preview70Theme{self->dark}.Ink(),bg=Preview70Theme{self->dark}.Raised();
                SetBkMode(reinterpret_cast<HDC>(w),OPAQUE);
                SetTextColor(reinterpret_cast<HDC>(w),RGB(static_cast<int>(color.r*255),static_cast<int>(color.g*255),static_cast<int>(color.b*255)));
                SetBkColor(reinterpret_cast<HDC>(w),RGB(static_cast<int>(bg.r*255),static_cast<int>(bg.g*255),static_cast<int>(bg.b*255)));
                return reinterpret_cast<LRESULT>(self->inputBackground);
            }
            case WM_COMMAND:
                if(!l&&(LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)){self->Key(LOWORD(w)==IDOK?VK_RETURN:VK_ESCAPE);return 0;}
                if (self->visible && !self->closing && HIWORD(w) == EN_CHANGE)
                {
                    SetTimer(window, 83, 120, nullptr);
                    self->changedEdit=reinterpret_cast<HWND>(l);
                    return 0;
                }
                break;
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                if(self->LocalDockPreview()){if(message==WM_MBUTTONUP)self->MiddleActivate(self->Hit(GET_X_LPARAM(l)*96/self->dpi,GET_Y_LPARAM(l)*96/self->dpi));return 0;}
            case WM_LBUTTONDBLCLK: {
                int index = self->Hit(GET_X_LPARAM(l) * 96 / self->dpi, GET_Y_LPARAM(l) * 96 / self->dpi);
                if(self->kind==L"@magic/apps-menu"&&message==WM_RBUTTONUP){POINT p{};GetCursorPos(&p);self->AppMenu(index,p);return 0;}
                if(self->LocalPicker()&&message==WM_RBUTTONUP){POINT p{};GetCursorPos(&p);self->PickerMenu(index,p);return 0;}
                if (index >= 0 && self->rows[index].control == L"tray")
                    self->Command(self->rows[index], -1, message == WM_RBUTTONUP ? 2 : message == WM_MBUTTONUP ? 1 : 3);
                return 0;
            }
            case WM_SYSKEYUP:
                if (w == VK_MENU && self->kind == L"@magic/task-switcher")
                {
                    auto args = self->Data().GetNamedObject(L"taskArgs");
                    if (Bool(args, L"autoConfirm"))
                    {
                        self->Activate(self->focus);
                        self->Hide(true, true);
                        return 0;
                    }
                }
                break;
            case WM_SYSKEYDOWN:
                if (w == VK_TAB && self->kind == L"@magic/task-switcher")
                {
                    self->Key((GetKeyState(VK_SHIFT) & 0x8000) ? VK_UP : VK_DOWN);
                    return 0;
                }
                break;
            case WM_MOUSEWHEEL:
                if(self->LocalDockPreview()){self->previewScroll-=GET_WHEEL_DELTA_WPARAM(w)/120.f*96;self->BuildRows();self->Draw();return 0;}
                if(self->LocalContext()){
                    if(self->ContextOverflows()){
                        float step=GET_WHEEL_DELTA_WPARAM(w)>0?-1.f:1.f;
                        self->contextScroll=std::clamp(self->contextScroll+step*36.f,0.f,std::max(0.f,self->contextTotal-self->ContextViewport()));
                        self->BuildRows();self->Draw();
                    }
                    return 0;
                }
                if (self->generic || self->kind == L"@magic/keyboard-selector")
                {
                    self->scroll += (GET_WHEEL_DELTA_WPARAM(w)>0?-1:1)*(self->LocalPicker()?self->PickerColumns():1);
                    self->BuildRows();
                    self->Draw();
                }
                return 0;
            case WM_ACTIVATE: {
                // A standing hold means the tuning panel borrowed the foreground, so
                // losing focus is not the reader leaving. Identifying the focus window
                // is not an option: on 2026-11-10 17:26:22.081 the card held
                // holdPid=26116 / holdWindow=0000000000282CA8, yet this message carried
                // focusPid=0, so the panel-process test never matched and the card hid
                // under the reader's slider. Only the hold's own presence is
                // trustworthy; popup-hide, WM_CLOSE, session lock, a replacing popup and
                // the panel's CloseCard remain the closers.
                HWND next=reinterpret_cast<HWND>(l);
                bool held=self->holdPid!=0||self->holdWindow!=nullptr;
                bool willHide=LOWORD(w) == WA_INACTIVE && !self->closing&&!self->contextMenuOpen && !held && !(self->contextFamily&&self->contextFamily(next)) && next != self->hwnd && !IsChild(self->hwnd, next)&&!self->InputFamily(next);
                if(willHide)self->Hide();
                }
                return 0;
            case WM_CLOSE:
                self->Hide();
                return 0;
            case WM_WTSSESSION_CHANGE:
                if (w == WTS_SESSION_LOCK)
                    self->FinishHide(true);
                return 0;
            case WM_TIMER:
                if(w==96){KillTimer(window,96);if(self->visible&&!self->dismissing&&self->LocalContext()&&self->hover>=0&&static_cast<size_t>(self->hover)<self->rows.size()&&self->contextPrepare){auto prepare=self->contextPrepare;prepare(self->rows[self->hover].id);}return 0;}
                if(w==95){self->StopRefreshFeedback();self->Draw();return 0;}
                if(w==94){self->FinishResize(true,true,true);return 0;}
                if(w==91){self->FinishFlyoutResize();return 0;}
                if(w==90){KillTimer(window,90);if(self->Flyout())self->SendFlyoutValue();else self->SendRangeValue();return 0;}
                if(w==88){KillTimer(window,88);if(self->dismissing)self->FinishHide();return 0;}
                if(w==89){if(self->LocalDockPreview()){self->TickPreviewMotion();return 0;}KillTimer(window,89);if(self->opening){self->ResetPanelMotion();self->FocusReadyInput();}return 0;}
                if (w == 84)
                {
                    KillTimer(window, 84);
                    self->Hide(false);
                }
                else if (w == 85)
                {
                    if (!(GetAsyncKeyState(VK_MENU) & 0x8000))
                    {
                        KillTimer(window, 85);
                        self->Activate(self->focus);
                        self->Hide(true, true);
                    }
                }
                else if (w == 83)
                {
                    KillTimer(window, 83);
                    for (auto const &row : self->rows)
                        if ((row.id == L"search" ||row.id==L"folder-name")&&self->edits.contains(row.id)&&self->edits[row.id]==self->changedEdit)
                        {
                            self->Command(row);
                            break;
                        }
                }
                else if (w == 82)
                {
                    if(!self->ConfirmationSeconds()){KillTimer(window,82);self->armed.clear();self->armedUntil=0;}
                    self->Draw();
                }
                else if (w == 81)
                {
                    KillTimer(window, 81);
                    SYSTEMTIME now{};
                    GetLocalTime(&now);
                    self->Draw();
                    if (self->visible)
                        SetTimer(window, 81, 60000 - now.wSecond * 1000 - now.wMilliseconds, nullptr);
                }
                return 0;
            case WM_DPICHANGED: {
                if(self->dismissing){self->FinishHide();return 0;}self->FinishResize();self->FinishFlyoutResize();self->ResetPanelMotion();
                self->dpi = HIWORD(w);
                auto r = reinterpret_cast<RECT *>(l);
                SetWindowPos(window, HWND_TOPMOST, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOACTIVATE);
                if(self->Modal()){
                    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);float scale=self->dpi/96;
                    int w=monitor.rcWork.right-monitor.rcWork.left,h=monitor.rcWork.bottom-monitor.rcWork.top;
                    self->contentX=(w/scale-self->width)/2;self->contentY=(h/scale-self->height)/2;
                    SetWindowPos(window,HWND_TOPMOST,monitor.rcWork.left,monitor.rcWork.top,w,h,SWP_NOACTIVATE);
                    if(self->popupRoot)self->popupRoot.Size({static_cast<float>(w),static_cast<float>(h)});if(self->popupScene)self->popupScene.Size({static_cast<float>(w),static_cast<float>(h)});
                    if(self->shade)self->shade.Size({static_cast<float>(w),static_cast<float>(h)});
                    self->visual.Offset({self->contentX*scale,self->contentY*scale,0});
                }
                self->Release();
                if(self->inputFont)DeleteObject(self->inputFont);
                self->inputFont=CreateFontW(-static_cast<int>(13*self->dpi/96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,NativeUiFont::Family().c_str());
                self->BuildRows();if(self->AnimatedContext())self->visual.Offset({self->contentX*self->dpi/96,self->contentY*self->dpi/96,0});self->Draw();
                return 0;
            }
            case WM_SETTINGCHANGE:
                self->FinishResize();self->FinishFlyoutResize();Preview70Theme::Refresh();self->dark=Preview70Theme::systemDark;if(self->dismissing)self->FinishHide();else {self->ResetPanelMotion();self->Draw();}return 0;
            case WM_POWERBROADCAST:
                if(self->dismissing)self->FinishHide();else {self->FinishResize();self->FinishFlyoutResize();self->ResetPanelMotion();self->Draw();}return TRUE;
            case WM_GETOBJECT:
                if (static_cast<LONG>(l) == UiaRootObjectId && self->life)
                {
                    auto provider = PopupProvider(self->life);
                    LRESULT result = UiaReturnRawElementProvider(window, w, l, provider);
                    provider->Release();
                    return result;
                }
                break;
            case WM_DESTROY:
                WTSUnRegisterSessionNotification(window);
                if (self->life)
                    self->life->owner = nullptr;
                self->Release();
                self->target = nullptr;
                self->visual = nullptr;
                self->hwnd = nullptr;
                return 0;
            }
        }
        catch (...)
        {
            self->Hide();
            self->app.Telemetry(L"popup-error");
        }
        return DefWindowProcW(window, message, w, l);
    }
};

#include "native-popup-input.h"
#include "native-flyout.h"
#include "native-range.h"
#include "native-popup-appearance.h"
#include "native-popup-resize.h"

#include "native-card-picker.h"
#include "native-popup-feedback.h"
