#pragma once
#include "native-center-panels.h"
// preview70 functional organization on the current native rows/commands. Every
// live action retains its authenticated id; rectangles are shared by UIA/input.
inline void NativePopup::LayoutFunctionalPanel()
{
    if(CenterPanel()){LayoutCenterPanel();return;}
    functionalFrames.clear();functionalSections.clear();functionalPinned.clear();
    float left=18,right=width-18,y=58;functionalTop=58;functionalBottom=height-66;
    auto place=[&](PopupRow &r,D2D1_RECT_F rect,bool pin=false){r.rect=rect;r.offscreen=false;if(pin)functionalPinned.insert(r.id);};
    auto section=[&](std::wstring label){functionalSections.push_back({{left,y,right,y+24},label});y+=28;};
    auto footer=[&](PopupRow &r){place(r,{left,height-52,right,height-18},true);};
    auto flow=[&](PopupRow &r,float h){place(r,{left+8,y,right-8,y+h});y+=h+8;};
    bool power=PowerView(),bluetooth=kind==L"@magic/bluetooth-popup",network=kind==L"@magic/network-popup",media=kind==L"@magic/media-popup";
    // Radio controls belong to their own panel, not a duplicated Wi-Fi/BT pair.
    for(auto &r:rows)if(NativeConnectionRow(r)){
        bool bt=r.id.find(L"Bluetooth")!=r.id.npos||r.id==L"bluetooth"||r.label.find(L"蓝牙")!=r.label.npos;
        if((bluetooth&&!bt)||(network&&bt)){r.offscreen=true;r.enabled=false;place(r,{-100,-100,-90,-90},true);r.offscreen=true;continue;}
        if(bluetooth||network){place(r,{right-40,22,right,45},true);}
    }
    if(DisplayView()){
        for(auto &r:rows){
            if(r.id==L"display-back"){footer(r);continue;}
            flow(r,r.control==L"range"?78.f:r.control==L"toggle"?56.f:40.f);
        }
    }else if(power){
        // Group only existing rows; no new power actions or duplicate state.
        float modeTop=-1;
        auto closeModes=[&](){if(modeTop>=0){functionalFrames.push_back({{left,modeTop,right,y},L""});y+=8;modeTop=-1;}};
        for(auto &r:rows){
            if(r.id==L"power-refresh"){place(r,{right-56,21,right,49},true);continue;}
            if(r.id==L"quick-back"){place(r,{right-152,height-52,right-44,height-18},true);continue;}
            if(r.id==L"system-power"){place(r,{right-34,height-52,right,height-18},true);continue;}
            bool mode=r.id==L"power-mode"||r.id==L"saver"||r.id==L"honor-mode";
            if(mode){if(modeTop<0){modeTop=y;functionalSections.push_back({{left+10,y+5,right-10,y+29},Ui(L"电源模式",L"Power modes")});y+=30;}place(r,{left+10,y,right-10,y+32});y+=32;continue;}
            closeModes();
            if(r.id==L"power-status"){place(r,{left+12,y+8,right-12,y+90});functionalFrames.push_back({{left,y,right,y+98},L""});y+=106;continue;}
            if(r.id.rfind(L"battery:",0)==0){place(r,{left+10,y+4,right-10,y+68});functionalFrames.push_back({{left,y,right,y+76},L""});y+=84;continue;}
            flow(r,38);
        }closeModes();
    }else if(bluetooth){
        // The approved list design keeps actions behind a device disclosure.
        // Move complete blocks: never detach an authenticated action from its device.
        std::vector<std::vector<PopupRow>> groups[3];std::vector<PopupRow> other;
        for(size_t i=0;i<rows.size();){
            if(rows[i].id.rfind(L"device:",0)!=0){other.push_back(rows[i++]);continue;}
            int group=rows[i].detail==L"已连接"?0:rows[i].detail==L"已配对"?1:2;
            std::vector<PopupRow> block;block.push_back(rows[i++]);
            while(i<rows.size()&&(rows[i].id.rfind(L"connect:",0)==0||rows[i].id.rfind(L"disconnect:",0)==0||rows[i].id.rfind(L"pair:",0)==0||rows[i].id.rfind(L"forget:",0)==0))block.push_back(rows[i++]);
            groups[group].push_back(std::move(block));
        }
        rows=std::move(other);
        for(auto &group:groups)for(auto &block:group)for(auto &r:block)rows.push_back(std::move(r));
        for(auto &r:rows)if(r.id==L"bt-settings"){r.label=Ui(L"蓝牙偏好设置",L"Bluetooth preferences");place(r,{left+4,58,right-4,94},true);}
        functionalTop=140;functionalBottom=height-18;y=140;
        for(auto &r:rows)if(r.id==L"bt-discover")place(r,{right-28,108,right,134},true);
        bool expanded=false;int action=0;
        for(auto &r:rows){
            if(NativeConnectionRow(r)||r.id==L"bt-settings"||r.id==L"bt-discover")continue;
            if(r.id.starts_with(L"device:")){
                expanded=r.id==bluetoothExpanded;action=0;r.enabled=true;r.control=L"button";r.active=expanded;
                place(r,{left,y,right,y+36});y+=40;continue;
            }
            bool button=r.id.starts_with(L"connect:")||r.id.starts_with(L"disconnect:")||r.id.starts_with(L"pair:")||r.id.starts_with(L"forget:");
            if(button){
                if(!expanded){place(r,{-100,-100,-90,-90});r.offscreen=true;r.enabled=false;continue;}
                float bw=(right-left-44)/2;if(action&&action%2==0)y+=36;
                float x=left+36+(action%2)*(bw+8);place(r,{x,y,x+bw,y+28});++action;
                auto i=static_cast<size_t>(&r-rows.data());bool next=i+1<rows.size()&&(rows[i+1].id.starts_with(L"connect:")||rows[i+1].id.starts_with(L"disconnect:")||rows[i+1].id.starts_with(L"pair:")||rows[i+1].id.starts_with(L"forget:"));
                if(!next)y+=36;continue;
            }
            flow(r,r.control==L"text"||r.control==L"password"?64:34);
        }
    }else if(network){
        functionalTop=140;functionalBottom=height-18;y=140;
        std::vector<PopupRow> ordered;
        for(auto const &r:rows)if(!Str(NativeCenterMetadata(*this,r.id),L"networkGroup").empty()&&r.id.starts_with(L"wifi-group:")){
            ordered.push_back(r);for(auto const &action:rows)if(action.id!=r.id&&Str(NativeCenterMetadata(*this,action.id),L"networkGroup")==r.id)ordered.push_back(action);
        }
        for(auto const &r:rows)if(Str(NativeCenterMetadata(*this,r.id),L"networkGroup").empty())ordered.push_back(r);
        rows=std::move(ordered);bool radio=false,scan=false;
        for(auto &r:rows){
            if(NativeConnectionRow(r)){if(!radio){place(r,{right-40,22,right,45},true);radio=true;}else{place(r,{-100,-100,-90,-90},true);r.offscreen=true;}continue;}
            if(r.id.starts_with(L"adapter:")){place(r,{-100,-100,-90,-90},true);r.offscreen=true;continue;}
            if(r.id==L"network-settings"){place(r,{left+4,58,right-4,94},true);continue;}
            if(r.id.starts_with(L"scan:")){if(!scan){place(r,{right-28,108,right,134},true);scan=true;}else{place(r,{-100,-100,-90,-90},true);r.offscreen=true;}continue;}
            if(r.id.starts_with(L"wifi-group:")){r.enabled=true;r.active=r.id==networkExpanded;place(r,{left,y,right,y+36});y+=40;continue;}
            auto group=Str(NativeCenterMetadata(*this,r.id),L"networkGroup");
            if(!group.empty()&&group!=networkExpanded){place(r,{-100,-100,-90,-90});r.offscreen=true;continue;}
            flow(r,30);
        }
    }else if(media){
        auto divider=[&](){functionalSections.push_back({{left,y,right,y+1},L""});y+=12;};
        // Every offered command survives; secondary volume controls are disclosed
        // as one local group instead of filling the initial view with cards.
        for(auto &r:rows){r.offscreen=true;r.rect={-100,-100,-90,-90};}
        PopupRow *master=nullptr;
        for(auto &r:rows)if(r.id.starts_with(L"volume:")&&r.id!=L"volume:microphone"){place(r,{left,y,right,y+40});master=&r;y+=48;break;}
        if(master)for(auto &r:rows)if(r.id==L"mute:"+master->id.substr(7))place(r,{left,y-42,left+28,y-14});
        divider();
        bool advanced=std::any_of(rows.begin(),rows.end(),[](auto const&r){return r.id==L"volume:microphone"||r.id.starts_with(L"session:");});
        if(advanced){
            PopupRow disclosure;disclosure.key=disclosure.id=L"@layout:audio-advanced";disclosure.action=L"audio-advanced";disclosure.label=Ui(L"系统声音",L"System sound");disclosure.active=audioAdvanced;place(disclosure,{left,y,right,y+36});rows.push_back(disclosure);y+=44;
            if(audioAdvanced){
                for(auto &r:rows)if(r.id==L"volume:microphone"||r.id.starts_with(L"session:")){
                    place(r,{left,y,right,y+66});y+=72;
                    auto mute=r.id.starts_with(L"session:")?L"mute:"+r.id:L"mute:microphone";
                    for(auto &m:rows)if(m.id==mute)place(m,{left+2,r.rect.bottom-32,left+32,r.rect.bottom-4});
                }
            }
            divider();
        }
        section(Ui(L"输出",L"Output"));
        for(auto &r:rows)if(r.id.starts_with(L"default:")){
            float h=std::clamp(NativeNoticeHeight(app.graphics,r.label,right-left-50,12.5f)+10,36.f,54.f);place(r,{left,y,right,y+h});y+=h+4;
        }
        int transport=0;bool playing=false;
        for(auto &r:rows){
            if(r.id.find(L"settings")!=r.id.npos){r.label=Ui(L"声音偏好设置",L"Sound preferences");footer(r);continue;}
            if(r.id.starts_with(L"track:")){if(!playing){divider();section(Ui(L"正在播放",L"Now playing"));playing=true;}flow(r,50);transport=0;continue;}
            if(r.id.starts_with(L"player:")){float bw=(right-left-16)/3,x=left+(transport%3)*(bw+8);place(r,{x,y,x+bw,y+30});if(++transport%3==0)y+=38;continue;}
            if(!r.offscreen||r.id.starts_with(L"default:")||r.id.starts_with(L"volume:")||r.id.starts_with(L"mute:")||r.id.starts_with(L"session:"))continue;
            flow(r,38);
        }
    }else{
        // Notifications: mode header, grouped notification/action cards, clear footer.
        float cardTop=-1;int action=0;auto close=[&](){if(cardTop>=0){functionalFrames.push_back({{left,cardTop,right,y-4},L""});y+=8;cardTop=-1;}};
        D2D1_RECT_F notification{};
        for(auto &r:rows){
            if(r.id==L"focus"){place(r,{left,58,right,92},true);functionalTop=104;y=104;continue;}
            if(r.id==L"clear"){close();footer(r);continue;}
            if(r.id.rfind(L"notification:",0)==0){close();cardTop=y;flow(r,92);notification=r.rect;action=0;continue;}
            if(r.id.rfind(L"dismiss:",0)==0&&notification.right){place(r,{notification.right-26,notification.top+4,notification.right-2,notification.top+28});continue;}
            if(r.id.rfind(L"action:",0)==0){flow(r,30);++action;continue;}
            flow(r,r.control==L"text"||r.control==L"password"?64:38);
        }close();
    }
    functionalExtent=std::max(functionalTop,y-8);functionalOverflow=std::max(0.f,functionalExtent-functionalBottom);int maximum=static_cast<int>(std::ceil(functionalOverflow/56));scroll=std::clamp(scroll,0,maximum);float offset=std::min(scroll*56.f,functionalOverflow);
    for(auto &r:rows)if(!functionalPinned.contains(r.id)){r.rect.top-=offset;r.rect.bottom-=offset;r.offscreen=r.rect.bottom<=functionalTop||r.rect.top>=functionalBottom;}
    for(auto &v:functionalFrames){v.rect.top-=offset;v.rect.bottom-=offset;}
    for(auto &v:functionalSections){v.rect.top-=offset;v.rect.bottom-=offset;}
}
inline void NativePopup::PaintFunctionalPanel(ID2D1DeviceContext *c)
{
    if(CenterPanel()){PaintCenterPanel(c);return;}
    NativePaint p(c,app.graphics,dark);auto &t=p.theme;
    if(!resizePainting){if(!NativeLowPower(app.model))p.Shadow({10,10,width-10,height-10},NativeMetrics::FloatingRadius,NativeMetrics::PopupInset);
    p.Material({10,10,width-10,height-10},MaterialTier(),Glass(),NativeMetrics::FloatingRadius);}
    bool power=PowerView(),bt=kind==L"@magic/bluetooth-popup",wifi=kind==L"@magic/network-popup",media=kind==L"@magic/media-popup";
    p.Text(power?Ui(L"电量与性能",L"Battery & performance"):media?Ui(L"声音",L"Sound"):caption,{22,20,width-(power?84:72),48},15,t.Ink(),600);
    auto icon=[&](std::wstring const &name,D2D1_RECT_F rect,D2D1_COLOR_F ink){if(!officialPanelIcons.Draw(c,name,rect,ink))p.Icon(bt?L"bluetooth":wifi?L"wifi":media?L"volume":L"info",rect,ink);};
    auto paint=[&](int index){auto const &r=rows[index];if(r.offscreen)return;auto rect=r.rect;
        if(NativeConnectionRow(r)){p.Toggle(rect,r.value>.5,r.enabled);return;}
        p.Interaction(InteractionRegion(index),r.active,materialPreview&&index==hover,index==focus);
        auto ink=r.enabled||r.control==L"label"?t.Ink():t.MaterialMuted();
        if(DisplayView()&&r.control==L"toggle"){p.Text(r.label,{rect.left+4,rect.top,rect.right-54,rect.bottom},12.5f,ink,500);p.Toggle({rect.right-46,rect.top+16,rect.right-4,rect.top+38},r.value>.5,r.enabled);return;}
        if(r.id==L"focus"){
            auto label=r.label;if(label.find(L"PriorityOnly")!=label.npos)label=Ui(L"勿扰模式 · 仅优先通知",L"Do not disturb · Priority only");
            else if(label.find(L"AlarmsOnly")!=label.npos)label=Ui(L"勿扰模式 · 仅闹钟",L"Do not disturb · Alarms only");
            else if(label.find(L"All")!=label.npos)label=Ui(L"勿扰模式 · 已关闭",L"Do not disturb · Off");p.Fill(rect,t.SectionFilm(),6);p.Text(label,rect,12,ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);return;
        }
        if(r.id==L"power-refresh"){p.Text(r.label,rect,12,t.Ink(),500,DWRITE_TEXT_ALIGNMENT_CENTER);return;}
        if(r.id==L"system-power"){p.Icon(L"power",{rect.left+8,rect.top+8,rect.right-8,rect.bottom-8},ink);return;}
        if(r.id==L"power-status"){
            auto at=r.label.find(L' ');auto number=at==r.label.npos?r.label:r.label.substr(at+1);bool charging=r.detail.find(L"电源")!=r.detail.npos||r.detail.find(L"充电")!=r.detail.npos;
            double percent=std::clamp(_wtof(number.c_str()),0.,100.);auto bucket=std::clamp(static_cast<int>(std::ceil(percent/10)),1,10);
            icon((charging?L"BatteryAC":L"Battery")+std::to_wstring(bucket),{rect.left,rect.top+17,rect.left+26,rect.top+43},t.MaterialMuted());
            p.Text(number,{rect.left+34,rect.top,rect.right-94,rect.top+60},32,t.Ink(),500);
            p.Text(r.detail,{rect.right-94,rect.top,rect.right,rect.top+60},12,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_TRAILING);
            // preview70's read-only progress bar, not an interactive slider.
            D2D1_RECT_F bar{rect.left,rect.bottom-12,rect.right,rect.bottom-8};p.Fill(bar,t.Gray(900,.12f),2);
            if(percent>0){bar.right=bar.left+(bar.right-bar.left)*static_cast<float>(percent/100);p.Fill(bar,t.Accent(),2);}return;
        }
        if(power&&(r.id==L"power-mode"||r.id==L"saver"||r.id==L"honor-mode")){
            p.Text(r.label,{rect.left,rect.top,rect.right-98,rect.bottom},12.5f,t.MaterialMuted());p.Text(r.detail,{rect.right-98,rect.top,rect.right,rect.bottom},12.5f,r.id==L"honor-mode"?t.MaterialMuted():t.Ink(),500,DWRITE_TEXT_ALIGNMENT_TRAILING);return;
        }
        if(power&&r.id.rfind(L"battery:",0)==0){p.Text(r.label,{rect.left,rect.top+8,rect.right,rect.top+32},13,t.Ink(),500);p.Text(r.detail,{rect.left,rect.top+32,rect.right,rect.bottom},11.5f,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);return;}
        if(r.id.rfind(L"dismiss:",0)==0){p.Icon(L"close",{rect.left+5,rect.top+5,rect.right-5,rect.bottom-5},t.MaterialMuted());return;}
        if(r.id.rfind(L"player:",0)==0){auto suffix=r.id.substr(r.id.find_last_of(L':')+1);float x=(rect.left+rect.right)/2;p.Icon(suffix==L"prev"?L"previous":suffix==L"next"?L"next":r.active?L"pause":L"play",{x-9,rect.top+8,x+9,rect.bottom-8},ink);return;}
        if(media&&r.action==L"audio-advanced"){
            icon(L"Speaker",{rect.left+4,rect.top+8,rect.left+24,rect.top+28},ink);p.Text(r.label,{rect.left+36,rect.top,rect.right-28,rect.bottom},12.5f,ink);
            float x=rect.right-14,cy=(rect.top+rect.bottom)/2,dy=r.active?-3.f:3.f;
            p.Line(x-4,cy-dy/2,x,cy+dy/2,ink,1.5f);p.Line(x,cy+dy/2,x+4,cy-dy/2,ink,1.5f);return;
        }
        if(media&&r.id.starts_with(L"default:")){
            float cy=(rect.top+rect.bottom)/2;c->FillEllipse({{rect.left+16,cy},13,13},p.Color(r.active?t.Accent():t.SectionFilm()));
            icon(L"Speaker",{rect.left+8,cy-8,rect.left+24,cy+8},r.active?D2D1::ColorF(0xffffff):ink);
            p.Text(r.label,{rect.left+36,rect.top,rect.right-6,rect.bottom},12.5f,ink,400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);return;
        }
        if(wifi&&!r.id.starts_with(L"wifi-group:")&&!Str(NativeCenterMetadata(*this,r.id),L"networkGroup").empty()){
            p.Fill(rect,t.Film(),NativeMetrics::ControlRadius);p.Text(r.label,rect,12,ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);return;
        }
        if(bt&&r.id==L"bt-discover"||wifi&&r.id.starts_with(L"scan:")){if(!refreshVisual||refreshId!=r.id)p.Icon(L"refresh",{rect.left+4,rect.top+4,rect.right-4,rect.bottom-4},t.MaterialMuted());return;}
        if((bt&&r.id==L"bt-settings")||(wifi&&r.id==L"network-settings")){p.Text(r.label,rect,12.5f,ink);p.Line(rect.left,rect.top-4,rect.right,rect.top-4,t.Edge());p.Line(rect.left,rect.bottom,rect.right,rect.bottom,t.Edge());return;}
        if(media&&r.control==L"range"&&r.id.starts_with(L"volume:")&&r.id!=L"volume:microphone"){p.Slider(RangeTrack(r),r.value,r.enabled);return;}
        if(r.control==L"range"){
            auto title=r.label;auto split=title.rfind(L" · ");if(split!=title.npos)title.resize(split);
            p.Text(title,{rect.left+4,rect.top,rect.right-48,rect.top+30},12.5f,ink,500);p.Text(std::to_wstring(static_cast<int>(std::round(r.value*100)))+L"%",{rect.right-46,rect.top,rect.right,rect.top+30},11.5f,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_TRAILING);p.Slider(RangeTrack(r),r.value,r.enabled);return;
        }
        if(r.id.rfind(L"mute:",0)==0){bool mic=false;for(auto const &v:rows)if(v.id==L"volume:"+r.id.substr(5))mic=v.label.find(L"麦克风")!=v.label.npos||v.label.find(L"Microphone")!=v.label.npos;
            icon(mic?L"Microphone":r.label.find(L"取消")!=r.label.npos?L"SpeakerMute":L"volume",rect,t.Ink());return;}
        if(r.control==L"text"||r.control==L"password"){p.Text(r.label,{rect.left+6,rect.top,rect.right-6,rect.top+24},12,t.MaterialMuted());return;}
        bool device=r.id.rfind(L"device:",0)==0,network=r.id.rfind(L"network:",0)==0||r.id==L"network",notification=r.id.rfind(L"notification:",0)==0,track=r.id.rfind(L"track:",0)==0;
        if(wifi&&r.id.starts_with(L"wifi-group:")){
            auto meta=NativeCenterMetadata(*this,r.id);bool connected=Bool(meta,L"connected");float cy=(rect.top+rect.bottom)/2;
            c->FillEllipse({{rect.left+16,cy},13,13},p.Color(connected?t.Accent():t.SectionFilm()));
            icon(L"WiFi4",{rect.left+8,cy-8,rect.left+24,cy+8},connected?D2D1::ColorF(0xffffff):ink);
            p.Text(r.label,{rect.left+36,rect.top,rect.right-4,rect.bottom},12.5f,ink);return;
        }
        if(bt&&device){
            float cy=(rect.top+rect.bottom)/2;bool connected=r.detail==L"已连接";
            if(r.active)p.Fill(rect,t.SectionFilm(),NativeMetrics::ControlRadius);
            c->FillEllipse({{rect.left+16,cy},13,13},p.Color(connected?t.Accent():t.SectionFilm()));
            auto meta=NativeCenterMetadata(*this,r.id);auto glyph=Str(meta,L"deviceIcon");if(glyph.empty())glyph=L"TbBluetooth";
            if(!previewSvg.Draw(c,glyph,{rect.left+8,cy-8,rect.left+24,cy+8},connected?D2D1::ColorF(0xffffff):t.Ink()))icon(L"Bluetooth",{rect.left+8,cy-8,rect.left+24,cy+8},connected?D2D1::ColorF(0xffffff):t.Ink());
            double battery=Num(meta,L"batteryPercent",-1);p.Text(r.label,{rect.left+36,rect.top,rect.right-(battery>=0?46:4),rect.bottom},12.5f,t.Ink());
            if(battery>=0)p.Text(std::to_wstring(static_cast<int>(battery))+L"%",{rect.right-44,rect.top,rect.right-4,rect.bottom},11,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_TRAILING);return;
        }
        if(device||network||notification||track){
            float size=notification?20.f:24.f;icon(device?L"Bluetooth":network?L"WiFi4":track?L"Speaker":L"havenotify",{rect.left+2,rect.top+12,rect.left+2+size,rect.top+12+size},t.Ink());
            auto label=r.label;bool connected=network&&label.rfind(L"✓ ",0)==0;if(connected){label=label.substr(2);icon(L"checkmark2",{rect.right-22,rect.top+12,rect.right-2,rect.top+32},t.Accent());}
            if(!track||resizePainting||!mediaMarquees.contains(r.id))p.Text(label,{rect.left+36,rect.top+3,rect.right-(notification||connected?28:6),rect.top+30},12.5f,t.Ink(),500);
            p.Text(r.detail,{rect.left+36,rect.top+30,rect.right-6,rect.bottom-4},11.5f,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);return;
        }
        p.Fill(rect,t.Film(),NativeMetrics::ControlRadius);p.Text(r.label,rect,12.5f,ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    if(bt||wifi)p.Text(bt?Ui(L"设备",L"Devices"):Ui(L"网络",L"Networks"),{22,108,width-52,134},12,t.MaterialMuted());
    // Pinned radio/refresh/mode/footer controls stay outside the scroll clip.
    for(int i=0;i<static_cast<int>(rows.size());++i)if(functionalPinned.contains(rows[i].id))paint(i);
    c->PushAxisAlignedClip({14,functionalTop,width-14,functionalBottom},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for(auto const &v:functionalFrames){p.Fill(v.rect,t.SectionFilm(),8);auto edge=t.Edge();edge.a*=.65f;c->DrawRoundedRectangle(D2D1::RoundedRect(v.rect,8,8),p.Color(edge),.7f);}
    for(auto const &v:functionalSections)if(v.label.empty())p.Line(v.rect.left,v.rect.top,v.rect.right,v.rect.top,t.Edge());else p.Text(v.label,v.rect,11,t.MaterialMuted(),600);
    for(int i=0;i<static_cast<int>(rows.size());++i)if(!functionalPinned.contains(rows[i].id))paint(i);
    if(functionalOverflow>0){float view=functionalBottom-functionalTop,thumb=std::max(24.f,view*view/(view+functionalOverflow)),offset=std::min(scroll*56.f,functionalOverflow);float top=functionalTop+(view-thumb)*offset/functionalOverflow;p.Fill({width-16,top,width-13,top+thumb},t.Gray(900,.25f),1.5f);}
    c->PopAxisAlignedClip();
    if(rows.empty())p.Text(Ui(L"暂无内容",L"Nothing here yet"),{22,80,width-22,height-40},13,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_CENTER);
}
