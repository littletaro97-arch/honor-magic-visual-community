#pragma once
// Implementations use the existing PopupRow command identities and native EDITs.
// The same rectangles feed drawing, hit-testing and UI Automation.
inline bool NativeConnectionRow(PopupRow const &r){return r.id.rfind(L"radio:",0)==0||r.id==L"wifi"||r.id==L"bluetooth";}
inline bool NativePopup::PowerView() const
{
    if(!generic||!packet)return false;auto data=Data();auto value=data.TryLookup(L"rows");if(!value||value.ValueType()!=JsonValueType::Array)return false;
    for(auto const &v:value.GetArray())if(Str(v.GetObject(),L"id")==L"power-status")return true;return false;
}
#include "native-functional-panels.h"
inline int NativeAppsColumns(float width){return std::clamp(static_cast<int>(std::floor((width-28)/108)),2,6);}
inline int NativeAppsRows(float height){return std::clamp(static_cast<int>(std::floor((height-150)/112)),1,3);}
inline void NativePopup::LayoutPreview70()
{
    if(Flyout()){BuildFlyout();return;}
    if(LocalDockPreview()||LocalPicker()||LocalContext())return;
    if(kind==L"@magic/apps-menu")for(auto const &value:Data().GetNamedArray(L"rows",JsonArray{})){
        auto row=value.GetObject();auto key=Str(row,L"id"),source=Str(row,L"image")+L":"+Str(row,L"iconUmid")+L":"+Str(row,L"imageVersion");
        if(imageSources.contains(key)&&imageSources[key]!=source){auto found=appearanceIcons.values.find(key);if(found!=appearanceIcons.values.end()){appearanceIcons.bytes-=found->second.bytes;appearanceIcons.values.erase(found);appearanceIcons.glyphs.erase(key);}iconRequests.erase(key);}
        imageSources[key]=source;auto png=Str(row,L"iconPng");
        if(!png.empty()){if(!appearanceIcons.values.contains(key))appearanceIcons.Load(app.graphics,key,png);row.Remove(L"iconPng");}
    }
    appearanceOverflow=appearanceTop=appearanceView=appearanceOffset=userMenuBottom=0;
    float left=18,right=width-18,y=58;
    auto set=[&](PopupRow &r,D2D1_RECT_F b){r.rect=b;r.offscreen=false;};
    if(FunctionalPanel()){LayoutFunctionalPanel();}
    else if(kind==L"@magic/calendar-popup"){
        float cell=(width-36)/7;
        for(auto &r:rows){
            if(r.key==L"previous")set(r,{width-90,22,width-58,54});
            else if(r.key==L"heading")set(r,{18,22,width-100,54});
            else if(r.key==L"next")set(r,{width-54,22,width-22,54});
            else if(r.action==L"day"){
                using namespace std::chrono;
                auto first=year_month_day{std::chrono::year{year},std::chrono::month{static_cast<unsigned>(month)},day{1}};
                int lead=(static_cast<int>(weekday{sys_days{first}}.c_encoding())-firstWeekday+7)%7;
                int index=lead+_wtoi(r.id.c_str())-1;
                set(r,{left+index%7*cell,88+index/7*cell,left+(index%7+1)*cell-4,88+(index/7+1)*cell-4});
            }else if(r.action==L"month"){
                int i=_wtoi(r.id.c_str())-1;float w=(width-40)/3;
                set(r,{20+i%3*w,82+i/3*58.f,20+(i%3+1)*w-8,132+i/3*58.f});
            }else if(r.key==L"today")set(r,{18,height-52,78,height-20});
            else if(r.key==L"settings")set(r,{86,height-52,width-18,height-20});
        }
    }else if(kind==L"@magic/user-menu"){
        // Row height and the gap between rows belong to the reader: the card is read
        // at a glance and some people need it airier than the vendor shipped it.
        double wantedRow=Num(app.model.preferences,L"accountCardRowHeight");
        double wantedGap=Num(app.model.preferences,L"accountCardGap");
        float rowHeight=(wantedRow>=28&&wantedRow<=52)?static_cast<float>(wantedRow):38.f;
        float rowGap=(wantedGap>=0&&wantedGap<=16)?static_cast<float>(wantedGap):0.f;
        y=76;
        for(auto &r:rows){
            if(r.control==L"label"){set(r,{left+10,y+5,right-10,y+6});y+=13+rowGap;continue;}
            set(r,{left,y,right,y+rowHeight});y+=rowHeight+rowGap;
        }
        // The height contract reads this before it settles the card size: the label
        // rows advance the flow, so their 13 DIP plus gap is part of the content.
        userMenuBottom=rows.empty()?76.f:rows.back().rect.bottom;
        // Past the work-area ceiling the card scrolls instead of compressing rows the
        // reader chose. The bound is the real content, not a visibleRows window: these
        // rows are 30-52 DIP tall, so a fixed 56 DIP count would strand the tail.
        float viewportBottom=height-18,origin=rows.empty()?76.f:rows.front().rect.top;
        int maximum=0;
        if(userMenuBottom>viewportBottom+.5f)
            for(int k=1;k<static_cast<int>(rows.size());++k){
                maximum=k;
                if(userMenuBottom-(rows[k].rect.top-origin)<=viewportBottom+.5f)break;
            }
        scroll=std::clamp(scroll,0,maximum);
        // One offset for both the paint and the hit test, and rows outside the card's
        // content window are marked here rather than left to the generic row window.
        float offset=scroll>0&&scroll<static_cast<int>(rows.size())?rows[scroll].rect.top-origin:0.f;
        // The generic thumb painter reads these four fields. The card feeds them from
        // its real content, so overflow shows without hovering a row.
        appearanceTop=origin;
        appearanceView=std::max(1.f,viewportBottom-origin);
        appearanceOffset=offset;
        appearanceOverflow=std::max(0.f,userMenuBottom-viewportBottom);
        for(auto &r:rows){if(offset){r.rect.top-=offset;r.rect.bottom-=offset;}r.offscreen=r.rect.bottom<=origin||r.rect.top>=height-16;}
    }else if(!generic){
        y=54;for(auto &r:rows){if(r.key==L"settings")set(r,{left,height-52,right,height-20});
            else {set(r,{left,y,right,y+54});y+=58;}}
    }else{
        bool apps=kind==L"@magic/apps-menu",tray=kind==L"@magic/system-tray",quick=kind==L"@magic/quick-settings"&&!PowerView();
        bool powerMenu=kind==L"@magic/power-menu",tasks=kind==L"@magic/task-switcher";
        bool secondary=kind==L"@magic/network-popup"||kind==L"@magic/bluetooth-popup"||kind==L"@magic/media-popup";
        if(quick){y=18;std::stable_partition(rows.begin(),rows.end(),[](auto const &r){return r.control==L"toggle";});}
        if(powerMenu)y=116;
        int radioIndex=0,modeIndex=0;
        if(quick){int radios=0,modes=0;for(auto const &r:rows)if(r.control==L"toggle"){if(NativeConnectionRow(r))++radios;else ++modes;}y=22+std::max(radios,modes)*48.f+8;}
        int column=0;float tileTop=y;float previousTileHeight=80;D2D1_RECT_F notification{};
        D2D1_RECT_F volume{};
        for(auto &r:rows){
            if(quick&&r.control==L"toggle"){
                bool radio=NativeConnectionRow(r);int index=radio?radioIndex++:modeIndex++;float half=(right-left-8)/2,x=radio?left:left+half+8;
                set(r,{x+4,22+index*48.f,x+half-4,66+index*48.f});continue;
            }
            if(secondary&&r.id.find(L"settings")!=r.id.npos){set(r,{left,height-52,right,height-18});continue;}
            if(secondary&&r.control==L"toggle"&&NativeConnectionRow(r)){
                // Keep every offered radio operation; only a sole toggle fits in the header.
                int radios=0;for(auto const &row:rows)radios+=row.control==L"toggle"&&NativeConnectionRow(row)?1:0;
                if(radios==1){set(r,{right-42,22,right,44});continue;}
            }
            if(apps&&(r.id==L"apps-display"||r.id==L"apps-power")){set(r,{-100,-100,-80,-80});r.offscreen=true;continue;}
            if(apps&&r.id==L"apps-view"){set(r,{width-104,10,width-18,44});r.label=Str(Data(),L"appsView")==L"all"?Ui(L"已固定",L"Pinned"):Ui(L"全部应用",L"All apps");continue;}
            if(apps&&(r.id==L"apps-previous"||r.id==L"apps-next")){bool next=r.id==L"apps-next";set(r,{width-240+(next?90.f:0),height-50,width-160+(next?90.f:0),height-18});continue;}
            if(apps&&r.id==L"apps-back"){set(r,{240,height-50,350,height-18});continue;}
            if(apps&&r.id==L"apps-power"){set(r,{width-58,height-50,width-24,height-18});continue;}
            if(kind==L"@magic/quick-settings"&&(r.id==L"power"||r.id==L"quick-back")){set(r,{width-164,height-54,width-62,height-22});continue;}
            if(kind==L"@magic/quick-settings"&&r.id==L"system-power"){set(r,{width-54,height-54,width-22,height-22});continue;}
            if(quick&&r.id.rfind(L"mute:",0)==0&&volume.right>0){set(r,{volume.left,volume.top+32,volume.left+32,volume.bottom});continue;}
            if(apps&&r.id==L"search"){bool all=Str(Data(),L"appsView")==L"all";set(r,{left,all?48.f:-100.f,right,all?82.f:-66.f});r.offscreen=!all;y=all?96:66;tileTop=y;continue;}
            if(kind==L"@magic/notifications"&&r.id.rfind(L"dismiss:",0)==0&&notification.right>0){set(r,{notification.right-34,notification.top+4,notification.right-4,notification.top+34});continue;}
            bool transport=r.id.rfind(L"player:",0)==0;
            if(transport){float w=(right-left-16)/3;if(column==0)tileTop=y;set(r,{left+column*(w+8),tileTop,left+column*(w+8)+w,tileTop+36});
                column++;if(column==3){column=0;y=tileTop+44;}previousTileHeight=36;continue;}
            bool tile=(apps&&r.id.rfind(L"app:",0)==0)||tray||(quick&&r.control==L"toggle")||powerMenu||tasks;
            if(tile){int cols=apps?NativeAppsColumns(width):tray?5:powerMenu||tasks?3:2;float gap=8,w=(right-left-(cols-1)*gap)/cols;
                float h=apps?std::max(64.f,(height-62-(Str(Data(),L"appsView")==L"all"?96.f:66.f)-8*(NativeAppsRows(height)-1))/NativeAppsRows(height)):tray?48.f:powerMenu?100.f:tasks?174.f:80.f;
                if(column==0)tileTop=y;
                float bottom=tileTop+h;if(apps&&std::abs(bottom-(height-62))<.05f)bottom=height-62;
                set(r,{left+column*(w+gap),tileTop,left+column*(w+gap)+w,bottom});
                previousTileHeight=h;column++;if(column==cols){column=0;y=tileTop+h+8;}
            }else{
                if(column){y=tileTop+previousTileHeight+8;column=0;}
                float h=r.control==L"range"?(quick?76.f:64.f):r.control==L"text"||r.control==L"password"?64:r.detail.empty()?36:56;
                if(kind==L"@magic/notifications"&&r.id.rfind(L"notification:",0)==0)h=96;
                if(secondary&&r.control!=L"range"&&r.control!=L"text"&&r.control!=L"password")h=48;
                if(PowerView()){
                    if(r.id==L"power-refresh"){set(r,{width-76,22,width-18,50});continue;}
                    if(r.id==L"power-status")h=96;
                    else if(r.id==L"power-mode"){y+=24;h=32;}
                    else if(r.id==L"saver"||r.id==L"honor-mode")h=32;
                    else if(r.id.rfind(L"battery:",0)==0){y+=16;h=76;}
                    else if(r.id==L"quick-back"||r.id==L"power-settings")h=38;
                }
                set(r,{left,y,right,y+h});if(kind==L"@magic/notifications"&&r.id.rfind(L"notification:",0)==0)notification=r.rect;if(quick&&r.id.rfind(L"volume:",0)==0)volume=r.rect;y+=h+(PowerView()&&(r.id==L"power-mode"||r.id==L"saver")?0:8);
            }
        }
        // Scroll by complete row bands; search and refresh remain anchored.
        int first=std::clamp(scroll,0,std::max(0,static_cast<int>(rows.size())-1));
        float origin=apps?(Str(Data(),L"appsView")==L"all"?96.f:66.f):quick?18:58,offset=scroll&&first<static_cast<int>(rows.size())?std::max(0.f,rows[first].rect.top-origin):0;
        if(apps){std::vector<float> bands;float bottom=origin;for(auto const &row:rows){if(row.id==L"search"||row.id.rfind(L"apps-",0)==0)continue;if(bands.empty()||bands.back()!=row.rect.top)bands.push_back(row.rect.top);bottom=std::max(bottom,row.rect.bottom);}
            int maximum=0;while(maximum+1<static_cast<int>(bands.size())&&bottom-bands[maximum]>height-62-origin)++maximum;scroll=std::clamp(scroll,0,maximum);offset=bands.empty()?0:bands[scroll]-origin;
        }
        appearanceTop=origin;appearanceView=std::max(1.f,(apps?height-62:height-18)-origin);appearanceOffset=offset;
        for(auto &r:rows){if(apps&&((r.id==L"search"&&Str(Data(),L"appsView")!=L"all")||r.id==L"apps-display"||r.id==L"apps-power")){r.offscreen=true;continue;}bool pinned=(secondary&&r.id.find(L"settings")!=r.id.npos)||(secondary&&r.control==L"toggle"&&r.rect.top==22)||(apps&&(r.id==L"search"||r.id==L"apps-power"||r.id==L"apps-view"||r.id==L"apps-back"||r.id==L"apps-previous"||r.id==L"apps-next"||r.id==L"apps-display"))||kind==L"@magic/quick-settings"&&(r.id==L"power"||r.id==L"quick-back"||r.id==L"system-power")||(PowerView()&&r.id==L"power-refresh");
            if(!pinned){appearanceOverflow=std::max(appearanceOverflow,r.rect.bottom-(appearanceTop+appearanceView));r.rect.top-=offset;r.rect.bottom-=offset;r.offscreen=r.rect.bottom<=origin||r.rect.top>=height-18||(apps||secondary)&&r.rect.bottom>height-62;}
        }
    }
    for(auto const &row:rows){auto it=edits.find(row.id);if(it==edits.end())continue;
        auto r=row.rect;bool search=kind==L"@magic/apps-menu"&&row.id==L"search";
        float top=search?r.top+6:r.top+25;
        PlaceInput(row.id,static_cast<int>((r.left+(search?34:8))*dpi/96),static_cast<int>(top*dpi/96),
            static_cast<int>((r.right-r.left-(search?44:16))*dpi/96),static_cast<int>(24*dpi/96),!opening&&!dismissing&&!resizeInputsBlocked&&!row.offscreen&&(r.top>=58||search));
        if(reinterpret_cast<HFONT>(SendMessageW(it->second,WM_GETFONT,0,0))!=inputFont)
            SendMessageW(it->second,WM_SETFONT,reinterpret_cast<WPARAM>(inputFont),TRUE);
    }
}
inline void NativePopup::PaintPreview70(ID2D1DeviceContext *c)
{
    if(Flyout()){PaintFlyout(c);return;}
    if(FunctionalPanel()){PaintFunctionalPanel(c);return;}
    if(LocalDockPreview()){PaintDockPreview(c);return;}
    if(LocalTooltip()){
        NativePaint p(c,app.graphics,dark);p.Material({0,0,width,height},NativeMaterial::Menu,Glass(),10);
        appearanceIcons.Draw(c,L"attachment",{8,8,width-8,144});
        if(tooltipLayout.text)c->DrawTextLayout({8,tooltipLayout.top},tooltipLayout.text.get(),p.Color(p.theme.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    if(LocalContext()){PaintContext(c);return;}
    if(LocalPicker()){PaintPicker(c);return;}
    NativePaint p(c,app.graphics,dark);auto &t=p.theme;
    D2D1_RECT_F outer{NativeMetrics::PopupInset,NativeMetrics::PopupInset,width-NativeMetrics::PopupInset,height-NativeMetrics::PopupInset};if(!resizePainting){if(!NativeLowPower(app.model))p.Shadow(outer,MaterialRadius(),NativeMetrics::PopupInset);p.Material(outer,MaterialTier(),Glass(),MaterialRadius(),kind==L"@magic/system-tray"||kind==L"@magic/apps-menu");}
    bool calendar=kind==L"@magic/calendar-popup",user=kind==L"@magic/user-menu",apps=kind==L"@magic/apps-menu";
    if(apps)p.Text(Ui(L"应用",L"Apps"),{22,10,width-120,44},15,t.MaterialMuted(),400);
    if(user){
        // Brand header, same shape as the brand menu: machine brand on top,
        // product and version below, hairline before the first group.
        p.Text(NativeBrandName(),{26,14,width-26,38},15,t.Ink(),600);
        auto version=Str(Account(),L"version");
        std::wstring subtitle=version.empty()?std::wstring(L"Magic 视界社区版"):std::wstring(L"Magic 视界社区版 ")+version;
        p.Text(subtitle,{26,38,width-26,56},11,t.MaterialMuted());
        p.Line(22,64,width-22,64,t.Gray(900,.10f));
    }else if(kind==L"@magic/power-menu"){
        c->FillEllipse({{48,50},24,24},p.Color(t.Accent()));p.Icon(L"user",{34,36,62,64},t.OnAccent());
        p.Text(Ui(L"再见，",L"Goodbye, ")+Str(Account(),L"name"),{88,28,width-24,52},14,t.Ink(),600);
        p.Text(L"Magic 视界社区版",{88,54,width-24,76},12,t.Gray(700));p.Line(24,96,width-24,96,t.Gray(100));
    }else if(!calendar&&!apps&&(kind!=L"@magic/quick-settings"||PowerView()))p.Text(PowerView()?Ui(L"电量与性能",L"Battery & performance"):caption,{22,21,width-76,47},PowerView()?16.f:13.5f,t.Ink(),600);
    if(calendar&&!yearMode){float cell=(width-36)/7;wchar_t const *zh=L"日一二三四五六";wchar_t const *en[]={L"Su",L"Mo",L"Tu",L"We",L"Th",L"Fr",L"Sa"};
        for(int i=0;i<7;i++)p.Text(english?en[(i+firstWeekday)%7]:std::wstring(1,zh[(i+firstWeekday)%7]),
            {18+i*cell,56,18+(i+1)*cell,84},12.8f,t.Gray(700),600,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if(PowerView()){
        D2D1_RECT_F modes{};
        for(auto const &r:rows){
            if(r.id==L"power-refresh")p.Text(r.label,r.rect,12.8f,t.Ink(),400,DWRITE_TEXT_ALIGNMENT_CENTER);
            if(r.id==L"power-mode"){modes=r.rect;modes.top-=24;}
            if(r.id==L"saver"||r.id==L"honor-mode")modes.bottom=r.rect.bottom+8;
        }
        if(modes.right>modes.left){p.Fill(modes,t.SectionFilm(),NativeMetrics::PanelRadius);p.Text(Ui(L"电源模式",L"Power modes"),{modes.left+8,modes.top+2,modes.right-8,modes.top+24},12,t.Gray(700),600);}
    }
    if(kind==L"@magic/quick-settings"&&!PowerView())for(bool radio:{true,false}){
        D2D1_RECT_F group{};for(auto const &r:rows)if(r.control==L"toggle"&&NativeConnectionRow(r)==radio){if(!group.right)group=r.rect;else group.bottom=std::max(group.bottom,r.rect.bottom);}
        if(group.right){group.left-=4;group.right+=4;group.top-=4;group.bottom+=4;p.Fill(group,t.SectionFilm(),16);}
    }
    bool secondary=kind==L"@magic/network-popup"||kind==L"@magic/bluetooth-popup"||kind==L"@magic/media-popup";
    if(secondary)for(auto const &r:rows)if(r.control==L"toggle"&&r.rect.top==22)p.Toggle(r.rect,r.value>.5,r.enabled);
    float clipTop=apps?18:calendar?10:ContentTop();
    c->PushAxisAlignedClip({14,clipTop,width-14,height-16},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if(apps)p.Fill({18,height-58,width-18,height-16},t.SectionFilm(),NativeMetrics::PanelRadius);
    for(int i=0;i<static_cast<int>(rows.size());i++){
        auto const &r=rows[i];if(r.offscreen||PowerView()&&r.id==L"power-refresh"||secondary&&r.control==L"toggle"&&r.rect.top==22)continue;auto rect=r.rect;
        if(kind==L"@magic/user-menu"&&r.control==L"label"){p.Line(rect.left+10,(rect.top+rect.bottom)/2,rect.right-10,(rect.top+rect.bottom)/2,t.Gray(900,.10f));continue;}
        bool active=r.active||r.control==L"toggle"&&r.value>.5;
        bool grid=calendar,quick=kind==L"@magic/quick-settings"&&!PowerView()&&r.control==L"toggle";
        bool tile=apps&&r.id.rfind(L"app:",0)==0||kind==L"@magic/system-tray"||kind==L"@magic/power-menu";
        bool input=r.control==L"text"||r.control==L"password";
        bool mode=PowerView()&&(r.id==L"power-mode"||r.id==L"saver"||r.id==L"honor-mode");
        bool card=PowerView()&&!mode||kind==L"@magic/notifications"&&r.id.rfind(L"notification:",0)==0||r.id.rfind(L"track:",0)==0;
        bool rangeCard=kind==L"@magic/quick-settings"&&!PowerView()&&r.control==L"range";
        if(card||input||rangeCard)p.Fill(rect,t.SectionFilm(),card||rangeCard?16:NativeMetrics::ControlRadius);
        // Tray promotion controls ordering, not a selected tile frame.
        if(kind!=L"@magic/system-tray")p.Interaction(InteractionRegion(i),r.active&&!grid&&!quick,materialPreview&&i==hover,i==focus&&!grid);
        // Calendar date and toggle state colors remain functional accents.
        if(r.active&&grid)p.Fill(rect,t.Accent(),NativeMetrics::ControlRadius);
        auto ink=user&&(r.key==L"exit"||r.key==L"shutdown")?t.Danger():!r.enabled&&r.control!=L"label"&&!card?t.Gray(400):active&&grid?t.OnAccent():t.Ink();
        std::wstring label=r.label;
        int confirmation=armed==r.action?ConfirmationSeconds():0;
        if(confirmation){ink=t.Danger();if(kind==L"@magic/power-menu")label=Ui(L"再次确认 ",L"Confirm ")+std::to_wstring(confirmation);}
        if(r.control==L"toggle"&&label.size()>2&&(label[0]==L'●'||label[0]==L'○'))label=label.substr(3);
        if(quick&&r.id==L"dark")label=r.value>.5?L"深色模式":L"浅色模式";
        if(quick&&r.id==L"night")label=L"护眼模式";
        if(PowerView()&&r.id==L"power-status"){
            auto percent=label.find(L' ');auto number=percent==label.npos?label:label.substr(percent+1);
            p.Text(number,{rect.left+12,rect.top+6,rect.left+145,rect.top+60},32,t.Ink(),500);
            p.Fill({rect.right-96,rect.top+16,rect.right-12,rect.top+44},t.SectionFilm(),4);
            p.Text(r.detail,{rect.right-92,rect.top+16,rect.right-16,rect.top+44},12,t.Gray(700),500,DWRITE_TEXT_ALIGNMENT_TRAILING);
            auto track=D2D1::RectF(rect.left+12,rect.bottom-20,rect.right-12,rect.bottom-16);p.Fill(track,t.Gray(900,.10f),2);
            track.right=track.left+(track.right-track.left)*Clamp(static_cast<float>(_wtof(number.c_str())/100),0,1);p.Fill(track,t.Accent(),2);
        }else if(kind==L"@magic/notifications"&&r.id.rfind(L"dismiss:",0)==0){
            p.Icon(L"close",{rect.left+7,rect.top+7,rect.left+23,rect.top+23},t.MaterialMuted());
        }else if(mode){
            p.Text(label,{rect.left+8,rect.top,rect.right-100,rect.bottom},12.8f,t.Gray(700));
            p.Text(r.detail,{rect.right-100,rect.top,rect.right-8,rect.bottom},12.8f,r.id==L"honor-mode"?t.Gray(400):t.Ink(),500,DWRITE_TEXT_ALIGNMENT_TRAILING);
        }else if(r.id.rfind(L"player:",0)==0){
            p.Fill(rect,t.SectionFilm(),6);auto suffix=r.id.substr(r.id.find_last_of(L':')+1);
            float cx=(rect.left+rect.right)/2;p.Icon(suffix==L"prev"?L"previous":suffix==L"next"?L"next":L"play",{cx-10,rect.top+8,cx+10,rect.top+28},ink);
        }else if(grid){
            if(r.key==L"previous"||r.key==L"next")p.Icon(r.key,{rect.left+7,rect.top+7,rect.left+25,rect.top+25},ink);
            else p.Text(label,rect,r.action==L"day"?12.8f:13.f,ink,r.key==L"heading"?600:400,
                r.key==L"heading"?DWRITE_TEXT_ALIGNMENT_LEADING:DWRITE_TEXT_ALIGNMENT_CENTER);
        }else if(quick){
            auto key=r.id;bool radio=NativeConnectionRow(r);
            if(radio){bool bluetooth=label.find(L"Bluetooth")!=label.npos||label.find(L"蓝牙")!=label.npos||r.id==L"bluetooth";key=bluetooth?L"bluetooth":L"wifi";label=bluetooth?L"蓝牙":L"Wi-Fi";}
            else if(r.id==L"dark")key=r.value>.5?L"dark":L"brightness";else if(r.id==L"night")key=L"eye";
            auto icon=active?t.Accent():t.MaterialMuted();p.Icon(key,{rect.left+8,rect.top+12,rect.left+28,rect.top+32},icon);
            p.Text(label,{rect.left+36,rect.top+3,rect.right-4,rect.top+24},12.5f,ink,500);
            p.Text(r.detail.empty()?(active?Ui(L"已开启",L"On"):Ui(L"已关闭",L"Off")):r.detail,{rect.left+36,rect.top+24,rect.right-4,rect.bottom-2},10.5f,t.MaterialMuted());
        }else if(tile){
            auto key=r.id;if(quick){key=label.find(L"蓝牙")!=label.npos?L"bluetooth":label.find(L"Wi")!=label.npos?L"wifi":r.id;}
            if(quick&&r.id==L"dark"&&r.value<.5)key=L"brightness";if(quick&&r.id==L"night")key=L"eye";
            if(tile&&apps){auto source=Data().GetNamedArray(L"rows");key=i<static_cast<int>(source.Size())&&Bool(source.GetAt(i).GetObject(),L"folder")?L"folder":L"apps";}if(kind==L"@magic/system-tray")key=L"tray";
            float cx=(rect.left+rect.right)/2,size=quick?30.f:tile&&apps?48.f:24.f;
            if(!appearanceIcons.Draw(c,r.id,{cx-size/2,rect.top+12,cx+size/2,rect.top+12+size},p.Color(ink)))p.Icon(key,{cx-size/2,rect.top+12,cx+size/2,rect.top+12+size},ink);
            if(kind==L"@magic/system-tray")p.Icon(r.active?L"unpin":L"pin",{rect.right-14,rect.top+2,rect.right-3,rect.top+13},i==hover?t.Ink():t.MaterialMuted());
            if(kind!=L"@magic/system-tray")p.Text(label,{rect.left+6,rect.top+size+18,rect.right-6,rect.bottom-4},12.8f,ink,quick?500:600,DWRITE_TEXT_ALIGNMENT_CENTER,0,true);
        }else if((kind==L"@magic/network-popup"||kind==L"@magic/bluetooth-popup"||kind==L"@magic/media-popup")&&r.control==L"toggle"&&rect.top==22){
            p.Toggle(rect,r.value>.5,r.enabled);
        }else if(r.id==L"system-power"||r.id==L"apps-power"){
            p.Icon(L"power",{rect.left+7,rect.top+7,rect.left+25,rect.top+25},ink);
        }else if(kind==L"@magic/quick-settings"&&r.id.rfind(L"mute:",0)==0){
            p.Icon(L"volume",{rect.left+6,rect.top+6,rect.left+26,rect.top+26},ink);
        }else if(kind==L"@magic/task-switcher"){
            p.Fill({rect.left+6,rect.top+8,rect.right-6,rect.bottom-40},t.Sunken(),NativeMetrics::PanelRadius);
            p.Icon(L"apps",{(rect.left+rect.right)/2-20,rect.top+46,(rect.left+rect.right)/2+20,rect.top+86},t.Gray(300));
            appearanceIcons.Draw(c,r.id,{rect.left+10,rect.bottom-28,rect.left+28,rect.bottom-10});
            p.Text(r.detail.empty()?label:r.detail,{rect.left+36,rect.bottom-34,rect.right-10,rect.bottom-4},12.8f,ink,600);
        }else if(r.control==L"range"){
            auto key=r.id.rfind(L"brightness:",0)==0?L"brightness":L"volume";
            bool compact=kind==L"@magic/quick-settings"&&!PowerView();
            if(!compact||r.id.rfind(L"brightness:",0)==0)p.Icon(key,{rect.left+6,compact?rect.bottom-26:rect.top+8,rect.left+26,compact?rect.bottom-6:rect.top+28},t.Gray(700));
            auto title=label;auto separator=title.rfind(L" · ");if(compact&&separator!=title.npos)title.resize(separator);
            p.Text(title,{rect.left+(compact?10:34),rect.top+3,rect.right-8,rect.top+29},12.8f,ink);
            p.Slider(RangeTrack(r),r.value,r.enabled);
            if(compact)p.Text(std::to_wstring(static_cast<int>(std::round(r.value*100)))+L"%",{rect.right-36,rect.bottom-28,rect.right-2,rect.bottom-2},12.8f,ink,400,DWRITE_TEXT_ALIGNMENT_TRAILING);
        }else if(input){
            if(apps&&r.id==L"search"){
                p.Icon(L"search",{rect.left+8,rect.top+9,rect.left+28,rect.top+29},t.MaterialMuted());
                if(!hwnd)p.Text(label,{rect.left+36,rect.top,rect.right-8,rect.bottom},13,t.MaterialMuted());
            }
            else p.Text(label,{rect.left+8,rect.top+2,rect.right-8,rect.top+24},12,t.Gray(700));
        }else if((kind==L"@magic/network-popup"||kind==L"@magic/bluetooth-popup"||kind==L"@magic/media-popup")&&r.id.find(L"settings")==r.id.npos){
            bool selected=r.active||r.value>.5;float cy=(rect.top+rect.bottom)/2;
            c->FillEllipse({{rect.left+20,cy},16,16},p.Color(selected?t.Accent():t.SectionFilm()));
            p.Icon(kind==L"@magic/bluetooth-popup"?L"bluetooth":kind==L"@magic/network-popup"?L"wifi":L"volume",{rect.left+10,cy-10,rect.left+30,cy+10},selected?t.OnAccent():t.MaterialMuted());
            p.Text(label,{rect.left+48,rect.top+2,rect.right-8,r.detail.empty()?rect.bottom:rect.top+25},12.5f,ink,500);
            if(!r.detail.empty())p.Text(r.detail,{rect.left+48,rect.top+25,rect.right-8,rect.bottom-2},10.5f,t.MaterialMuted());
            if(r.control==L"toggle")p.Toggle({rect.right-46,cy-10,rect.right-6,cy+10},r.value>.5,r.enabled);
        }else{
            float inset=user||kind==L"@magic/keyboard-selector"||kind==L"@magic/task-switcher"?36:10;
            // The row font is one of the card's visual parameters, not a constant.
            float rowFont=12.8f;
            if(user){double wanted=Num(app.model.preferences,L"accountCardFontSize");if(wanted>=11&&wanted<=16)rowFont=static_cast<float>(wanted);}
            if(kind==L"@magic/task-switcher")appearanceIcons.Draw(c,r.id,{rect.left+6,rect.top+14,rect.left+30,rect.top+38});
            if(user)p.Icon(r.key,{rect.left+8,rect.top+11,rect.left+24,rect.top+27},ink);
            bool keyboard=kind==L"@magic/keyboard-selector";float keyboardCenter=r.detail.empty()?(rect.top+rect.bottom)/2:rect.top+15;
            if(keyboard)p.Icon(L"keyboard",{rect.left+8,keyboardCenter-9,rect.left+26,keyboardCenter+9},t.Gray(700));
            bool confirm=armed==r.action||r.id==L"confirm-yes";
            if(confirm||user&&(r.key==L"exit"||r.key==L"shutdown"))ink=t.Danger();
            float headerBottom=r.detail.empty()?rect.bottom:rect.top+27;
            auto hint=ConfirmationBounds(r);
            float labelRight=confirmation?hint.left-8:rect.right-(r.id.rfind(L"notification:",0)==0?36:10);
            p.Text(label,{rect.left+inset,keyboard?rect.top:rect.top+3,labelRight,headerBottom},rowFont,ink,card||r.active?600:400);
            if(confirmation)p.Text(Ui(L"再次点击确认 · ",L"Click again · ")+std::to_wstring(confirmation),hint,11,t.Danger(),400,DWRITE_TEXT_ALIGNMENT_TRAILING);
            if(!r.detail.empty()&&!user)p.Text(r.detail,{rect.left+inset,rect.top+27,rect.right-10,rect.bottom-5},12,t.Gray(700),400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
            if(!r.detail.empty()&&user){
                // This shared painter reserved rect.top+27..rect.bottom-5 for the second
                // line, which is 6 DIP in a 38 DIP card row but 14.2 DIP of text: the
                // paragraph was centred in a box shorter than its own line, and CLIP cut
                // the caps and the baseline. The row is not too short - 17 DIP sit below
                // the label ink - so the subtitle takes that band, and shrinks only when
                // the reader picked a row that cannot hold a readable line at all.
                // HONOR Sans CN: ascent .968, ink top .846, ink bottom -.085 em.
                float labelInkBottom=rect.top+3+(24-1.185f*rowFont)*.5f+1.053f*rowFont;
                float room=rect.bottom-1-(labelInkBottom+1);
                float detailSize=std::min(12.f,room/.931f);
                if(detailSize>=8){
                    auto format=p.graphics.Format(detailSize,false,400);
                    auto wrapping=format->GetWordWrapping();format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    com_ptr<IDWriteTextLayout> layout;
                    if(SUCCEEDED(p.graphics.text->CreateTextLayout(r.detail.c_str(),static_cast<UINT32>(r.detail.size()),format,std::max(1.f,rect.right-10-(rect.left+inset)),1.6f*detailSize,layout.put()))){
                        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> ellipsis;
                        if(SUCCEEDED(p.graphics.text->CreateEllipsisTrimmingSign(format,ellipsis.put())))layout->SetTrimming(&trim,ellipsis.get());
                        float inkTop=labelInkBottom+1+(room-.931f*detailSize)*.5f;
                        p.c->DrawTextLayout({rect.left+inset,inkTop-.122f*detailSize},layout.get(),p.Color(t.Gray(700)),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    format->SetWordWrapping(wrapping);
                }
            }
            if(r.control==L"toggle")p.Toggle({rect.right-48,rect.top+8,rect.right-8,rect.top+28},r.value>.5,r.enabled);
        }
    }
    if(apps){c->FillEllipse({{36,height-36},12,12},p.Color(t.Accent()));p.Text(Str(Account(),L"name"),{58,height-52,150,height-20},12.8f,t.Ink(),600);}
    if(rows.empty())p.Text(Ui(L"暂无内容",L"Nothing here yet"),{18,58,width-18,height-18},12,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_CENTER);
    if(generic&&(hover>=0||kind==L"@magic/user-menu")&&appearanceOverflow>.5f){
        float thumb=std::max(24.f,appearanceView*appearanceView/(appearanceView+appearanceOverflow));
        float y=appearanceTop+(appearanceView-thumb)*std::clamp(appearanceOffset/appearanceOverflow,0.f,1.f);
        auto ink=t.Ink();ink.a=.25f;p.Fill({width-16,y,width-13,y+thumb},ink,1.5f);
    }
    c->PopAxisAlignedClip();
    if(!status.empty())p.Text(status,{22,height-48,width-22,height-18},11,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
}

inline void NativePopup::RequestAppearanceIcons()
{
    if(!visible||!life||!hwnd||!generic||LocalPicker()||LocalContext()||!iconReady)return;
    JsonArray keys;auto data=Data().GetNamedArray(L"rows");
    for(auto const &v:data){auto item=v.GetObject();auto key=Str(item,L"id"),source=Str(item,L"image"),umid=Str(item,L"iconUmid");if(source.empty()&&umid.empty())continue;source+=L":"+umid+L":"+Str(item,L"imageVersion");
        if(imageSources.contains(key)&&imageSources[key]!=source){auto found=appearanceIcons.values.find(key);if(found!=appearanceIcons.values.end()){appearanceIcons.bytes-=found->second.bytes;appearanceIcons.values.erase(found);appearanceIcons.glyphs.erase(key);}iconRequests.erase(key);}imageSources[key]=source;
    }
    for(auto const &r:rows)if(!r.offscreen&&!appearanceIcons.values.contains(r.id)&&!iconRequests.contains(r.id)){
        bool image=false;for(auto const &v:data)if(Str(v.GetObject(),L"id")==r.id&&(!Str(v.GetObject(),L"image").empty()||!Str(v.GetObject(),L"iconUmid").empty())){image=true;break;}
        if(image){keys.Append(::Text(r.id));iconRequests.insert(r.id);if(keys.Size()==8)break;}
    }
    if(!keys.Size())return;JsonObject request;Put(request,L"op",std::wstring(L"popup-icons"));Put(request,L"token",token);request.Insert(L"keys",keys);
    auto owner=life;app.Request(request,[owner](JsonObject const &reply){auto popup=owner->owner;if(!popup||!popup->visible||!Bool(reply,L"ok"))return;
        for(auto const &v:reply.GetNamedObject(L"result").GetNamedArray(L"icons")){auto item=v.GetObject();bool glyph=false;for(auto const &row:popup->Data().GetNamedArray(L"rows"))if(Str(row.GetObject(),L"id")==Str(item,L"key")){glyph=Bool(row.GetObject(),L"glyph");break;}popup->appearanceIcons.Load(popup->app.graphics,Str(item,L"key"),Str(item,L"png"),glyph);}
        popup->Draw();popup->RequestAppearanceIcons();
    });
}

inline void NativePopup::UpdateTaskPreviews()
{
    for(auto thumbnail:taskThumbnails)DwmUnregisterThumbnail(thumbnail);taskThumbnails.clear();taskPreviewBytes=0;
    previewThumbnailProps.clear();
    if(dismissing||previewPreparing)return;
    if(LocalDockPreview()&&visible&&hwnd){if(opening&&GetTickCount64()-previewEnterAt<static_cast<ULONGLONG>(previewMotion.Delay()))return;UpdateDockPreviewThumbnails();return;}
    if(opening)return;
    if(kind!=L"@magic/task-switcher"||!visible||!hwnd)return;
    float scale=dpi/96;
    for(auto const &row:rows){if(row.offscreen||row.id.rfind(L"window:",0)!=0)continue;
        HWND source=reinterpret_cast<HWND>(static_cast<INT_PTR>(_wcstoi64(row.id.substr(7).c_str(),nullptr,10)));if(!IsWindow(source)||source==hwnd)continue;
        auto r=row.rect;RECT dest{static_cast<LONG>((contentX+r.left+6)*scale),static_cast<LONG>((contentY+r.top+8)*scale),static_cast<LONG>((contentX+r.right-6)*scale),static_cast<LONG>((contentY+r.bottom-40)*scale)};
        size_t bytes=static_cast<size_t>(dest.right-dest.left)*(dest.bottom-dest.top)*4;if(taskPreviewBytes+bytes>8*1024*1024||taskThumbnails.size()>=6)break;
        HTHUMBNAIL thumbnail=nullptr;if(FAILED(DwmRegisterThumbnail(hwnd,source,&thumbnail)))continue;
        SIZE size{};if(SUCCEEDED(DwmQueryThumbnailSourceSize(thumbnail,&size))&&size.cx>0&&size.cy>0){
            float fit=std::min((dest.right-dest.left)/static_cast<float>(size.cx),(dest.bottom-dest.top)/static_cast<float>(size.cy));
            LONG w=static_cast<LONG>(size.cx*fit),h=static_cast<LONG>(size.cy*fit),cx=(dest.left+dest.right)/2,cy=(dest.top+dest.bottom)/2;
            dest={cx-w/2,cy-h/2,cx+(w+1)/2,cy+(h+1)/2};}
        DWM_THUMBNAIL_PROPERTIES props{};props.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY|DWM_TNP_SOURCECLIENTAREAONLY;
        props.rcDestination=dest;props.fVisible=TRUE;props.opacity=255;props.fSourceClientAreaOnly=FALSE;
        if(FAILED(DwmUpdateThumbnailProperties(thumbnail,&props))){DwmUnregisterThumbnail(thumbnail);continue;}
        taskThumbnails.push_back(thumbnail);taskPreviewBytes+=bytes;
    }
}
