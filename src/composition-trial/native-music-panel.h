#pragma once
// Shared PopupRow rectangles are the paint, input and accessibility bounds.
inline void NativePopup::LayoutMusicPanel(){
    functionalFrames.clear();functionalSections.clear();functionalPinned.clear();
    functionalTop=82;functionalBottom=height-18;float y=functionalTop;
    for(size_t i=0;i<rows.size();++i){auto &r=rows[i];r.offscreen=false;
        if(r.control==L"music-source"){
            float left=22,right=width-22;D2D1_RECT_F body{left,y,right,y+56};
            functionalFrames.push_back({body,r.id});r.rect={left+104,y,right-38,y+56};
            for(size_t j=i+1;j<rows.size()&&rows[j].control!=L"music-source";++j){auto &button=rows[j];
                int slot=button.control==L"music-play"?0:button.control==L"music-previous"?1:2;
                float x=button.control==L"music-pin"?right-34:left+slot*34;
                button.rect={x,y+11,x+32,y+45};button.offscreen=false;
            }
            y+=64;
        }else if(r.id==L"music-empty"){r.rect={22,y,width-22,y+72};y+=80;}
    }
    functionalExtent=y-8;functionalOverflow=std::max(0.f,functionalExtent-functionalBottom);
    scroll=std::clamp(scroll,0,static_cast<int>(std::ceil(functionalOverflow/56)));
    float offset=std::min(scroll*56.f,functionalOverflow);
    for(auto &r:rows){r.rect.top-=offset;r.rect.bottom-=offset;r.offscreen=r.rect.bottom<=functionalTop||r.rect.top>=functionalBottom;}
    for(auto &f:functionalFrames){f.rect.top-=offset;f.rect.bottom-=offset;}
}
inline void NativeMusicPin(NativePaint &p,D2D1_RECT_F box,bool pinned){
    // Original vector pushpin. One path; no bitmap plate or font dependency.
    auto ink=pinned?p.theme.Accent():p.theme.Ink();float cx=(box.left+box.right)/2,cy=(box.top+box.bottom)/2;
    com_ptr<ID2D1PathGeometry> path;com_ptr<ID2D1Factory> factory;p.c->GetFactory(factory.put());factory->CreatePathGeometry(path.put());
    com_ptr<ID2D1GeometrySink> sink;path->Open(sink.put());sink->BeginFigure({cx-4,cy-8},D2D1_FIGURE_BEGIN_FILLED);
    D2D1_POINT_2F points[]={{cx+4,cy-8},{cx+3,cy-1},{cx+6,cy+3},{cx-6,cy+3},{cx-3,cy-1}};
    sink->AddLines(points,5);sink->EndFigure(D2D1_FIGURE_END_CLOSED);sink->Close();
    if(pinned)p.c->FillGeometry(path.get(),p.Color(ink));else p.c->DrawGeometry(path.get(),p.Color(ink),1.5f);
    p.Line(cx,cy+3,cx,cy+9,ink,1.5f);
}
inline void NativePopup::PaintMusicPanel(ID2D1DeviceContext *c){
    NativePaint p(c,app.graphics,dark);auto &t=p.theme;
    if(!resizePainting)p.Material({10,10,width-10,height-10},MaterialTier(),Glass(),NativeMetrics::FloatingRadius);
    p.Text(Ui(L"音乐",L"Music"),{24,20,width-24,48},16,t.Ink(),600);
    p.Text(Ui(L"拖动标题排序 · 固定的来源显示在顶栏",L"Drag to reorder · Pin a source to the bar"),{24,50,width-24,72},11,t.Muted());
    c->PushAxisAlignedClip({18,functionalTop,width-18,functionalBottom},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for(auto const &frame:functionalFrames){
        if(frame.rect.bottom<=functionalTop||frame.rect.top>=functionalBottom)continue;
        auto r=std::find_if(rows.begin(),rows.end(),[&](auto const &v){return v.id==frame.label;});
        if(r!=rows.end()&&r->active)p.Fill(frame.rect,t.Raised(),NativeMetrics::ControlRadius);
        if(musicDragging&&frame.label==musicDropTarget){
            auto source=std::find_if(rows.begin(),rows.end(),[&](auto const &v){return v.id==musicPress;});
            float y=source<r?frame.rect.bottom+4:frame.rect.top-4;
            auto ink=t.Ink();ink.a=.45f;p.Line(frame.rect.left+8,y,frame.rect.right-8,y,ink,2);
        }
    }
    for(size_t i=0;i<rows.size();++i){auto const &r=rows[i];if(r.offscreen)continue;auto rect=r.rect;
        auto ink=r.enabled?t.Ink():t.Muted();if(!r.enabled)ink.a=.4f;
        if(r.control==L"music-source"){
            auto format=app.graphics.Format(13,false,500);com_ptr<IDWriteTextLayout> text;
            check_hresult(app.graphics.text->CreateTextLayout(r.label.c_str(),static_cast<UINT32>(r.label.size()),format,std::max(1.f,rect.right-rect.left),26,text.put()));
            text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> ellipsis;app.graphics.text->CreateEllipsisTrimmingSign(format,ellipsis.put());text->SetTrimming(&trim,ellipsis.get());
            c->DrawTextLayout({rect.left,rect.top+4},text.get(),p.Color(t.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            p.Text(r.detail,{rect.left,rect.top+28,rect.right,rect.bottom-4},10,t.Muted());
        }else if(r.control==L"music-pin")NativeMusicPin(p,rect,r.active);
        else if(r.control.starts_with(L"music-")){
            auto item=Data().GetNamedArray(L"rows").GetAt(static_cast<uint32_t>(i)).GetObject();
            auto icon=r.control==L"music-play"?(Bool(item,L"playing")?L"pause":L"play"):r.control==L"music-previous"?L"track-previous":L"track-next";
            float cx=(rect.left+rect.right)/2,cy=(rect.top+rect.bottom)/2;p.Icon(icon,{cx-9,cy-9,cx+9,cy+9},ink);
        }else{p.Text(r.label,{rect.left,rect.top,rect.right,rect.top+32},14,t.Ink());p.Text(r.detail,{rect.left,rect.top+34,rect.right,rect.bottom},11,t.Muted());}
    }
    c->PopAxisAlignedClip();
    if(functionalOverflow>0){float vh=functionalBottom-functionalTop,h=std::max(24.f,vh*vh/(vh+functionalOverflow));float y=functionalTop+(vh-h)*std::min(scroll*56.f,functionalOverflow)/functionalOverflow;auto ink=t.Muted();ink.a=.35f;p.Fill({width-16,y,width-13,y+h},ink,2);}
}
inline void NativePopup::MusicReorder(std::wstring const &from,std::wstring const &to){
    if(from.empty()||to.empty()||from==to)return;
    auto source=std::find_if(rows.begin(),rows.end(),[&](auto const &row){return row.id==from;});
    if(source==rows.end()||source->active)return;
    JsonObject result,request;Put(result,L"action",std::wstring(L"native-command"));Put(result,L"id",from);Put(result,L"destination",to);
    Put(request,L"op",std::wstring(L"popup-command"));Put(request,L"token",token);request.Insert(L"result",result);app.Request(request);
}
inline bool NativePopup::MusicPointer(UINT message,WPARAM w,LPARAM l){
    if(!MusicView())return false;
    if(message==WM_CAPTURECHANGED){musicPress.clear();musicDropTarget.clear();musicDragging=false;return false;}
    if(message==WM_KEYDOWN&&w==VK_ESCAPE&&!musicPress.empty()){musicPress.clear();musicDragging=false;if(GetCapture()==hwnd)ReleaseCapture();Draw();return true;}
    float x=GET_X_LPARAM(l)*96/dpi,y=GET_Y_LPARAM(l)*96/dpi;
    if(message==WM_LBUTTONDOWN){int i=Hit(x,y);if(i>=0&&rows[i].control==L"music-source"){
        if(rows[i].active){focus=i;return true;}
        musicPress=rows[i].id;musicPressPoint={x,y};musicDragging=false;focus=i;SetCapture(hwnd);return true;}return false;}
    if(message==WM_MOUSEMOVE&&!musicPress.empty()){
        musicDragging|=std::hypot(x-musicPressPoint.x,y-musicPressPoint.y)>5;
        if(musicDragging){float closest=FLT_MAX;std::wstring next;for(auto const &r:rows)if(r.control==L"music-source"&&!r.offscreen){float distance=std::abs((r.rect.top+r.rect.bottom)/2-(y-contentY));if(distance<closest){closest=distance;next=r.id;}}
            if(next!=musicDropTarget){musicDropTarget=next;Draw();}}return true;
    }
    if(message==WM_LBUTTONUP&&!musicPress.empty()){
        auto from=std::exchange(musicPress,L""),to=std::exchange(musicDropTarget,L"");bool commit=std::exchange(musicDragging,false);
        if(GetCapture()==hwnd)ReleaseCapture();if(commit&&x>=contentX&&x<=contentX+width&&y>=contentY+functionalTop&&y<=contentY+functionalBottom)MusicReorder(from,to);Draw();return true;
    }
    return false;
}
