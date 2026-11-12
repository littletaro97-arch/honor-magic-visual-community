#pragma once
namespace NativeMediaMarquee {inline constexpr float Speed=16,Gap=28;}
// Retained text only: rasterize on title/geometry/theme changes, interpolate the
// offset on the compositor. No timer, no repaint per frame, no hidden loop.
struct NativeMarquee {
    ContainerVisual clip{nullptr};SpriteVisual text{nullptr};CompositionDrawingSurface surface{nullptr};size_t bytes=0;std::wstring key;
    void Release(){if(text)text.StopAnimation(L"Offset");if(clip&&clip.Parent())clip.Parent().Children().Remove(clip);clip=nullptr;text=nullptr;surface=nullptr;bytes=0;key.clear();}
    bool Sync(Graphics &g,ContainerVisual const &parent,std::wstring title,D2D1_RECT_F r,D2D1_RECT_F viewport,float scale,bool dark,bool enabled,size_t budget,float font=12.5f,int weight=500,bool muted=false){
        float w=r.right-r.left,h=r.bottom-r.top;if(title.size()>512)title.resize(512);
        com_ptr<IDWriteTextLayout> layout;auto format=g.Format(font,false,weight);check_hresult(g.text->CreateTextLayout(title.c_str(),static_cast<UINT32>(title.size()),format,16384,128,layout.put()));layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);DWRITE_TEXT_METRICS metrics{};check_hresult(layout->GetMetrics(&metrics));
        float tw=std::ceil(metrics.widthIncludingTrailingWhitespace)+3;
        if(!enabled||w<8||h<=0||tw<=w+.5f||r.bottom<=viewport.top||r.top>=viewport.bottom){Release();return false;}
        auto stamp=title+L":"+std::to_wstring(w)+L":"+std::to_wstring(h)+L":"+std::to_wstring(font)+L":"+std::to_wstring(weight)+L":"+std::to_wstring(muted)+L":"+std::to_wstring(scale)+L":"+std::to_wstring(dark)+L":"+std::to_wstring(Preview70Theme::revision);
        if(key!=stamp){
            Release();float period=tw+NativeMediaMarquee::Gap,renderWidth=period*2;
            float raster=std::min(scale,std::sqrt(std::max(0.f,static_cast<float>(budget)-8192)/std::max(1.f,renderWidth*h*4)));if(raster<.35f)return false;
            int pw=static_cast<int>(std::ceil(renderWidth*raster)),ph=static_cast<int>(std::ceil(h*raster));if(static_cast<size_t>(pw)*ph*4>budget)return false;
            surface=g.composition.CreateDrawingSurface({pw*1.f,ph*1.f},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            auto draw=surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT o{};check_hresult(draw->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&o));
            try{c->SetDpi(96*raster,96*raster);c->SetTransform(D2D1::Matrix3x2F::Translation(o.x/raster,o.y/raster));c->Clear(D2D1::ColorF(0,0));NativePaint p(c.get(),g,dark);auto ink=muted?p.theme.MaterialMuted():p.theme.Ink();p.Text(title,{0,0,tw,h},font,ink,weight);p.Text(title,{period,0,period+tw,h},font,ink,weight);check_hresult(draw->EndDraw());}catch(...){draw->EndDraw();throw;}
            clip=g.compositor.CreateContainerVisual();text=g.compositor.CreateSpriteVisual();text.Brush(g.compositor.CreateSurfaceBrush(surface));text.Size({renderWidth*scale,h*scale});clip.Children().InsertAtTop(text);parent.Children().InsertAtTop(clip);
            auto a=g.compositor.CreateVector3KeyFrameAnimation();a.InsertKeyFrame(0,{0,0,0});a.InsertKeyFrame(1,{-period*scale,0,0},g.compositor.CreateLinearEasingFunction());a.Duration(std::chrono::milliseconds(static_cast<int>(period/NativeMediaMarquee::Speed*1000)));a.IterationBehavior(AnimationIterationBehavior::Forever);text.StartAnimation(L"Offset",a);bytes=static_cast<size_t>(pw)*ph*4;key=stamp;
        }
        clip.Offset({r.left*scale,r.top*scale,0});clip.Size({w*scale,h*scale});auto crop=g.compositor.CreateInsetClip();crop.TopInset(std::max(0.f,viewport.top-r.top)*scale);crop.BottomInset(std::max(0.f,r.bottom-viewport.bottom)*scale);clip.Clip(crop);return true;
    }
};
