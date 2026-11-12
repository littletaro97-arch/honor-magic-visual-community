#pragma once
#include "native-card-text.h"
inline bool NativeMediaSession(JsonObject const &data){return data&&Bool(data,L"available")&&Bool(data,L"hasSession")&&!Str(data,L"id").empty();}
inline bool NativeMediaPlaying(JsonObject const &data){return NativeMediaSession(data)&&Num(data,L"status")==4;}
inline double NativeMediaPosition(JsonObject const &data){
    if(!NativeMediaSession(data))return 0;double value=Num(data,L"positionMs");if(NativeMediaPlaying(data))value+=std::max(0.,static_cast<double>(GetTickCount64())-Num(data,L"observedTick",static_cast<double>(GetTickCount64())))*std::clamp(Num(data,L"playbackRate",1),.25,4.);
    return std::clamp(value,0.,std::max(0.,Num(data,L"durationMs")));
}
inline std::wstring NativeMediaTime(double milliseconds){int seconds=static_cast<int>(std::clamp(milliseconds/1000,0.,604800.));return std::to_wstring(seconds/60)+L":"+(seconds%60<10?L"0":L"")+std::to_wstring(seconds%60);}
inline std::wstring NativeMediaDisplayLine(JsonObject const &data){auto lyric=NativeMediaPlaying(data)?Str(data,L"lyricsText"):L"";return lyric.empty()?Str(data,L"title"):lyric;}
inline D2D1_RECT_F NativeMediaTitleRect(float w,float h){return h<100?D2D1::RectF(36,9,w-12,30):D2D1::RectF(14,38,w-14,64);}
inline D2D1_RECT_F NativeMediaSeekRect(float w,float h){return h<100?D2D1_RECT_F{}:D2D1::RectF(14,94,w-14,120);}
inline int NativeMediaActionAt(float x,float y,float w,float h){
    if(h<100){if(y>=47&&y<=82){int index=static_cast<int>((x-24)/46);return x>=24&&x<=162?std::clamp(index,0,2)+1:0;}return 0;}
    if(y>=138&&y<=176){float center=w/2;return x>=center-70&&x<=center+70?x<center-24?1:x>center+24?3:2:0;}return 0;
}
inline bool NativeMediaSeekSupported(Tile const &tile,JsonObject const &data){return NativeMediaSession(data)&&Bool(data,L"canSeek")&&Num(tile.card,L"rows",2)>1&&Bool(tile.card.GetNamedObject(L"settings",JsonObject{}),L"allowSeek",true)&&Num(data,L"seekMaxMs")>Num(data,L"seekMinMs");}
inline void PaintNativeMedia(Window &window,NativePaint &p,Tile const &tile,float w,float h){
    auto data=window.app.model.mediaData;bool ready=NativeMediaSession(data),compact=h<100,playing=NativeMediaPlaying(data);auto ink=p.theme.Ink(),muted=p.theme.MaterialMuted();
    p.Icon(L"play",{14,compact?13.f:15.f,compact?28.f:26.f,compact?27.f:27.f},muted);
    if(!compact)NativeCardText(p,ready?Str(data,L"appId"):L"音乐播放器",{33,10,w-14,32},9.5f);
    auto title=NativeMediaTitleRect(w,h);if(!window.mediaTitles.contains(tile.key))NativeCardText(p,ready?NativeMediaDisplayLine(data):!data?L"正在读取媒体…":Bool(data,L"available")?compact?L"暂无播放":L"暂无正在播放的媒体":L"媒体状态暂不可用",title,compact?12.f:14.f,600);
    if(!compact){NativeCardText(p,ready?Str(data,L"artist"):L"",{14,66,w-14,86},11);auto track=NativeMediaSeekRect(w,h);double duration=ready?Num(data,L"durationMs"):0,position=window.mediaSeekKey==tile.key?window.mediaSeekValue:NativeMediaPosition(data);float y=(track.top+track.bottom)/2;
        p.Fill({track.left,y-2,track.right,y+2},p.theme.SectionFilm(),2);if(duration>0){float end=track.left+static_cast<float>(std::clamp(position/duration,0.,1.))*(track.right-track.left);p.Fill({track.left,y-2,end,y+2},p.theme.Accent(),2);if(NativeMediaSeekSupported(tile,data))p.c->FillEllipse({{end,y},3.5f,3.5f},p.Color(p.theme.Accent()));}
        p.Text(ready?NativeMediaTime(position):L"—",{14,116,w/2,134},8.5f,muted);p.Text(ready?NativeMediaTime(duration):L"—",{w/2,116,w-14,134},8.5f,muted,400,DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    float centerY=compact?64.f:156.f,centerX=w/2,gap=compact?46.f:48.f;if(data&&!Bool(data,L"available")){p.Icon(L"refresh",{centerX-10,centerY-10,centerX+10,centerY+10},ink);return;}for(int i=0;i<3;i++){bool enabled=ready&&(i==0?Bool(data,L"canPrevious"):i==2?Bool(data,L"canNext"):Bool(data,L"canPlay")||Bool(data,L"canPause"));auto box=D2D1::RectF(centerX+(i-1)*gap-15,centerY-15,centerX+(i-1)*gap+15,centerY+15);
        if(i==1)p.Fill(box,p.theme.SectionFilm(),15);p.Icon(i==0?L"previous":i==2?L"next":playing?L"pause":L"play",{box.left+7,box.top+7,box.right-7,box.bottom-7},enabled?ink:muted);
        if(window.focusKey==tile.key&&window.mediaControlFocus==i+2)p.c->DrawRoundedRectangle(D2D1::RoundedRect(box,15,15),p.Color(p.theme.Accent()),1);
    }
}
inline void Window::MediaSend(std::wstring const &key,std::wstring const &kind,double value,std::wstring const &session,std::wstring const &track){
    auto tile=Find(key);if(!tile||!tile->card||Str(tile->card,L"type")!=L"system.media")return;auto data=app.model.mediaData;bool retry=!data||!Bool(data,L"available");if(!retry&&!NativeMediaSession(data))return;JsonObject request;Put(request,L"op",std::wstring(L"card-action"));Put(request,L"board",boardId);Put(request,L"cardId",Str(tile->card,L"id"));Put(request,L"kind",retry?std::wstring(L"media-retry"):kind);Put(request,L"sessionId",session.empty()?(data?Str(data,L"id"):L""):session);Put(request,L"trackId",track.empty()?(data?Str(data,L"trackId"):L""):track);Put(request,L"positionMs",value);if(kind==L"media-open")AllowSetForegroundWindow(app.parent);app.Request(request);
}
inline bool Window::MediaPointer(UINT message,float x,float y){
    if(message==WM_CAPTURECHANGED){bool had=!mediaSeekKey.empty();mediaSeekKey.clear();if(had)DrawCards(true,L"system.media");return had;}
    if(!mediaSeekKey.empty()){
        auto tile=Find(mediaSeekKey);if(!tile||!tile->card){mediaSeekKey.clear();return true;}auto rect=RootBounds(tile->key);float w=static_cast<float>(Num(tile->card,L"columns",2))*96-8,h=static_cast<float>(Num(tile->card,L"rows",2))*96-8,scale=(rect.right-rect.left)/w;auto track=NativeMediaSeekRect(w,h);auto data=app.model.mediaData;
        double ratio=std::clamp(((x-rect.left)/scale-track.left)/std::max(1.f,track.right-track.left),0.f,1.f);mediaSeekValue=std::clamp(ratio*std::max(0.,Num(data,L"durationMs")),std::max(0.,Num(data,L"seekMinMs")),std::max(0.,Num(data,L"seekMaxMs")));
        if(message==WM_LBUTTONUP){auto key=mediaSeekKey,session=mediaSeekSession,identity=mediaSeekTrack;mediaSeekKey.clear();ReleaseCapture();MediaSend(key,L"media-seek",mediaSeekValue,session,identity);}DrawCards(true,L"system.media");return true;
    }
    if(message!=WM_LBUTTONDOWN)return false;auto tile=Find(Hit(x,y));if(!tile||!tile->card||Str(tile->card,L"type")!=L"system.media"||!NativeMediaSeekSupported(*tile,app.model.mediaData))return false;
    auto rect=RootBounds(tile->key);float w=static_cast<float>(Num(tile->card,L"columns",2))*96-8,h=static_cast<float>(Num(tile->card,L"rows",2))*96-8,scale=(rect.right-rect.left)/w;auto track=NativeMediaSeekRect(w,h);float ty=(y-rect.top)/scale,tx=(x-rect.left)/scale;
    if(tx<track.left||tx>track.right||ty<track.top||ty>track.bottom)return false;Select(tile->key);mediaSeekKey=tile->key;mediaSeekSession=Str(app.model.mediaData,L"id");mediaSeekTrack=Str(app.model.mediaData,L"trackId");SetCapture(hwnd);MediaPointer(WM_MOUSEMOVE,x,y);return true;
}
inline bool Window::MediaKey(UINT key){
    if(GetKeyState(VK_CONTROL)&0x8000)return false;
    auto tile=Find(focusKey);if(!tile||!tile->card||Str(tile->card,L"type")!=L"system.media")return false;auto data=app.model.mediaData;
    if(key==VK_TAB){mediaControlFocus=(mediaControlFocus+((GetKeyState(VK_SHIFT)&0x8000)?4:1))%5;if(mediaControlFocus==1&&!NativeMediaSeekSupported(*tile,data))mediaControlFocus=2;DrawCards(true,L"system.media");return true;}
    if(mediaControlFocus==1&&(key==VK_LEFT||key==VK_RIGHT||key==VK_PRIOR||key==VK_NEXT||key==VK_HOME||key==VK_END)){
        double value=NativeMediaPosition(data)+(key==VK_LEFT?-5000:key==VK_RIGHT?5000:key==VK_PRIOR?30000:-30000);if(key==VK_HOME)value=Num(data,L"seekMinMs");if(key==VK_END)value=Num(data,L"seekMaxMs");value=std::clamp(value,std::max(0.,Num(data,L"seekMinMs")),std::max(0.,Num(data,L"seekMaxMs")));MediaSend(tile->key,L"media-seek",value);return true;}
    if(key==VK_RETURN||key==VK_SPACE){MediaSend(tile->key,mediaControlFocus==2?L"media-previous":mediaControlFocus==3?L"media-toggle":mediaControlFocus==4?L"media-next":L"media-open");return true;}return false;
}
inline void Window::ClearMediaTitles(){
    for(auto &[_,v]:mediaTitles){if(v.visual)v.visual.StopAnimation(L"Offset");if(v.clip)if(auto parent=v.clip.Parent())parent.Children().Remove(v.clip);}mediaTitles.clear();
}
inline void Window::SyncMediaTitles(){
    if(!pageLayer||GridEditing()||sessionPaused||!NativeMotion(app.model)||!NativeMediaPlaying(app.model.mediaData)){ClearMediaTitles();return;}auto board=app.model.Find(boardId);if(!board){ClearMediaTitles();return;}
    std::set<std::wstring> live;size_t used=0;auto title=NativeMediaDisplayLine(app.model.mediaData);if(title.size()>512)title.resize(512);
    for(auto const &tile:board->tiles){if(!tile.card||Str(tile.card,L"type")!=L"system.media"||!Bool(tile.card.GetNamedObject(L"settings",JsonObject{}),L"marqueeTitle",true)||live.size()>=8)continue;
        auto rect=RootBounds(tile.key);float w=static_cast<float>(Num(tile.card,L"columns",2))*96-8,h=static_cast<float>(Num(tile.card,L"rows",2))*96-8;auto box=NativeMediaTitleRect(w,h);float ratio=(rect.right-rect.left)/w*dpi/96,font=h<100?12.f:14.f,viewport=box.right-box.left;
        com_ptr<IDWriteTextLayout> measure;check_hresult(app.graphics.text->CreateTextLayout(title.c_str(),static_cast<UINT32>(title.size()),app.graphics.Format(font,false,600),65536,32,measure.put()));DWRITE_TEXT_METRICS metric{};measure->GetMetrics(&metric);float textWidth=std::min(metric.widthIncludingTrailingWhitespace+3,1024.f);if(textWidth<=viewport)continue;
        float period=textWidth+28,renderWidth=period*2,raster=std::min(ratio,std::sqrt(262144.f/std::max(1.f,renderWidth*26*4))),pw=std::ceil(renderWidth*raster),ph=std::ceil(26*raster);size_t bytes=static_cast<size_t>(pw)*static_cast<size_t>(ph)*4;if(used+bytes>2*1024*1024)continue;used+=bytes;live.insert(tile.key);
        auto key=title+L":"+std::to_wstring(viewport)+L":"+std::to_wstring(ratio)+L":"+std::to_wstring(Preview70Theme::systemDark);auto &v=mediaTitles[tile.key];
        if(!v.clip){v.clip=app.graphics.compositor.CreateContainerVisual();v.visual=app.graphics.compositor.CreateSpriteVisual();v.clip.Children().InsertAtTop(v.visual);pageLayer.Children().InsertAtTop(v.clip);}
        v.clip.Offset({rect.left*dpi/96+box.left*ratio,rect.top*dpi/96+box.top*ratio,0});v.clip.Size({viewport*ratio,(box.bottom-box.top)*ratio});v.clip.Clip(app.graphics.compositor.CreateInsetClip());
        if(v.key==key)continue;v.visual.StopAnimation(L"Offset");v.pixels=app.graphics.composition.CreateDrawingSurface({pw,ph},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);auto interop=v.pixels.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT origin{};check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&origin));
        try{c->SetDpi(96*raster,96*raster);c->SetTransform(D2D1::Matrix3x2F::Translation(origin.x/raster,origin.y/raster));c->Clear(D2D1::ColorF(0,0));NativePaint p(c.get(),app.graphics,Preview70Theme::systemDark);p.Text(title,{0,0,textWidth,26},font,p.theme.Ink(),600);p.Text(title,{period,0,period+textWidth,26},font,p.theme.Ink(),600);check_hresult(interop->EndDraw());}catch(...){interop->EndDraw();throw;}
        v.visual.Brush(app.graphics.compositor.CreateSurfaceBrush(v.pixels));v.visual.Size({renderWidth*ratio,26*ratio});v.visual.Offset({0,0,0});auto motion=app.graphics.compositor.CreateVector3KeyFrameAnimation();motion.InsertKeyFrame(0,{0,0,0});motion.InsertKeyFrame(1,{-period*ratio,0,0},app.graphics.compositor.CreateLinearEasingFunction());motion.Duration(std::chrono::milliseconds(static_cast<int>(period/16*1000)));motion.IterationBehavior(AnimationIterationBehavior::Forever);v.visual.StartAnimation(L"Offset",motion);v.key=key;v.bytes=bytes;
    }
    for(auto it=mediaTitles.begin();it!=mediaTitles.end();)if(!live.contains(it->first)){if(it->second.visual)it->second.visual.StopAnimation(L"Offset");if(auto parent=it->second.clip.Parent())parent.Children().Remove(it->second.clip);it=mediaTitles.erase(it);}else ++it;
}
