#pragma once
#include <d2d1effects.h>
// Native equivalent of weg.scss and the built-in item components. Item and
// indicator visuals are cached separately so status transitions use Composition.
inline void NativeDockBitmap(ID2D1DeviceContext *c,ID2D1Bitmap1 *bitmap,D2D1_RECT_F r,bool hover=false,bool background=false) {
    if(!hover&&!background){c->DrawBitmap(bitmap,r,1,D2D1_INTERPOLATION_MODE_LINEAR);return;}
    float saturation=hover?1.1f:1,brightness=background?1.5625f:hover?1.05f:1,offset=background?-.125f:0;
    D2D1_MATRIX_5X4_F matrix{
        (.213f+.787f*saturation)*brightness,(.213f-.213f*saturation)*brightness,(.213f-.213f*saturation)*brightness,0,
        (.715f-.715f*saturation)*brightness,(.715f+.285f*saturation)*brightness,(.715f-.715f*saturation)*brightness,0,
        (.072f-.072f*saturation)*brightness,(.072f-.072f*saturation)*brightness,(.072f+.928f*saturation)*brightness,0,
        0,0,0,1,offset,offset,offset,0};
    com_ptr<ID2D1Effect> color,transform,blur;check_hresult(c->CreateEffect(CLSID_D2D1ColorMatrix,color.put()));check_hresult(D2D1::SetDpiCompensatedEffectInput(c,color.get(),0,bitmap));check_hresult(color->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,matrix));
    check_hresult(c->CreateEffect(CLSID_D2D12DAffineTransform,transform.put()));transform->SetInputEffect(0,color.get());auto size=bitmap->GetSize();
    check_hresult(transform->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,D2D1::Matrix3x2F::Scale((r.right-r.left)/size.width,(r.bottom-r.top)/size.height)*D2D1::Matrix3x2F::Translation(r.left,r.top)));
    if(background){check_hresult(c->CreateEffect(CLSID_D2D1GaussianBlur,blur.put()));blur->SetInputEffect(0,transform.get());check_hresult(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,10.f));check_hresult(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD));c->DrawImage(blur.get());}
    else c->DrawImage(transform.get());
}
inline D2D1_COLOR_F NativeDockBadgeColor(bool dark,std::wstring const &name){
    float l=dark?.63f:.54f,h=name==L"green"?150.f:name==L"orange"?50.f:30.f,c=name==L"green"?(dark?.193f:.166f):name==L"orange"?(dark?.151f:.129f):(dark?.225f:.193f);
    float a=c*std::cos(h*.0174532925f),b=c*std::sin(h*.0174532925f),x=l+.3963377774f*a+.2158037573f*b,y=l-.1055613458f*a-.0638541728f*b,z=l-.0894841775f*a-1.291485548f*b;x=x*x*x;y=y*y*y;z=z*z*z;
    auto channel=[](float value){return std::clamp(value<=.0031308f?12.92f*value:1.055f*std::pow(value,1/2.4f)-.055f,0.f,1.f);};return D2D1::ColorF(channel(4.0767416621f*x-3.3077115913f*y+.2309699292f*z),channel(-1.2684380046f*x+2.6097574011f*y-.3413193965f*z),channel(-.0041960863f*x-.7034186147f*y+1.707614701f*z));
}
inline void NativeDockPath(NativePaint &p,std::vector<D2D1_POINT_2F> const &points,D2D1_COLOR_F color,float stroke=0) {
    if(points.empty())return;com_ptr<ID2D1Factory> factory;p.c->GetFactory(factory.put());com_ptr<ID2D1PathGeometry> path;check_hresult(factory->CreatePathGeometry(path.put()));com_ptr<ID2D1GeometrySink> sink;check_hresult(path->Open(sink.put()));sink->BeginFigure(points.front(),stroke?D2D1_FIGURE_BEGIN_HOLLOW:D2D1_FIGURE_BEGIN_FILLED);for(size_t i=1;i<points.size();++i)sink->AddLine(points[i]);sink->EndFigure(stroke?D2D1_FIGURE_END_OPEN:D2D1_FIGURE_END_CLOSED);check_hresult(sink->Close());
    if(stroke){com_ptr<ID2D1StrokeStyle> style;auto props=D2D1::StrokeStyleProperties();props.startCap=props.endCap=D2D1_CAP_STYLE_ROUND;props.lineJoin=D2D1_LINE_JOIN_ROUND;check_hresult(factory->CreateStrokeStyle(props,nullptr,0,style.put()));p.c->DrawGeometry(path.get(),p.Color(color),stroke,style.get());}else p.c->FillGeometry(path.get(),p.Color(color));
}
inline void NativeDockCanvas(NativePaint &p,std::wstring const &rawPlugin,JsonObject const &item,D2D1_RECT_F r,std::wstring const &side) {
    auto plugin=CanonicalNativeBarPlugin(rawPlugin);
    D2D1_MATRIX_3X2_F old;p.c->GetTransform(&old);float w=r.right-r.left,h=r.bottom-r.top;p.c->SetTransform(D2D1::Matrix3x2F::Translation(r.left,r.top)*old);auto ink=p.theme.Ink();
    if(plugin==L"@magic/dock-system-tray"){
        float cx=w/2,cy=h/2,s=w*.22f;std::vector<D2D1_POINT_2F> points;
        if(side==L"Top")points={{cx-s,cy-s/2},{cx,cy+s/2},{cx+s,cy-s/2}};else if(side==L"Left")points={{cx-s/2,cy-s},{cx+s/2,cy},{cx-s/2,cy+s}};else if(side==L"Right")points={{cx+s/2,cy-s},{cx-s/2,cy},{cx+s/2,cy+s}};else points={{cx-s,cy+s/2},{cx,cy-s/2},{cx+s,cy+s/2}};NativeDockPath(p,points,ink,w*.085f);
    }else if(plugin==L"@magic/dock-calendar-popup"){
        SYSTEMTIME now{};GetLocalTime(&now);float cx=w/2,cy=h/2,line=w*.06f,radius=std::min(w,h)/2-line/2;
        p.c->DrawEllipse(D2D1::Ellipse({cx,cy},radius,radius),p.Color(ink),line);
        auto hand=[&](float angle,float length,float stroke){NativeDockPath(p,{{cx,cy},{cx+std::cos(angle)*length,cy+std::sin(angle)*length}},ink,stroke);};
        float minute=now.wMinute+now.wSecond/60.f;
        hand(((now.wHour%12)+minute/60)*.5235987756f-1.570796327f,radius*.5f,line/1.5f);
        hand(minute*.1047197551f-1.570796327f,radius*.75f,line/2);
        p.c->FillEllipse(D2D1::Ellipse({cx,cy},line,line),p.Color(ink));
    }else if(plugin==L"@magic/dock-media-popup"){
        float cy=h/2,line=w*.06f,body=w*.2f,x=w*.06f;bool muted=Bool(item,L"dockMuted");float volume=muted?0:static_cast<float>(Num(item,L"dockVolume"));
        NativeDockPath(p,{{x,cy-h*.17f},{x+body,cy-h*.17f},{w*.5f,cy-h*.31f},{w*.5f,cy+h*.31f},{x+body,cy+h*.17f},{x,cy+h*.17f}},ink);
        for(int n=0;n<3;n++){float radius=w*.14f+n*(w*.52f-line/2-w*.14f)/2;std::vector<D2D1_POINT_2F> points;
            for(int j=0;j<=20;j++){float angle=(-.24f+.48f*j/20)*3.141592654f;points.push_back({w*.48f+std::cos(angle)*radius,cy+std::sin(angle)*radius});}
            NativeDockPath(p,points,volume>=(n+1)/4.f?ink:p.theme.Muted(),line);}
        if(muted)NativeDockPath(p,{{w*.95f,h*.05f},{w*.05f,h*.95f}},p.theme.Muted(),line);
    }else if(plugin==L"@magic/dock-bluetooth-popup"){
        float cx=w/2,top=h*.1f,bottom=h*.9f,quarter=(bottom-top)/4;
        NativeDockPath(p,{{cx-w*.25f,top+quarter},{cx+w*.25f,bottom-quarter},{cx,bottom},{cx,top},{cx+w*.25f,top+quarter},{cx-w*.25f,bottom-quarter}},ink,w*.07f);
        if(Bool(item,L"dockConnected"))for(float direction:{-1.f,1.f})p.c->FillEllipse(D2D1::Ellipse({cx+direction*w*.4f,h/2},w*.0385f,w*.0385f),p.Color(ink));
    }else if(plugin==L"@magic/dock-keyboard-selector"){
        auto language=Str(item,L"dockLanguage");if(language.empty())language=L"?";auto keyboard=Str(item,L"dockKeyboard");
        if(keyboard.empty())p.Text(language,{0,0,w,h},h*.4f,ink,700,DWRITE_TEXT_ALIGNMENT_CENTER);
        else {p.Text(language,{0,h*.15f,w,h*.65f},h*.4f,ink,700,DWRITE_TEXT_ALIGNMENT_CENTER);p.Text(keyboard,{0,h*.57f,w,h*.95f},h*.25f,p.theme.Muted(),600,DWRITE_TEXT_ALIGNMENT_CENTER);}
    }else if(plugin==L"@magic/dock-notifications"){
        float cx=w/2,line=w*.07f,knob=h*.056f,top=knob+line*.4f+h*.3f,bottom=h-line*2.1f;
        std::vector<D2D1_POINT_2F> points;
        for(int j=0;j<=24;j++){float angle=3.141592654f+j*3.141592654f/24;points.push_back({cx+std::cos(angle)*w*.3f,top+std::sin(angle)*h*.3f});}
        points.push_back({cx+w*.3f,top+(bottom-top)*.6f});points.push_back({cx+w*.39f,bottom});points.push_back({cx-w*.39f,bottom});points.push_back({cx-w*.3f,top+(bottom-top)*.6f});points.push_back(points.front());NativeDockPath(p,points,ink,line);
        p.c->FillEllipse(D2D1::Ellipse({cx,knob},line*.5f,line*.5f),p.Color(ink));p.c->FillEllipse(D2D1::Ellipse({cx,h-line*.55f},line*.55f,line*.55f),p.Color(ink));
        if(Bool(item,L"dockDnd"))p.Text(L"zZ",{w*.2f,h*.3f,w*.8f,h*.8f},h*.3f,ink,700,DWRITE_TEXT_ALIGNMENT_CENTER);
    }else if(plugin==L"@magic/dock-network-popup"){
        bool online=Bool(item,L"dockOnline");auto color=online?ink:p.theme.Muted();float line=w*.08f,cx=w/2;
        if(!online||Bool(item,L"dockWifi")){
            float base=h*.78f;p.c->FillEllipse(D2D1::Ellipse({cx,base},line*.6f,line*.6f),p.Color(color));
            for(int n=1;n<=3;n++){float radius=h*.62f*n/3;std::vector<D2D1_POINT_2F> points;for(int j=0;j<=20;j++){float angle=(1.25f+.5f*j/20)*3.141592654f;points.push_back({cx+std::cos(angle)*radius,base+std::sin(angle)*radius});}NativeDockPath(p,points,color,line);}
            if(!online)NativeDockPath(p,{{w*.18f,h*.18f},{w*.82f,h*.82f}},color,line*1.1f);
        }else{
            NativeDockPath(p,{{cx,h*.18f},{cx,h*.55f}},color,line);p.c->DrawRectangle({cx-w*.16f,h*.55f,cx+w*.16f,h*.83f},p.Color(color),line);
            for(int n=1;n<=3;n++){float x=cx-w*.16f+w*.32f*n/4;NativeDockPath(p,{{x,h*.83f},{x,h*.83f+line*.8f}},color,line);}
        }
    }else{
        auto batteries=item.GetNamedArray(L"batteries",JsonArray{});bool has=batteries.Size()>0,charging=Num(item,L"acLineStatus")==1;float percent=0;for(auto const &v:batteries){auto b=v.GetObject();percent+=static_cast<float>(Num(b,L"percentage"));charging|=Str(b,L"state")==L"charging";}if(has)percent/=batteries.Size();
        float cy=h/2,line=w*.06f,bh=h*.62f,capW=w*.08f,capH=bh*.6f,bx=capW*1.1f,by=cy-bh/2,bw=w-bx-line/2,cx=bx+bw/2,br=bh*.3f;
        p.Fill({0,cy-capH/2,capW,cy+capH/2},ink,capH*.35f);p.c->DrawRoundedRectangle(D2D1::RoundedRect({bx,by,bx+bw,by+bh},br,br),p.Color(ink),line);
        float inset=line*1.7f,ix=bx+inset,iy=by+inset,iw=bw-2*inset,ih=bh-2*inset,ir=std::max(ih*.25f,1.f);
        if(has){float fill=std::max(iw*std::clamp(percent,0.f,100.f)/100,ih);p.Fill({ix,iy,ix+fill,iy+ih},p.theme.Accent(),std::min(ir,fill/2));}
        if(has&&charging){float boltW=ih*.5f,boltH=ih*1.05f;NativeDockPath(p,{{cx+boltW*.18f,cy-boltH/2},{cx-boltW*.42f,cy+boltH*.05f},{cx,cy+boltH*.05f},{cx-boltW*.18f,cy+boltH/2},{cx+boltW*.42f,cy-boltH*.05f},{cx,cy-boltH*.05f}},ink);}
        else if(!has){float size=ih*.95f,half=size*.58f,top=cy-size*.55f,bottom=cy+size*.45f;NativeDockPath(p,{{cx,top},{cx+half,bottom},{cx-half,bottom}},D2D1::ColorF(0xe5484d));float bar=size*.12f;p.Fill({cx-bar/2,top+size*.3f,cx+bar/2,top+size*.6f},p.theme.Surface(),0);p.Fill({cx-bar/2,top+size*.72f,cx+bar/2,top+size*.72f+bar},p.theme.Surface(),0);}
    }
    p.c->SetTransform(old);
}
inline int NativeBar::MediaButtonAt(int index,float x,float y)const {
    if(!dock&&index>=0&&index<static_cast<int>(entries.size())&&Str(entries[index],L"type")==L"Music"){
        auto r=rows[index].rect;return NativeMusicLayout::Hit(entries[index],r.right-r.left,r.bottom-r.top,x-contentX-r.left,y-contentY-r.top);
    }
    if(index<0||index>=static_cast<int>(entries.size())||Str(entries[index],L"type")!=L"Media"||!Bool(entries[index],L"hasPlayer"))return -1;
    auto r=rows[index].rect;auto pose=DockPose(index);x-=contentX;y-=contentY;float w=r.right-r.left,h=r.bottom-r.top;x=r.left+pose.origin.x+(x-r.left-pose.origin.x-pose.offset.x)/pose.scale;y=r.top+pose.origin.y+(y-r.top-pose.origin.y-pose.offset.y)/pose.scale;x-=r.left;y-=r.top;
    for(int i=0;i<3;i++){float cx=Vertical()?w/2:w/2+(i-1)*22,cy=Vertical()?h/2+10+(i-1)*28:h/2+8.28f;if(std::abs(x-cx)<=10&&std::abs(y-cy)<=8)return i;}return -1;
}
inline void NativeBar::PaintDockItem(ID2D1DeviceContext *c,int index,bool retainedMedia) {
    auto row=rows[index];auto item=entries[index];NativePaint p(c,app.graphics,dark);auto &t=p.theme;
    float w=row.rect.right-row.rect.left,h=row.rect.bottom-row.rect.top,press=downId==row.id&&Str(item,L"type")!=L"Media"?2.f:0.f;
    D2D1_RECT_F body{0,press,w,h+press};float radius=iconSize*.25f;
    if(Str(item,L"type")==L"Recovery"){
        auto ink=t.Ink();float s=iconSize/48;if(HonorScheme(app.model.preferences)!=L"honor-translucent")p.Fill(body,t.Raised(),radius);
        p.Line(7*s,13*s,26*s,13*s,ink,1.7f*s);p.Line(7*s,13*s,7*s,30*s,ink,1.7f*s);p.Line(7*s,30*s,26*s,30*s,ink,1.7f*s);p.Line(26*s,13*s,26*s,30*s,ink,1.7f*s);p.Line(16*s,30*s,16*s,35*s,ink,1.7f*s);p.Line(11*s,35*s,21*s,35*s,ink,1.7f*s);
        for(int n=0;n<3;++n){float a=(25+n*6.f)*s;p.Line(a,19*s,a+3*s,19*s,ink,1.5f*s);p.Line(a,37*s,a+3*s,37*s,ink,1.5f*s);float b=(21+n*6.f)*s;p.Line(25*s,b,25*s,b+3*s,ink,1.5f*s);p.Line(43*s,b,43*s,b+3*s,ink,1.5f*s);}
        auto text=std::to_wstring(static_cast<int>(Num(item,L"recoveryCount")));p.Fill({iconSize-20,0,iconSize,17},D2D1::ColorF(0x0067c0),8);p.Text(text,{iconSize-20,0,iconSize,17},9,D2D1::ColorF(0xffffff),600,DWRITE_TEXT_ALIGNMENT_CENTER);return;
    }
    if(Str(item,L"type")==L"Separator") {if(!row.id.starts_with(L"@boundary:")&&Bool(item,L"separatorVisible",true)){if(Vertical())p.Line(0,h/2,w,h/2,t.Muted());else p.Line(w/2,0,w/2,h,t.Muted());}return;}
    app.honor.Scheme(HonorScheme(app.model.preferences));
    auto officialFile=HonorDockFile(item,HonorScheme(app.model.preferences));bool officialPainted=false;
    // Official application PNGs include their own white/translucent plate.
    if(!officialFile.empty()){
        float inset=Str(item,L"plugin")==L"@default/weg-start-menu"&&HonorScheme(app.model.preferences)==L"honor-translucent"?iconSize*.15f:0;
        officialPainted=app.honor.Draw(app.graphics,c,officialFile,{inset,press+inset,iconSize-inset,iconSize+press-inset},dark);
    }
    if(!officialPainted){
        p.Shadow(body,radius);
        if(Str(item,L"type")==L"Media"){if(!retainedMedia)p.Fill(body,t.Film(true),radius);}
        else p.Fill(body,HonorScheme(app.model.preferences)==L"honor-translucent"?D2D1::ColorF(128/255.f,128/255.f,128/255.f,46/255.f):D2D1::ColorF(1,1,1,.97f),radius);
    }
    float inner=iconSize*.15f,size=iconSize-2*inner;
    if(Str(item,L"type")==L"Media") {
        auto ink=t.Ink();
        // The cached title visual scrolls separately; no cover image or fallback plate.
        if(!Bool(item,L"hasPlayer"))toolbarSvg.Draw(c,L"MagicMediaVideo",{inner,inner+press,inner+size,inner+size+press},ink);
        if(Bool(item,L"hasPlayer"))for(int n=0;n<3;++n){float x=Vertical()?w/2:w/2+(n-1)*22,y=Vertical()?h/2+10+(n-1)*28:h/2+8.28f;
            // Hover geometry is handled by Composition; no selection-colored overlay.
            toolbarSvg.Draw(c,n==0?L"IoPlaySkipBack":n==2?L"IoPlaySkipForward":Bool(item,L"playing")?L"IoPause":L"IoPlay",{x-6,y-6,x+6,y+6},ink);}
        return;
    }
    D2D1_RECT_F icon{inner,inner+press,inner+size,inner+size+press};
    auto found=shell.icons.find(Str(item,L"key"));
    if(Bool(item,L"squareIcon"))icon=body;
    if(officialPainted){}
    else if(found!=shell.icons.end()){
        com_ptr<ID2D1RoundedRectangleGeometry> mask;if(Bool(item,L"squareIcon")){com_ptr<ID2D1Factory> factory;c->GetFactory(factory.put());check_hresult(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(icon,radius,radius),mask.put()));c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),mask.get()),nullptr);}
        NativeDockBitmap(c,found->second.bitmap.get(),icon,index==focus);if(mask)c->PopLayer();
    }
    else if(Str(item,L"type")==L"AppOrFile")toolbarSvg.Draw(c,L"MagicDock_missing",icon,t.Ink());
    else {auto plugin=CanonicalNativeBarPlugin(Str(item,L"plugin"));std::wstring asset;
        auto official=HonorModuleGlyph(item);
        if(!official.empty()&&toolbarSvg.Draw(c,official,icon,HonorScheme(app.model.preferences)==L"honor-white"?Preview70Theme{false}.Ink():t.Ink())){}
        else if(plugin==L"@default/weg-power"||plugin.starts_with(L"@magic/dock-")){NativeDockCanvas(p,plugin,item,icon,Str(settings,L"position"));}
        else {if(plugin==L"@default/weg-start-menu")asset=dark?L"MagicDock_start_menu_dark":L"MagicDock_start_menu";
        else if(plugin==L"@default/weg-show-desktop")asset=dark?L"MagicDock_desktop_dark":L"MagicDock_desktop_light";
        else if(plugin==L"@default/weg-trash-bin")asset=Num(item,L"trashCount")>0?(dark?L"MagicDock_trash_bin_full_dark":L"MagicDock_trash_bin_full_light"):(dark?L"MagicDock_trash_bin_empty_dark":L"MagicDock_trash_bin_empty_light");
        else if(plugin==L"@magic/dock-system-tray")asset=L"IoIosArrowDropdown";
        else asset=L"VscLock";
        toolbarSvg.Draw(c,asset,icon,t.Ink());}
    }
    auto windows=item.GetNamedArray(L"windows",JsonArray{});
    if(!Vertical()&&Bool(settings,L"showWindowTitle")&&windows.Size()) {
        auto title=Str(windows.GetAt(0).GetObject(),L"title");float start=inner+size+inner;
        p.Text(title,{start,press,w-inner,h+press},std::max(iconSize/3,12.f),t.Ink(),600);
    }
    auto badge=ShellObject(item,L"badge");auto value=Str(badge,L"value"),type=Str(badge,L"kind");
    if(!type.empty()) {
        float bw=type==L"count"?std::max(16.f,MeasureToolbarText(value,10)+4):16.f;D2D1_RECT_F r{w-bw*.75f,-4,w+bw*.25f,12};
        if(type!=L"image")p.Fill(r,t.Accent(),8);
        if(type==L"count")p.Text(value,r,10,t.OnAccent(),600,DWRITE_TEXT_ALIGNMENT_CENTER);
        else if(type==L"icon")toolbarSvg.Draw(c,value,{r.left+2,r.top+2,r.right-2,r.bottom-2},t.OnAccent());
        else if(type==L"image") {if(auto image=shell.icons.find(Str(item,L"key")+L":badge");image!=shell.icons.end())c->DrawBitmap(image->second.bitmap.get(),r,1,D2D1_INTERPOLATION_MODE_LINEAR);}
        else if(type==L"color")p.Fill(r,NativeDockBadgeColor(dark,value),0);
    }
    if(windows.Size()>1&&Bool(settings,L"showInstanceCounter",true)) {
        D2D1_RECT_F r{w-12,h-20,w+4,h-4};p.Fill(r,t.Gray(400),8);p.Text(std::to_wstring(windows.Size()),r,10,t.Gray(100),600,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}
inline void NativeBar::SyncDockState(NativeToolbarVisual &v,int index,float extra) {
    auto const &row=rows[index];auto item=entries[index];auto &compositor=app.graphics.compositor;
    auto windows=item.GetNamedArray(L"windows",JsonArray{});bool focused=false;std::wstring attention=L"None";
    for(auto const &w:windows){auto win=w.GetObject();focused|=Num(win,L"hwnd")==Num(data.GetNamedObject(L"focused"),L"hwnd");auto a=Str(win,L"attention");if(a==L"Flashing"||a==L"Highlighted"&&attention!=L"Flashing")attention=a;}
    float w=row.rect.right-row.rect.left,h=row.rect.bottom-row.rect.top,scale=dpi/96;
    bool sign=windows.Size()&&!Bool(settings,L"showWindowTitle");
    if(!v.indicator){v.indicator=compositor.CreateSpriteVisual();v.indicator.AnchorPoint({.5f,.5f});v.indicatorBrush=compositor.CreateColorBrush();v.indicator.Brush(v.indicatorBrush);v.group.Children().InsertAtTop(v.indicator);}
    float length=sign?(focused?iconSize*.5f:3):0;auto side=Str(settings,L"position");
    float3 point{(extra+w/2)*scale,(extra+h+padding/2)*scale,0};
    if(side==L"Top")point.y=(extra-padding/2)*scale;
    if(side==L"Left"||side==L"Right"){point.x=(side==L"Left"?extra-padding/2:extra+w+padding/2)*scale;point.y=(extra+h/2)*scale;}
    float2 size=Vertical()?float2{3*scale,length*scale}:float2{length*scale,3*scale};
    bool changed=v.indicatorSize.x!=size.x||v.indicatorSize.y!=size.y;
    if(changed&&NativeMotion(app.model)){auto animation=compositor.CreateVector2KeyFrameAnimation();animation.InsertExpressionKeyFrame(0,L"this.StartingValue");animation.InsertKeyFrame(1,size,compositor.CreateLinearEasingFunction());animation.Duration(std::chrono::milliseconds(200));v.indicator.Size(size);v.indicator.StartAnimation(L"Size",animation);}
    else v.indicator.Size(size);
    auto theme=Preview70Theme{dark};auto ink=focused||attention!=L"None"?theme.Accent():theme.Gray(600);Windows::UI::Color color{static_cast<BYTE>(ink.a*255),static_cast<BYTE>(ink.r*255),static_cast<BYTE>(ink.g*255),static_cast<BYTE>(ink.b*255)};
    if(v.indicatorColor!=color&&NativeMotion(app.model)){auto animation=compositor.CreateColorKeyFrameAnimation();animation.InsertExpressionKeyFrame(0,L"this.StartingValue");animation.InsertKeyFrame(1,color,compositor.CreateLinearEasingFunction());animation.Duration(std::chrono::milliseconds(200));v.indicatorBrush.Color(color);v.indicatorBrush.StartAnimation(L"Color",animation);}else v.indicatorBrush.Color(color);
    v.indicatorColor=color;v.indicatorSize=size;v.indicator.Offset(point);v.indicator.Opacity(sign?1.f:0.f);
    if(!v.attention){v.attention=compositor.CreateShapeVisual();auto geometry=compositor.CreateRoundedRectangleGeometry();geometry.Size({w*scale,h*scale});geometry.CornerRadius({iconSize*.25f*scale,iconSize*.25f*scale});v.attentionGeometry=geometry;
        auto shape=compositor.CreateSpriteShape(geometry);v.attentionBrush=compositor.CreateColorBrush();shape.FillBrush(v.attentionBrush);v.attention.Shapes().Append(shape);v.hoverLayer.Children().InsertAtTop(v.attention);}
    auto accent=Preview70Theme::systemAccent;v.attentionBrush.Color({51,static_cast<BYTE>(accent.r*255),static_cast<BYTE>(accent.g*255),static_cast<BYTE>(accent.b*255)});v.attentionGeometry.CornerRadius({iconSize*.25f*scale,iconSize*.25f*scale});
    v.attentionGeometry.Size({w*scale,h*scale});v.attention.Size({w*scale,h*scale});v.attention.Offset({extra*scale,(extra+(downId==row.id?2:0))*scale,0});
    bool motion=NativeMotion(app.model);if(attention!=v.attentionState||motion!=v.attentionMotion){v.attentionMotion=motion;v.attention.StopAnimation(L"Opacity");v.attention.Opacity(attention==L"None"?0.f:1.f);
        if(attention==L"Flashing"&&NativeMotion(app.model)){auto animation=compositor.CreateScalarKeyFrameAnimation();animation.InsertKeyFrame(0,0);animation.InsertKeyFrame(1,1,compositor.CreateCubicBezierEasingFunction({.42f,0},{.58f,1}));animation.Duration(std::chrono::milliseconds(500));animation.Direction(AnimationDirection::Alternate);animation.IterationBehavior(AnimationIterationBehavior::Forever);v.attention.StartAnimation(L"Opacity",animation);}v.attentionState=attention;
    }

}

inline void NativeBar::SyncMediaTitle(NativeToolbarVisual &v,int index,float extra){
 auto item=entries[index];bool show=dock&&Str(item,L"type")==L"Media"&&Bool(item,L"hasPlayer");
 auto clear=[&]{if(v.mediaClip)v.hoverLayer.Children().Remove(v.mediaClip);v.mediaTitle=nullptr;v.mediaClip=nullptr;v.mediaSurface=nullptr;v.mediaBytes=0;v.mediaKey.clear();};
 if(!show){if(v.mediaClip)clear();return;}
 float width=rows[index].rect.right-rows[index].rect.left;
 float height=rows[index].rect.bottom-rows[index].rect.top,viewport=std::max(8.f,width-16),ratio=dpi/96;
 bool animate=NativeMotion(app.model)&&!NativeLowPower(app.model)&&!Vertical();
 std::wstring title=rows[index].label;if(title.size()>512)title.resize(512);
 auto key=title+L":"+std::to_wstring(viewport)+L":"+std::to_wstring(dpi)+L":"+std::to_wstring(dark)+L":"+std::to_wstring(animate);
 if(key==v.mediaKey)return;
 float textWidth=std::min(MeasureToolbarText(title,11.2f)+3,1024.f),gap=NativeMediaMarquee::Gap;bool scroll=animate&&textWidth>viewport;
 float period=textWidth+gap,renderWidth=scroll?period*2:viewport;
 // Cap raster resolution; Composition handles display scaling for unusually large DPI.
 float raster=std::min(ratio,std::sqrt(262144.f/std::max(1.f,renderWidth*20*4)));
 float pw=std::ceil(renderWidth*raster),ph=std::ceil(20*raster);size_t size=static_cast<size_t>(pw)*static_cast<size_t>(ph)*4;
 size_t used=0;for(auto const &[_,other]:toolbarVisuals)used+=other.bytes+other.mediaBytes;
 if(size>270336||used-v.mediaBytes+size>8*1024*1024){clear();return;}
 if(!v.mediaClip){v.mediaClip=app.graphics.compositor.CreateContainerVisual();v.mediaTitle=app.graphics.compositor.CreateSpriteVisual();v.mediaClip.Children().InsertAtTop(v.mediaTitle);v.hoverLayer.Children().InsertAtTop(v.mediaClip);}
 v.mediaClip.Offset({(extra+8)*ratio,(extra+(Vertical()?4:height/2-18))*ratio,0});v.mediaClip.Size({viewport*ratio,20*ratio});v.mediaClip.Clip(app.graphics.compositor.CreateInsetClip());v.mediaClip.Comment(L"");
 v.mediaSurface=app.graphics.composition.CreateDrawingSurface({pw,ph},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
 auto interop=v.mediaSurface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT origin{};check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&origin));
 try{c->SetDpi(96*raster,96*raster);c->SetTransform(D2D1::Matrix3x2F::Translation(origin.x/raster,origin.y/raster));c->Clear(D2D1::ColorF(0,0));NativePaint p(c.get(),app.graphics,dark);p.Text(title,{0,0,scroll?textWidth:viewport,20},11.2f,p.theme.Ink(),600);if(scroll)p.Text(title,{period,0,period+textWidth,20},11.2f,p.theme.Ink(),600);check_hresult(interop->EndDraw());}catch(...){interop->EndDraw();throw;}
 v.mediaTitle.Brush(app.graphics.compositor.CreateSurfaceBrush(v.mediaSurface));v.mediaTitle.Size({renderWidth*ratio,20*ratio});v.mediaTitle.StopAnimation(L"Offset");v.mediaTitle.Offset({0,0,0});
 if(scroll){auto motion=app.graphics.compositor.CreateVector3KeyFrameAnimation();motion.InsertKeyFrame(0,{0,0,0});motion.InsertKeyFrame(1,{-period*ratio,0,0},app.graphics.compositor.CreateLinearEasingFunction());motion.Duration(std::chrono::milliseconds(static_cast<int>(period/NativeMediaMarquee::Speed*1000)));motion.IterationBehavior(AnimationIterationBehavior::Forever);v.mediaTitle.StartAnimation(L"Offset",motion);}
 v.mediaBytes=size;v.mediaKey=key;
}

// Resize the media plate as geometry, not as a freshly swapped bitmap. Text and
// controls keep their pixel size; the compositor follows the plate's centre and
// clips the title to its current width. No frame callback or extra raster copy.
inline void NativeBar::SyncMediaSize(NativeToolbarVisual &v,int index,float extra,bool fresh){
    if(!dock||Str(entries[index],L"type")!=L"Media")return;
    auto &c=app.graphics.compositor;float s=dpi/96;auto r=rows[index].rect;
    float2 next{(r.right-r.left)*s,(r.bottom-r.top)*s};bool created=!v.mediaGeometry;
    if(created){
        v.mediaBody=c.CreateShapeVisual();v.mediaGeometry=c.CreateRoundedRectangleGeometry();v.mediaBrush=c.CreateColorBrush();
        auto plate=c.CreateSpriteShape(v.mediaGeometry);plate.FillBrush(v.mediaBrush);v.mediaBody.Shapes().Append(plate);v.hoverLayer.Children().InsertAtBottom(v.mediaBody);
    }
    auto color=Preview70Theme{dark}.Film(true);v.mediaBrush.Color({static_cast<BYTE>(std::lround(color.a*255)),255,255,255});
    bool changed=created||next.x!=v.mediaSize.x||next.y!=v.mediaSize.y;
    if(changed){
        bool animate=!created&&!fresh&&!barDrag&&NativeMotion(app.model);
        // Reserve overshoot once; normal frames only animate geometry.Size.
        v.mediaBody.Size({std::max(next.x,v.mediaSize.x)+std::abs(next.x-v.mediaSize.x)*.13f+2*extra*s,std::max(next.y,v.mediaSize.y)+std::abs(next.y-v.mediaSize.y)*.13f+2*extra*s});
        v.mediaGeometry.Offset({extra*s,extra*s});v.mediaGeometry.CornerRadius({iconSize*.25f*s,iconSize*.25f*s});
        if(animate){
            auto a=c.CreateVector2KeyFrameAnimation();a.InsertExpressionKeyFrame(0,L"this.StartingValue");a.SetVector2Parameter(L"destination",next);
            for(int i=1;i<=40;++i){float p=i/40.f,k=NativeInteractionMotion::DockSpring(p);a.InsertExpressionKeyFrame(p,L"this.StartingValue * (1 - "+std::to_wstring(k)+L") + destination * "+std::to_wstring(k),c.CreateLinearEasingFunction());}
            a.Duration(std::chrono::milliseconds(NativeInteractionMotion::DockBodyMs));v.mediaGeometry.StartAnimation(L"Size",a);
            if(!v.mediaResizeAt)v.visual.Opacity(0);
            NativeInteractionMotion::Opacity(c,v.visual,1,NativeInteractionMotion::DockMs);
            if(v.mediaClip){if(!v.mediaResizeAt)v.mediaClip.Opacity(0);NativeInteractionMotion::Opacity(c,v.mediaClip,1,NativeInteractionMotion::DockMs);}
            v.mediaResizeAt=GetTickCount64();
            if(!SetTimer(hwnd,DockBodyTimer,NativeInteractionMotion::DockBodyMs+16,nullptr)){v.mediaGeometry.StopAnimation(L"Size");v.mediaGeometry.Size(next);v.mediaResizeAt=0;v.visual.StopAnimation(L"Opacity");v.visual.Opacity(1);if(v.mediaClip){v.mediaClip.StopAnimation(L"Opacity");v.mediaClip.Opacity(1);}}
        }else {v.mediaGeometry.StopAnimation(L"Size");v.mediaGeometry.Size(next);v.mediaResizeAt=0;v.visual.StopAnimation(L"Opacity");v.visual.Opacity(1);if(v.mediaClip){v.mediaClip.StopAnimation(L"Opacity");v.mediaClip.Opacity(1);}}
        v.mediaSize=next;
        v.visual.AnchorPoint({.5f,.5f});auto centre=c.CreateExpressionAnimation(L"Vector3(plate.Size.X * .5 + pad, plate.Size.Y * .5 + pad, 0)");centre.SetReferenceParameter(L"plate",v.mediaGeometry);centre.SetScalarParameter(L"pad",extra*s);v.visual.StartAnimation(L"Offset",centre);
        auto clip=c.CreateInsetClip();auto horizontal=c.CreateExpressionAnimation(L"Max(0, (pixels - plate.Size.X) * .5)");horizontal.SetReferenceParameter(L"plate",v.mediaGeometry);horizontal.SetScalarParameter(L"pixels",v.visual.Size().x);clip.StartAnimation(L"LeftInset",horizontal);clip.StartAnimation(L"RightInset",horizontal);
        auto vertical=c.CreateExpressionAnimation(L"Max(0, (pixels - plate.Size.Y) * .5)");vertical.SetReferenceParameter(L"plate",v.mediaGeometry);vertical.SetScalarParameter(L"pixels",v.visual.Size().y);clip.StartAnimation(L"TopInset",vertical);clip.StartAnimation(L"BottomInset",vertical);v.visual.Clip(clip);
    }
    auto side=Str(settings,L"position");if(changed||v.mediaBody.Comment()!=side){
        auto pivot=c.CreateExpressionAnimation(side==L"Left"?L"Vector3(pad, plate.Size.Y * .5 + pad, 0)":side==L"Right"?L"Vector3(plate.Size.X + pad, plate.Size.Y * .5 + pad, 0)":side==L"Top"?L"Vector3(plate.Size.X * .5 + pad, pad, 0)":L"Vector3(plate.Size.X * .5 + pad, plate.Size.Y + pad, 0)");
        pivot.SetReferenceParameter(L"plate",v.mediaGeometry);pivot.SetScalarParameter(L"pad",extra*s);v.hoverLayer.StartAnimation(L"CenterPoint",pivot);v.mediaBody.Comment(side);
    }
    if(v.mediaClip&&(changed||v.mediaClip.Comment()!=L"media-size-bound")){
        auto viewport=c.CreateExpressionAnimation(L"Vector2(Max(minimum, plate.Size.X - padding), line)");viewport.SetReferenceParameter(L"plate",v.mediaGeometry);viewport.SetScalarParameter(L"minimum",8*s);viewport.SetScalarParameter(L"padding",16*s);viewport.SetScalarParameter(L"line",20*s);v.mediaClip.StartAnimation(L"Size",viewport);v.mediaClip.Comment(L"media-size-bound");
    }
}
