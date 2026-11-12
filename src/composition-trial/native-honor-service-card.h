#pragma once
inline std::vector<JsonObject> NativeServiceRows(Window &window,Tile const &tile){
    std::set<std::wstring> removed;auto settings=tile.card.GetNamedObject(L"settings",JsonObject{});for(auto const &v:settings.GetNamedArray(L"removed",JsonArray{}))if(v.ValueType()==JsonValueType::String)removed.insert(std::wstring(v.GetString()));
    std::vector<JsonObject> rows;if(window.app.model.honorData)for(auto const &v:window.app.model.honorData.GetNamedArray(L"items",JsonArray{})){auto row=v.GetObject();if(!removed.contains(Str(row,L"id")))rows.push_back(row);}return rows;
}
inline int NativeServiceCapacity(Tile const &tile){return Num(tile.card,L"columns",4)>=4?8:4;}
inline int NativeServicePage(Window &window,Tile const &tile,int count){
    auto &page=window.honorServicePages[tile.key];page=std::clamp(page,0,std::max(0,(count+NativeServiceCapacity(tile)-1)/NativeServiceCapacity(tile)-1));return page;
}
inline D2D1_RECT_F NativeServiceCell(int index,float width){int columns=width>200?4:2;float gap=6,cell=(width-28-(columns-1)*gap)/columns;return {14+(index%columns)*(cell+gap),48+(index/columns)*63.f,14+(index%columns)*(cell+gap)+cell,108+(index/columns)*63.f};}
inline int NativeServiceHit(Window &window,Tile const &tile,float x,float y,float width){
    if(y>=10&&y<=35&&x>=width-80&&x<=width-10)return x<width-52?-2:x>width-34?-3:-1;
    auto rows=NativeServiceRows(window,tile);int capacity=NativeServiceCapacity(tile),page=NativeServicePage(window,tile,static_cast<int>(rows.size()));
    for(int i=0;i<capacity&&page*capacity+i<static_cast<int>(rows.size());i++){auto r=NativeServiceCell(i,width);if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)return static_cast<int>(Num(rows[page*capacity+i],L"index",-1));}return -1;
}
inline void Window::HonorPage(Tile const &tile,int direction){auto rows=NativeServiceRows(*this,tile);int last=std::max(0,(static_cast<int>(rows.size())+NativeServiceCapacity(tile)-1)/NativeServiceCapacity(tile)-1);auto &page=honorServicePages[tile.key];page=std::clamp(page+direction,0,last);honorServiceFocus[tile.key]=-1;DrawCards(true,L"system.honor-service");}
inline void PaintNativeHonorService(Window &window,NativePaint &paint,Tile const &tile,float width,float){
    if(!window.app.model.honorSignedIn){paint.Text(L"卡片暂不可用",{14,60,width-14,164},12,paint.theme.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_CENTER);return;}
    auto rows=NativeServiceRows(window,tile);int capacity=NativeServiceCapacity(tile),page=NativeServicePage(window,tile,static_cast<int>(rows.size())),pages=std::max(1,(static_cast<int>(rows.size())+capacity-1)/capacity);
    paint.Text(width>200?L"荣耀服务中心":L"荣耀服务",{14,10,width-84,35},width>200?14.f:12.f,paint.theme.Ink(),600);
    paint.Icon(L"previous",{width-80,18,width-68,30},paint.theme.MaterialMuted());paint.Text(std::to_wstring(page+1)+L"/"+std::to_wstring(pages),{width-67,10,width-34,35},9,paint.theme.Ink(),400,DWRITE_TEXT_ALIGNMENT_CENTER);paint.Icon(L"next",{width-26,18,width-14,30},paint.theme.MaterialMuted());
    if(rows.empty()){paint.Text(window.app.model.honorData&&Bool(window.app.model.honorData,L"loading")?L"正在读取本机服务…":L"没有保留的入口\n右键卡片可全部恢复",{14,60,width-14,164},12,paint.theme.MaterialMuted(),400,DWRITE_TEXT_ALIGNMENT_CENTER,0,true);return;}
    for(int i=0;i<capacity&&page*capacity+i<static_cast<int>(rows.size());i++){
        auto row=rows[page*capacity+i];auto rect=NativeServiceCell(i,width);auto ink=Bool(row,L"available")?paint.theme.Ink():paint.theme.MaterialMuted();paint.Fill(rect,paint.theme.SectionFilm(),6);
        std::wstring key=Str(row,L"id"),png=Str(row,L"png");bool painted=false;
        if(!png.empty()){auto &app=window.app;if(app.honorServicePng[key]!=png){Preview70Icons icons;icons.Load(app.graphics,key,png);if(auto found=icons.values.find(key);found!=icons.values.end()){app.honorServiceIcons[key]=found->second;app.honorServicePng[key]=png;}}
            if(auto found=app.honorServiceIcons.find(key);found!=app.honorServiceIcons.end()){paint.c->DrawBitmap(found->second.bitmap.get(),{rect.left+7,rect.top+7,rect.left+23,rect.top+23},Bool(row,L"available")?1.f:.45f,D2D1_INTERPOLATION_MODE_LINEAR);painted=true;}}
        if(!painted)paint.Icon(L"apps",{rect.left+7,rect.top+7,rect.left+23,rect.top+23},ink);
        paint.Text(Str(row,L"name"),{rect.left+5,rect.top+27,rect.right-4,rect.top+43},9.5f,ink,500);
        paint.Text(Bool(row,L"available")&&Str(row,L"status")==L"可用"?Str(row,L"group"):Str(row,L"status"),{rect.left+5,rect.top+43,rect.right-3,rect.bottom-2},7.5f,paint.theme.MaterialMuted());
        if(window.honorServiceFocus.contains(tile.key)&&window.honorServiceFocus[tile.key]==page*capacity+i)paint.c->DrawRoundedRectangle(D2D1::RoundedRect(rect,6,6),paint.Color(paint.theme.Accent()),1.5f);
    }
}
inline bool Window::HonorKey(UINT key){
    auto tile=Find(focusKey);if(!tile||!tile->card||Str(tile->card,L"type")!=L"system.honor-service")return false;
    if(key==VK_LEFT||key==VK_RIGHT){HonorPage(*tile,key==VK_LEFT?-1:1);return true;}
    auto rows=NativeServiceRows(*this,*tile);if(rows.empty())return false;
    if(key==VK_TAB){auto &index=honorServiceFocus[tile->key];index=std::clamp(index+((GetKeyState(VK_SHIFT)&0x8000)?-1:1),0,static_cast<int>(rows.size())-1);honorServicePages[tile->key]=index/NativeServiceCapacity(*tile);DrawCards(true,L"system.honor-service");return true;}
    if(key==VK_RETURN&&honorServiceFocus.contains(tile->key)){int index=honorServiceFocus[tile->key];if(index>=0&&index<static_cast<int>(rows.size()))CardAction(tile->key,L"honor-open",static_cast<int>(Num(rows[index],L"index")));return true;}return false;
}
