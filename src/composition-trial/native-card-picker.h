#pragma once
// The preview70 E layout, rendered by the same card painters as the desktop.
// Only the selected static preview is painted; no card hosts or timers mount.
inline JsonObject NativeCardDependency(JsonObject const &definition){
    auto value=definition.TryLookup(L"dependency");return value&&value.ValueType()==JsonValueType::Object?value.GetObject():JsonObject{};
}
inline JsonObject NativePopup::PickerDefinition() const
{
    for(auto const &v:PickerDefinitions()){auto d=v.GetObject();if(Str(d,L"type")==pickerType)return d;}
    return PickerDefinitions().Size()?PickerDefinitions().GetAt(0).GetObject():JsonObject{};
}
inline void NativePopup::BuildPickerRows()
{
    bool found=false;for(auto const &v:PickerDefinitions())found|=Str(v.GetObject(),L"type")==pickerType;
    if(!found)pickerType=PickerDefinitions().Size()?Str(PickerDefinitions().GetAt(0).GetObject(),L"type"):L"";
    int columns=PickerColumns();
    scroll=std::clamp(scroll,0,std::max(0,static_cast<int>(PickerDefinitions().Size())/columns*columns));
    if(pickerConfirm){
        auto add=[&](std::wstring id,std::wstring label,float left){PopupRow row;row.key=row.id=id;row.label=label;row.action=id;row.rect={left,height/2+48,left+132,height/2+82};rows.push_back(row);};
        add(L"picker-cancel",L"取消",width/2-146);add(L"picker-uninstall",L"卸载卡片",width/2+14);return;
    }
    float split=width*.68f,cardW=(split-48-(columns-1)*12)/columns,top=106;int index=0;bool customGroup=false;
    for(auto const &v:PickerDefinitions()){auto d=v.GetObject();bool custom=d.TryLookup(L"custom")&&d.GetNamedValue(L"custom").ValueType()==JsonValueType::Object;
        if(custom&&!customGroup){top+=std::ceil(index/static_cast<float>(columns))*132+40;index=0;customGroup=true;}
        float x=24+(index%columns)*(cardW+12),y=top+(index/columns)*132-(scroll/columns)*132.f;PopupRow row;
        row.key=row.id=L"pick:"+Str(d,L"type");row.label=Str(d,L"title");row.action=L"pick";row.detail=custom?L"custom":L"system";row.rect={x,y,x+cardW,y+116};row.active=Str(d,L"type")==pickerType;
        row.offscreen=y+116<=100||y>=height-44;rows.push_back(row);index++;
    }
    auto add=[&](std::wstring id,std::wstring label,D2D1_RECT_F rect,std::wstring action){PopupRow r;r.key=r.id=id;r.label=label;r.rect=rect;r.action=action;rows.push_back(r);};
    auto definition=PickerDefinition();auto sizes=definition.TryLookup(L"sizes");
    if(NativeCardDependency(definition).Size())add(L"picker-dependency",L"前往 GitHub Releases",{split+28,height-202,width-34,height-174},L"picker-dependency");
    if(sizes&&sizes.ValueType()==JsonValueType::Array){auto list=sizes.GetArray();pickerSize=std::clamp(pickerSize,0,std::max(0,static_cast<int>(list.Size())-1));
        for(UINT i=0;i<list.Size()&&i<4;i++){auto size=list.GetAt(i).GetObject();float x=split+18+(i%2)*100.f,y=height-142+(i/2)*30.f;
            add(L"size:"+std::to_wstring(i),std::to_wstring(static_cast<int>(Num(size,L"rows")))+L" 行 × "+std::to_wstring(static_cast<int>(Num(size,L"columns")))+L" 列",{x,y,x+90,y+26},L"size");rows.back().active=static_cast<int>(i)==pickerSize;}}
    add(L"picker-add",L"添加到桌面",{split+18,height-72,width-24,height-38},L"picker-add");rows.back().enabled=PickerDefinitions().Size()>0;
    add(L"picker-import",L"打开卡片目录",{width-220,18,width-66,48},L"picker-import");
    add(L"picker-close",L"关闭",{width-50,16,width-18,48},L"picker-close");
}
inline void NativePopup::PaintPicker(ID2D1DeviceContext *c)
{
    NativePaint p(c,app.graphics,dark);auto ink=p.theme.Ink(),muted=p.theme.MaterialMuted();
    auto surface=p.theme.Material(NativeMaterial::Panel,Glass()),rail=p.theme.Film(),line=p.theme.Gray(900,.10f),soft=p.theme.Film();
    if(pickerConfirm){
        p.Fill({10,10,width-10,height-10},surface,10);auto d=PickerDefinition();
        p.Text(L"确认卸载「"+Str(d,L"title")+L"」？",{24,height/2-80,width-24,height/2-40},20,ink,600,DWRITE_TEXT_ALIGNMENT_CENTER);
        p.Text(L"将移除 "+std::to_wstring(PickerInstances())+L" 张桌面卡片，卡片包送入回收站。",{24,height/2-26,width-24,height/2+14},13,muted,400,DWRITE_TEXT_ALIGNMENT_CENTER);
        for(auto const &row:rows){p.Fill(row.rect,soft,6);p.Text(row.label,row.rect,13,row.id==L"picker-uninstall"?p.theme.Danger():ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);}
        return;
    }
    float split=width*.68f;if(!NativeLowPower(app.model))p.Shadow({10,10,width-10,height-10},NativeMetrics::FloatingRadius,NativeMetrics::PopupInset);p.Material({10,10,width-10,height-10},NativeMaterial::Panel,Glass(),NativeMetrics::FloatingRadius);
    p.Fill({split,62,width-10,height-30},rail,0);p.Line(18,62,width-18,62,line);p.Line(split,62,split,height-30,line);
    p.Text(L"新建卡片",{26,18,width-62,40},17,ink,600);p.Text(L"系统卡片与我的卡片",{26,40,width-62,56},11,muted);
    p.Text(L"系统卡片",{24,74,split-24,98},13,ink,600);p.Text(L"预览",{split+18,72,width-24,94},11,muted);
    for(auto const &r:rows)if(r.action==L"pick"&&r.detail==L"custom"){
        if(r.rect.top-28>=100&&r.rect.top-4<height-44)p.Text(L"我的卡片",{24,r.rect.top-28,split-24,r.rect.top-4},13,ink,600);break;}
    int thumbnails=0;
    for(int i=0;i<static_cast<int>(rows.size());i++){auto const &r=rows[i];if(r.offscreen)continue;
        if(r.action==L"pick"){
            c->PushAxisAlignedClip({18,100,split-12,height-44},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            auto box=r.rect;p.Fill(box,soft,10);c->DrawRoundedRectangle(D2D1::RoundedRect(box,10,10),p.Color(line),1.f);
            p.Fill({box.left+1,box.top+1,box.right-1,box.top+78},soft,9);
            auto type=r.id.substr(5);if(type==L"system.clock")p.Fill({box.left+1,box.top+1,box.right-1,box.top+78},D2D1::ColorF(0x363636),9);
            for(auto const &v:PickerDefinitions()){auto d=v.GetObject();if(Str(d,L"type")!=type)continue;
                Tile sample;sample.key=L"thumbnail";sample.name=r.label;sample.card=JsonObject{};Put(sample.card,L"type",type);sample.card.Insert(L"settings",JsonObject{});if(auto custom=d.TryLookup(L"custom"))sample.card.Insert(L"custom",custom);
                auto sizes=d.GetNamedArray(L"sizes");auto size=sizes.GetAt(0).GetObject();float cols=static_cast<float>(Num(size,L"columns",2)),rs=static_cast<float>(Num(size,L"rows",2));Put(sample.card,L"columns",cols);Put(sample.card,L"rows",rs);
                float bw=cols*96-8,bh=rs*96-8,scale=std::min((box.right-box.left-16)/bw,64.f/bh),cx=(box.left+box.right)/2,cy=box.top+39;
                c->PushAxisAlignedClip({box.left+1,box.top+1,box.right-1,box.top+78},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                D2D1_MATRIX_3X2_F old;c->GetTransform(&old);c->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(cx-bw*scale/2,cy-bh*scale/2)*old);
                Window preview(app,L"thumbnail");preview.materialPreview=Glass();preview.PaintCard(c,sample,{0,0,bw,bh});c->SetTransform(old);c->PopAxisAlignedClip();
                if(NativeCardDependency(d).Size()){
                    D2D1_RECT_F badge{box.right-68,box.top+6,box.right-6,box.top+24};p.Fill(badge,D2D1::ColorF(0x202126,.82f),5);
                    p.Text(L"↓ 需下载",badge,10,D2D1::ColorF(0xffffff),500,DWRITE_TEXT_ALIGNMENT_CENTER);
                }break;
            }
            p.Interaction(InteractionRegion(i),r.active,i==hover,i==focus);
            p.Text(r.label,{box.left+10,box.top+82,box.right-10,box.bottom-8},12,ink,r.active?600:500);thumbnails++;c->PopAxisAlignedClip();
        }else if(r.action==L"picker-close")p.Icon(L"close",{r.rect.left+8,r.rect.top+8,r.rect.left+24,r.rect.top+24},muted);
        else if(r.action!=L"picker-dependency"){p.Fill(r.rect,r.action==L"picker-add"?p.theme.Accent():soft,6);p.Text(r.label,r.rect,12,r.action==L"picker-add"?p.theme.OnAccent():muted,500,DWRITE_TEXT_ALIGNMENT_CENTER);}
        // Keyboard focus remains available through UIA without a colored outline.
    }
    auto definition=PickerDefinition();if(!Str(definition,L"type").empty()){
        auto dependency=NativeCardDependency(definition);bool external=dependency.Size()>0;
        D2D1_RECT_F stage{split+18,100,width-24,external?height-354:height-220};p.Fill(stage,D2D1::ColorF(0x363636),10);

        Tile tile;tile.name=Str(definition,L"title");tile.key=L"static-preview";tile.card=JsonObject{};Put(tile.card,L"id",std::wstring(L"static-preview"));Put(tile.card,L"type",Str(definition,L"type"));
        tile.card.Insert(L"settings",JsonObject{});if(auto custom=definition.TryLookup(L"custom"))tile.card.Insert(L"custom",custom);
        auto sizes=definition.GetNamedArray(L"sizes");auto selected=sizes.GetAt(std::clamp(pickerSize,0,static_cast<int>(sizes.Size())-1)).GetObject();Put(tile.card,L"columns",Num(selected,L"columns"));Put(tile.card,L"rows",Num(selected,L"rows"));
        float w=static_cast<float>(Num(selected,L"columns"))*96-8,h=static_cast<float>(Num(selected,L"rows"))*96-8,scale=std::min((stage.right-stage.left-24)/w,(stage.bottom-stage.top-24)/h);
        float x=(stage.left+stage.right-w*scale)/2,y=(stage.top+stage.bottom-h*scale)/2;
        D2D1_MATRIX_3X2_F old;c->GetTransform(&old);c->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(x,y)*old);
        Window card(app,L"static-preview");card.materialPreview=Glass();card.PaintCard(c,tile,{0,0,w,h});c->SetTransform(old);
        if(external){
            p.Text(Str(definition,L"title"),{split+18,height-346,width-24,height-318},15.5f,ink,500);
            D2D1_RECT_F notice{split+18,height-310,width-24,height-164};p.Fill(notice,soft,10);
            p.Text(Str(dependency,L"title"),{notice.left+10,notice.top+6,notice.right-10,notice.top+34},12,ink,600);
            p.Text(Str(dependency,L"description"),{notice.left+10,notice.top+34,notice.right-10,notice.top+104},11,muted,400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
            for(int i=0;i<static_cast<int>(rows.size());++i)if(rows[i].action==L"picker-dependency"){
                auto const &r=rows[i];p.Fill(r.rect,soft,6);p.Interaction(InteractionRegion(i),false,i==hover,i==focus);
                p.Text(r.label+L" ↗",r.rect,11,ink,500,DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }else{
            p.Text(Str(definition,L"title"),{split+18,height-208,width-24,height-180},15.5f,ink,500);
            p.Text(Str(definition,L"description"),{split+18,height-178,width-24,height-148},12,muted,400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
        }
    }
    p.Line(18,height-30,width-18,height-30,line);p.Text(std::to_wstring(PickerDefinitions().Size())+L" 种卡片",{24,height-28,split-24,height-12},11,muted);
    if(!status.empty())p.Text(status,{24,height-60,split-24,height-32},12,p.theme.Danger(),400,DWRITE_TEXT_ALIGNMENT_LEADING,0,true);
    if(!thumbnails)p.Text(L"暂无卡片",{24,110,split-24,160},13,muted);
}
inline void NativePopup::PickerActivate(PopupRow const &row)
{
    if(row.action==L"picker-dependency"){
        JsonObject r;Put(r,L"op",std::wstring(L"card-manage"));Put(r,L"board",pickerBoard);Put(r,L"kind",std::wstring(L"open-dependency"));Put(r,L"type",pickerType);
        auto owner=life;app.Request(r,[owner](JsonObject const &reply){if(!owner->owner)return;auto popup=owner->owner;popup->status=Bool(reply,L"ok")?L"":Str(reply,L"error");popup->Draw();});return;
    }
    if(row.action==L"picker-import"){
        JsonObject r;Put(r,L"op",std::wstring(L"card-manage"));Put(r,L"board",pickerBoard);Put(r,L"kind",std::wstring(L"open-directory"));app.Request(r);return;
    }
    if(row.action==L"picker-cancel"){pickerConfirm=false;BuildRows();Draw();return;}
    if(row.action==L"picker-uninstall"){
        auto definition=PickerDefinition();auto custom=definition.TryLookup(L"custom");if(!custom||custom.ValueType()!=JsonValueType::Object)return;
        JsonObject r;Put(r,L"op",std::wstring(L"card-manage"));Put(r,L"board",pickerBoard);Put(r,L"kind",std::wstring(L"uninstall"));Put(r,L"type",pickerType);
        Put(r,L"fingerprint",Str(custom.GetObject(),L"fingerprint"));Put(r,L"instances",static_cast<double>(PickerInstances()));Put(r,L"catalogRevision",static_cast<double>(pickerRevision));
        auto owner=life;app.Request(r,[owner](JsonObject const &reply){if(!owner->owner)return;auto popup=owner->owner;popup->pickerConfirm=false;
            popup->status=Bool(reply,L"ok")?L"卡片包已送入回收站":Str(reply,L"error");popup->LoadPickerCatalog();});return;
    }
    if(row.action==L"pick"){pickerType=row.id.substr(5);pickerSize=0;BuildRows();Draw();return;}
    if(row.action==L"size"){pickerSize=_wtoi(row.id.substr(5).c_str());BuildRows();Draw();return;}
    if(row.action==L"picker-close"){Hide(false,true);return;}
    if(row.action!=L"picker-add")return;auto definition=PickerDefinition();auto sizes=definition.GetNamedArray(L"sizes");if(!sizes.Size())return;
    auto size=sizes.GetAt(static_cast<UINT>(pickerSize)).GetObject();JsonObject request;
    Put(request,L"op",std::wstring(L"card-action"));Put(request,L"board",pickerBoard);Put(request,L"kind",std::wstring(L"create"));Put(request,L"type",pickerType);
    Put(request,L"columns",Num(size,L"columns"));Put(request,L"rows",Num(size,L"rows"));request.Insert(L"settings",JsonObject{});
    if(pickerHasCell){Put(request,L"x",static_cast<double>(pickerCell.x));Put(request,L"y",static_cast<double>(pickerCell.y));}
    auto owner=life;app.Request(request,[owner](JsonObject const &response){if(!owner->owner)return;if(Bool(response,L"ok"))owner->owner->Hide(false,true);
        else{owner->owner->status=Str(response,L"error");owner->owner->Draw();}});
}

inline JsonArray NativePopup::PickerDefinitions()const
{
    return pickerDefinitions?pickerDefinitions:app.model.cardDefinitions;
}
inline int NativePopup::PickerInstances()const
{for(auto const &v:pickerCatalog){auto row=v.GetObject();if(Str(row.GetNamedObject(L"definition"),L"type")==pickerType)return static_cast<int>(Num(row,L"instances"));}return 0;}
inline void NativePopup::LoadPickerCatalog()
{
    if(!visible||!life)return;JsonObject r;Put(r,L"op",std::wstring(L"card-manage"));Put(r,L"board",pickerBoard);Put(r,L"kind",std::wstring(L"list"));auto owner=life;
    app.Request(r,[owner](JsonObject const &response){if(!owner->owner)return;auto popup=owner->owner;
        if(Bool(response,L"ok")){auto result=response.GetNamedObject(L"result");popup->pickerCatalog=result.GetNamedArray(L"types");popup->pickerDefinitions=JsonArray{};for(auto const &v:popup->pickerCatalog)popup->pickerDefinitions.Append(v.GetObject().GetNamedObject(L"definition"));popup->pickerRevision=static_cast<long long>(Num(result,L"revision"));}
        else popup->status=Str(response,L"error");popup->BuildRows();popup->Draw();});
}
inline void NativePopup::PickerMenu(int index,POINT point)
{
    if(index<0||index>=static_cast<int>(rows.size())||rows[index].action!=L"pick")return;
    pickerType=rows[index].id.substr(5);auto definition=PickerDefinition();auto custom=definition.TryLookup(L"custom");if(!custom||custom.ValueType()!=JsonValueType::Object)return;
    HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"卸载卡片");
    contextMenuOpen=true;int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);contextMenuOpen=false;
    if(choice==1){pickerConfirm=true;BuildRows();Draw();}
}
