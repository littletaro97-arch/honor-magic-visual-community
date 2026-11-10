#pragma once
// Seelen/preview70 organization, painted by the current native host. Only rows
// offered by the adapter invoke commands; device disclosure is local UI state.
inline JsonObject NativeCenterMetadata(NativePopup const &popup,std::wstring const &id)
{
    for(auto const &value:popup.Data().GetNamedArray(L"rows")){auto item=value.GetObject();if(Str(item,L"id")==id)return item;}return JsonObject{};
}
inline std::pair<std::wstring,std::wstring> NativeNoticeText(std::wstring const &detail)
{
    auto split=detail.find(L'\n');return {detail.substr(0,split),split==detail.npos?L"":detail.substr(split+1)};
}
inline float NativeNoticeHeight(Graphics &g,std::wstring const &text,float width,float size,int weight=400)
{
    if(text.empty())return 0;com_ptr<IDWriteTextLayout> layout;auto format=g.Format(size,false,weight);
    auto wrap=format->GetWordWrapping();format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    auto result=g.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,std::max(40.f,width),4096,layout.put());format->SetWordWrapping(wrap);check_hresult(result);
    DWRITE_TEXT_METRICS metrics{};check_hresult(layout->GetMetrics(&metrics));return std::ceil(metrics.height);
}
inline std::wstring NativeNoticeClock(JsonObject const &item)
{
    auto stamp=Str(item,L"createdFileTime");if(stamp.empty())return L"";
    ULARGE_INTEGER number{};number.QuadPart=_wcstoui64(stamp.c_str(),nullptr,10);FILETIME utc{number.LowPart,number.HighPart},local{};SYSTEMTIME time{},now{};
    if(!FileTimeToLocalFileTime(&utc,&local)||!FileTimeToSystemTime(&local,&time))return L"";GetLocalTime(&now);wchar_t text[32]{};
    if(time.wYear==now.wYear&&time.wMonth==now.wMonth&&time.wDay==now.wDay)swprintf_s(text,L"%02u:%02u",time.wHour,time.wMinute);
    else swprintf_s(text,L"%u/%u",time.wMonth,time.wDay);return text;
}
inline void NativePopup::LayoutCenterPanel()
{
    functionalFrames.clear();functionalSections.clear();functionalPinned.clear();
    bool stackedHeader=kind==L"@magic/notifications"&&width<330&&std::any_of(rows.begin(),rows.end(),[](auto const &r){return r.id==L"focus"||r.id==L"clear";});float headerY=stackedHeader?52.f:20.f;
    float left=22,right=width-22,y=stackedHeader?90.f:58.f;functionalTop=y;functionalBottom=std::max(y+32,height-62);
    auto place=[&](PopupRow &r,D2D1_RECT_F rect,bool pin=false){r.rect=rect;r.offscreen=false;if(pin)functionalPinned.insert(r.id);};
    for(auto &r:rows){r.offscreen=true;r.rect={-100,-100,-90,-90};}
    bool quick=kind==L"@magic/quick-settings";
    if(quick){
        bool devices=std::any_of(rows.begin(),rows.end(),[](auto const &r){return r.id.starts_with(L"default:");});
        if(!devices)quickDevices=false;
        if(quickDevices){
            for(auto &r:rows)if(r.id.starts_with(L"default:")){place(r,{left,y,right,y+52});y+=56;}
        }else{
            int count=0;float cell=(right-left-8)/2;std::vector<PopupRow*> tiles;
            for(auto &r:rows){
                bool tile=r.control==L"toggle"&&!r.id.starts_with(L"mute:")||r.id==L"quick-network"||r.id==L"quick-bluetooth"||r.id==L"display-settings"||r.id==L"focus-settings";
                if(tile)tiles.push_back(&r);
            }
            std::stable_partition(tiles.begin(),tiles.end(),[](auto r){return NativeConnectionRow(*r)||r->id==L"quick-network"||r->id==L"quick-bluetooth";});
            for(auto r:tiles){float x=left+(count%2)*(cell+8),top=y+(count/2)*76.f;place(*r,{x,top,x+cell,top+68});++count;}
            if(count)y+=std::ceil(count/2.f)*76+4;
            D2D1_RECT_F volume{};
            for(auto &r:rows)if(r.control==L"range"){
                place(r,{left,y,right,y+64});functionalFrames.push_back({r.rect,L""});y+=72;
                if(r.id.starts_with(L"volume:"))volume=r.rect;
            }
            for(auto &r:rows)if(r.id.starts_with(L"mute:")){
                auto target=L"volume:"+r.id.substr(5);auto range=std::find_if(rows.begin(),rows.end(),[&](auto const &v){return v.id==target;});
                if(range!=rows.end()&&!range->offscreen)place(r,{range->rect.left+6,range->rect.bottom-29,range->rect.left+36,range->rect.bottom-1});
            }
            // Keep unrecognized offered controls reachable rather than dropping them.
            for(auto &r:rows)if(r.offscreen&&!r.id.starts_with(L"default:")&&r.id!=L"quick-settings"&&r.id!=L"quick-power"&&r.id!=L"system-power"&&r.id!=L"power"&&r.id!=L"quick-back"){
                place(r,{left,y,right,y+40});y+=48;
            }
            if(devices){PopupRow selector;selector.key=selector.id=L"@layout:quick-devices";selector.action=L"quick-devices";selector.label=Ui(L"选择输出设备",L"Choose sound output");
                auto current=std::find_if(rows.begin(),rows.end(),[](auto const &r){return r.id.starts_with(L"default:")&&(r.active||!r.detail.empty());});
                if(current!=rows.end())selector.detail=current->label;place(selector,{left,y,right,y+42});rows.push_back(std::move(selector));y+=50;}
        }
        for(auto &r:rows){
            if(r.id==L"quick-settings")place(r,{right-34,height-50,right,height-18},true);
            else if(r.id==L"quick-power"||r.id==L"system-power")place(r,{right-76,height-50,right-42,height-18},true);
            else if(r.id==L"power"||r.id==L"quick-back")place(r,{left,height-50,right-86,height-18},true);
        }
        if(quickDevices){PopupRow back;back.key=back.id=L"@layout:quick-back";back.action=L"quick-devices";back.label=Ui(L"返回控制中心",L"Back to Control Center");place(back,{left,20,left+32,48},true);rows.push_back(std::move(back));}
    }else{
        float cardTop=-1;D2D1_RECT_F notice{};bool hasClear=std::any_of(rows.begin(),rows.end(),[](auto const&r){return r.id==L"clear";});
        auto close=[&](){if(cardTop>=0){functionalFrames.push_back({{left,cardTop,right,y},L""});y+=10;cardTop=-1;}};
        for(auto &r:rows){
            if(r.id==L"focus"){place(r,{right-(hasClear?192.f:112.f),headerY,right-(hasClear?80.f:0.f),headerY+28},true);continue;}
            if(r.id==L"clear"){place(r,{right-72,headerY,right,headerY+28},true);continue;}
            if(r.id==L"notification-settings"){place(r,{left,height-50,right,height-18},true);continue;}
            if(r.id.starts_with(L"notification:")){
                close();cardTop=y;auto [title,body]=NativeNoticeText(r.detail);float w=right-left-24;
                float h=48+NativeNoticeHeight(app.graphics,title,w,13,600)+NativeNoticeHeight(app.graphics,body,w,12)+(body.empty()?0:6);
                place(r,{left,y,right,y+std::max(86.f,h)});notice=r.rect;y=r.rect.bottom+4;continue;
            }
            if(r.id.starts_with(L"dismiss:")&&notice.right){place(r,{right-34,notice.top+4,right-6,notice.top+32});continue;}
            if(!r.id.starts_with(L"action:")&&r.control!=L"text"&&r.control!=L"password")close();
            float h=r.control==L"text"||r.control==L"password"?64:!r.detail.empty()?64:34;place(r,{left+8,y,right-8,y+h});y+=h+6;
        }close();
        bool content=std::any_of(rows.begin(),rows.end(),[&](auto const &r){return !functionalPinned.contains(r.id)&&!r.offscreen;});
        if(!content)y=234; // Deliberate empty body, with a centered empty-state message.
    }
    functionalExtent=std::max(functionalTop,y);functionalOverflow=std::max(0.f,functionalExtent-functionalBottom);
    int maximum=static_cast<int>(std::ceil(functionalOverflow/56));scroll=std::clamp(scroll,0,maximum);float offset=std::min(scroll*56.f,functionalOverflow);
    for(auto &r:rows)if(!r.offscreen&&!functionalPinned.contains(r.id)){r.rect.top-=offset;r.rect.bottom-=offset;r.offscreen=r.rect.bottom<=functionalTop||r.rect.top>=functionalBottom;}
    for(auto &frame:functionalFrames){frame.rect.top-=offset;frame.rect.bottom-=offset;}
}
inline void NativePopup::PaintCenterPanel(ID2D1DeviceContext *c)
{
    NativePaint p(c,app.graphics,dark);auto &t=p.theme;bool quick=kind==L"@magic/quick-settings";
    D2D1_RECT_F outer{10,10,width-10,height-10};if(!resizePainting){if(!NativeLowPower(app.model))p.Shadow(outer,NativeMetrics::FloatingRadius,NativeMetrics::PopupInset);p.Material(outer,MaterialTier(),Glass());}
    float titleRight=width-24;
    if(!quick)for(auto const &r:rows)if(!r.offscreen&&r.rect.top<32&&(r.id==L"focus"||r.id==L"clear"))titleRight=std::min(titleRight,r.rect.left-8);
    p.Text(quick?(quickDevices?Ui(L"声音输出",L"Sound output"):Ui(L"控制中心",L"Control Center")):Ui(L"通知中心",L"Notifications"),
        {quickDevices?62.f:24.f,20,titleRight,48},15,t.Ink(),600);
    auto icon=[&](std::wstring const &key,D2D1_RECT_F rect,D2D1_COLOR_F color){p.Icon(key,rect,color);};
    auto paint=[&](int i){auto const &r=rows[i];if(r.offscreen)return;auto b=r.rect;auto ink=r.enabled||r.control==L"label"?t.Ink():t.MaterialMuted();
        auto feedback=[&](){p.Interaction(InteractionRegion(i),false,materialPreview&&i==hover,i==focus);};
        if(r.action==L"quick-devices"){
            if(quickDevices){feedback();icon(L"left",{b.left+6,b.top+4,b.left+26,b.top+24},ink);return;}
            p.Fill(b,t.SectionFilm(),10);feedback();icon(L"volume",{b.left+9,b.top+11,b.left+29,b.top+31},ink);
            p.Text(r.detail.empty()?r.label:r.detail,{b.left+38,b.top,b.right-26,b.bottom},11.5f,ink);icon(L"right",{b.right-24,b.top+12,b.right-6,b.top+30},t.MaterialMuted());return;
        }
        if(quick&&(r.id==L"quick-settings"||r.id==L"quick-power"||r.id==L"system-power")){feedback();icon(r.id==L"quick-settings"?L"settings":L"power",{b.left+7,b.top+6,b.right-7,b.bottom-6},ink);return;}
        if(!quick&&r.id==L"focus"){p.Fill(b,r.active?t.Accent():t.SectionFilm(),8);feedback();p.Text(r.label,{b.left+6,b.top,b.right-6,b.bottom},10.5f,r.active?t.OnAccent():ink,500,DWRITE_TEXT_ALIGNMENT_CENTER,1);return;}
        if(!quick&&(r.id==L"clear"||r.id==L"notification-settings")){
            if(r.id==L"clear")p.Fill(b,t.SectionFilm(),8);feedback();p.Text(r.label,b,r.id==L"clear"?11.f:12.f,ink,400,r.id==L"clear"?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING);return;
        }
        if(r.id.starts_with(L"dismiss:")){feedback();icon(L"close",{b.left+7,b.top+7,b.right-7,b.bottom-7},t.MaterialMuted());return;}
        if(!quick&&r.id.starts_with(L"notification:")){
            auto meta=NativeCenterMetadata(*this,r.id);auto time=NativeNoticeClock(meta);feedback();
            auto image=D2D1::RectF(b.left+10,b.top+10,b.left+28,b.top+28);if(!appearanceIcons.Draw(c,r.id,image))icon(L"notifications",image,t.MaterialMuted());
            p.Text(r.label,{b.left+36,b.top+6,b.right-(time.empty()?36.f:80.f),b.top+32},11,t.MaterialMuted(),600);
            if(!time.empty())p.Text(time,{b.right-82,b.top+6,b.right-40,b.top+32},10,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_TRAILING);
            p.Line(b.left+12,b.top+35,b.right-12,b.top+35,t.Edge(),.6f);
            auto [title,body]=NativeNoticeText(r.detail);float th=NativeNoticeHeight(app.graphics,title,b.right-b.left-24,13,600);
            p.Text(title,{b.left+12,b.top+42,b.right-12,b.top+42+th},13,ink,600,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
            if(!body.empty())p.Text(body,{b.left+12,b.top+48+th,b.right-12,b.bottom-6},12,ink,400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);return;
        }
        if(quick&&r.control==L"range"){
            auto title=r.label;auto split=title.find(L" · ");if(split!=title.npos)title.resize(split);
            p.Text(title,{b.left+12,b.top+2,b.right-12,b.top+28},11.5f,t.MaterialMuted());
            bool hasMute=std::any_of(rows.begin(),rows.end(),[&](auto const &v){return v.id==L"mute:"+r.id.substr(r.id.find(L':')+1);});
            if(!hasMute)icon(r.id.starts_with(L"brightness")?L"brightness":L"volume",{b.left+12,b.bottom-24,b.left+30,b.bottom-6},ink);
            p.Slider(RangeTrack(r),r.value,r.enabled);p.Text(r.enabled?std::to_wstring(static_cast<int>(std::round(r.value*100)))+L"%":L"不可用",{b.right-40,b.bottom-30,b.right-8,b.bottom},11,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_TRAILING);return;
        }
        if(quick&&r.id.starts_with(L"mute:")){feedback();icon(L"volume",{b.left+5,b.top+4,b.right-5,b.bottom-4},ink);if(r.value>.5)p.Line(b.left+6,b.top+4,b.right-5,b.bottom-4,ink,1.4f);return;}
        if(quick&&r.id.starts_with(L"default:")){
            if(r.active)p.Fill(b,t.SectionFilm(),10);feedback();icon(L"volume",{b.left+8,b.top+16,b.left+28,b.top+36},ink);
            p.Text(r.label,{b.left+40,b.top+3,b.right-28,r.detail.empty()?b.bottom-3:b.top+27},12.5f,ink,r.active?600:400);
            if(!r.detail.empty())p.Text(r.detail,{b.left+40,b.top+27,b.right-28,b.bottom-3},10.5f,t.MaterialMuted());
            if(r.active)icon(L"check",{b.right-26,b.top+17,b.right-8,b.top+35},t.Accent());return;
        }
        bool tile=quick&&(r.control==L"toggle"||r.id==L"quick-network"||r.id==L"quick-bluetooth"||r.id==L"display-settings"||r.id==L"focus-settings");
        if(tile){
            bool active=r.active||(r.control==L"toggle"&&r.value>.5);p.Fill(b,active?t.Accent(.85f):t.SectionFilm(),12);feedback();auto color=active?t.OnAccent():ink;
            auto key=r.id,label=r.label;while(!label.empty()&&(label.front()==L'●'||label.front()==L'○'||label.front()==L' '))label.erase(label.begin());
            if(key.find(L"Bluetooth")!=key.npos||key.find(L"bluetooth")!=key.npos)key=L"bluetooth";
            else if(key.find(L"WiFi")!=key.npos||key.find(L"network")!=key.npos||key==L"wifi")key=L"wifi";
            else if(key==L"display-settings")key=L"brightness";else if(key==L"focus-settings")key=L"night";else if(key==L"night")key=L"eye";
            float cx=(b.left+b.right)/2;icon(key,{cx-12,b.top+10,cx+12,b.top+34},color);
            p.Text(label,{b.left+6,b.top+39,b.right-6,b.bottom-5},12,color,500,DWRITE_TEXT_ALIGNMENT_CENTER);return;
        }
        feedback();if(r.control==L"text"||r.control==L"password"){p.Text(r.label,{b.left+6,b.top,b.right-6,b.top+24},12,t.MaterialMuted());return;}
        p.Fill(b,t.SectionFilm(),8);p.Text(r.label,{b.left+8,b.top,b.right-8,r.detail.empty()?b.bottom:b.top+28},12,ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);
        if(!r.detail.empty())p.Text(r.detail,{b.left+8,b.top+28,b.right-8,b.bottom-4},11,t.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_CENTER,0,true);
    };
    for(int i=0;i<static_cast<int>(rows.size());++i)if(functionalPinned.contains(rows[i].id))paint(i);
    c->PushAxisAlignedClip({14,functionalTop,width-14,functionalBottom},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for(auto const &frame:functionalFrames){p.Fill(frame.rect,t.SectionFilm(),12);auto edge=t.Edge();edge.a*=.6f;c->DrawRoundedRectangle(D2D1::RoundedRect(frame.rect,12,12),p.Color(edge),.6f);}
    for(int i=0;i<static_cast<int>(rows.size());++i)if(!functionalPinned.contains(rows[i].id))paint(i);
    if(!quick&&std::none_of(rows.begin(),rows.end(),[](auto const &r){return r.id.starts_with(L"notification:")||r.id==L"authorize";})){
        float cy=(functionalTop+functionalBottom)/2;icon(L"notifications",{width/2-15,cy-32,width/2+15,cy-2},t.MaterialMuted());
        p.Text(Ui(L"暂无通知",L"No notifications"),{22,cy+8,width-22,cy+34},12,t.MaterialMuted(),500,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if(functionalOverflow>.5f){float view=functionalBottom-functionalTop,thumb=std::max(24.f,view*view/(view+functionalOverflow)),offset=std::min(scroll*56.f,functionalOverflow);float top=functionalTop+(view-thumb)*offset/functionalOverflow;p.Fill({width-16,top,width-13,top+thumb},t.Gray(900,.25f),1.5f);}
    c->PopAxisAlignedClip();
}
