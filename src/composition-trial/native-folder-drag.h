#pragma once
#include <cfloat>
// Pure insertion plan, shared by the live preview and final authenticated request.
struct NativeFolderPlan {
    std::vector<std::wstring> order;int insertion=-1,nearest=-1;
    static NativeFolderPlan Build(std::vector<Tile> const &tiles,std::set<std::wstring> const &keys,std::wstring const &anchor,int nearest){
        NativeFolderPlan p;if(tiles.empty()||!keys.contains(anchor))return p;
        std::vector<std::wstring> selected,rest;int rank=-1;
        for(auto const &t:tiles){if(keys.contains(t.key)){if(t.key==anchor)rank=static_cast<int>(selected.size());selected.push_back(t.key);}else rest.push_back(t.key);}
        if(rank<0||selected.size()!=keys.size())return p;
        p.nearest=std::clamp(nearest,0,static_cast<int>(tiles.size())-1);p.insertion=std::clamp(p.nearest-rank,0,static_cast<int>(rest.size()));p.order=std::move(rest);p.order.insert(p.order.begin()+p.insertion,selected.begin(),selected.end());return p;
    }
};
struct NativeFolderAvoidance {
    struct Item{SpriteVisual visual{nullptr};CompositionDrawingSurface surface{nullptr};int slot=-1;};
    ContainerVisual layer{nullptr};std::map<std::wstring,Item> items;size_t bytes=0;
    void Release(ContainerVisual const &parent){if(layer&&parent)parent.Children().Remove(layer);layer=nullptr;items.clear();bytes=0;}
    bool Sync(Window &w,NativeFolderPlan const &plan){
        auto &g=w.app.graphics;float scale=w.dpi/96;auto const &tiles=w.Tiles();
        try{
            if(!layer){layer=g.compositor.CreateContainerVisual();w.folderMotion.Children().InsertAtTop(layer);layer.Size(w.panel.Size());auto clip=g.compositor.CreateInsetClip();clip.LeftInset((w.contentRect.left-w.panelRect.left)*scale);clip.TopInset((w.contentRect.top-w.panelRect.top)*scale);clip.RightInset((w.panelRect.right-w.contentRect.right)*scale);clip.BottomInset((w.panelRect.bottom-w.contentRect.bottom)*scale);layer.Clip(clip);}
            for(int i=0;i<static_cast<int>(plan.order.size());++i){auto const &key=plan.order[i];if(w.folderPreviewKeys.contains(key))continue;
                auto original=std::find_if(tiles.begin(),tiles.end(),[&](auto const &t){return t.key==key;});if(original==tiles.end())continue;int from=static_cast<int>(original-tiles.begin());
                auto start=FolderCellBounds(w.contentRect,w.columns,w.scrollRow,from),dest=FolderCellBounds(w.contentRect,w.columns,w.scrollRow,i);
                auto visible=[&](D2D1_RECT_F r){return r.bottom>w.contentRect.top&&r.top<w.contentRect.bottom;};if(!visible(start)&&!visible(dest))continue;
                float cw=start.right-start.left,ch=start.bottom-start.top;auto found=items.find(key);
                if(found==items.end()){
                    int pw=static_cast<int>(std::ceil(cw*scale)),ph=static_cast<int>(std::ceil(ch*scale));size_t cost=static_cast<size_t>(pw)*ph*4;
                    if(items.size()>=128||bytes+cost>8*1024*1024||w.estimatedSurfaceBytes+bytes+cost>SurfaceLimit){Release(w.folderMotion);return false;}
                    Item item;item.slot=from;item.surface=g.composition.CreateDrawingSurface({static_cast<float>(pw),static_cast<float>(ph)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
                    auto draw=item.surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};check_hresult(draw->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));
                    bool ended=false;try{c->SetDpi(w.dpi,w.dpi);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x/scale,offset.y/scale));c->Clear(D2D1::ColorF(0,0));w.PaintTile(c.get(),*original,{0,0,cw,ch},from,true,true);auto result=draw->EndDraw();ended=true;check_hresult(result);}catch(...){if(!ended)draw->EndDraw();throw;}
                    item.visual=g.compositor.CreateSpriteVisual();item.visual.Size({static_cast<float>(pw),static_cast<float>(ph)});item.visual.Brush(g.compositor.CreateSurfaceBrush(item.surface));item.visual.Offset({(start.left-w.panelRect.left)*scale,(start.top-w.panelRect.top)*scale,0});layer.Children().InsertAtTop(item.visual);found=items.emplace(key,std::move(item)).first;bytes+=cost;
                }
                auto &item=found->second;if(item.slot!=i){auto a=g.compositor.CreateVector3KeyFrameAnimation();a.InsertExpressionKeyFrame(0,L"this.StartingValue");a.InsertKeyFrame(1,{(dest.left-w.panelRect.left)*scale,(dest.top-w.panelRect.top)*scale,0},g.compositor.CreateCubicBezierEasingFunction({.22f,1},{.28f,1}));a.Duration(std::chrono::milliseconds(260));item.visual.StartAnimation(L"Offset",a);item.slot=i;}
            }
            return true;
        }catch(...){Release(w.folderMotion);return false;}
    }
};
inline int Window::FolderDropIndex(JsonObject const &drag,float x,float y){
    auto const &tiles=Tiles();if(tiles.empty())return -1;auto cell=FolderCellBounds(contentRect,columns,scrollRow,0);
    float cx=x-static_cast<float>(Num(drag,L"grabX"))+(cell.right-cell.left)/2,cy=y-static_cast<float>(Num(drag,L"grabY"))+(cell.bottom-cell.top)/2;
    int best=0;float distance=FLT_MAX;for(int i=0;i<static_cast<int>(tiles.size());++i){auto r=FolderCellBounds(contentRect,columns,scrollRow,i);float dx=cx-(r.left+r.right)/2,dy=(cy-(r.top+r.bottom)/2)*1.25f,d=dx*dx+dy*dy;if(d<distance){distance=d;best=i;}}return best;
}
inline void Window::ResetFolderAvoidance(){if(folderAvoidance){folderAvoidance->Release(folderMotion);folderAvoidance.reset();}if(folderPreviewActive)panelStamp.valid=false;folderPreviewActive=folderPreviewCommitted=false;folderPreviewOrder.clear();folderPreviewKeys.clear();folderPreviewStamp.clear();}
inline bool Window::PreviewFolderDrag(JsonObject const &drag,float x,float y){
    std::set<std::wstring> keys;for(auto const &key:drag.GetNamedArray(L"keys"))keys.insert(std::wstring(key.GetString()));auto plan=NativeFolderPlan::Build(Tiles(),keys,Str(drag,L"anchor"),FolderDropIndex(drag,x,y));if(plan.insertion<0)return false;
    auto stamp=std::wstring(drag.Stringify())+L":"+std::to_wstring(plan.insertion)+L":"+std::to_wstring(scrollRow);if(stamp==folderPreviewStamp)return true;
    bool first=!folderPreviewActive;folderPreviewActive=true;folderPreviewRevision=app.model.revision;folderPreviewOrder=plan.order;folderPreviewKeys=std::move(keys);folderPreviewStamp=stamp;
    if(interactionHover){interactionHover->Release(root);interactionHover.reset();}
    if(NativeMotion(app.model,true)){if(!folderAvoidance)folderAvoidance=std::make_shared<NativeFolderAvoidance>();if(!folderAvoidance->Sync(*this,plan))folderAvoidance.reset();}
    else if(folderAvoidance){folderAvoidance->Release(folderMotion);folderAvoidance.reset();}
    if(first||!folderAvoidance){panelStamp.valid=false;Draw(false);}return true;
}
