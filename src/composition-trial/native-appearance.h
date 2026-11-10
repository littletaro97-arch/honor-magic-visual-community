#pragma once
#include "trial.h"
#include "native-material.h"
#include "native-window-layers.h"
#include "native-motion.h"
#include "native-interaction.h"
#pragma comment(lib,"advapi32.lib")
#include <winrt/Windows.UI.ViewManagement.h>
// Native equivalents of preview70's colors.css, shared/index.scss, radius.css
// and spacings.css. No browser, screen sampling or per-frame theme query.
struct Preview70Theme
{
    static inline unsigned long long revision=0;
    static inline bool systemDark = false;
    static inline D2D1_COLOR_F accentLight = D2D1::ColorF(0x0067c0), accentDark = D2D1::ColorF(0x60cdff);
    static inline D2D1_COLOR_F systemAccent=D2D1::ColorF(0x0078d4);
    bool dark = false;
    static void Refresh()
    {
        ++revision;
        DWORD light = 1, bytes = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
        systemDark = light == 0;
        try {
            Windows::UI::ViewManagement::UISettings ui;
            auto convert = [](Windows::UI::Color v) { return D2D1::ColorF(v.R/255.f,v.G/255.f,v.B/255.f); };
            systemAccent = convert(ui.GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent));
            accentLight = convert(ui.GetColorValue(Windows::UI::ViewManagement::UIColorType::AccentDark1));
            accentDark = convert(ui.GetColorValue(Windows::UI::ViewManagement::UIColorType::AccentLight1));
        } catch (...) {} // Retain last valid Windows accent during session teardown.
    }
    D2D1_COLOR_F Gray(int level, float alpha=1) const
    {
        float l;
        switch(level) {
        case 25:l=dark?.18f:.99f;break;case 50:l=dark?.21f:.96f;break;case 75:l=dark?.24f:.93f;break;
        case 100:l=dark?.27f:.90f;break;case 200:l=dark?.36f:.81f;break;case 300:l=dark?.45f:.72f;break;
        case 400:l=dark?.54f:.63f;break;case 500:l=dark?.63f:.54f;break;case 600:l=dark?.72f:.45f;break;case 700:l=dark?.79f:.36f;break;case 800:l=dark?.86f:.27f;break;
        default:l=dark?.92f:.18f;break;
        }
        float linear=l*l*l,v=linear<=.0031308f?12.92f*linear:1.055f*std::pow(linear,1/2.4f)-.055f;
        return D2D1::ColorF(v,v,v,alpha);
    }
    D2D1_COLOR_F Surface()const{return Gray(50);}
    D2D1_COLOR_F Raised()const{return Gray(dark?75:25);}
    D2D1_COLOR_F Sunken()const{return Gray(dark?25:75);}
    D2D1_COLOR_F Ink()const{return Gray(900);}
    D2D1_COLOR_F Muted()const{return Gray(500);}
    D2D1_COLOR_F Accent(float alpha=1)const{auto c=dark?accentDark:accentLight;c.a=alpha;return c;}
    D2D1_COLOR_F OnAccent()const{return Gray(100);}
    D2D1_COLOR_F Material(NativeMaterial tier,bool glass,bool icons=false)const
    {
        // Opaque Dock uses the same light/dark surface as cards and panels.
        unsigned rgb=tier==NativeMaterial::Dock&&glass?(dark?0x808080:0xffffff):dark?0x202020:0xffffff;
        float alpha=tier==NativeMaterial::Card?(dark?.50f:.60f):tier==NativeMaterial::Menu?(dark?.70f:.76f):tier==NativeMaterial::Dock?.18f:icons?(dark?.64f:.72f):dark?.62f:.68f;
        return D2D1::ColorF(rgb,glass?alpha:1.f);
    }
    D2D1_COLOR_F MaterialMuted()const{return Gray(dark?600:700);}
    D2D1_COLOR_F Film(bool window=false)const{return D2D1::ColorF(0xffffff,window?(dark?.08f:.70f):(dark?.07f:.26f));}
    D2D1_COLOR_F SectionFilm()const{return D2D1::ColorF(dark?0xffffff:0x111118,dark?.065f:.035f);}
    D2D1_COLOR_F Edge()const{return D2D1::ColorF(dark?0xffffff:0x111118,dark?.20f:.085f);}
    D2D1_COLOR_F Highlight()const{return D2D1::ColorF(0xffffff,dark?.13f:.44f);}
    std::array<D2D1_GRADIENT_STOP,5> Contour()const{return {{{0,D2D1::ColorF(0xffffff,dark?.30f:.28f)},{.24f,D2D1::ColorF(0xffffff,dark?.18f:.12f)},{.52f,Edge()},{.78f,D2D1::ColorF(0xffffff,dark?.06f:.08f)},{1,D2D1::ColorF(0x101018,dark?.40f:.20f)}}};}
    D2D1_COLOR_F Danger()const{return D2D1::ColorF(dark?0xff8a90:0xc52a35);}
    D2D1_COLOR_F Folder()const{return D2D1::ColorF(dark?0x141414:0xffffff,dark?.78f:.84f);}
};
// The same finite D2D primitives are shared by live windows and the offscreen
// appearance gallery. Brushes die with the drawing context; no extra surfaces.
// Host backdrop transparency is controlled by Windows power policy. A legacy
// preference cannot keep it translucent after Windows has disabled sampling.
inline bool NativeLowPower(Model const &model){SYSTEM_POWER_STATUS power{};return model.saverKnown?model.saverActive:GetSystemPowerStatus(&power)&&power.SystemStatusFlag==1;}
// Explicit user-requested feedback may retain a light compositor-only transition
// under Energy Saver. The system and application animation switches still win.
inline bool NativeFeedbackMotion(Model const &model){BOOL enabled=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);return enabled&&Bool(model.preferences,L"windowAnimations",true);}
inline bool NativeMotion(Model const &model,bool folder=false){BOOL enabled=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);return enabled&&!NativeLowPower(model)&&Bool(model.preferences,folder?L"folderAnimations":L"windowAnimations",true);}
inline Windows::UI::Color NativeDropColor(Model const &model,bool dark){BYTE value=dark?0:255;return {static_cast<BYTE>(NativeLowPower(model)?255:184),value,value,value};}
struct NativePaint
{
    ID2D1DeviceContext *c;
    Graphics &graphics;
    Preview70Theme theme;
    com_ptr<ID2D1SolidColorBrush> brush;
    NativePaint(ID2D1DeviceContext *context,Graphics &g,bool dark):c(context),graphics(g),theme{dark}
    {check_hresult(c->CreateSolidColorBrush(theme.Ink(),brush.put()));}
    ID2D1SolidColorBrush *Color(D2D1_COLOR_F v){brush->SetColor(v);return brush.get();}
    void Fill(D2D1_RECT_F r,D2D1_COLOR_F color,float radius=NativeMetrics::ControlRadius)
    {c->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),Color(color));}
    void EmptyFrame(D2D1_RECT_F r,D2D1_COLOR_F edge){
        com_ptr<ID2D1Factory> factory;c->GetFactory(factory.put());com_ptr<ID2D1StrokeStyle> stroke;auto props=D2D1::StrokeStyleProperties();props.dashStyle=D2D1_DASH_STYLE_DASH;check_hresult(factory->CreateStrokeStyle(props,nullptr,0,stroke.put()));
        edge.a=.4f;c->DrawRoundedRectangle(D2D1::RoundedRect(r,10,10),Color(edge),1.5f,stroke.get());
    }
    void Line(float x1,float y1,float x2,float y2,D2D1_COLOR_F color,float width=1)
    {c->DrawLine({x1,y1},{x2,y2},Color(color),width);}
    void Text(std::wstring const &s,D2D1_RECT_F r,float size,D2D1_COLOR_F color,int weight=400,
              DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_LEADING,int face=0,bool wrap=false)
    {
        auto f=graphics.Format(size,align==DWRITE_TEXT_ALIGNMENT_CENTER,weight,face);
        auto old=f->GetTextAlignment();auto oldWrap=f->GetWordWrapping();auto oldVertical=f->GetParagraphAlignment();
        f->SetTextAlignment(align);f->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c->DrawTextW(s.c_str(),static_cast<UINT32>(s.size()),f,r,Color(color),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        f->SetTextAlignment(old);f->SetWordWrapping(oldWrap);f->SetParagraphAlignment(oldVertical);
    }
    void FileLabel(std::wstring const &text,D2D1_RECT_F bounds,float font=12){
        float line=font*1.4f,available=bounds.bottom-bounds.top;if(available<line)return;float h=std::min(2.f,std::floor(available/line))*line;
        auto format=graphics.Format(font,true);com_ptr<IDWriteTextLayout> layout;check_hresult(graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,bounds.right-bounds.left,h,layout.put()));
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,font*1.08f);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> ellipsis;check_hresult(graphics.text->CreateEllipsisTrimmingSign(format,ellipsis.put()));layout->SetTrimming(&trim,ellipsis.get());c->DrawTextLayout({bounds.left,bounds.top},layout.get(),Color(theme.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Material(D2D1_RECT_F r,NativeMaterial tier,bool glass,float radius=NativeMetrics::FloatingRadius,bool icons=false)
    {
        Fill(r,theme.Material(tier,glass,icons),radius);
        if(!glass){c->DrawRoundedRectangle(D2D1::RoundedRect({r.left+.5f,r.top+.5f,r.right-.5f,r.bottom-.5f},radius,radius),Color(theme.Edge()),1);return;}
        // Full curved perimeter, not a detached top line or an exterior band.
        auto stops=theme.Contour();
        com_ptr<ID2D1GradientStopCollection> colors;check_hresult(c->CreateGradientStopCollection(stops.data(),static_cast<UINT32>(stops.size()),colors.put()));com_ptr<ID2D1LinearGradientBrush> edge;
        check_hresult(c->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.left,r.top},{r.right,r.bottom}),colors.get(),edge.put()));
        c->DrawRoundedRectangle(D2D1::RoundedRect({r.left+.5f,r.top+.5f,r.right-.5f,r.bottom-.5f},radius,radius),edge.get(),1);
        edge->SetOpacity(.38f);edge->SetStartPoint({r.right,r.top});edge->SetEndPoint({r.left,r.bottom});
        float inner=std::max(0.f,radius-1);c->DrawRoundedRectangle(D2D1::RoundedRect({r.left+1.4f,r.top+1.4f,r.right-1.4f,r.bottom-1.4f},inner,inner),edge.get(),.65f);
    }
    void Shadow(D2D1_RECT_F r,float radius=NativeMetrics::FloatingRadius,float room=0.f)
    {
        // A contact contour plus soft halos that fade outwards. A white card
        // on a white window has no other way to announce its own edge, and the halo
        // belongs outside the material - inside it would be painted over.
        // Room is the margin the caller owns outside its material. A caller that
        // names none gets nothing: dock bodies and tray icons pass a bare rect, and
        // a halo there reads as a black ring around the icon rather than a shadow.
        if(room<1.5f)return;
        float ink=theme.dark?1.f:0.f,mix=theme.dark?.30f:1.f;
        // Eight rings instead of three: at this radius a stepped halo reads as a
        // banded edge, and one ring per pixel and a quarter disappears into a gradient.
        for(int ring=7;ring>=0;--ring){
            float t=(ring+1)/8.f,spread=room*t,drop=room*.34f*t;
            Fill({r.left-spread,r.top-spread+drop,r.right+spread,r.bottom+spread+drop},
                 D2D1::ColorF(ink,ink,ink,(.038f+.077f*(1.f-t))*mix),radius+spread*.8f);
        }
        // Half a pixel: the eye reads a contour, never a border.
        c->DrawRoundedRectangle(D2D1::RoundedRect({r.left-.5f,r.top-.5f,r.right+.5f,r.bottom+.5f},radius+.5f,radius+.5f),
                                Color(D2D1::ColorF(ink,ink,ink,theme.dark?.10f:.14f)),1.f);
    }
    void Interaction(NativeInteractionRegion const &region,bool selected,bool hovered=false,bool focused=false)
    {
        if(!region.Valid()||(!selected&&!hovered&&!focused))return;auto r=region.bounds;auto clip=region.Visible();
        c->PushAxisAlignedClip(clip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        D2D1_GRADIENT_STOP stops[]={{0,D2D1::ColorF(0xffffff,selected?(theme.dark?.20f:.18f):.12f)},{.45f,D2D1::ColorF(0xffffff,selected?.065f:.045f)},{1,D2D1::ColorF(0xffeedc,selected?.10f:.08f)}};
        com_ptr<ID2D1GradientStopCollection> colors;check_hresult(c->CreateGradientStopCollection(stops,3,colors.put()));com_ptr<ID2D1LinearGradientBrush> gradient;
        check_hresult(c->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.left,r.top},{r.left,r.bottom}),colors.get(),gradient.put()));
        c->FillRoundedRectangle(D2D1::RoundedRect(r,region.radius,region.radius),gradient.get());
        auto edge=D2D1::ColorF(theme.dark?0xffffff:0x2b2928,selected?(theme.dark?.62f:.34f):focused?.28f:.15f);
        c->DrawRoundedRectangle(D2D1::RoundedRect({r.left+.5f,r.top+.5f,r.right-.5f,r.bottom-.5f},region.radius,region.radius),Color(edge),selected?1.2f:1.f);
        // Selection shares one rounded perimeter; never add a white top stripe.
        c->PopAxisAlignedClip();
    }
    void Slider(D2D1_RECT_F r,double v,bool enabled=true)
    {
        v=std::clamp(v,0.,1.);float y=(r.top+r.bottom)/2,x=r.left+(r.right-r.left)*static_cast<float>(v);
        Fill({r.left-10,y-10,r.right+10,y+10},theme.SectionFilm(),10);
        if(v>0)Fill({r.left-10,y-10,x+10,y+10},theme.Accent(enabled?1.f:.4f),10);
        c->FillEllipse({{x,y+1},9.5f,9.5f},Color(D2D1::ColorF(0x000000,.12f)));
        c->FillEllipse({{x,y},9,9},Color(D2D1::ColorF(0xffffff,enabled?1.f:.65f)));
    }
    void Toggle(D2D1_RECT_F r,bool on,bool enabled=true)
    {
        // Match the settings switch: 40x23 DIP, white 19 DIP thumb, 2 DIP inset.
        Fill(r,on?D2D1::ColorF(Preview70Theme::systemAccent.r,Preview70Theme::systemAccent.g,Preview70Theme::systemAccent.b,enabled?1.f:.5f):theme.Muted(),12);
        float y=(r.top+r.bottom)/2,rad=std::min(9.5f,(r.bottom-r.top-4)/2);
        c->FillEllipse({{on?r.right-2-rad:r.left+2+rad,y},rad,rad},Color(D2D1::ColorF(0xffffff,enabled?1.f:.65f)));
    }
    void Ring(float x,float y,float radius,float value,D2D1_COLOR_F ink,float thickness=2)
    {
        value=Clamp(value,0,1);auto track=ink;track.a*=.18f;
        c->DrawEllipse({{x,y},radius,radius},Color(track),thickness);
        int n=std::max(1,static_cast<int>(std::ceil(value*64)));
        for(int i=0;i<n&&value>0;i++){float a=-1.5707963f+6.2831853f*value*i/n,b=-1.5707963f+6.2831853f*value*(i+1)/n;
            Line(x+radius*std::cos(a),y+radius*std::sin(a),x+radius*std::cos(b),y+radius*std::sin(b),ink,thickness);}
    }
    void Icon(std::wstring key,D2D1_RECT_F r,D2D1_COLOR_F ink)
    {
        D2D1_MATRIX_3X2_F old;c->GetTransform(&old);float s=std::min(r.right-r.left,r.bottom-r.top)/24;
        c->SetTransform(D2D1::Matrix3x2F::Scale(s,s)*D2D1::Matrix3x2F::Translation(r.left,r.top)*old);
        auto line=[&](float a,float b,float x,float y){Line(a,b,x,y,ink,1.7f);};
        auto circle=[&](float x,float y,float rad){c->DrawEllipse({{x,y},rad,rad},Color(ink),1.7f);};
        if(key==L"left"||key==L"right"){bool left=key==L"left";line(left?15:9,5,left?8:16,12);line(left?8:16,12,left?15:9,19);}
        else if(key==L"cut"){circle(6,6,3);circle(6,18,3);line(8,8,20,20);line(8,16,20,4);}
        else if(key==L"copy"||key==L"copyaspath"){c->DrawRoundedRectangle(D2D1::RoundedRect({8,8,21,21},2,2),Color(ink),1.7f);line(4,16,4,3);line(4,3,17,3);}
        else if(key==L"paste"){c->DrawRoundedRectangle(D2D1::RoundedRect({5,5,20,22},2,2),Color(ink),1.7f);Fill({9,2,16,7},theme.Raised(),1);c->DrawRectangle({9,2,16,7},Color(ink),1.5f);}
        else if(key==L"delete"){line(4,6,20,6);line(9,3,15,3);line(6,8,8,21);line(8,21,17,21);line(17,21,19,8);line(10,10,11,18);line(15,10,14,18);}
        else if(key==L"rename"){c->DrawRectangle({3,6,21,19},Color(ink),1.5f);line(12,3,12,22);line(9,3,15,3);line(9,22,15,22);}
        else if(key==L"sharing"||key==L"modernsharing"||key==L"share"){line(4,13,4,21);line(4,21,20,21);line(20,21,20,13);line(12,16,12,3);line(12,3,6,9);line(12,3,18,9);}
        else if(key==L"open"||key==L"folder"){line(3,19,3,5);line(3,5,10,5);line(10,5,13,8);line(13,8,21,8);line(21,8,21,19);line(21,19,3,19);}
        else if(key==L"more"){for(int i=0;i<3;i++)c->FillEllipse({{5+i*7.f,12},1.5f,1.5f},Color(ink));}
        else if(key==L"add"){line(12,4,12,20);line(4,12,20,12);}
        else if(key==L"resize"){line(3,10,3,3);line(3,3,10,3);line(3,3,10,10);line(21,14,21,21);line(21,21,14,21);line(21,21,14,14);}
        else if(key.find(L"bluetooth")!=key.npos){line(12,3,18,8);line(18,8,7,17);line(7,7,18,16);line(18,16,12,21);line(12,21,12,3);}
        else if(key.find(L"network")!=key.npos||key.find(L"wifi")!=key.npos){for(float radius:{15.6f,10.8f,6.f})for(int i=0;i<10;i++){
            float a=(-130+i*8.f)*3.14159265f/180,b=(-122+i*8.f)*3.14159265f/180;line(12+radius*std::cos(a),20+radius*std::sin(a),12+radius*std::cos(b),20+radius*std::sin(b));}c->FillEllipse({{12,20},1.4f,1.4f},Color(ink));}
        else if(key.find(L"battery")!=key.npos||key.find(L"@default/power")!=key.npos){c->DrawRoundedRectangle(D2D1::RoundedRect({2,7,20,17},2,2),Color(ink),1.5f);Fill({4,9,15,15},ink,1);line(22,10,22,14);}
        else if(key==L"power"||key==L"shutdown"||key==L"exit"){circle(12,13,8);Fill({9,2,15,9},theme.Surface(),0);line(12,2,12,12);}
        else if(key.find(L"notification")!=key.npos){line(6,17,6,9);line(6,9,9,5);line(9,5,15,5);line(15,5,18,9);line(18,9,18,17);line(4,18,20,18);line(10,21,14,21);line(12,2,12,5);}
        else if(key.find(L"quick")!=key.npos||key==L"settings"){line(3,6,21,6);line(3,12,21,12);line(3,18,21,18);circle(8,6,2.5f);circle(16,12,2.5f);circle(10,18,2.5f);}
        else if(key.find(L"tray")!=key.npos){circle(12,12,9);line(8,10,12,14);line(12,14,16,10);}
        else if(key.find(L"user")!=key.npos||key==L"account"){circle(12,7,4);c->DrawRoundedRectangle(D2D1::RoundedRect({4,14,20,22},6,6),Color(ink),1.7f);}
        else if(key.find(L"calendar")!=key.npos){c->DrawRoundedRectangle(D2D1::RoundedRect({3,5,21,22},3,3),Color(ink),1.7f);line(3,10,21,10);line(8,2,8,7);line(16,2,16,7);}
        else if(key.find(L"keyboard")!=key.npos){c->DrawRoundedRectangle(D2D1::RoundedRect({2,5,22,20},2,2),Color(ink),1.5f);for(int i=0;i<4;i++){line(5+i*4.f,9,6+i*4.f,9);line(5+i*4.f,13,6+i*4.f,13);}line(7,17,17,17);}
        else if(key==L"lock"){c->DrawRoundedRectangle(D2D1::RoundedRect({5,10,19,22},2,2),Color(ink),1.7f);circle(12,8,5);line(12,15,12,18);}
        else if(key==L"sleep"||key==L"night"||key==L"dark"){
            // A hollow crescent, without an opaque erase disk over glass.
            com_ptr<ID2D1Factory> factory;c->GetFactory(factory.put());com_ptr<ID2D1PathGeometry> moon;check_hresult(factory->CreatePathGeometry(moon.put()));com_ptr<ID2D1GeometrySink> path;check_hresult(moon->Open(path.put()));
            path->BeginFigure({13,3},D2D1_FIGURE_BEGIN_HOLLOW);path->AddBezier(D2D1::BezierSegment({1,2},{0,21},{12,21}));path->AddBezier(D2D1::BezierSegment({18,21},{21,17},{21,12}));
            path->AddBezier(D2D1::BezierSegment({11,18},{6,9},{13,3}));path->EndFigure(D2D1_FIGURE_END_CLOSED);check_hresult(path->Close());c->DrawGeometry(moon.get(),Color(ink),1.7f);
        }
        else if(key==L"eye"){line(2,12,7,6);line(7,6,17,6);line(17,6,22,12);line(22,12,17,18);line(17,18,7,18);line(7,18,2,12);circle(12,12,4);}
        else if(key==L"brightness"){circle(12,12,4);for(int i=0;i<8;i++){float a=i*3.14159265f/4;line(12+7*std::cos(a),12+7*std::sin(a),12+10*std::cos(a),12+10*std::sin(a));}}
        else if(key==L"pin"||key==L"unpin"){line(7,4,17,4);line(8,4,9,11);line(16,4,15,11);line(9,11,6,15);line(6,15,18,15);line(18,15,15,11);line(12,15,12,22);if(key==L"unpin")line(3,3,21,21);}
        else if(key==L"pause"){Fill({6,4,10,20},ink,1);Fill({14,4,18,20},ink,1);}
        else if(key==L"edit"){line(4,20,5,15);line(5,15,16,4);line(16,4,20,8);line(20,8,9,19);line(9,19,4,20);line(13,7,17,11);}
        else if(key==L"image"){c->DrawRoundedRectangle(D2D1::RoundedRect({3,4,21,20},3,3),Color(ink),1.7f);circle(8,9,1.5f);line(4,18,10,12);line(10,12,14,16);line(14,16,17,12);line(17,12,21,16);}
        else if(key==L"location"){circle(12,9,5);circle(12,9,1.5f);line(8,13,12,21);line(12,21,16,13);}
        else if(key==L"close"){line(6,6,18,18);line(18,6,6,18);}
        else if(key==L"previous"||key==L"back"){line(15,5,8,12);line(8,12,15,19);}
        else if(key==L"next"){line(9,5,16,12);line(16,12,9,19);}
        else if(key==L"check"){line(4,12,10,18);line(10,18,21,5);}
        else if(key==L"restart"||key==L"update"||key==L"refresh"){circle(12,12,8);line(20,3,20,10);line(20,10,13,10);}
        else if(key==L"apps"||key.find(L"start")!=key.npos){for(int i=0;i<4;i++)Fill({3+(i%2)*11.f,3+(i/2)*11.f,10+(i%2)*11.f,10+(i/2)*11.f},ink,1);}
        else if(key==L"search"){circle(10,10,6);line(15,15,22,22);}
        else if(key.find(L"volume")!=key.npos||key.find(L"media")!=key.npos){line(3,9,7,9);line(7,9,12,4);line(12,4,12,20);line(12,20,7,15);line(7,15,3,15);line(3,15,3,9);line(16,7,19,10);line(19,10,19,14);line(19,14,16,17);}
        else if(key==L"play"){line(8,4,20,12);line(20,12,8,20);line(8,20,8,4);}
        else if(key==L"feedback"){c->DrawRoundedRectangle(D2D1::RoundedRect({3,3,21,18},3,3),Color(ink),1.7f);line(6,18,6,23);line(6,23,12,18);line(8,8,16,8);line(8,12,15,12);}
        else if(key==L"about"){circle(12,12,9);line(12,10,12,17);circle(12,6,0.7f);}
        else if(key==L"gauge"){circle(12,12,9);line(12,12,15.5f,8.5f);}
        else if(key==L"temperature"){line(10,13,10,5);line(14,5,14,13);circle(12,5,2);circle(12,16.5f,4);}
        else if(key==L"storage"){c->DrawRoundedRectangle(D2D1::RoundedRect({3,4,21,20},3,3),Color(ink),1.7f);line(3,14,21,14);line(16,17,18,17);}
        else {c->DrawRoundedRectangle(D2D1::RoundedRect({4,4,20,20},3,3),Color(ink),1.7f);line(8,9,16,9);line(8,15,16,15);}
        c->SetTransform(old);
    }
};
struct Preview70Icons
{
    std::map<std::wstring,Icon> values;
    size_t bytes=0;long long age=0;std::set<std::wstring> glyphs;
    void Clear(){values.clear();glyphs.clear();bytes=0;}
    void Load(Graphics &g,std::wstring const &key,std::wstring const &png,bool glyph=false)
    {
        DWORD n=0;
        if(png.empty()||!CryptStringToBinaryW(png.c_str(),static_cast<DWORD>(png.size()),CRYPT_STRING_BASE64,nullptr,&n,nullptr,nullptr)||n>65536)return;
        std::vector<BYTE> data(n);if(!CryptStringToBinaryW(png.c_str(),static_cast<DWORD>(png.size()),CRYPT_STRING_BASE64,data.data(),&n,nullptr,nullptr))return;
        com_ptr<IStream> stream;stream.attach(SHCreateMemStream(data.data(),n));com_ptr<IWICBitmapDecoder> decoder;
        if(FAILED(g.wic->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.put())))return;
        com_ptr<IWICBitmapFrameDecode> frame;if(FAILED(decoder->GetFrame(0,frame.put())))return;UINT w=0,h=0;
        frame->GetSize(&w,&h);if(!w||!h||w>256||h>256)return;size_t size=static_cast<size_t>(w)*h*4;
        if(auto old=values.find(key);old!=values.end()){bytes-=old->second.bytes;glyphs.erase(old->first);values.erase(old);}
        while(!values.empty()&&(values.size()>=128||bytes+size>8*1024*1024)){
            auto old=std::min_element(values.begin(),values.end(),[](auto const &a,auto const &b){return a.second.age<b.second.age;});
            bytes-=old->second.bytes;glyphs.erase(old->first);values.erase(old);}
        com_ptr<IWICFormatConverter> converter;check_hresult(g.wic->CreateFormatConverter(converter.put()));
        check_hresult(converter->Initialize(frame.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
        com_ptr<ID2D1DeviceContext> c;check_hresult(g.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,c.put()));
        Icon icon;check_hresult(c->CreateBitmapFromWicBitmap(converter.get(),nullptr,icon.bitmap.put()));icon.bytes=size;icon.age=++age;
        values[key]=std::move(icon);if(glyph)glyphs.insert(key);bytes+=size;
    }
    bool Draw(ID2D1DeviceContext *c,std::wstring const &key,D2D1_RECT_F r,ID2D1Brush *tint=nullptr)
    {auto found=values.find(key);if(found==values.end())return false;found->second.age=++age;
        if(tint&&glyphs.contains(key)){auto mode=c->GetAntialiasMode();c->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);c->FillOpacityMask(found->second.bitmap.get(),tint,&r,nullptr);c->SetAntialiasMode(mode);}
        else c->DrawBitmap(found->second.bitmap.get(),r,1,D2D1_INTERPOLATION_MODE_LINEAR);return true;}
};
