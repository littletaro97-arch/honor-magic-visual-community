#pragma once
#include "native-appearance.h"
// Declarative preview70 cards: no HTML engine, script evaluation or network I/O.
inline std::wstring CardNumber(double value, int precision=0)
{
    wchar_t text[64];swprintf_s(text,L"%.*f",precision,value);return text;
}
inline std::wstring CardPercent(JsonObject const &data,wchar_t const *key)
{
    auto value=data?data.TryLookup(key):nullptr;
    return value&&value.ValueType()==JsonValueType::Number?CardNumber(Clamp(static_cast<float>(value.GetNumber()),0,100))+L"%":L"—";
}
#include "native-system-cards.h"
void Window::PaintCard(ID2D1DeviceContext *c,Tile const &tile,D2D1_RECT_F r)
{
    D2D1_MATRIX_3X2_F transform;c->GetTransform(&transform);
    try{PaintCardContent(c,tile,r);c->SetTransform(transform);}
    catch(hresult_error const&error){c->SetTransform(transform);NativePaint p(c,app.graphics,Preview70Theme::systemDark);p.Fill(r,p.theme.Raised(),NativeMetrics::PanelRadius);p.Text(L"卡片暂不可用",r,13,p.theme.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_CENTER);JsonObject d;Put(d,L"type",Str(tile.card,L"type"));Put(d,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));app.Telemetry(L"card-content-failed",d);}
}
void Window::PaintCardContent(ID2D1DeviceContext *c,Tile const &tile,D2D1_RECT_F r)
{
    if(PaintPresetCard(*this,c,tile,r))return;
    auto type=Str(tile.card,L"type");bool clock=type==L"system.media"&&NativeMediaPlaying(app.model.mediaData)||type==L"system.clock",wide=clock&&Num(tile.card,L"columns",4)>=4,system=type.rfind(L"system.",0)==0;
    float baseWidth=wide?376.0f:184.0f,baseHeight=clock&&!wide?88.f:184.f;
    float scale=std::min((r.right-r.left)/baseWidth,(r.bottom-r.top)/baseHeight);
    if(!system){baseWidth=r.right-r.left;baseHeight=r.bottom-r.top;scale=1;}
    D2D1_MATRIX_3X2_F old;c->GetTransform(&old);
    c->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(r.left,r.top)*old);
    auto customValue=tile.card.TryLookup(L"custom");JsonObject custom=nullptr;
    if(customValue&&customValue.ValueType()==JsonValueType::Object)custom=customValue.GetObject();
    bool dark=system?Preview70Theme::systemDark:custom&&Str(custom,L"tone")==L"light";auto align=custom?Str(custom,L"align"):L"left";
    NativePaint p(c,app.graphics,dark);auto white=p.theme.Ink();auto muted=p.theme.MaterialMuted();
    const float radius=NativeMetrics::PanelRadius/std::max(.01f,scale);
    if(!clock&&!NativeLowPower(app.model))p.Shadow({0,0,baseWidth,baseHeight},radius);
    if(!clock){if(system)p.Material({0,0,baseWidth,baseHeight},NativeMaterial::Card,!NativeLowPower(app.model)&&(cardBackdrops.contains(tile.key)||materialPreview),radius);else p.Material({0,0,baseWidth,baseHeight},NativeMaterial::Card,!NativeLowPower(app.model)&&(cardBackdrops.contains(tile.key)||materialPreview),radius);}
    auto text=[&](std::wstring const &value,D2D1_RECT_F bounds,float size=12.0f,bool center=false){
        p.Text(value,bounds,size,system&&!wide?white:D2D1::ColorF(dark?0xffffff:0x191919),400,
            center||align==L"center"?DWRITE_TEXT_ALIGNMENT_CENTER:align==L"right"?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
    };
    SYSTEMTIME now{};GetLocalTime(&now);
    wchar_t time[32];swprintf_s(time,L"%02u:%02u",now.wHour,now.wMinute);
    JsonObject content=custom?custom.GetNamedObject(L"content"):JsonObject{};
    auto kind=custom?Str(custom,L"kind"):L"";
    if(type==L"system.clock"||kind==L"clock"){
        std::wstring date=std::to_wstring(now.wMonth)+L"月"+std::to_wstring(now.wDay)+L"日 星期"+std::wstring(1,L"日一二三四五六"[now.wDayOfWeek]);
        if(kind==L"clock"&&Bool(content,L"hour12"))swprintf_s(time,L"%02u:%02u",(now.wHour+11)%12+1,now.wMinute);
        if(kind==L"clock"&&Bool(content,L"showSeconds"))swprintf_s(time,L"%02u:%02u:%02u",Bool(content,L"hour12")?(now.wHour+11)%12+1:now.wHour,now.wMinute,now.wSecond);
        float size=82;if(auto settings=tile.card.TryLookup(L"settings");settings&&settings.ValueType()==JsonValueType::Object)size=static_cast<float>(Num(settings.GetObject(),L"fontSize",82));
        if(size!=56&&size!=72&&size!=82&&size!=92&&size!=108)size=82;
        int weight=900;if(auto s=tile.card.TryLookup(L"settings");s&&s.ValueType()==JsonValueType::Object)weight=static_cast<int>(Num(s.GetObject(),L"fontWeight",900));if(weight!=300&&weight!=900)weight=900;
        if(clock){if(!wide)size=56;float opacity=.82f;if(auto settings=tile.card.TryLookup(L"settings");settings&&settings.ValueType()==JsonValueType::Object)opacity=Clamp(static_cast<float>(Num(settings.GetObject(),L"inkOpacity",.82)),0,1);auto ink=p.theme.Ink();if(!materialPreview)ink=D2D1::ColorF(dark?0x121212:0xffffff,opacity);else ink.a=opacity;auto dim=ink;dim.a*=.85f;
            float dateHeight=wide?18.f:14.f,gap=wide?13.f:6.f,groupHeight=dateHeight+gap+size,top=(baseHeight-groupHeight)/2;
            p.Text(date,{0,top,baseWidth,top+dateHeight},wide?15.f:11.f,dim,weight==300?300:500,DWRITE_TEXT_ALIGNMENT_CENTER,1);
            p.Text(time,{0,top+dateHeight+gap,baseWidth,top+groupHeight},size,ink,weight,DWRITE_TEXT_ALIGNMENT_CENTER,1);
        }else{text(time,{14,baseHeight/2-26,baseWidth-14,baseHeight/2+26},36,align==L"center");}
    }else if(type==L"system.calendar"){
        p.Text(std::to_wstring(now.wMonth)+L"月",{14,14,170,28},12.5f,white,500);
        using namespace std::chrono;year_month_day first{year{now.wYear},month{now.wMonth},day{1}};
        int lead=static_cast<int>(weekday{sys_days{first}}.iso_encoding())-1;
        int days=static_cast<int>(static_cast<unsigned>((year{now.wYear}/month{now.wMonth}/last).day()));
        int rows=(lead+days+6)/7;float rh=133.0f/(rows+1),cell=156.0f/7;
        for(int i=0;i<7;i++)p.Text(std::wstring(1,L"一二三四五六日"[i]),{14+i*cell,37,14+(i+1)*cell,37+rh},8.5f,muted,400,DWRITE_TEXT_ALIGNMENT_CENTER);
        for(int d=1;d<=days;d++){
            int index=lead+d-1;float x=14+(index%7)*cell,y=37+(index/7+1)*rh;
            if(d==now.wDay)c->FillEllipse({{x+cell/2,y+rh/2},9.5f,9.5f},p.Color(p.theme.Accent()));
            p.Text(std::to_wstring(d),{x,y,x+cell,y+rh},10.5f,d==now.wDay?p.theme.OnAccent():white,d==now.wDay?700:400,DWRITE_TEXT_ALIGNMENT_CENTER,1);
        }
    }else if(type==L"system.status"){
        auto section=[&](wchar_t const *label,float top,wchar_t const *key){
            p.Text(label,{14,top,170,top+15},12,white,500);
            for(int i=0;i<2;i++){float y=top+25+i*24;
                p.Icon(i?L"temperature":L"gauge",{14,y+6,25,y+17},muted);
                p.Text(i?L"温度":L"使用率",{31,y,117,y+24},10,muted);
                p.Text(i?L"—":CardPercent(app.model.cardData,key),{118,y,170,y+24},13,white,600,DWRITE_TEXT_ALIGNMENT_TRAILING,1);}
        };
        section(L"CPU",14,L"cpu");p.Line(14,92,170,92,p.theme.Gray(900,.1f));section(L"GPU",105,L"gpu");
    }else if(type==L"system.storage"){
        double total=0,free=0,diskTotal=0,diskFree=0;
        if(app.model.cardData){
            auto memory=app.model.cardData.TryLookup(L"memory");if(memory&&memory.ValueType()==JsonValueType::Object){auto m=memory.GetObject();total=Num(m,L"total");free=Num(m,L"free");}
            auto disks=app.model.cardData.TryLookup(L"disks");if(disks&&disks.ValueType()==JsonValueType::Array)for(auto const &v:disks.GetArray()){auto d=v.GetObject();diskTotal+=Num(d,L"total");diskFree+=Num(d,L"free");}
        }
        auto block=[&](wchar_t const *name,float top,double all,double available,bool disk){
            float used=all>0?Clamp(static_cast<float>((all-available)/all),0,1):0;
            auto ink=disk&&used>=.75f?D2D1::ColorF(0xff8a4c):white;
            p.Text(name,{14,top,125,top+17},12,white,500);
            auto gib=[](double v){return CardNumber(v/1073741824,v/1073741824>=100?0:1);};
            p.Text(all>0?gib(all-available)+L"/"+gib(all)+L" GiB":L"—",{14,top+22,128,top+38},11.5f,ink,600,DWRITE_TEXT_ALIGNMENT_LEADING,1);
            p.Ring(153,top+22,14,used,ink,2.6f);
            p.Text(all>0?CardNumber(used*100)+L"%":L"—",{136,top+5,170,top+39},8.5f,ink,600,DWRITE_TEXT_ALIGNMENT_CENTER,1);
            if(disk&&app.model.honorSignedIn)p.Text(L"打开管家清理",{14,153,128,176},9.5f,white);
        };
        block(L"RAM",26,total,free,false);p.Line(14,92,170,92,p.theme.Gray(900,.1f));block(L"磁盘",112,diskTotal,diskFree,true);
    }else if(kind==L"text"){
        text(Str(content,L"title"),{14,13,baseWidth-14,47},12);
        text(Str(content,L"body"),{14,53,baseWidth-14,baseHeight-12},Str(content,L"bodySize")==L"l"?22.0f:Str(content,L"bodySize")==L"s"?13.0f:16.0f);
    }else if(kind==L"shortcut"){
        text(tile.name,{14,10,baseWidth-14,35},12);int i=0;
        for(auto const &value:content.GetNamedArray(L"items")){auto item=value.GetObject();p.Fill({14,39+i*22.0f,baseWidth-14,59+i*22.0f},D2D1::ColorF(0x888888,.13f),6);text(Str(item,L"label"),{24,39+i*22.0f,baseWidth-24,61+i*22.0f},12);i++;}
    }else if(kind==L"countdown"){
        text(Str(content,L"label"),{14,17,baseWidth-14,47},12);auto target=Str(content,L"target");
        int year=now.wYear,month=now.wMonth,day=now.wDay,hour=0,minute=0;
        if(target.size()==5)swscanf_s(target.c_str(),L"%d:%d",&hour,&minute);
        else swscanf_s(target.c_str(),L"%d-%d-%d %d:%d",&year,&month,&day,&hour,&minute);
        using namespace std::chrono;auto date=year_month_day{std::chrono::year{year},std::chrono::month{static_cast<unsigned>(month)},std::chrono::day{static_cast<unsigned>(day)}};
        auto today=year_month_day{std::chrono::year{now.wYear},std::chrono::month{now.wMonth},std::chrono::day{now.wDay}};
        long long left=duration_cast<seconds>(sys_days{date}-sys_days{today}+hours{hour-now.wHour}+minutes{minute-now.wMinute}-seconds{now.wSecond}).count();
        if(left<0&&target.size()==5)left+=86400;
        auto format=Str(content,L"format");std::wstring value=left<=0?Str(content,L"doneText"):format==L"d"?std::to_wstring((left+86399)/86400)+L"天":std::to_wstring(left/3600)+L":"+(left%3600/60<10?L"0":L"")+std::to_wstring(left%3600/60);
        if(format==L"hms"&&left>0)value+=L":"+(left%60<10?std::wstring(L"0"):L"")+std::to_wstring(left%60);
        text(value,{12,baseHeight/2-10,baseWidth-12,baseHeight/2+42},36,align==L"center");
    }else text(tile.name,{14,32,170,150},18,true);
    c->SetTransform(old);
}
void Window::CardAction(std::wstring const &key,std::wstring const &kind,int index)
{
    auto tile=Find(key);if(!tile||!tile->card)return;
    if(kind==L"handoff-edit"||kind==L"honor-open"||kind==L"image-save"||kind==L"weather-city"||kind==L"weather-info"||kind==L"cleanup"||kind==L"shortcut"||kind==L"todo-add"||kind==L"todo-edit"||kind==L"todo-completed")AllowSetForegroundWindow(app.parent);
    JsonObject request;Put(request,L"op",std::wstring(L"card-action"));Put(request,L"board",boardId);Put(request,L"cardId",Str(tile->card,L"id"));Put(request,L"kind",kind);Put(request,L"index",static_cast<double>(index));
    if(Str(tile->card,L"type")==L"system.week")Put(request,L"date",NativeWeekSelected(*this,*tile));
    if((kind==L"todo-toggle"||kind==L"todo-edit")&&index>=0){auto values=NativeTodoData(*this,*tile).GetNamedArray(L"items",JsonArray{});
        if(index<static_cast<int>(values.Size())){auto item=values.GetAt(index).GetObject();Put(request,L"itemId",Str(item,L"id"));request.Insert(L"done",JsonValue::CreateBooleanValue(!Bool(item,L"done")));}
        else if(kind==L"todo-toggle")return;
    }
    app.Request(request);
}

void Window::UpdateCardClock()
{
    KillTimer(hwnd,73);
    if(!app.model.components||sessionPaused||!IsWindowVisible(hwnd)||IsIconic(hwnd))return;
    bool clock=false,seconds=false;auto board=app.model.Find(boardId);
    if(board)for(auto const &tile:board->tiles)if(tile.card){
        auto type=Str(tile.card,L"type");if(type==L"system.media"&&NativeMediaPlaying(app.model.mediaData)){clock=true;seconds=true;}if(type==L"system.clock"||type==L"system.calendar"||type==L"system.week")clock=true;
        auto custom=tile.card.TryLookup(L"custom");
        if(custom&&custom.ValueType()==JsonValueType::Object){auto value=custom.GetObject();auto kind=Str(value,L"kind");clock|=kind==L"clock"||kind==L"countdown";
            if(kind==L"clock"||kind==L"countdown"){auto content=value.GetNamedObject(L"content");seconds|=Bool(content,L"showSeconds")||Str(content,L"format")==L"hms";}}
    }
    SYSTEMTIME now{};GetLocalTime(&now);
    if(clock)SetTimer(hwnd,73,seconds?1000-now.wMilliseconds:60000-now.wSecond*1000-now.wMilliseconds,nullptr);
}

// Retain the root surface, update only demanded card rectangles. The returned
// interop offset identifies the update rectangle's top-left in the draw target.
void Window::DrawCards(bool resources,std::wstring const &filter)
{
    if(GridEditing())return;
    if(!ProjectionReady())return;
    if(!hwnd||sessionPaused||!IsWindowVisible(hwnd)||IsIconic(hwnd))return;
    if(!desktopSurface||!desktopStamp.valid){Draw();return;}
    NativeRenderScope rendering(app.relay);if(!rendering){InvalidateRect(hwnd,nullptr,FALSE);return;}
    auto targetSurface=desktopSurface;if(!targetSurface)return;
    auto board=app.model.Find(boardId);if(!board)return;
    if(filter==L"system.clipboard-image"){SyncCardBackdrops();if(!desktopStamp.valid){Draw();return;}}
    for(auto const &tile:board->tiles){
        if(!tile.card)continue;
        auto type=Str(tile.card,L"type");bool resource=type==L"system.status"||type==L"system.storage";
        auto custom=tile.card.TryLookup(L"custom");std::wstring kind;
        if(custom&&custom.ValueType()==JsonValueType::Object)kind=Str(custom.GetObject(),L"kind");
        bool clock=type==L"system.clock"||type==L"system.calendar"||type==L"system.week"||kind==L"clock"||kind==L"countdown";
        if(filter.empty()?(resources?!resource:!clock):type!=filter)continue;
        auto r=RootBounds(tile.key);float scale=dpi/96;
        RECT update{std::max(0L,static_cast<LONG>(std::floor(r.left*scale))),std::max(0L,static_cast<LONG>(std::floor(r.top*scale))),
                    std::min(static_cast<LONG>(rootPixelWidth),static_cast<LONG>(std::ceil(r.right*scale))),std::min(static_cast<LONG>(rootPixelHeight),static_cast<LONG>(std::ceil(r.bottom*scale)))};
        if(update.right<=update.left||update.bottom<=update.top)continue;
        auto interop=targetSurface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};
        check_hresult(interop->BeginDraw(&update,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));bool ended=false,clipped=false;
        try{
            c->SetDpi(dpi,dpi);c->SetTransform(D2D1::Matrix3x2F::Translation((offset.x-update.left)/scale,(offset.y-update.top)/scale));
            c->PushAxisAlignedClip({update.left/scale,update.top/scale,update.right/scale,update.bottom/scale},D2D1_ANTIALIAS_MODE_ALIASED);clipped=true;
            c->Clear(D2D1::ColorF(0xf1f4fb,app.model.desktopRuntime?0.0f:1.0f));PaintCard(c.get(),tile,r);if(folderId.empty()&&(selected.contains(tile.key)||focusKey==tile.key)){NativePaint selection(c.get(),app.graphics,Preview70Theme::systemDark);selection.Interaction(NativeRegion(r,{0,0,width,height},NativeMetrics::PanelRadius),selected.contains(tile.key),false,focusKey==tile.key);}c->PopAxisAlignedClip();clipped=false;
            HRESULT hr=interop->EndDraw();ended=true;check_hresult(hr);++cardRedraws;++redraws;
            cardUpdatePixels+=static_cast<unsigned long long>(update.right-update.left)*(update.bottom-update.top);
        }catch(...){if(clipped)c->PopAxisAlignedClip();if(!ended)interop->EndDraw();throw;}
    }
}
