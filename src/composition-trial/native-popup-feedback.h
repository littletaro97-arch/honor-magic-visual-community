#pragma once
inline void NativePopup::AnimatePreview(bool close){
    auto &c=app.graphics.compositor;float s=dpi/96;
    if(close){
        opening=false;previewEnterAt=0;KillTimer(hwnd,89);for(auto t:taskThumbnails)DwmUnregisterThumbnail(t);taskThumbnails.clear();previewThumbnailProps.clear();taskPreviewBytes=0;previewMounted=false;
        auto ease=c.CreateCubicBezierEasingFunction({.4f,0},{.9f,.6f});
        auto scale=c.CreateVector3KeyFrameAnimation();scale.InsertExpressionKeyFrame(0,L"this.StartingValue");scale.InsertKeyFrame(1,{.93f,.93f,1},ease);scale.Duration(std::chrono::milliseconds(NativeInteractionMotion::CloseMs));popupRoot.StartAnimation(L"Scale",scale);
        auto move=c.CreateVector3KeyFrameAnimation();move.InsertExpressionKeyFrame(0,L"this.StartingValue");move.InsertKeyFrame(1,NativePreviewMotion::Shift(previewSide,8*s),ease);move.Duration(std::chrono::milliseconds(NativeInteractionMotion::CloseMs));popupRoot.StartAnimation(L"Offset",move);
        auto alpha=c.CreateScalarKeyFrameAnimation();alpha.InsertExpressionKeyFrame(0,L"this.StartingValue");alpha.InsertKeyFrame(1,0,ease);alpha.Duration(std::chrono::milliseconds(NativeInteractionMotion::CloseMs));popupRoot.StartAnimation(L"Opacity",alpha);return;
    }
    ResetPanelMotion(false);opening=true;previewMounted=false;previewEnterAt=GetTickCount64();
    for(auto t:taskThumbnails)DwmUnregisterThumbnail(t);taskThumbnails.clear();taskPreviewBytes=0;previewThumbnailProps.clear();
    popupRoot.CenterPoint({previewOrigin.x*s,previewOrigin.y*s,0});popupRoot.Scale({previewMotion.scale,previewMotion.scale,1});popupRoot.Offset(NativePreviewMotion::Shift(previewSide,previewMotion.shift*s));popupRoot.Opacity(0);
    NativeInteractionMotion::Vector(c,popupRoot,L"Scale",{1,1,1},previewMotion.duration);NativeInteractionMotion::Vector(c,popupRoot,L"Offset",{},previewMotion.duration);NativeInteractionMotion::Opacity(c,popupRoot,1,previewMotion.duration);
    if(!SetTimer(hwnd,89,previewMotion.Delay(),nullptr))ResetPanelMotion();
}
inline void NativePopup::TickPreviewMotion(){
    KillTimer(hwnd,89);if(!visible||dismissing||!previewEnterAt)return;
    auto elapsed=static_cast<int>(GetTickCount64()-previewEnterAt);int delay=previewMotion.Delay(),fade=previewMotion.Fade();
    if(!previewMounted&&elapsed>=delay){previewMounted=true;UpdateTaskPreviews();}
    float progress=NativeInteractionMotion::Progress(elapsed/static_cast<float>(previewMotion.duration)),k=previewMotion.scale+(1-previewMotion.scale)*progress,s=dpi/96;
    auto shift=NativePreviewMotion::Shift(previewSide,previewMotion.shift*s*(1-progress));float ox=previewOrigin.x*s,oy=previewOrigin.y*s;
    BYTE opacity=static_cast<BYTE>(255*NativeInteractionMotion::Progress((elapsed-delay)/static_cast<float>(fade)));
    for(auto const &[t,base]:previewThumbnailProps){auto p=base;auto r=base.rcDestination;p.opacity=opacity;p.rcDestination={static_cast<LONG>(std::lround(ox+(r.left-ox)*k+shift.x)),static_cast<LONG>(std::lround(oy+(r.top-oy)*k+shift.y)),static_cast<LONG>(std::lround(ox+(r.right-ox)*k+shift.x)),static_cast<LONG>(std::lround(oy+(r.bottom-oy)*k+shift.y))};DwmUpdateThumbnailProperties(t,&p);}
    if(elapsed>=previewMotion.duration&&opening){opening=false;for(auto property:{L"Scale",L"Offset",L"Opacity"})popupRoot.StopAnimation(property);popupRoot.Scale({1,1,1});popupRoot.Offset({0,0,0});popupRoot.Opacity(1);}
    int end=std::max(previewMotion.duration,delay+fade);if(elapsed>=end){previewEnterAt=0;return;}
    // Reuse the existing opening-completion timer only for the brief DWM fade.
    // DWM thumbnails cannot consume a Composition opacity animation themselves.
    if(!SetTimer(hwnd,89,std::min(16,end-elapsed),nullptr))ResetPanelMotion();
}
inline void NativePopup::SyncMediaMarquees(){
    if(kind!=L"@magic/media-popup"||!visible||dismissing||resizePainting){ClearMediaMarquees();return;}
    std::set<std::wstring> live;marqueeBytes=0;
    for(auto const &[_,m]:mediaMarquees)marqueeBytes+=m.bytes;
    for(auto const &r:rows)if(r.id.starts_with(L"track:")&&!r.offscreen){
        for(bool artist:{false,true}){
            auto id=r.id+(artist?L":artist":L"");auto &m=mediaMarquees[id];size_t prior=m.bytes;
            D2D1_RECT_F text{contentX+r.rect.left+36,contentY+r.rect.top+(artist?30:3),contentX+r.rect.right-6,contentY+(artist?r.rect.bottom-4:r.rect.top+30)};
            size_t used=marqueeBytes-prior,budget=used>=2*1024*1024?0:std::min<size_t>(512*1024,2*1024*1024-used);
            bool retained=m.Sync(app.graphics,popupRoot,artist?r.detail:r.label,text,{contentX+14,contentY+functionalTop,contentX+width-14,contentY+functionalBottom},dpi/96,dark,NativeMotion(app.model),budget,artist?11.5f:12.5f,artist?400:500,artist);
            marqueeBytes=used+m.bytes;if(retained)live.insert(id);
        }
    }
    for(auto i=mediaMarquees.begin();i!=mediaMarquees.end();)if(!live.contains(i->first)){i->second.Release();i=mediaMarquees.erase(i);}else ++i;
}
// One finite compositor rotation per accepted refresh, never a CPU frame loop.
inline void NativePopup::StopRefreshFeedback(){
    if(hwnd)KillTimer(hwnd,95);if(refreshVisual){refreshVisual.StopAnimation(L"RotationAngleInDegrees");if(refreshVisual.Parent())refreshVisual.Parent().Children().Remove(refreshVisual);}
    refreshVisual=nullptr;refreshSurface=nullptr;refreshId.clear();refreshUntil=0;refreshBytes=0;
}
inline void NativePopup::SyncRefreshFeedback(){
    if(refreshId.empty())return;if(GetTickCount64()>=refreshUntil||!visible||dismissing){StopRefreshFeedback();return;}
    auto row=std::find_if(rows.begin(),rows.end(),[&](auto const &r){return r.id==refreshId;});if(row==rows.end()||row->offscreen){StopRefreshFeedback();return;}
    if(refreshVisual){auto r=row->rect;float s=dpi/96;refreshVisual.Offset({(contentX+r.left+4)*s,(contentY+r.top+4)*s,0});}
}
inline bool NativePopup::StartRefreshFeedback(PopupRow const &row){
    if(!refreshId.empty()&&GetTickCount64()<refreshUntil)return false;
    StopRefreshFeedback();refreshId=row.id;refreshUntil=GetTickCount64()+900;
    if(!SetTimer(hwnd,95,900,nullptr)){StopRefreshFeedback();return true;}
    if(!NativeFeedbackMotion(app.model)||!popupRoot)return true;
    try{
        float w=row.rect.right-row.rect.left-8,h=row.rect.bottom-row.rect.top-8,s=dpi/96;int pw=static_cast<int>(std::ceil(w*s)),ph=static_cast<int>(std::ceil(h*s));
        if(pw<=0||ph<=0||static_cast<size_t>(pw)*ph*4>64*1024)return true;
        refreshSurface=app.graphics.composition.CreateDrawingSurface({static_cast<float>(pw),static_cast<float>(ph)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto draw=refreshSurface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT o{};check_hresult(draw->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&o));
        try{c->SetDpi(dpi,dpi);c->SetTransform(D2D1::Matrix3x2F::Translation(o.x/s,o.y/s));c->Clear(D2D1::ColorF(0,0));NativePaint p(c.get(),app.graphics,dark);p.Icon(L"refresh",{0,0,w,h},p.theme.MaterialMuted());check_hresult(draw->EndDraw());}catch(...){draw->EndDraw();throw;}
        refreshVisual=app.graphics.compositor.CreateSpriteVisual();refreshVisual.Size({pw*1.f,ph*1.f});refreshVisual.CenterPoint({pw/2.f,ph/2.f,0});refreshVisual.Brush(app.graphics.compositor.CreateSurfaceBrush(refreshSurface));popupRoot.Children().InsertAtTop(refreshVisual);refreshBytes=static_cast<size_t>(pw)*ph*4;SyncRefreshFeedback();
        auto spin=app.graphics.compositor.CreateScalarKeyFrameAnimation();spin.InsertKeyFrame(0,0);spin.InsertKeyFrame(1,1080,app.graphics.compositor.CreateLinearEasingFunction());spin.Duration(std::chrono::milliseconds(900));refreshVisual.StartAnimation(L"RotationAngleInDegrees",spin);
        Draw();
    }catch(...){if(refreshVisual&&refreshVisual.Parent())refreshVisual.Parent().Children().Remove(refreshVisual);refreshVisual=nullptr;refreshSurface=nullptr;refreshBytes=0;}
    return true;
}
