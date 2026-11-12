#pragma once
// Fold motion retains two bounded text rasters; only the rounded geometry changes.
struct NativeFlyoutResize {
    static constexpr int Duration=240;
    NativeMorphFrame frame;SpriteVisual previous{nullptr},next{nullptr};RECT target{};size_t bytes=0;
    void Release(NativePopup &p){frame.Release();previous=nullptr;next=nullptr;bytes=0;if(p.visual)p.visual.IsVisible(true);if(p.materialVisual)p.materialVisual.IsVisible(true);}
    void Start(NativePopup &p,RECT before,RECT after,CompositionDrawingSurface const &old,size_t oldBytes,float oldHeight){
        target=after;bytes=oldBytes;auto &g=p.app.graphics;auto c=g.compositor;float unit=p.dpi/96;
        RECT extent{std::min(before.left,after.left),std::min(before.top,after.top),std::max(before.right,after.right),std::max(before.bottom,after.bottom)};
        NativeLayerPosition(p.hwnd,extent.left,extent.top,extent.right-extent.left,extent.bottom-extent.top,SWP_NOACTIVATE);
        auto region=CreateRectRgn(0,0,extent.right-extent.left,extent.bottom-extent.top);if(!SetWindowRgn(p.hwnd,region,FALSE))DeleteObject(region);
        auto start=float2{(p.width-20)*unit,(oldHeight-20)*unit},end=float2{(p.width-20)*unit,(p.height-20)*unit};
        Preview70Theme theme{p.dark};auto blur=p.materialVisual?p.materialVisual.Brush():CompositionBrush{nullptr};
        frame.Create(g,p.popupRoot,{start.x,std::max(start.y,end.y)},start,28*unit,theme.Material(p.MaterialTier(),p.Glass()),p.dark,blur,unit);
        frame.root.Offset({static_cast<float>(before.left-extent.left)+10*unit,static_cast<float>(before.top-extent.top)+10*unit,0});
        auto pixels=[&](CompositionDrawingSurface const &surface,float height){auto v=c.CreateSpriteVisual();v.Size({p.width*unit,height*unit});v.Offset({-10*unit,-10*unit,0});v.Brush(c.CreateSurfaceBrush(surface));frame.root.Children().InsertAtTop(v);return v;};
        previous=pixels(old,oldHeight);next=pixels(p.surface,p.height);next.Opacity(0);p.visual.IsVisible(false);if(p.materialVisual)p.materialVisual.IsVisible(false);
        frame.Animate(c,end,28*unit,theme.Material(p.MaterialTier(),p.Glass()),Duration);
        NativeInteractionMotion::Vector(c,frame.root,L"Offset",{static_cast<float>(after.left-extent.left)+10*unit,static_cast<float>(after.top-extent.top)+10*unit,0},Duration);
        NativeInteractionMotion::Opacity(c,previous,0,Duration);NativeInteractionMotion::Opacity(c,next,1,Duration);
    }
};
inline size_t NativePopup::FlyoutResizeBytes()const{return flyoutResize?flyoutResize->bytes:0;}
inline void NativePopup::FinishFlyoutResize(bool redraw){
    if(hwnd)KillTimer(hwnd,91);if(!flyoutResize)return;auto motion=std::move(flyoutResize);auto target=motion->target;motion->Release(*this);
    if(hwnd){float scale=dpi/96;int w=target.right-target.left,h=target.bottom-target.top;NativeLayerPosition(hwnd,target.left,target.top,w,h,SWP_NOACTIVATE);
        if(popupRoot)popupRoot.Size({static_cast<float>(w),static_cast<float>(h)});if(popupScene)popupScene.Size({static_cast<float>(w),static_cast<float>(h)});
        auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(56*scale),static_cast<int>(56*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
    }
    if(redraw&&visible&&!dismissing)Draw();
}
// Bounded transient flyout. Uses the Dock's compositor spring, no frame timer.
inline void NativePopup::BuildFlyout(){
    rows.clear();generic=true;auto data=Data();auto body=Str(data,L"text");if(body.size()>32768)body.resize(32768);
    float textWidth=std::max(40.f,width-52);auto format=app.graphics.Format(13,false);
    com_ptr<IDWriteTextLayout> layout;check_hresult(app.graphics.text->CreateTextLayout(body.c_str(),static_cast<UINT32>(body.size()),format,textWidth,131072,layout.put()));
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,20,15);
    DWRITE_TEXT_METRICS metrics{};layout->GetMetrics(&metrics);flyoutFullHeight=body.empty()?0:metrics.height;flyoutOverflow=flyoutFullHeight>60.5f;
    float bodyHeight=body.empty()?0:std::min(flyoutFullHeight,flyoutExpanded?flyoutBodyLimit:60.f);
    flyoutBody={26,60,width-26,60+bodyHeight};flyoutScroll=std::clamp(flyoutScroll,0.f,std::max(0.f,flyoutFullHeight-bodyHeight));
    auto add=[&](std::wstring id,std::wstring label,D2D1_RECT_F r,std::wstring control=L"button",double value=0){PopupRow row;row.key=row.id=id;row.action=id;row.label=label;row.rect=r;row.enabled=true;row.control=control;row.value=value;rows.push_back(row);};
    add(L"flyout-close",L"关闭",{width-52,18,width-22,46});
    if(Bool(data,L"openSource")){
        add(L"flyout-source-title",L"打开通知来源",{26,18,width-60,48},L"notification-body");
        if(bodyHeight>0)add(L"flyout-source-body",L"打开通知来源",flyoutBody,L"notification-body");
    }
    float y=flyoutBody.bottom+(body.empty()?0:8);
    if(flyoutOverflow){add(L"flyout-fold",flyoutExpanded?L"收起":L"展开全文",{26,y,width-26,y+28});y+=32;}
    if(Num(data,L"value",-1)>=0){add(L"flyout-level",Str(data,L"levelLabel"),{26,y,width-26,y+46},L"range",Num(data,L"value"));y+=52;}
    if(Bool(data,L"canOpen")&&!Bool(data,L"openSource")){add(L"flyout-open",L"打开完整面板",{26,y,width-26,y+28});y+=32;}
    height=std::max(100.f,y+18);
}
inline void NativePopup::ResizeFlyout(bool animate){
    if(!hwnd)return;FinishFlyoutResize();RECT before{};GetWindowRect(hwnd,&before);auto oldHeight=height;CompositionDrawingSurface oldSurface{nullptr};size_t oldBytes=0;
    BuildFlyout();float unit=dpi/96;
    auto raster=[&](float h){return static_cast<size_t>(std::ceil(width*unit))*static_cast<size_t>(std::ceil(h*unit))*4;};
    // Budget before allocating, including the old foreground during repaint.
    bool moving=animate&&visible&&NativeMotion(app.model)&&!measurement&&2*std::max(raster(oldHeight),raster(height))<=8*1024*1024;
    if(moving){flyoutExpanded=!flyoutExpanded;BuildFlyout();flyoutResizePainting=true;surface=nullptr;Draw();oldSurface=surface;oldBytes=surfaceBytes;flyoutExpanded=!flyoutExpanded;flyoutResizePainting=false;}
    BuildFlyout();float scale=dpi/96;auto payload=packet.GetNamedObject(L"payload"),pos=payload.GetNamedObject(L"desiredPosition");
    POINT anchor{static_cast<LONG>(Num(pos,L"x")),static_cast<LONG>(Num(pos,L"y"))};MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&monitor);
    int w=static_cast<int>(std::ceil(width*scale)),h=static_cast<int>(std::ceil(height*scale));
    auto origin=NativeHintPlacement::Origin(payload,monitor.rcWork,w,h);int x=origin.x,y=origin.y;
    NativeLayerPosition(hwnd,x,y,w,h,SWP_NOACTIVATE);
    if(popupRoot)popupRoot.Size({static_cast<float>(w),static_cast<float>(h)});if(popupScene)popupScene.Size({static_cast<float>(w),static_cast<float>(h)});
    auto region=CreateRoundRectRgn(0,0,w+1,h+1,static_cast<int>(56*scale),static_cast<int>(56*scale));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
    if(moving){flyoutResizePainting=true;surface=nullptr;Draw();flyoutResizePainting=false;
        if(oldSurface&&oldBytes+surfaceBytes<=8*1024*1024){
            auto motion=std::make_shared<NativeFlyoutResize>();
            try{motion->Start(*this,before,{x,y,x+w,y+h},oldSurface,oldBytes,oldHeight);flyoutResize=motion;if(!SetTimer(hwnd,91,256,nullptr))FinishFlyoutResize();return;}
            catch(...){motion->Release(*this);}
        }
    }
    Draw();
}
inline void NativePopup::PaintFlyout(ID2D1DeviceContext *c){
    NativePaint p(c,app.graphics,dark);if(!flyoutResizePainting)p.Material({10,10,width-10,height-10},MaterialTier(),Glass(),28);
    auto title=Str(Data(),L"title");if(title.size()>512)title.resize(512);
    auto trimText=[&](std::wstring const &text,D2D1_RECT_F r,float size,bool wrap){
        auto format=app.graphics.Format(size,false,wrap?400:600);com_ptr<IDWriteTextLayout> layout;
        check_hresult(app.graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,r.right-r.left,r.bottom-r.top,layout.put()));
        layout->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> ellipsis;check_hresult(app.graphics.text->CreateEllipsisTrimmingSign(format,ellipsis.put()));layout->SetTrimming(&trim,ellipsis.get());
        if(wrap)layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,20,15);c->DrawTextLayout({r.left,r.top},layout.get(),p.Color(p.theme.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    trimText(title,{26,24,width-60,48},16,false);
    if(flyoutFullHeight>0){
        auto text=Str(Data(),L"text");if(text.size()>32768)text.resize(32768);
        if(!flyoutExpanded)trimText(text,flyoutBody,13,true);
        else{c->PushAxisAlignedClip(flyoutBody,D2D1_ANTIALIAS_MODE_ALIASED);auto r=flyoutBody;r.top-=flyoutScroll;r.bottom=r.top+flyoutFullHeight;com_ptr<IDWriteTextLayout> layout;check_hresult(app.graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),app.graphics.Format(13,false),r.right-r.left,flyoutFullHeight,layout.put()));layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,20,15);c->DrawTextLayout({r.left,r.top},layout.get(),p.Color(p.theme.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);c->PopAxisAlignedClip();}
    }
    for(auto const &row:rows){
        if(row.control==L"notification-body")continue;
        if(row.id==L"flyout-close"){p.Icon(L"close",{row.rect.left+8,row.rect.top+8,row.rect.right-8,row.rect.bottom-8},p.theme.Muted());continue;}
        if(row.control==L"range"){auto track=RangeTrack(row);p.Slider(track,row.value);continue;}
        p.Text(row.label,row.rect,12,p.theme.Accent(),500);
    }
}
inline void NativePopup::SendFlyoutValue(bool final){
    if(hwnd)KillTimer(hwnd,90);flyoutScheduled=false;flyoutFinalPending|=final;
    if(!Flyout()||!visible||flyoutInFlight||!flyoutPending||!flyoutAdjust)return;
    auto now=GetTickCount64();if(hwnd&&!flyoutFinalPending&&now-flyoutLastSent<40){flyoutScheduled=true;SetTimer(hwnd,90,static_cast<UINT>(40-(now-flyoutLastSent)),nullptr);return;}
    flyoutFinalPending=false;flyoutLastSent=now;flyoutPending=false;flyoutInFlight=true;double value=Num(Data(),L"value");auto owner=life;auto adjust=flyoutAdjust;
    adjust(value,std::exchange(flyoutFeedback,false),[owner](bool success,double actual){
        auto self=owner?owner->owner:nullptr;if(!self||!self->visible||!self->Flyout())return;self->flyoutInFlight=false;
        if(!success){self->flyoutPending=false;Put(self->Data(),L"text",std::wstring(L"设备未完成调节，请重试。"));self->ResizeFlyout();return;}
        self->flyoutEchoUntil=GetTickCount64()+1200;
        if(!self->flyoutPending&&std::isfinite(actual)&&actual>=0&&actual<=1){Put(self->Data(),L"value",actual);Put(self->Data(),L"title",Str(self->Data(),L"levelLabel")+L" "+std::to_wstring(static_cast<int>(std::lround(actual*100)))+L"%");self->caption=Str(self->Data(),L"title");for(auto &r:self->rows)if(r.control==L"range")r.value=actual;self->Draw();}
        if(self->flyoutPending)self->SendFlyoutValue();
    });
}
inline void NativePopup::SetFlyoutValue(double value){
    if(!Flyout()||!std::isfinite(value))return;value=std::clamp(value,0.,1.);Put(Data(),L"value",value);
    auto label=Str(Data(),L"levelLabel");Put(Data(),L"title",label+L" "+std::to_wstring(static_cast<int>(std::lround(value*100)))+L"%");caption=Str(Data(),L"title");
    for(auto &row:rows)if(row.id==L"flyout-level")row.value=value;
    flyoutPending=true;Draw();if(!hwnd){flyoutPending=false;return;}
    if(!flyoutInFlight&&!flyoutScheduled){flyoutScheduled=true;SetTimer(hwnd,90,40,nullptr);}KillTimer(hwnd,84);if(!flyoutDragging)SetTimer(hwnd,84,4200,nullptr);
}
inline bool NativePopup::FlyoutPointer(UINT message,WPARAM w,LPARAM l){
    if(!Flyout()||!visible||dismissing)return false;
    POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(message==WM_MOUSEWHEEL)ScreenToClient(hwnd,&point);
    float x=point.x*96/dpi,y=point.y*96/dpi;
    if(message==WM_MOUSEWHEEL){int hit=Hit(x,y);if(hit>=0&&rows[hit].control==L"range"&&Inside(RangeTrack(rows[hit]),x,y)){SetFlyoutValue(rows[hit].value+GET_WHEEL_DELTA_WPARAM(w)/120.*.02);return true;}}
    if(message==WM_LBUTTONDOWN){
        KillTimer(hwnd,84);int hit=Hit(x,y);
        if(hit>=0&&rows[hit].control==L"range"){flyoutDragging=true;flyoutStartValue=rows[hit].value;SetCapture(hwnd);auto r=RangeTrack(rows[hit]);SetFlyoutValue((x-r.left)/(r.right-r.left));return true;}
    }
    if(message==WM_MOUSEMOVE){KillTimer(hwnd,84);if(flyoutDragging)for(auto const &r:rows)if(r.control==L"range"){auto t=RangeTrack(r);SetFlyoutValue((x-t.left)/(t.right-t.left));return true;}}
    if(message==WM_LBUTTONUP&&flyoutDragging){flyoutDragging=false;for(auto const &r:rows)if(r.control==L"range"){auto t=RangeTrack(r);SetFlyoutValue((x-t.left)/(t.right-t.left));break;}flyoutFeedback=Str(Data(),L"levelKind")==L"volume"&&std::abs(Num(Data(),L"value")-flyoutStartValue)>.0001;if(GetCapture()==hwnd)ReleaseCapture();SendFlyoutValue(true);SetTimer(hwnd,84,4200,nullptr);return true;}
    if(message==WM_MOUSELEAVE&&!flyoutDragging){SetTimer(hwnd,84,flyoutExpanded?9000:1800,nullptr);}
    if(message==WM_CAPTURECHANGED&&flyoutDragging){flyoutDragging=false;SendFlyoutValue(true);SetTimer(hwnd,84,4200,nullptr);}
    if(message==WM_MOUSEWHEEL&&flyoutExpanded){flyoutScroll=std::clamp(flyoutScroll-GET_WHEEL_DELTA_WPARAM(w)/120.f*40,0.f,std::max(0.f,flyoutFullHeight-(flyoutBody.bottom-flyoutBody.top)));Draw();return true;}
    return false;
}
inline void NativePopup::ActivateFlyout(PopupRow const &row){
    if(row.id==L"flyout-close"){Hide(false);return;}
    if(row.id==L"flyout-fold"){flyoutExpanded=!flyoutExpanded;flyoutScroll=0;ResizeFlyout(true);if(hwnd)SetTimer(hwnd,84,flyoutExpanded?9000:4200,nullptr);return;}
    if(row.id==L"flyout-open"||row.control==L"notification-body"){auto action=passiveClick;Hide(false);if(action)action();return;}
}
