#pragma once
// Visual-only desktop menu motion. Rows, callbacks and Shell queries stay in NativeMenus.
namespace NativeContextMotion {
inline constexpr int RootMs=380, SubMs=300, CloseMs=110;
struct Placement {int x=0,y=0;float2 origin{};};
struct ChildSide {int x=0;bool flipped=false;};
inline RECT ExcludeBar(RECT work,std::wstring const &side,int boundary,float scale){
    // The Dock can be overlay/auto-hidden, so rcWork alone does not reserve it.
    int gap=static_cast<int>(std::ceil(6*scale));
    if(side==L"Bottom")work.bottom=std::clamp<LONG>(boundary-gap,work.top+1,work.bottom);
    else if(side==L"Top")work.top=std::clamp<LONG>(boundary+gap,work.top,work.bottom-1);
    else if(side==L"Left")work.left=std::clamp<LONG>(boundary+gap,work.left,work.right-1);
    else if(side==L"Right")work.right=std::clamp<LONG>(boundary-gap,work.left+1,work.right);
    return work;
}
inline ChildSide ChildX(int parentLeft,int parentRight,int width,RECT work,float scale){
    int gap=static_cast<int>(std::ceil(2*scale)),pad=static_cast<int>(std::ceil(6*scale));
    int right=parentRight+gap,left=parentLeft-width-gap;
    bool flip=right+width>work.right-pad&&(left>=work.left+pad||parentLeft-work.left>work.right-parentRight);
    return {std::clamp(flip?left:right,static_cast<int>(work.left)+pad,std::max(static_cast<int>(work.left)+pad,static_cast<int>(work.right)-pad-width)),flip};
}
inline Placement Place(POINT anchor,RECT work,int width,int height,float scale,bool child,bool flipped,float rowAnchor){
    int pad=static_cast<int>(std::ceil(6*scale));
    int left=work.left+pad,top=work.top+pad,right=std::max(left,static_cast<int>(work.right)-pad-width),bottom=std::max(top,static_cast<int>(work.bottom)-pad-height);
    int x=anchor.x,y=anchor.y;
    if(!child){if(x+width>work.right-pad)x-=width;if(y+height>work.bottom-pad)y-=height;}
    x=std::clamp(x,left,right);y=std::clamp(y,top,bottom);
    float w=width/scale,h=height/scale;
    return {x,y,{child?(flipped?w-4:4):std::clamp((anchor.x-x)/scale,4.f,std::max(4.f,w-4)),child?std::clamp((rowAnchor-y)/scale,4.f,std::max(4.f,h-4)):std::clamp((anchor.y-y)/scale,4.f,std::max(4.f,h-4))}};
}
inline bool Contains(float2 point,float2 size,float radius){
    if(point.x<4||point.y<4||point.x>size.x-4||point.y>size.y-4)return false;
    radius=std::max(0.f,std::min({radius,(size.x-8)/2,(size.y-8)/2}));float x=point.x-std::clamp(point.x,4+radius,size.x-4-radius),y=point.y-std::clamp(point.y,4+radius,size.y-4-radius);return x*x+y*y<=radius*radius;
}
inline float Gutter(float side){return std::ceil(side*.03f+2);}
inline void Enter(Compositor const &c,ContainerVisual const &root,bool child){
    float start=child?.9f:.78f;auto a=NativeInteractionMotion::DockSpringVector(c,{start,start,1},{1,1,1});a.Duration(std::chrono::milliseconds(child?SubMs:RootMs));root.StartAnimation(L"Scale",a);
    NativeInteractionMotion::Opacity(c,root,1,120);
}
// Same spring with rebound fitted to the existing 10-DIP popup inset.
inline void EnterPanel(Compositor const &c,ContainerVisual const &root,float extent){
    auto a=c.CreateVector3KeyFrameAnimation();float gain=std::min(1.f,(8/std::max(1.f,extent))/(.22f*.1192f));
    for(int i=0;i<=40;i++){float p=i/40.f,k=NativeInteractionMotion::DockSpring(p);if(k>1)k=1+(k-1)*gain;float s=.78f+.22f*k;a.InsertKeyFrame(p,{s,s,1},c.CreateLinearEasingFunction());}a.Duration(std::chrono::milliseconds(RootMs));root.StartAnimation(L"Scale",a);NativeInteractionMotion::Opacity(c,root,1,120);
}
inline void Exit(Compositor const &c,ContainerVisual const &root,bool resetOffset=true){
    auto ease=c.CreateCubicBezierEasingFunction({.4f,0},{.8f,.3f});
    auto scale=c.CreateVector3KeyFrameAnimation();scale.InsertExpressionKeyFrame(0,L"this.StartingValue");scale.InsertKeyFrame(1,{.94f,.94f,1},ease);scale.Duration(std::chrono::milliseconds(CloseMs));root.StartAnimation(L"Scale",scale);
    if(resetOffset){auto offset=c.CreateVector3KeyFrameAnimation();offset.InsertExpressionKeyFrame(0,L"this.StartingValue");offset.InsertKeyFrame(1,{0,0,0},ease);offset.Duration(std::chrono::milliseconds(CloseMs));root.StartAnimation(L"Offset",offset);}
    auto opacity=c.CreateScalarKeyFrameAnimation();opacity.InsertExpressionKeyFrame(0,L"this.StartingValue");opacity.InsertKeyFrame(1,0,ease);opacity.Duration(std::chrono::milliseconds(CloseMs));root.StartAnimation(L"Opacity",opacity);
}
}
