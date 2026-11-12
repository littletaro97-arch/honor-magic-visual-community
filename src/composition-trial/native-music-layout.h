#pragma once
// One retained toolbar surface. Lyrics never change the component width.
namespace NativeMusicLayout {
inline constexpr float ContentWidth=288.f, ToggleWidth=32.f;
inline D2D1_RECT_F Button(int index,float width,float height){
    float x=index==1?ToggleWidth/2:ToggleWidth+(width-ToggleWidth)*(index==0?.3f:.7f);
    return {x-14,0,x+14,height};
}
inline bool Enabled(JsonObject const &item,int index){
    return Bool(item,L"enabled",true)&&Bool(item,L"hasPlayer")&&Bool(item,index==0?L"canPrevious":index==2?L"canNext":Bool(item,L"playing")?L"canPause":L"canPlay");
}
inline int Hit(JsonObject const &item,float width,float height,float x,float y){
    for(int i:{1})if(Enabled(item,i)){
        auto r=Button(i,width,height);if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)return i;
    }return -1;
}
inline void Paint(NativePaint &p,JsonObject const &item,float width,float height,float iconSize,int hovered=-1,int pressed=-1){
    for(int i:{1}){
        auto r=Button(i,width,height);bool enabled=Enabled(item,i);auto ink=enabled?p.theme.Ink():p.theme.Muted();
        if(enabled&&(hovered==i||pressed==i))p.Fill({r.left,2,r.right,height-2},p.theme.Raised(),5);
        float size=std::min(iconSize,height-6),cx=(r.left+r.right)/2,cy=height/2;
        p.Icon(i==0?L"previous":i==2?L"next":Bool(item,L"playing")?L"pause":L"play",{cx-size/2,cy-size/2,cx+size/2,cy+size/2},ink);
    }
    auto text=Str(item,L"lyricsText");if(text.empty())text=Str(item,L"label");if(text.empty())text=L"暂无播放源";float left=ToggleWidth+2,space=std::max(1.f,width-left-4);
    auto format=p.graphics.Format(iconSize*.8f,true,500);com_ptr<IDWriteTextLayout> layout;
    check_hresult(p.graphics.text->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,space,height,layout.put()));
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> sign;check_hresult(p.graphics.text->CreateEllipsisTrimmingSign(format,sign.put()));layout->SetTrimming(&trim,sign.get());
    p.c->DrawTextLayout({left,0},layout.get(),p.Color(p.theme.Ink()),D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
}
