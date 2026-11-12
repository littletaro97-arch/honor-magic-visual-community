#pragma once
inline void NativePopup::PaintTrayItem(ID2D1DeviceContext *c,PopupRow const &row,D2D1_RECT_F rect,bool hot){
    NativePaint p(c,app.graphics,dark);auto ink=row.enabled?p.theme.Ink():p.theme.MaterialMuted();
    float cx=(rect.left+rect.right)/2;D2D1_RECT_F icon{cx-12,rect.top+12,cx+12,rect.top+36};
    if(!appearanceIcons.Draw(c,row.id,icon,p.Color(ink)))p.Icon(L"tray",icon,ink);
    p.Icon(row.active?L"unpin":L"pin",{rect.right-14,rect.top+2,rect.right-3,rect.top+13},hot?p.theme.Ink():p.theme.MaterialMuted());
}
inline D2D1_RECT_F NativePopup::TrayItemRect(int index)const{
    if(!trayDrag||index<0||index>=static_cast<int>(trayDrag->original.size()))return rows[index].rect;
    auto &d=*trayDrag;auto found=std::find(d.order.begin(),d.order.end(),index);
    auto r=d.original[static_cast<int>(found-d.order.begin())].rect;
    if(d.moving&&index==d.from){float w=r.right-r.left,h=r.bottom-r.top;r={d.pointer.x-d.grab.x,d.pointer.y-d.grab.y,d.pointer.x-d.grab.x+w,d.pointer.y-d.grab.y+h};}
    return r;
}
inline void NativePopup::ResetTrayDrag(){
    if(hwnd)KillTimer(hwnd,97);
    auto old=std::exchange(trayDrag,{});if(old&&old->layer&&popupRoot)popupRoot.Children().Remove(old->layer);
    pressedTray=-1;trayDragging=false;
}
inline void NativePopup::SyncTrayDrag(){
    auto d=trayDrag;if(!d||!popupRoot)return;auto &g=app.graphics;float scale=dpi/96;
    bool animate=NativeMotion(app.model);
    if(!animate&&d->layer){popupRoot.Children().Remove(d->layer);d->layer=nullptr;d->items.clear();d->bytes=0;}
    if(!d->visualAttempted&&animate){
        d->visualAttempted=true;
        try{
            size_t cost=0;for(auto const&r:d->original)cost+=static_cast<size_t>(std::ceil((r.rect.right-r.rect.left)*scale))*static_cast<size_t>(std::ceil((r.rect.bottom-r.rect.top)*scale))*4;
            // Match folder avoidance's bounded, gesture-owned cache. The normal
            // popup plus these temporary cells must also fit the popup budget.
            if(d->original.size()>128||cost+surfaceBytes>8*1024*1024)return;
            auto layer=g.compositor.CreateContainerVisual();layer.Size({width*scale,height*scale});layer.Offset({contentX*scale,contentY*scale,0});
            auto clip=g.compositor.CreateInsetClip();clip.LeftInset(14*scale);clip.RightInset(14*scale);clip.TopInset(48*scale);clip.BottomInset(16*scale);layer.Clip(clip);
            std::vector<NativeTrayDrag::Item> items;
            for(int i=0;i<static_cast<int>(d->original.size());++i){auto const&r=d->original[i];float w=r.rect.right-r.rect.left,h=r.rect.bottom-r.rect.top;
                NativeTrayDrag::Item item;item.slot=i;item.surface=g.composition.CreateDrawingSurface({std::ceil(w*scale),std::ceil(h*scale)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
                auto draw=item.surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};check_hresult(draw->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));
                bool ended=false;try{c->SetDpi(dpi,dpi);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x/scale,offset.y/scale));c->Clear(D2D1::ColorF(0,0));PaintTrayItem(c.get(),r,{0,0,w,h},false);auto hr=draw->EndDraw();ended=true;check_hresult(hr);}catch(...){if(!ended)draw->EndDraw();throw;}
                item.visual=g.compositor.CreateSpriteVisual();item.visual.Size({w*scale,h*scale});item.visual.Brush(g.compositor.CreateSurfaceBrush(item.surface));item.visual.Offset({r.rect.left*scale,r.rect.top*scale,0});layer.Children().InsertAtTop(item.visual);items.push_back(std::move(item));
            }
            if(trayDrag!=d)return;
            layer.Children().Remove(items[d->from].visual);layer.Children().InsertAtTop(items[d->from].visual);
            d->items=std::move(items);d->bytes=cost;d->layer=layer;popupRoot.Children().InsertAtTop(layer);
        }catch(...){if(d->layer&&popupRoot){try{popupRoot.Children().Remove(d->layer);}catch(...){}}d->layer=nullptr;d->items.clear();d->bytes=0;return;}
    }
    if(!d->layer)return;
    for(int i=0;i<static_cast<int>(d->items.size());++i){auto &item=d->items[i];auto rect=TrayItemRect(i);float3 dest{rect.left*scale,rect.top*scale,0};
        int slot=static_cast<int>(std::find(d->order.begin(),d->order.end(),i)-d->order.begin());
        if(d->moving&&i==d->from){item.visual.StopAnimation(L"Offset");item.visual.Offset(dest);item.visual.Opacity(.94f);item.slot=-1;}
        else if(item.slot!=slot){auto a=g.compositor.CreateVector3KeyFrameAnimation();a.InsertExpressionKeyFrame(0,L"this.StartingValue");a.InsertKeyFrame(1,dest,g.compositor.CreateCubicBezierEasingFunction({.22f,1},{.28f,1}));a.Duration(std::chrono::milliseconds(260));item.visual.StartAnimation(L"Offset",a);item.visual.Opacity(1);item.slot=slot;}
    }
}
inline void NativePopup::MoveTrayDrag(float x,float y){
    if(kind!=L"@magic/system-tray"||pressedTray<0||pressedTray>=static_cast<int>(rows.size())||dismissing)return;
    x-=contentX;y-=contentY;bool first=!trayDrag;
    {
    NativeRenderScope render(app.relay);if(!render)return;
    if(first){auto d=std::make_shared<NativeTrayDrag>();d->original=rows;d->from=d->to=pressedTray;d->order=NativeTrayOrder::Build(static_cast<int>(rows.size()),pressedTray,pressedTray);
        auto r=rows[pressedTray].rect;d->grab={trayPress.x*96/dpi-contentX-r.left,trayPress.y*96/dpi-contentY-r.top};trayDrag=d;
        hover=-1;if(tooltip)SendMessageW(tooltip,TTM_POP,0,0);
    }
    auto d=trayDrag;if(!d||!d->moving)return;trayDragging=true;d->pointer={x,y};auto cell=d->original[d->from].rect;
    d->to=NativeTrayOrder::Nearest(d->original,x-d->grab.x+(cell.right-cell.left)/2,y-d->grab.y+(cell.bottom-cell.top)/2);
    d->order=NativeTrayOrder::Build(static_cast<int>(rows.size()),d->from,d->to);SyncTrayDrag();
    }
    // Only the first event removes the baked icons. Following pointer events
    // move cached Composition visuals; there is no CPU frame timer or logging.
    if(first)Draw();else if(!TrayOwnsPixels()&&hwnd)InvalidateRect(hwnd,nullptr,FALSE);
}
inline void NativePopup::EndTrayDrag(bool commit){
    auto d=trayDrag;if(!d||!d->moving)return;
    commit=commit&&Inside({14,48,width-14,height-16},d->pointer.x,d->pointer.y)&&d->from!=d->to;
    d->moving=false;pressedTray=-1;trayDragging=false;
    if(GetCapture()==hwnd)ReleaseCapture();
    if(!commit){d->to=d->from;d->order=NativeTrayOrder::Build(static_cast<int>(d->original.size()),d->from,d->from);}
    auto now=GetTickCount64();d->finishAt=now+(NativeMotion(app.model)?276:0);d->deadline=now+1500;
    if(commit){d->awaiting=true;d->receipt=std::to_wstring(++app.inputEpoch)+L":"+std::to_wstring(now);d->deferred=nullptr;
        auto result=d->Result();JsonObject command;
        Put(command,L"op",std::wstring(L"popup-command"));Put(command,L"token",token);command.Insert(L"result",result);app.Request(command);
    }
    Draw();
    if(hwnd&&!SetTimer(hwnd,97,static_cast<UINT>(std::max<ULONGLONG>(1,d->finishAt-now)),nullptr)){d->awaiting=false;FinishTrayFeedback();}
}
inline void NativePopup::FinishTrayFeedback(){
    auto d=trayDrag;if(!d||d->moving)return;auto now=GetTickCount64();
    if(d->awaiting&&now<d->deadline){if(hwnd&&SetTimer(hwnd,97,static_cast<UINT>(d->deadline-now),nullptr))return;}
    auto deferred=d->deferred;ResetTrayDrag();
    if(deferred)Update(deferred);else Draw();
}
inline bool NativePopup::DeferTrayUpdate(JsonObject const &value){
    auto next=value.GetNamedObject(L"data").GetNamedArray(L"rows",JsonArray{});bool same=next.Size()==rows.size();
    if(same)for(size_t i=0;i<rows.size();++i)if(Str(next.GetAt(static_cast<uint32_t>(i)).GetObject(),L"id")!=rows[i].id){same=false;break;}
    auto d=trayDrag;
    if(!d){if(pressedTray>=0&&!same){pressedTray=-1;trayDragging=false;if(GetCapture()==hwnd)ReleaseCapture();}return false;}
    if(d->moving){
        if(!same){ResetTrayDrag();if(GetCapture()==hwnd)ReleaseCapture();return false;}
        d->deferred=value;return true; // Icon/status-only refresh cannot steal a drag.
    }
    if(!d->awaiting&&d->deferred&&Str(d->deferred.GetNamedObject(L"data"),L"trayReorderReceipt")==d->receipt)return true;
    d->deferred=value;
    if(d->awaiting&&Str(value.GetNamedObject(L"data"),L"trayReorderReceipt")==d->receipt){
        d->awaiting=false;auto now=GetTickCount64();if(now>=d->finishAt){FinishTrayFeedback();return true;}
        if(hwnd)SetTimer(hwnd,97,static_cast<UINT>(d->finishAt-now),nullptr);
    }
    return true;
}
