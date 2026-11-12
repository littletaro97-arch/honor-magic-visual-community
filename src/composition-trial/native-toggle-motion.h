#pragma once
// Shared native switch feedback. Two short compositor animations, no frame
// timer/surface redraw loop; each popup releases these with its other visuals.
struct NativeToggleVisual {
    ContainerVisual root{nullptr};SpriteVisual off{nullptr},on{nullptr},thumb{nullptr};
    CompositionRoundedRectangleGeometry track{nullptr},knob{nullptr};bool value=false;float lastScale=0,lastWidth=0,lastHeight=0;
    void Release(){if(root&&root.Parent())root.Parent().Children().Remove(root);root=nullptr;off=nullptr;on=nullptr;thumb=nullptr;track=nullptr;knob=nullptr;}
    void Sync(Compositor const &c,ContainerVisual const &parent,D2D1_RECT_F r,D2D1_RECT_F viewport,float scale,bool next,bool enabled,bool motion,Preview70Theme const &theme){
        auto color=[](D2D1_COLOR_F v){return Windows::UI::Color{static_cast<BYTE>(v.a*255),static_cast<BYTE>(v.r*255),static_cast<BYTE>(v.g*255),static_cast<BYTE>(v.b*255)};};
        bool fresh=!root;if(fresh){root=c.CreateContainerVisual();off=c.CreateSpriteVisual();on=c.CreateSpriteVisual();thumb=c.CreateSpriteVisual();track=c.CreateRoundedRectangleGeometry();knob=c.CreateRoundedRectangleGeometry();off.Clip(c.CreateGeometricClip(track));on.Clip(c.CreateGeometricClip(track));thumb.Clip(c.CreateGeometricClip(knob));root.Children().InsertAtTop(off);root.Children().InsertAtTop(on);root.Children().InsertAtTop(thumb);parent.Children().InsertAtTop(root);}
        float w=(r.right-r.left)*scale,h=(r.bottom-r.top)*scale,d=std::min(19*scale,h-4*scale),x=next?w-2*scale-d:2*scale;
        root.Size({w,h});root.Offset({r.left*scale,r.top*scale,0});root.Opacity(enabled?1.f:.5f);
        root.Clip(c.CreateInsetClip(std::max(0.f,viewport.left-r.left)*scale,std::max(0.f,viewport.top-r.top)*scale,std::max(0.f,r.right-viewport.right)*scale,std::max(0.f,r.bottom-viewport.bottom)*scale));
        off.Size({w,h});on.Size({w,h});track.Size({w,h});track.CornerRadius({h/2,h/2});thumb.Size({d,d});knob.Size({d,d});knob.CornerRadius({d/2,d/2});
        off.Brush(c.CreateColorBrush(color(theme.Muted())));on.Brush(c.CreateColorBrush(color(theme.Accent())));thumb.Brush(c.CreateColorBrush(Windows::UI::Color{255,255,255,255}));
        if(fresh||!motion||scale!=lastScale||w!=lastWidth||h!=lastHeight){on.StopAnimation(L"Opacity");thumb.StopAnimation(L"Offset");on.Opacity(next?1.f:0.f);thumb.Offset({x,(h-d)/2,0});}
        else if(next!=value){NativeInteractionMotion::Opacity(c,on,next?1.f:0.f,180);NativeInteractionMotion::Vector(c,thumb,L"Offset",{x,(h-d)/2,0},180);}
        value=next;lastScale=scale;lastWidth=w;lastHeight=h;
    }
};
