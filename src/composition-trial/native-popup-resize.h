#pragma once
// Reuses the notification/folder rounded morph. Two unscaled foreground rasters,
// one live material; one latest-only update slot and one finite cleanup timer.
struct NativePopupResize {
    static constexpr int Duration=240,Timer=94;
    NativeMorphFrame frame;SpriteVisual previous{nullptr},next{nullptr};RECT target{};size_t bytes=0;
    static void PlaceFinal(NativePopup &p,RECT target){
        if(!p.hwnd)return;int w=target.right-target.left,h=target.bottom-target.top;NativeLayerPosition(p.hwnd,target.left,target.top,w,h,SWP_NOACTIVATE);
        if(p.popupRoot)p.popupRoot.Size({static_cast<float>(w),static_cast<float>(h)});if(p.popupScene)p.popupScene.Size({static_cast<float>(w),static_cast<float>(h)});
        auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(2*p.MaterialRadius()*p.dpi/96),static_cast<int>(2*p.MaterialRadius()*p.dpi/96));if(!SetWindowRgn(p.hwnd,region,FALSE))DeleteObject(region);
    }
    void Release(NativePopup &p){if(p.refreshVisual&&p.refreshVisual.Parent()!=p.popupRoot){if(p.refreshVisual.Parent())p.refreshVisual.Parent().Children().Remove(p.refreshVisual);p.popupRoot.Children().InsertAtTop(p.refreshVisual);p.SyncRefreshFeedback();}
        for(auto &[id,toggle]:p.toggleVisuals)if(toggle.root&&toggle.root.Parent()!=p.popupRoot){auto offset=toggle.root.Offset();if(toggle.root.Parent())toggle.root.Parent().Children().Remove(toggle.root);p.popupRoot.Children().InsertAtTop(toggle.root);toggle.root.Offset({offset.x+10*p.dpi/96,offset.y+10*p.dpi/96,0});}
        frame.Release();previous=nullptr;next=nullptr;bytes=0;if(p.visual)p.visual.IsVisible(true);if(p.materialVisual)p.materialVisual.IsVisible(true);}
    void Start(NativePopup &p,RECT before,RECT after,CompositionDrawingSurface const &old,size_t oldBytes,float oldHeight){
        target=after;bytes=oldBytes;float unit=p.dpi/96;auto &g=p.app.graphics;auto c=g.compositor;
        RECT extent{std::min(before.left,after.left),std::min(before.top,after.top),std::max(before.right,after.right),std::max(before.bottom,after.bottom)};
        NativeLayerPosition(p.hwnd,extent.left,extent.top,extent.right-extent.left,extent.bottom-extent.top,SWP_NOACTIVATE);
        if(p.popupRoot)p.popupRoot.Size({static_cast<float>(extent.right-extent.left),static_cast<float>(extent.bottom-extent.top)});
        if(p.popupScene)p.popupScene.Size({static_cast<float>(extent.right-extent.left),static_cast<float>(extent.bottom-extent.top)});
        auto region=CreateRectRgn(0,0,extent.right-extent.left,extent.bottom-extent.top);if(!SetWindowRgn(p.hwnd,region,FALSE))DeleteObject(region);
        float2 start{(p.width-20)*unit,(oldHeight-20)*unit},end{(p.width-20)*unit,(p.height-20)*unit};Preview70Theme theme{p.dark};
        frame.Create(g,p.popupRoot,{start.x,std::max(start.y,end.y)},start,p.MaterialRadius()*unit,theme.Material(p.MaterialTier(),p.Glass(),p.kind==L"@magic/system-tray"),p.dark,p.materialVisual?p.materialVisual.Brush():CompositionBrush{nullptr},unit);
        frame.root.Offset({static_cast<float>(before.left-extent.left)+10*unit,static_cast<float>(before.top-extent.top)+10*unit,0});
        auto pixels=[&](CompositionDrawingSurface const &surface,float height){auto v=c.CreateSpriteVisual();v.Size({p.width*unit,height*unit});v.Offset({-10*unit,-10*unit,0});v.Brush(c.CreateSurfaceBrush(surface));frame.root.Children().InsertAtTop(v);return v;};
        previous=pixels(old,oldHeight);next=pixels(p.surface,p.height);next.Opacity(0);p.visual.IsVisible(false);if(p.materialVisual)p.materialVisual.IsVisible(false);
        if(p.refreshVisual){auto offset=p.refreshVisual.Offset();if(p.refreshVisual.Parent())p.refreshVisual.Parent().Children().Remove(p.refreshVisual);frame.root.Children().InsertAtTop(p.refreshVisual);p.refreshVisual.Offset({offset.x-10*unit,offset.y-10*unit,0});}
        // Switches stay live while their device list changes the panel height.
        // Reparent the same visuals; neither raster snapshot paints a second knob.
        for(auto &[id,toggle]:p.toggleVisuals)if(toggle.root){auto offset=toggle.root.Offset();if(toggle.root.Parent())toggle.root.Parent().Children().Remove(toggle.root);frame.root.Children().InsertAtTop(toggle.root);toggle.root.Offset({offset.x-10*unit,offset.y-10*unit,0});}
        frame.Animate(c,end,p.MaterialRadius()*unit,theme.Material(p.MaterialTier(),p.Glass(),p.kind==L"@magic/system-tray"),Duration);
        NativeInteractionMotion::Vector(c,frame.root,L"Offset",{static_cast<float>(after.left-extent.left)+10*unit,static_cast<float>(after.top-extent.top)+10*unit,0},Duration);
        NativeInteractionMotion::Opacity(c,previous,0,Duration);NativeInteractionMotion::Opacity(c,next,1,Duration);
    }
};
inline size_t NativePopup::ResizeBytes()const{return resizeMotion?resizeMotion->bytes:0;}
inline void NativePopup::FinishResize(bool redraw,bool flush,bool animatePending){
    if(hwnd)KillTimer(hwnd,NativePopupResize::Timer);auto pending=std::exchange(resizePending,JsonObject{nullptr});
    if(resizeMotion){auto motion=std::move(resizeMotion);auto target=motion->target;motion->Release(*this);resizeInputsBlocked=false;
        NativePopupResize::PlaceFinal(*this,target);
        if(redraw&&visible&&!dismissing){Draw();SetMotionInputs(!opening);}
    }
    if(flush&&pending&&visible&&!dismissing&&Str(pending,L"token")==token){
        bool before=std::exchange(resizeImmediate,!animatePending);try{Update(pending);}catch(...){resizeImmediate=before;throw;}resizeImmediate=before;
    }
}
inline bool NativePopup::TryResizeUpdate(JsonObject const &value){
    if(resizeMotion){resizePending=value;return true;}
    if(!ToolbarDropdown()||!(FunctionalPanel()||kind==L"@magic/system-tray")||!hwnd||!surface||!visual||resizeImmediate||opening||measurement||app.popupCycles||app.stopping||!NativeMotion(app.model)||GetCapture()==hwnd)return false;
    auto incoming=value.GetNamedObject(L"data");if(Bool(incoming,L"dark",dark)!=dark)return false;
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);float unit=dpi/96;
    NativePopup probe(app);probe.kind=kind;probe.width=width;probe.height=height;probe.dpi=dpi;probe.english=english;probe.packet=value;probe.scroll=scroll;probe.quickDevices=quickDevices;probe.bluetoothExpanded=bluetoothExpanded;probe.networkExpanded=networkExpanded;probe.audioAdvanced=audioAdvanced;probe.BuildRows();
    if(probe.PowerView()!=PowerView())return false;float nextHeight=probe.FunctionalHeight(monitor.rcWork,unit);
    size_t nextBytes=static_cast<size_t>(std::ceil(width*unit))*static_cast<size_t>(std::ceil(nextHeight*unit))*4;
    if((!resizeDisclosure&&std::abs(nextHeight-height)<=.5f)||surfaceBytes+nextBytes>8*1024*1024)return false;
    RECT before{};GetWindowRect(hwnd,&before);float oldHeight=height;auto old=surface;size_t oldBytes=surfaceBytes,oldGlass=NativeGlassStates()[hwnd].bytes;
    struct Preparing{NativePopup &p;~Preparing(){p.resizePreparing=p.resizePainting=false;if(!p.resizeMotion){p.resizeInputsBlocked=false;if(p.visible&&!p.dismissing)p.SetMotionInputs(!p.opening);}}} scope{*this};
    resizeInputsBlocked=true;SetMotionInputs(false);resizePainting=true;auto painted=redraws;Draw();if(painted==redraws)return false;
    // The recursive pass uses the existing update/layout path exactly once.
    // It paints the final content without another material/tint underneath it.
    // A disclosure can rearrange rows while the screen-limited height stays
    // unchanged. Preserve the old foreground instead of repainting it in place.
    surface=nullptr;surfaceBytes=0;resizePreparing=true;Update(value);resizePreparing=resizePainting=false;
    RECT after{};GetWindowRect(hwnd,&after);
    if(!visible||dismissing||!surface||oldBytes+surfaceBytes>8*1024*1024){Draw();return true;}
    auto motion=std::make_shared<NativePopupResize>();
    try{motion->Start(*this,before,after,old,oldBytes,oldHeight);resizeMotion=motion;interactionVisual.Release(popupRoot);NativeGlassStates()[hwnd].bytes=std::max(oldGlass,NativeGlassStates()[hwnd].bytes);
        if(!SetTimer(hwnd,NativePopupResize::Timer,NativePopupResize::Duration+16,nullptr))FinishResize();
    }catch(...){motion->Release(*this);resizeMotion=nullptr;resizeInputsBlocked=false;NativePopupResize::PlaceFinal(*this,after);Draw();}
    return true;
}

inline void NativePopup::RelayoutDisclosure(){
    if(!visible||!hwnd){BuildRows();Draw();return;}
    bool before=std::exchange(resizeDisclosure,true);
    try{Update(packet);}catch(...){resizeDisclosure=before;throw;}
    resizeDisclosure=before;
}
