#pragma once
// Generic, declarative text/indicator renderer. No provider-specific names or
// resources, external paths, scripts, browser windows or per-frame CPU timers.
inline JsonObject NativeExtensionView(JsonObject const &item){auto v=item.TryLookup(L"extensionView");return v&&v.ValueType()==JsonValueType::Object?v.GetObject():JsonObject{};}
inline JsonObject NativeExtensionOverlay(JsonObject const &view){auto v=view.TryLookup(L"overlay");return v&&v.ValueType()==JsonValueType::Object?v.GetObject():nullptr;}
inline float NativeBar::ExtensionTextWidth(std::wstring const &text,bool bold){
    float size=iconSize*.8f;auto key=L"extension:"+std::to_wstring(size)+(bold?L":bold:":L":regular:")+text;auto found=textWidths.find(key);if(found!=textWidths.end())return found->second;
    com_ptr<IDWriteTextLayout> layout;check_hresult(app.graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),app.graphics.Format(size,false,bold?600:400),65536,128,layout.put()));DWRITE_TEXT_METRICS metrics{};check_hresult(layout->GetMetrics(&metrics));
    if(textWidths.size()>=512)textWidths.clear();textWidths.emplace(key,metrics.widthIncludingTrailingWhitespace);return metrics.widthIncludingTrailingWhitespace;
}
inline float NativeExtensionSegmentWidth(NativeBar &bar,JsonObject const &segment){
    float width=0;auto indicator=segment.TryLookup(L"indicator");if(indicator&&indicator.ValueType()==JsonValueType::Object)width=static_cast<float>(Num(indicator.GetObject(),L"size",6))+4;
    for(auto const &v:segment.GetNamedArray(L"runs",JsonArray{})){auto run=v.GetObject();width+=bar.ExtensionTextWidth(Str(run,L"text"),Bool(run,L"bold"));}return width;
}
inline float NativeBar::ExtensionNaturalWidth(JsonObject const &item){
    auto view=NativeExtensionView(item);auto segments=view.GetNamedArray(L"segments",JsonArray{});if(!segments.Size()||segments.Size()>16)return 0;float width=0;
    for(auto const &v:segments)width+=NativeExtensionSegmentWidth(*this,v.GetObject());width+=std::max(0,static_cast<int>(segments.Size())-1)*static_cast<float>(Num(view,L"gap",6.4));
    return std::clamp(width,80.f,static_cast<float>(Num(view,L"maxWidth",680)));
}
inline void NativeBar::BudgetExtensions(){
    extensionWidthsReady=false;extensionWidths.clear();std::vector<JsonObject> extensions;float occupied=4*padding+16+8*iconSize*.25f;
    for(auto section:{L"left",L"center",L"right"})for(auto const &item:GroupItems(section)){
        occupied+=iconSize*.25f;if(Str(item,L"type")==L"Extension")extensions.push_back(item);else occupied+=ItemWidth(item);
    }
    float share=extensions.empty()?0:std::max(0.f,width-occupied)/extensions.size()-padding;
    for(auto const &item:extensions)extensionWidths[Str(item,L"id")]=share>=80?std::min(share,ExtensionNaturalWidth(item)):0;
    extensionWidthsReady=true;
}
inline D2D1_RECT_F NativeExtensionTrailing(NativeBar &bar,JsonObject const &overlay,float width,float height){
    auto trailing=overlay.TryLookup(L"trailingAction");if(!trailing||trailing.ValueType()!=JsonValueType::Object)return {};
    float w=std::min(std::max(24.f,bar.ExtensionTextWidth(Str(trailing.GetObject(),L"label"))+12),width*.35f);return {std::max(0.f,width-w),2,width,height-2};
}
inline std::wstring NativeBar::ExtensionActionAt(int index,float x,bool context){
    if(index<0||index>=static_cast<int>(entries.size()))return L"";auto item=entries[index];auto view=NativeExtensionView(item);float width=std::max(0.f,rows[index].rect.right-rows[index].rect.left-padding);
    if(auto overlay=NativeExtensionOverlay(view)){
        if(context){for(auto const &v:view.GetNamedArray(L"segments",JsonArray{})){auto action=Str(v.GetObject(),L"contextAction");if(!action.empty())return action;}return L"";}
        auto trailing=NativeExtensionTrailing(*this,overlay,width,rows[index].rect.bottom-rows[index].rect.top);
        if(trailing.right>trailing.left&&x>=trailing.left){auto value=overlay.GetNamedObject(L"trailingAction");return Str(value,L"id");}return Str(overlay,L"clickAction");
    }
    auto segments=view.GetNamedArray(L"segments",JsonArray{});float full=0,gap=static_cast<float>(Num(view,L"gap",6.4));for(auto const &v:segments)full+=NativeExtensionSegmentWidth(*this,v.GetObject())+gap;full=std::max(0.f,full-gap);
    float scale=full>width?width/std::max(1.f,full):1,x0=0;for(auto const &v:segments){auto segment=v.GetObject();float w=NativeExtensionSegmentWidth(*this,segment)*scale;
        if((x>=x0&&x<=x0+w)||x<0||x>width){auto action=Str(segment,context?L"contextAction":L"clickAction");if(!action.empty())return action;if(x>=0&&x<=width)return L"";}x0+=w+gap*scale;
    }return L"";
}
inline void NativeExtensionText(NativeBar &bar,NativePaint &paint,std::wstring const &text,D2D1_RECT_F bounds,float size,int weight,DWRITE_TEXT_ALIGNMENT alignment=DWRITE_TEXT_ALIGNMENT_LEADING){
    if(bounds.right-bounds.left<1||bounds.bottom<=bounds.top||text.empty())return;
    auto format=bar.app.graphics.Format(size,false,weight);com_ptr<IDWriteTextLayout> layout;
    check_hresult(bar.app.graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,bounds.right-bounds.left,bounds.bottom-bounds.top,layout.put()));
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);layout->SetTextAlignment(alignment);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> ellipsis;
    check_hresult(bar.app.graphics.text->CreateEllipsisTrimmingSign(format,ellipsis.put()));layout->SetTrimming(&trim,ellipsis.get());
    paint.c->DrawTextLayout({bounds.left,bounds.top},layout.get(),paint.Color(paint.theme.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
inline void NativeBar::PaintExtensionItem(ID2D1DeviceContext *c,int index){
    NativePaint p(c,app.graphics,ToolbarItemDark(index));auto item=entries[index];float width=std::max(0.f,rows[index].rect.right-rows[index].rect.left-padding),height=rows[index].rect.bottom-rows[index].rect.top;
    auto view=NativeExtensionView(item);float size=iconSize*.8f;c->PushAxisAlignedClip({0,0,width,height},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    try{
        if(auto overlay=NativeExtensionOverlay(view)){
            auto source=Str(overlay,L"source"),text=Str(overlay,L"text");int count=static_cast<int>(Num(overlay,L"count",1));if(count>1)text+=L" (+"+std::to_wstring(count-1)+L")";
            auto trailing=NativeExtensionTrailing(*this,overlay,width,height);float end=trailing.right>trailing.left?trailing.left-6:width;float sourceWidth=std::min(ExtensionTextWidth(source,true)+8,std::max(0.f,end)*.4f);
            NativeExtensionText(*this,p,source,{0,0,sourceWidth,height},size,600);
            NativeExtensionText(*this,p,text,{sourceWidth,0,std::max(sourceWidth,end),height},size,400);
            if(trailing.right>trailing.left){p.Fill(trailing,p.theme.SectionFilm(),6);NativeExtensionText(*this,p,Str(overlay.GetNamedObject(L"trailingAction"),L"label"),trailing,size,500,DWRITE_TEXT_ALIGNMENT_CENTER);}
        }else{
            auto segments=view.GetNamedArray(L"segments",JsonArray{});float full=0,gap=static_cast<float>(Num(view,L"gap",6.4));for(auto const &v:segments)full+=NativeExtensionSegmentWidth(*this,v.GetObject())+gap;full=std::max(0.f,full-gap);float fit=full>width?width/std::max(1.f,full):1,x=0;
            for(auto const &v:segments){auto segment=v.GetObject();float segmentWidth=NativeExtensionSegmentWidth(*this,segment)*fit,end=x+segmentWidth;auto dot=segment.TryLookup(L"indicator");
                if(dot&&dot.ValueType()==JsonValueType::Object){auto indicator=dot.GetObject();float d=static_cast<float>(Num(indicator,L"size",6));auto semantic=Str(indicator,L"semantic");auto color=semantic==L"positive"?D2D1::ColorF(dark?0x68d391:0x239449):semantic==L"busy"?D2D1::ColorF(dark?0xf6c86b:0xa56b00):semantic==L"attention"?p.theme.Accent():p.theme.MaterialMuted();c->FillEllipse({{x+d/2,height/2},d/2,d/2},p.Color(color));x+=d+4;}
                for(auto const &value:segment.GetNamedArray(L"runs",JsonArray{})){auto run=value.GetObject();auto text=Str(run,L"text");float runWidth=ExtensionTextWidth(text,Bool(run,L"bold"));NativeExtensionText(*this,p,text,{std::min(x,end),0,std::max(std::min(x+runWidth,end),std::min(x,end)),height},size,Bool(run,L"bold")?600:400);x+=runWidth;}
                x=end+gap*fit;
            }
        }
    }catch(hresult_error const &error){JsonObject detail;Put(detail,L"extension",Str(item,L"nativeExtension"));Put(detail,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));app.Telemetry(L"extension-render-failed",detail);}
    c->PopAxisAlignedClip();
}
inline void NativeBar::SyncExtensionMotion(NativeToolbarVisual &visual,JsonObject const &item){
    auto overlay=NativeExtensionOverlay(NativeExtensionView(item));bool blink=overlay&&Bool(overlay,L"attention")&&Bool(item,L"extensionGranted")&&NativeMotion(app.model)&&visible&&!edge&&!hidingToolbar&&!toolbarOccluded;
    auto key=blink?std::to_wstring(static_cast<int>(Num(NativeExtensionView(item),L"blinkPeriodMs",700))):L"";
    if(key==visual.extensionMotionKey)return;visual.extensionMotionKey=key;visual.visual.StopAnimation(L"Opacity");visual.visual.Opacity(1);
    if(blink){auto animation=app.graphics.compositor.CreateScalarKeyFrameAnimation();animation.InsertKeyFrame(0,1);animation.InsertKeyFrame(.5f,.45f);animation.InsertKeyFrame(1,1);animation.Duration(std::chrono::milliseconds(std::clamp(_wtoi(key.c_str()),350,2000)));animation.IterationBehavior(winrt::Windows::UI::Composition::AnimationIterationBehavior::Forever);visual.visual.StartAnimation(L"Opacity",animation);}
}
inline void NativeBar::ReportExtensions(bool remove){
    if(dock||!hwnd||!data)return;
    for(auto section:{L"left",L"center",L"right"})for(auto const &value:data.GetNamedObject(L"toolbar").GetNamedArray(section)){
        auto item=value.GetObject();if(Str(item,L"type")!=L"Extension")continue;auto id=Str(item,L"id"),session=Str(item,L"extensionSession");if(session.empty())continue;
        auto at=std::find_if(rows.begin(),rows.end(),[&](auto const &r){return r.id==id;});
        bool prepared=extensionPreparedSessions.contains(id)&&extensionPreparedSessions[id]==session;
        bool mounted=!remove&&prepared&&(!extensionWidths.contains(id)||extensionWidths[id]>0);
        bool shown=mounted&&visible&&!edge&&!hidingToolbar&&!toolbarOccluded&&!gridSuppressed&&IsWindowVisible(hwnd)&&at!=rows.end()&&!at->offscreen;
        auto state=session+L":"+std::to_wstring(mounted)+L":"+std::to_wstring(shown);if(extensionReports[id]==state)continue;extensionReports[id]=state;
        if(auto v=toolbarVisuals.find(id);v!=toolbarVisuals.end()&&!shown){v->second.visual.StopAnimation(L"Opacity");v->second.visual.Opacity(1);v->second.extensionMotionKey.clear();}
        JsonObject command;Put(command,L"id",id);Put(command,L"verb",std::wstring(L"extension-render-state"));Put(command,L"extensionSession",session);Put(command,L"extensionRevision",Str(item,L"extensionRevision"));command.Insert(L"mounted",JsonValue::CreateBooleanValue(mounted));command.Insert(L"visible",JsonValue::CreateBooleanValue(shown));
        JsonObject request;Put(request,L"op",std::wstring(L"shell-command"));Put(request,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));request.Insert(L"command",command);app.Request(request);
    }
    if(remove){extensionPreparedSessions.clear();extensionReports.clear();}
}
