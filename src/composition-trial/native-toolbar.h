#pragma once
#include "native-brand.h"
inline float NativeBar::MeasureToolbarText(std::wstring const &text,float font,int weight) {
    // Weight belongs to the key: the brand label is measured and painted at 600,
    // and reusing a 500 measurement clipped its last glyph.
    auto key=std::to_wstring(font)+L":"+std::to_wstring(weight)+L":"+text;auto found=textWidths.find(key);if(found!=textWidths.end())return found->second;
    com_ptr<IDWriteTextLayout> layout;auto f=app.graphics.Format(font,false,weight);
    check_hresult(app.graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),f,65536,128,layout.put()));
    DWRITE_TEXT_METRICS metrics{};check_hresult(layout->GetMetrics(&metrics));
    if(textWidths.size()>=512)textWidths.clear();auto width=metrics.widthIncludingTrailingWhitespace;textWidths.emplace(std::move(key),width);return width;
}
inline JsonArray NativeBar::ToolbarContent(JsonObject const &item) {
    if(Str(item,L"type")==L"AppOrFile"){JsonObject span;Put(span,L"kind",std::wstring(L"image"));JsonArray result;result.Append(span);return result;}
    if(Bool(item,L"clock")){JsonObject span;Put(span,L"kind",std::wstring(L"text"));Put(span,L"value",ToolbarClock(data));JsonArray result;result.Append(span);return result;}
    auto value=item.TryLookup(L"content");if(value&&value.ValueType()==JsonValueType::Array)return value.GetArray();
    JsonArray result;JsonObject span;Put(span,L"kind",std::wstring(L"text"));Put(span,L"value",Str(item,L"label"));result.Append(span);return result;
}
// The brand item mirrors the brand button in MyMagicBar: a device mark plus the
// machine name read from BIOS. A user image keeps the square cell instead.
// The left item is two independent components: a mark and a wordmark. Either can
// be hidden, and the wordmark can be the vendor name, this PC's name, or text the
// user typed.
inline bool NativeBrandCell(JsonObject const &item){
    return Str(item,L"nativeModule")==L"magic.user"&&!Bool(item,L"customIcon");
}
inline bool NativeBrandIconShown(JsonObject const &item){return Str(item,L"brandIconHide")!=L"1";}
// The default entry is a family, not one glyph: a laptop or a desktop. The tower
// shape comes from the toolbar set, the laptop from the Honour dock library.
inline std::wstring NativeBrandDeviceGlyph(JsonObject const &item){
    return Str(item,L"brandDevice")==L"desktop"?L"PiDesktopDuotone":L"Honor:matebook";
}
inline std::wstring NativeHostName(){
    wchar_t buffer[64]{};DWORD size=64;
    if(!GetComputerNameW(buffer,&size)||size==0)return L"PC";
    return std::wstring(buffer,size);
}
inline std::wstring NativeBrandLabelText(JsonObject const &item){
    auto mode=Str(item,L"brandLabel");
    if(mode.empty())mode=L"brand";
    if(mode==L"none")return L"";
    if(mode==L"host")return NativeHostName();
    if(mode==L"custom"){auto text=Str(item,L"brandText");if(text.size()>32)text.resize(32);return text;}
    return NativeBrandName();
}
inline float NativeBar::ToolbarContentWidth(JsonObject const &item) {
    if(Str(item,L"nativeModule")==L"magic.music")return NativeMusicLayout::ContentWidth;
    if(Str(item,L"type")==L"Extension")return ExtensionNaturalWidth(item);
    if(Str(item,L"type")==L"Tray")return iconSize;
    if(Bool(item,L"clock")){
        auto text=ToolbarClock(data);auto digits=NativeClockLocale(Str(data,L"language")).GetNamedArray(L"digits");std::wstring widest=L"0";float maximum=0;
        for(auto const &v:digits){auto digit=std::wstring(v.GetString());float width=MeasureToolbarText(digit,iconSize*.8f);if(width>maximum){maximum=width;widest=digit;}}
        std::wstring sample;for(size_t i=0;i<text.size();){bool number=false;for(auto const &v:digits){auto digit=std::wstring(v.GetString());if(!digit.empty()&&text.compare(i,digit.size(),digit)==0){sample+=widest;i+=digit.size();number=true;break;}}if(!number)sample+=text[i++];}
        return MeasureToolbarText(sample,iconSize*.8f)+1;
    }
    if(NativeBrandCell(item)){
        // A wordmark lockup is far wider than it is tall, so the cell grows with
        // it; otherwise the mark is a hairline inside a square.
        float mark=NativeBrandIconShown(item)?NativeBrandMarkWidth(Str(item,L"brandIcon"),iconSize):0;
        auto text=NativeBrandLabelText(item);
        // +1 for hinting, +2 so the text never touches the hover pill.
        float label=text.empty()?0:MeasureToolbarText(text,iconSize*.8f,600)+3+(mark>0?4:0);
        float total=mark+label;
        return total>0?total:iconSize;
    }
    float width=0;for(auto const &v:ToolbarContent(item)){auto s=v.GetObject();width+=Str(s,L"kind")==L"text"?MeasureToolbarText(Str(s,L"value"),iconSize*.8f):iconSize;}return width;
}
inline float NativeBar::ToolbarMetricWidth(JsonObject const &item){
    auto module=Str(item,L"nativeModule");
    if(module!=L"magic.cpu"&&module!=L"magic.gpu"&&module!=L"magic.memory"&&module!=L"magic.storage"&&module!=L"magic.network-usage")return 0;
    float digit=0;wchar_t widest=L'0';for(wchar_t c=L'0';c<=L'9';++c){float w=MeasureToolbarText(std::wstring(1,c),iconSize*.8f);if(w>digit){digit=w;widest=c;}}
    std::wstring digits(4,widest),sample=std::wstring(3,widest)+L"%";
    if(module==L"magic.network-usage"){
        float maximum=0;for(auto unit:{L"KB/s",L"MB/s",L"GB/s",L"TB/s",L"PB/s",L"EB/s"}){
            auto rate=digits+L"."+widest+L" "+unit;maximum=std::max(maximum,MeasureToolbarText(L"↓ "+rate+L" ↑ "+rate,iconSize*.8f));
        }return iconSize+maximum;
    }
    return iconSize+MeasureToolbarText(sample,iconSize*.8f);
}
inline std::wstring NativeDockTooltipText(std::wstring text){
    for(auto &c:text)if(c==L'\r'||c==L'\n'||c==L'\t')c=L' ';
    std::vector<size_t> ends;for(size_t i=0;i<text.size();){wchar_t c=text[i++];if(c>=0xd800&&c<=0xdbff&&i<text.size()&&text[i]>=0xdc00&&text[i]<=0xdfff)++i;ends.push_back(i);}
    if(ends.size()>10)text=text.substr(0,ends[8])+L"…";return text;
}
inline float NativeBar::ToolbarGroupWidth(std::vector<JsonObject> const &items){
    float result=0;std::wstring previous;for(size_t i=0;i<items.size();++i){auto compound=CompoundOwner(items[i]);if(i&&(compound.empty()||compound!=previous))result+=iconSize*.25f;if(!compound.empty()&&compound!=previous)result+=padding;result+=ItemWidth(items[i]);previous=compound;}return result;
}
inline void NativeBar::PaintToolbarItem(ID2D1DeviceContext *c,int index) {
    auto row=rows[index];auto item=entries[index];NativePaint p(c,app.graphics,dark);auto &theme=p.theme;
    if(Str(item,L"type")==L"Extension"){PaintExtensionItem(c,index);return;}
    app.honor.Scheme(HonorScheme(app.model.preferences));
    float w=row.rect.right-row.rect.left,h=row.rect.bottom-row.rect.top;D2D1_RECT_F local{0,0,w,h};
    if(Str(item,L"nativeModule")==L"magic.music"){
        NativeMusicLayout::Paint(p,item,w,h,iconSize,index==focus?mediaHover:-1,downId==row.id?mediaDown:-1);return;
    }
    struct RestoreTransform{ID2D1DeviceContext *c;D2D1_MATRIX_3X2_F old;~RestoreTransform(){c->SetTransform(old);}}restore{c};c->GetTransform(&restore.old);
    if(downId==row.id&&!CompoundOwner(item).empty())c->SetTransform(D2D1::Matrix3x2F::Scale(.98f,.98f,{w/2,h/2})*D2D1::Matrix3x2F::Translation(0,1)*restore.old);
    // Interaction focus is kept in the model/UIA; decoration is reserved for the future HDR treatment.
    float x=(w-ToolbarContentWidth(item))/2,y=(h-iconSize)/2;
    auto bitmap=[&](std::wstring const &key,D2D1_RECT_F target,bool glyph){if(!glyph&&app.honor.Draw(app.graphics,c,HonorAppFile(item,HonorScheme(app.model.preferences)),target))return true;auto found=shell.icons.find(key);if(found==shell.icons.end())return false;
        if(glyph){auto mode=c->GetAntialiasMode();c->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);c->FillOpacityMask(found->second.bitmap.get(),p.Color(theme.Ink()),&target,nullptr);c->SetAntialiasMode(mode);}
        else {
            // The icon is an opaque PNG from the app that owns it, so the bar cannot
            // recolour it the way it recolours a glyph: Tailscale's black mark simply
            // disappears on a black bar. The tone measured when the PNG arrived says
            // whether this artwork can be seen against this backdrop; when it cannot,
            // a plate of the opposite tone goes underneath and the artwork itself is
            // left exactly as shipped. A full-bleed tile already carries its own
            // field, so a plate behind it would only be hidden - skip those.
            auto tone=shell.tones.find(key);
            if(Bool(app.model.preferences,L"autoContrast",true)&&tone!=shell.tones.end()&&tone->second.coverage<.92f){
                bool pale=dark?tone->second.ink<.28f:tone->second.ink>.74f;
                if(pale){
                    float width=target.right-target.left,height=target.bottom-target.top,radius=std::min(width,height)*.22f;
                    com_ptr<ID2D1Factory> factory;c->GetFactory(factory.put());com_ptr<ID2D1RoundedRectangleGeometry> plate;
                    check_hresult(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(target,radius,radius),plate.put()));
                    com_ptr<ID2D1SolidColorBrush> brush;
                    check_hresult(c->CreateSolidColorBrush(dark?D2D1::ColorF(0xFFFFFF,.96f):D2D1::ColorF(0x1C1C1E,.96f),brush.put()));
                    c->FillGeometry(plate.get(),brush.get());
                }
            }
            com_ptr<ID2D1Factory> factory;c->GetFactory(factory.put());com_ptr<ID2D1RoundedRectangleGeometry> clip;check_hresult(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(target,iconSize*.3f,iconSize*.3f),clip.put()));c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),clip.get()),nullptr);c->DrawBitmap(found->second.bitmap.get(),target,1,D2D1_INTERPOLATION_MODE_LINEAR);c->PopLayer();}return true;};
    if(Str(item,L"type")==L"Tray") {bitmap(row.id,{x,y,x+iconSize,y+iconSize},Bool(item,L"glyph"));return;}
    if(row.id==L"@magic/pages"){auto ink=theme.Ink();ink.a=pageAlpha/100.f;p.Text(row.label,local,iconSize*.8f,ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);return;}
    for(auto const &v:ToolbarContent(item)){
        auto span=v.GetObject();auto kind=Str(span,L"kind"),value=Str(span,L"value");
        if(kind==L"text") {float size=MeasureToolbarText(value,iconSize*.8f);p.Text(value,{x,0,x+size+.25f,h},iconSize*.8f,theme.Ink(),500);x+=size;}
        else {
            bool brand=NativeBrandCell(item);
            bool showMark=!brand||NativeBrandIconShown(item);
            std::wstring text=brand?NativeBrandLabelText(item):std::wstring();
            std::wstring brandIcon=brand?Str(item,L"brandIcon"):std::wstring();
            float markWidth=NativeBrandMarkWidth(brandIcon,iconSize);
            float textW=text.empty()?0:MeasureToolbarText(text,iconSize*.8f,600);
            float span=brand?((showMark?markWidth:0)+(textW>0?textW+3+(showMark?4:0):0)):iconSize;
            if(span<=0)span=iconSize;
            auto r=D2D1::RectF(x,y,x+span,y+iconSize);
            if(kind==L"icon"){
                float cursor=x;bool drawn=false;
                if(showMark){
                    auto mark=D2D1::RectF(cursor,y,cursor+markWidth,y+iconSize);
                    // A chosen vendor mark wins, then an imported image, then the
                    // module glyph; the brand cell falls back to the device mark.
                    // Vendors with no colour lockup keep the glyph rather than
                    // dropping through to the device mark.
                    if(!brandIcon.empty()){
                        // Colour is what the user picked, but a dark lockup on a dark bar
                        // is a blank cell: fall through to the mono cut when the baked
                        // paint range says this artwork cannot be seen against the
                        // backdrop. The switch only silences that check, it never
                        // forces colour - a vendor with no colour file still gets mono.
                        bool colour=Str(item,L"brandStyle")==L"color";
                        bool readable=!Bool(app.model.preferences,L"autoContrast",true)||NativeBrandColourLegible(brandIcon,dark);
                        if(colour&&readable)drawn=toolbarSvg.Draw(c,L"Brand:"+brandIcon+L":color",mark,theme.Ink());
                        if(!drawn)drawn=toolbarSvg.Draw(c,L"Brand:"+brandIcon+(dark?L":mono-light":L":mono-dark"),mark,theme.Ink());
                    }
                    if(!drawn&&shell.icons.contains(row.id))drawn=bitmap(row.id,mark,false);
                    if(!drawn&&brand)drawn=toolbarSvg.Draw(c,NativeBrandDeviceGlyph(item),mark,theme.Ink());
                    if(!drawn&&!brand){
                        auto official=HonorModuleGlyph(item);
                        if(app.honor.Draw(app.graphics,c,HonorDockFile(item,HonorScheme(app.model.preferences)),mark))drawn=true;
                        else if(toolbarSvg.Draw(c,official.empty()?value:official,mark,theme.Ink()))drawn=true;
                        else toolbarSvg.Draw(c,L"TbDeviceUnknown",mark,theme.Ink());
                    }
                    cursor+=markWidth;
                }
                if(textW>0){
                    if(cursor>x)cursor+=4;
                    // +1 keeps the last stem inside the layout box (DrawTextW clips).
                    p.Text(text,{cursor,y,cursor+textW+1,y+iconSize},iconSize*.8f,theme.Ink(),600);
                }
            }
            else if(!bitmap(row.id,r,false))toolbarSvg.Draw(c,L"FaRegCircleUser",r,theme.Ink());
            x+=span;
        }
    }
    auto badge=Str(item,L"badge");if(!badge.empty()){
        float bw=std::max(12.f,MeasureToolbarText(badge,iconSize*.625f));D2D1_RECT_F r{w-bw,0,w,12};
        p.Fill(r,theme.Accent(),iconSize);p.Text(badge,r,iconSize*.625f,theme.OnAccent(),500,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}
inline void NativeBar::AnimateToolbarOffset(Visual const &visual,float3 offset,bool drop) {
    if(!NativeMotion(app.model)){visual.StopAnimation(L"Offset");visual.Offset(offset);return;}
    auto &compositor=app.graphics.compositor;auto animation=compositor.CreateVector3KeyFrameAnimation();
    if(dock&&!barDrag&&!drop){
        animation.InsertExpressionKeyFrame(0,L"this.StartingValue + Vector3("+std::to_wstring(dockShiftX)+L","+std::to_wstring(dockShiftY)+L",0)");
        animation.SetVector3Parameter(L"destination",offset);animation.SetVector3Parameter(L"shift",{dockShiftX,dockShiftY,0});
        for(int i=1;i<=40;i++){float p=i/40.f,k=NativeInteractionMotion::DockSpring(p);animation.InsertExpressionKeyFrame(p,L"(this.StartingValue + shift) * (1 - "+std::to_wstring(k)+L") + destination * "+std::to_wstring(k),compositor.CreateLinearEasingFunction());}
        animation.Duration(std::chrono::milliseconds(NativeInteractionMotion::DockBodyMs));visual.StartAnimation(L"Offset",animation);return;
    }
    animation.InsertExpressionKeyFrame(0,L"this.StartingValue");
    animation.InsertKeyFrame(1,offset,compositor.CreateCubicBezierEasingFunction(drop?float2{.25f,.1f}:float2{.25f,1},drop?float2{.25f,1}:float2{.5f,1}));
    // Preserve the presented position for this.StartingValue at drop time.
    animation.Duration(std::chrono::milliseconds(250));visual.StartAnimation(L"Offset",animation);
}
inline void NativeBar::ToolbarFade(Visual const &visual,bool entering) {
    auto &compositor=app.graphics.compositor;
    if(!NativeMotion(app.model)){visual.StopAnimation(L"Scale");visual.Scale({1,1,1});visual.StopAnimation(L"Opacity");visual.Opacity(entering?1.f:0.f);return;}
    if(dock&&!barDrag){
        auto scale=NativeInteractionMotion::DockSpringVector(compositor,entering?float3{0,0,1}:float3{1,1,1},entering?float3{1,1,1}:float3{.15f,.15f,1});visual.StartAnimation(L"Scale",scale);
        auto opacity=compositor.CreateScalarKeyFrameAnimation();for(int i=0;i<=40;i++){float p=i/40.f,k=std::clamp(NativeInteractionMotion::DockSpring(p),0.f,1.f);opacity.InsertKeyFrame(p,entering?k:1-k,compositor.CreateLinearEasingFunction());}opacity.Duration(std::chrono::milliseconds(NativeInteractionMotion::DockBodyMs));visual.StartAnimation(L"Opacity",opacity);return;
    }
    auto easing=compositor.CreateCubicBezierEasingFunction({.25f,.1f},{.25f,1});
    auto opacity=compositor.CreateScalarKeyFrameAnimation();opacity.InsertKeyFrame(0,entering?0.f:1.f);opacity.InsertKeyFrame(1,entering?1.f:0.f,easing);opacity.Duration(std::chrono::milliseconds(200));visual.StartAnimation(L"Opacity",opacity);
    auto scale=compositor.CreateVector3KeyFrameAnimation();float from=entering?.4f:1,to=entering?1.f:.4f;scale.InsertKeyFrame(0,{from,from,1});scale.InsertKeyFrame(1,{to,to,1},easing);scale.Duration(std::chrono::milliseconds(200));visual.StartAnimation(L"Scale",scale);
}
inline void NativeBar::SyncToolbarVisuals() {
    std::set<std::wstring> live;auto now=GetTickCount64();size_t bytes=0;for(auto const &[_,v]:toolbarVisuals)bytes+=v.bytes+v.mediaBytes;
    // Axial clipping is installed before any item becomes visible. Keep inward
    // room for magnification, but never expose icons beyond the scroll viewport.
    if(dock&&dockViewportSide!=Str(settings,L"position")){dockViewportClip=nullptr;dockViewportSide=Str(settings,L"position");}
    auto clip=dock?dockViewportClip:app.graphics.compositor.CreateInsetClip();if(!clip){clip=app.graphics.compositor.CreateInsetClip();dockViewportClip=clip;}
    if(dock){float halo=(iconSize*NativeInteractionMotion::DockGain+NativeInteractionMotion::DockLift+16)*dpi/96;auto side=Str(settings,L"position");if(Vertical()){if(side==L"Left")clip.RightInset(-halo);else clip.LeftInset(-halo);}else{if(side==L"Top")clip.BottomInset(-halo);else clip.TopInset(-halo);}}
    toolbarLayer.Size({width*dpi/96,height*dpi/96});toolbarLayer.Clip(clip);
    bool first=toolbarVisuals.empty();
    // T39: a whole-block tray drag keeps the button under the pointer and folds the
    // icons into it. Slots and offsets stay frozen; only each icon's inner visual
    // animates, so the gesture keeps following the pointer.
    bool trayFold=trayCollapseActive&&barDrag&&!dragTray;
    D2D1_RECT_F trayButtonRect{};bool trayButtonValid=false;
    if(trayFold)for(size_t k=0;k<rows.size()&&k<entries.size();++k)if(rows[k].control==L"drag-ghost"&&TrayRoot(entries[k])){trayButtonRect=rows[k].rect;trayButtonValid=true;break;}
    for(int i=0;i<static_cast<int>(rows.size());++i){auto const &row=rows[i];if(row.offscreen||Bool(entries[i],L"compoundParent")||(!dock&&row.control==L"bar-separator")||row.control==L"corner")continue;
        auto id=row.id;bool ghost=row.control==L"drag-ghost";if(ghost)id=L"@drag:"+id;live.insert(id);
        auto &v=toolbarVisuals[id];bool fresh=!v.visual;
        float w=row.rect.right-row.rect.left,h=row.rect.bottom-row.rect.top;
        float extra=dock?8.f:!CompoundOwner(entries[i]).empty()?2.f:0.f;int pw=std::max(1,static_cast<int>(std::ceil((w+2*extra)*dpi/96))),ph=std::max(1,static_cast<int>(std::ceil((h+2*extra)*dpi/96)));
        size_t size=static_cast<size_t>(pw)*ph*4;
        auto item=entries[i];auto source=dock?Str(item,L"key"):Str(item,L"module");if(source.empty())source=row.id;
        // Every field the painter reads has to be in this list, because it is what
        // decides whether the cached raster is still valid. The brand fields were
        // missing: swapping the mark kept the box the same size, so the signature
        // did not change and the old raster stayed on screen. Only a wordmark edit
        // repainted, and only because it changed the width.
        JsonObject ink;for(auto field:{L"label",L"icon",L"imageVersion",L"plugin",L"type",L"hasPlayer",L"playing",L"lyricsText",L"canPlay",L"canPause",L"canPrevious",L"canNext",L"artist",L"badge",L"trashCount",L"glyph",L"passive",L"squareIcon",L"batteries",L"acLineStatus",L"dockVolume",L"dockMuted",L"dockConnected",L"dockLanguage",L"dockKeyboard",L"dockDnd",L"dockOnline",L"dockWifi",L"batteryPercent",L"bluetoothOn",L"wifiSignal",L"hasNotifications",L"networkStateKnown",L"customIcon",L"iconExecutable",L"extensionSession",L"extensionRevision",L"extensionGranted",L"brandIcon",L"brandIconHide",L"brandLabel",L"brandText",L"brandStyle",L"brandDevice"})if(auto value=item.TryLookup(field))ink.Insert(field,value);
        if(dock){auto windows=item.GetNamedArray(L"windows",JsonArray{});Put(ink,L"instances",static_cast<double>(windows.Size()));if(Bool(settings,L"showWindowTitle")&&windows.Size())Put(ink,L"title",Str(windows.GetAt(0).GetObject(),L"title"));}
        std::wstring signature=HonorScheme(app.model.preferences)+std::wstring(ink.Stringify())+L":"+std::to_wstring(pw)+L":"+std::to_wstring(ph)+L":"+std::to_wstring(dpi)+L":"+std::to_wstring(dark)+L":"+std::to_wstring(Preview70Theme::revision)+L":"+std::to_wstring(dock&&i==focus&&!barDrag)+L":"+std::to_wstring(downId==row.id)+L":"+std::to_wstring(i==focus?mediaHover:-1)+L":"+std::to_wstring(downId==row.id?mediaDown:-1)+L":"+std::wstring(ToolbarContent(item).Stringify())+L":"+std::to_wstring(Bool(app.model.preferences,L"autoContrast",true));
        // Official Dock clock is a static asset; its tooltip still follows the minute event.
        if(row.id==L"@magic/pages")signature+=std::to_wstring(pageAlpha);
        if(auto badge=shell.icons.find(Str(item,L"key")+L":badge");badge!=shell.icons.end())signature+=std::to_wstring(reinterpret_cast<UINT_PTR>(badge->second.bitmap.get()));

        if(auto icon=shell.icons.find(dock?Str(item,L"key"):row.id);icon!=shell.icons.end())signature+=std::to_wstring(reinterpret_cast<UINT_PTR>(icon->second.bitmap.get()));
        if(fresh) {v.group=app.graphics.compositor.CreateContainerVisual();v.visual=app.graphics.compositor.CreateSpriteVisual();v.hoverLayer=app.graphics.compositor.CreateContainerVisual();v.hoverLayer.Children().InsertAtBottom(v.visual);v.group.Children().InsertAtBottom(v.hoverLayer);toolbarLayer.Children().InsertAtTop(v.group);v.group.Offset({(row.rect.left-extra)*dpi/96,(row.rect.top-extra)*dpi/96,0});}
        if(v.retireAt){v.group.StopAnimation(L"Scale");v.group.Scale({1,1,1});v.group.StopAnimation(L"Opacity");v.group.Opacity(1);if(v.visual){v.visual.StopAnimation(L"Scale");v.visual.Scale({1,1,1});v.visual.StopAnimation(L"Opacity");v.visual.Opacity(1);}}
        v.retireAt=0;v.visual.Size({static_cast<float>(pw),static_cast<float>(ph)});v.group.Size({static_cast<float>(pw),static_cast<float>(ph)});v.group.CenterPoint({pw/2.f,ph/2.f,0});
        if(ghost&&v.visual&&trayFold&&trayButtonValid&&Str(item,L"type")==L"Tray"&&!trayCollapsedGhosts.contains(id)&&
           NativeMotion(app.model)&&Bool(app.model.preferences,L"toolbarDragButtonOnly",true)){
            // The anchor is the button centre in the icon ghost's own local pixels:
            // rows were laid out minus the block origin, the group offset subtracts
            // extra, and the painted cell starts at extra again. Both rects live in
            // the same layer coordinates, so their difference is stable while the
            // pointer moves; dpi/96 converts DIP to the compositor's physical pixels.
            float anchorX=(trayButtonRect.left-row.rect.left+(trayButtonRect.right-trayButtonRect.left)/2+extra)*dpi/96;
            float anchorY=(trayButtonRect.top-row.rect.top+(trayButtonRect.bottom-trayButtonRect.top)/2+extra)*dpi/96;
            auto easing=app.graphics.compositor.CreateCubicBezierEasingFunction({.25f,.1f},{.25f,1});
            auto scale=app.graphics.compositor.CreateVector3KeyFrameAnimation();scale.InsertKeyFrame(0,{1,1,1});scale.InsertKeyFrame(1,{.15f,.15f,1},easing);scale.Duration(std::chrono::milliseconds(160));
            v.visual.CenterPoint({anchorX,anchorY,0});v.visual.StartAnimation(L"Scale",scale);
            auto opacity=app.graphics.compositor.CreateScalarKeyFrameAnimation();opacity.InsertKeyFrame(0,1.f);opacity.InsertKeyFrame(1,0.f,easing);opacity.Duration(std::chrono::milliseconds(160));
            v.visual.StartAnimation(L"Opacity",opacity);
            trayCollapsedGhosts.insert(id);
        }
        if(signature!=v.rasterKey) {
            if(!v.surface||v.bytes!=size){if(bytes-v.bytes+size>8*1024*1024){toolbarLayer.Children().Remove(v.group);bytes-=v.bytes+v.mediaBytes;toolbarVisuals.erase(id);continue;}
                bytes=bytes-v.bytes+size;v.bytes=size;v.surface=app.graphics.composition.CreateDrawingSurface({static_cast<float>(pw),static_cast<float>(ph)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);v.visual.Brush(app.graphics.compositor.CreateSurfaceBrush(v.surface));}
            auto interop=v.surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};
            check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));
            try {c->SetDpi(dpi,dpi);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x*96/dpi+extra,offset.y*96/dpi+extra));c->Clear(D2D1::ColorF(0,0));if(dock)PaintDockItem(c.get(),i,Str(item,L"type")==L"Media");else PaintToolbarItem(c.get(),i);check_hresult(interop->EndDraw());}
            catch(...){interop->EndDraw();throw;}v.rasterKey=std::move(signature);++itemRedraws;
        }
        size_t priorMediaBytes=v.mediaBytes;SyncMediaTitle(v,i,extra);bytes=bytes-priorMediaBytes+v.mediaBytes;
        SyncMediaSize(v,i,extra,fresh);
        float3 target{(row.rect.left-extra)*dpi/96,(row.rect.top-extra)*dpi/96,0};
        if(ghost){v.group.StopAnimation(L"Offset");v.group.Offset(target);toolbarLayer.Children().Remove(v.group);toolbarLayer.Children().InsertAtTop(v.group);}
        else if(!fresh&&(row.rect.left!=v.rect.left||row.rect.top!=v.rect.top||dockShiftX!=0||dockShiftY!=0))AnimateToolbarOffset(v.group,target);
        if(fresh&&!first&&!ghost&&!(dock&&dockReorderHoverBlocked))ToolbarFade(v.group,true);
        // A tray icon is grabbed on its own, so the row id is what marks it; every other
        // row is grabbed through the module it belongs to.
        float modelOpacity=!ghost&&barDrag&&(source==dragId||row.id==dragId)?(dock?.3f:.2f):!ghost&&source==dropId?0.f:1.f;if(modelOpacity!=v.modelOpacity){v.group.StopAnimation(L"Opacity");v.group.Opacity(modelOpacity);v.modelOpacity=modelOpacity;}
        if(dock)SyncDockState(v,i,extra);else if(Str(item,L"type")==L"Extension"){v.group.IsVisible(Bool(item,L"extensionGranted"));SyncExtensionMotion(v,item);if(v.surface)extensionPreparedSessions[row.id]=Str(item,L"extensionSession");}
        v.rect=row.rect;
    }
    for(auto it=toolbarVisuals.begin();it!=toolbarVisuals.end();) {
        auto &v=it->second;if(live.contains(it->first)){++it;continue;}
        if(!v.retireAt){bool ghost=it->first.starts_with(L"@drag:");
            if(ghost&&dropRows.contains(it->first.substr(6))){auto target=std::find_if(rows.begin(),rows.end(),[&](auto const &r){return r.id==it->first.substr(6);});
                if(target!=rows.end()){auto index=static_cast<size_t>(target-rows.begin());float extra=dock?8.f:CompoundOwner(entries[index]).empty()?0.f:2.f;AnimateToolbarOffset(v.group,{(target->rect.left-extra)*dpi/96,(target->rect.top-extra)*dpi/96,0},true);}
                v.retireAt=now+250;
                // T39: the folded icons unfold as the ghost lands, so the 250 ms
                // handoff hands a full-size icon to the real row.
                if(trayCollapsedGhosts.erase(it->first)&&v.visual&&NativeMotion(app.model)){
                    auto easing=app.graphics.compositor.CreateCubicBezierEasingFunction({.25f,.1f},{.25f,1});
                    auto scale=app.graphics.compositor.CreateVector3KeyFrameAnimation();scale.InsertExpressionKeyFrame(0,L"this.StartingValue");scale.InsertKeyFrame(1,{1,1,1},easing);scale.Duration(std::chrono::milliseconds(160));scale.DelayTime(std::chrono::milliseconds(80));v.visual.StartAnimation(L"Scale",scale);
                    auto opacity=app.graphics.compositor.CreateScalarKeyFrameAnimation();opacity.InsertExpressionKeyFrame(0,L"this.StartingValue");opacity.InsertKeyFrame(1,1.f,easing);opacity.Duration(std::chrono::milliseconds(160));opacity.DelayTime(std::chrono::milliseconds(80));v.visual.StartAnimation(L"Opacity",opacity);
                }
            }else{trayCollapsedGhosts.erase(it->first);if(dock&&(dockShiftX!=0||dockShiftY!=0))AnimateToolbarOffset(v.group,{(v.rect.left-8)*dpi/96+dockShiftX,(v.rect.top-8)*dpi/96+dockShiftY,0});ToolbarFade(v.group,false);v.retireAt=now+(dock&&!barDrag?NativeInteractionMotion::DockBodyMs:200);}
            SetTimer(hwnd,97,260,nullptr);
        }
        if(!NativeMotion(app.model)||now>=v.retireAt){trayCollapsedGhosts.erase(it->first);toolbarLayer.Children().Remove(v.group);it=toolbarVisuals.erase(it);}else ++it;
    }
    dockShiftX=dockShiftY=0;
    if(dock)SyncDockHover();else ReportExtensions();
    ULONGLONG next=dropUntil?dropUntil:MAXULONGLONG;for(auto const &[_,v]:toolbarVisuals)if(v.retireAt)next=std::min(next,v.retireAt);
    if(next!=MAXULONGLONG)SetTimer(hwnd,97,static_cast<UINT>(std::clamp<ULONGLONG>(next>now?next-now:1,1,1000)),nullptr);else KillTimer(hwnd,97);

}
