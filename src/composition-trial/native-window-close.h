#pragma once
#include <dwmapi.h>

// Observe explicit caption-X clicks without swallowing or replaying input.
// Prepare one bounded frame near X; suppress this HWND's DWM transition before
// releasing the click, then animate only after a real hide/destroy notification.
namespace NativeWindowClose {
inline constexpr unsigned DurationMs=400,ConfirmMs=400,ClickMs=1000;
inline constexpr unsigned FrameAgeMs=1200,HoverMessageMs=32;
inline constexpr size_t FrameLimit=8*1024*1024;
inline float Clamp(float p){return std::clamp(p,0.f,1.f);}
inline float Height(float p){return std::max(.0025f,std::pow(1-Clamp(p),6.f));}
inline float Content(float p){return Clamp(1-(p-.14f)/.38f);}
inline float Opacity(float p){return p<=.5f?1:Clamp(2-2*p);}
inline float Line(float p){return Clamp((p-.58f)/.2f)*Clamp((1-p)/.14f)*Opacity(p);}
inline bool ReleaseMatches(RECT const &button,POINT down,POINT up,DWORD elapsed){
    return elapsed<=ClickMs&&PtInRect(&button,down)&&PtInRect(&button,up);
}
inline bool Recent(DWORD from,DWORD to,DWORD limit){return static_cast<DWORD>(to-from)<=limit;}
struct Frame {
    HWND source=nullptr;DWORD pid=0;RECT rect{},windowRect{},button{};POINT down{};
    bool accessibleButton=false;
    UINT width=0,height=0;float radius=0;std::vector<BYTE> pixels;
};
inline bool SameWindow(HWND source,DWORD pid){DWORD current=0;return IsWindow(source)&&GetWindowThreadProcessId(source,&current)&&current==pid;}
inline bool Alive(Frame const &f){DWORD pid=0;GetWindowThreadProcessId(f.source,&pid);return pid==f.pid&&IsWindowVisible(f.source)&&!IsIconic(f.source);}
inline bool CaptionArea(RECT const &r,POINT point,float dpi){
    return PtInRect(&r,point)&&point.x>=r.right-static_cast<LONG>(100*dpi)&&point.y<=r.top+static_cast<LONG>(64*dpi);
}
inline bool CloseIdentity(std::wstring const &id,std::wstring const &name){
    return id==L"Close"||id==L"CloseButton"||id==L"CloseWindow"||id==L"WindowCloseButton"||id==L"PART_CloseButton"||
        name==L"Close"||name==L"关闭"||name==L"關閉"||name==L"关闭窗口";
}
// Called only from the short-lived MTA preparation thread. No Invoke(), UIA
// subscriptions, input injection or position-only guessing of client buttons.
inline bool AccessibleClose(Frame &f){
    com_ptr<IUIAutomation2> automation;
    if(FAILED(CoCreateInstance(__uuidof(CUIAutomation8),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IUIAutomation2),automation.put_void())))return false;
    automation->put_ConnectionTimeout(40);automation->put_TransactionTimeout(40);automation->put_AutoSetFocus(FALSE);
    com_ptr<IUIAutomationCacheRequest> cache;if(FAILED(automation->CreateCacheRequest(cache.put())))return false;
    for(auto p:{UIA_ControlTypePropertyId,UIA_AutomationIdPropertyId,UIA_NamePropertyId,UIA_BoundingRectanglePropertyId,UIA_ProcessIdPropertyId,UIA_IsEnabledPropertyId,UIA_IsOffscreenPropertyId})cache->AddProperty(p);
    com_ptr<IUIAutomationElement> element;if(FAILED(automation->ElementFromPointBuildCache(f.down,cache.get(),element.put()))||!element)return false;
    CONTROLTYPEID type=0;int pid=0;BOOL enabled=FALSE,offscreen=TRUE;RECT button{};BSTR id=nullptr,name=nullptr;
    element->get_CachedControlType(&type);element->get_CachedProcessId(&pid);element->get_CachedIsEnabled(&enabled);element->get_CachedIsOffscreen(&offscreen);element->get_CachedBoundingRectangle(&button);
    element->get_CachedAutomationId(&id);element->get_CachedName(&name);bool named=CloseIdentity(id?id:L"",name?name:L"");SysFreeString(id);SysFreeString(name);
    float dpi=GetDpiForWindow(f.source)/96.f;
    if(type!=UIA_ButtonControlTypeId||pid!=static_cast<int>(f.pid)||!enabled||offscreen||!named||!PtInRect(&button,f.down)||
        button.left<f.rect.right-100*dpi||button.right>f.windowRect.right||button.top<f.windowRect.top||button.bottom>f.rect.top+64*dpi)return false;
    // Confirm the accessible button belongs to this top-level window (a browser
    // tab's close button or another popup must never authorize the animation).
    com_ptr<IUIAutomationTreeWalker> walker;if(FAILED(automation->get_RawViewWalker(walker.put())))return false;
    auto until=GetTickCount64()+120;
    for(int i=0;i<8&&element&&GetTickCount64()<=until;i++){
        UIA_HWND handle=nullptr;element->get_CurrentNativeWindowHandle(&handle);
        if(handle&&reinterpret_cast<HWND>(handle)==f.source){f.button=button;f.accessibleButton=true;return true;}
        com_ptr<IUIAutomationElement> parent;if(FAILED(walker->GetParentElement(element.get(),parent.put())))break;element=std::move(parent);
    }
    return false;
}
inline bool Unobscured(HWND source,RECT rect){
    // Never fold pixels belonging to a different window into the close effect.
    struct Query {HWND source;RECT rect;bool clear=true;};Query q{source,rect};
    EnumWindows([](HWND h,LPARAM v)->BOOL{auto &q=*reinterpret_cast<Query*>(v);if(h==q.source)return FALSE;
        if(!IsWindowVisible(h)||IsIconic(h))return TRUE;BOOL cloaked=FALSE;DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));if(cloaked)return TRUE;
        RECT r{},intersection{};if(FAILED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r))))GetWindowRect(h,&r);
        if(IntersectRect(&intersection,&r,&q.rect)&&intersection.right-intersection.left>1&&intersection.bottom-intersection.top>1){q.clear=false;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&q));return q.clear;
}
inline bool Probe(Frame &f){
    if(!f.source||GetAncestor(f.source,GA_ROOT)!=f.source||!IsWindowEnabled(f.source)||!IsWindowVisible(f.source)||IsIconic(f.source))return false;
    GetWindowThreadProcessId(f.source,&f.pid);if(!f.pid||f.pid==GetCurrentProcessId())return false;
    auto style=GetWindowLongPtrW(f.source,GWL_STYLE),ex=GetWindowLongPtrW(f.source,GWL_EXSTYLE);
    if(ex&(WS_EX_NOACTIVATE|WS_EX_LAYOUTRTL))return false;
    DWORD affinity=0;if(GetWindowDisplayAffinity(f.source,&affinity)&&affinity!=WDA_NONE)return false;
    BOOL cloaked=FALSE;if(SUCCEEDED(DwmGetWindowAttribute(f.source,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return false;
    if(FAILED(DwmGetWindowAttribute(f.source,DWMWA_EXTENDED_FRAME_BOUNDS,&f.rect,sizeof(f.rect)))&&!GetWindowRect(f.source,&f.rect))return false;
    if(!GetWindowRect(f.source,&f.windowRect))return false;
    float dpi=GetDpiForWindow(f.source)/96.f;
    if(!PtInRect(&f.rect,f.down)||f.down.x<(f.rect.left+f.rect.right)/2||f.down.y>f.rect.top+64*dpi)return false;
    DWORD_PTR hit=0;if(!SendMessageTimeoutW(f.source,WM_NCHITTEST,0,MAKELPARAM(f.down.x,f.down.y),SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,25,&hit))return false;
    if(hit==HTCLOSE&&(style&WS_SYSMENU)){
        TITLEBARINFOEX title{};title.cbSize=sizeof(title);DWORD_PTR ignored=0;
        if(SendMessageTimeoutW(f.source,WM_GETTITLEBARINFOEX,0,reinterpret_cast<LPARAM>(&title),SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,25,&ignored)&&PtInRect(&title.rgrect[5],f.down))f.button=title.rgrect[5];
        else if(!AccessibleClose(f)){int tolerance=std::max(2,static_cast<int>(4*dpi));f.button={f.down.x-tolerance,f.down.y-tolerance,f.down.x+tolerance+1,f.down.y+tolerance+1};}
    }else if(hit!=HTCLIENT||!AccessibleClose(f))return false;
    f.radius=IsZoomed(f.source)?0:8*dpi;
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if(f.rect.left<desktop.left||f.rect.top<desktop.top||f.rect.right>desktop.right||f.rect.bottom>desktop.bottom)return false;
    return Alive(f)&&GetForegroundWindow()==f.source&&Unobscured(f.source,f.rect);
}
inline bool Capture(Frame &f){
    int w=f.rect.right-f.rect.left,h=f.rect.bottom-f.rect.top;if(w<=0||h<=0)return false;
    double scale=std::min(1.,std::sqrt(static_cast<double>(FrameLimit)/(static_cast<double>(w)*h*4)));
    f.width=std::max(1u,static_cast<UINT>(w*scale));f.height=std::max(1u,static_cast<UINT>(h*scale));
    if(static_cast<size_t>(f.width)*f.height*4>FrameLimit)return false;
    HDC screen=GetDC(nullptr),memory=screen?CreateCompatibleDC(screen):nullptr;void *pixels=nullptr;
    BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),static_cast<LONG>(f.width),-static_cast<LONG>(f.height),1,32,BI_RGB,0,0,0,0,0};
    HBITMAP bitmap=memory?CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;HGDIOBJ old=bitmap?SelectObject(memory,bitmap):nullptr;bool ok=false;
    if(bitmap&&Alive(f)){
        SetStretchBltMode(memory,HALFTONE);SetBrushOrgEx(memory,0,0,nullptr);
        ok=StretchBlt(memory,0,0,f.width,f.height,screen,f.rect.left,f.rect.top,w,h,SRCCOPY|CAPTUREBLT)!=FALSE;
        GdiFlush();RECT after{};if(FAILED(DwmGetWindowAttribute(f.source,DWMWA_EXTENDED_FRAME_BOUNDS,&after,sizeof(after))))GetWindowRect(f.source,&after);
        ok=ok&&Alive(f)&&EqualRect(&after,&f.rect)&&GetForegroundWindow()==f.source;
        if(ok){auto start=static_cast<BYTE*>(pixels);f.pixels.assign(start,start+static_cast<size_t>(f.width)*f.height*4);for(size_t i=3;i<f.pixels.size();i+=4)f.pixels[i]=255;}
    }
    if(old)SelectObject(memory,old);if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(screen)ReleaseDC(nullptr,screen);return ok;
}
struct Effect {
    HWND hwnd=nullptr,source=nullptr;Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
    ContainerVisual root{nullptr},body{nullptr};SpriteVisual content{nullptr},line{nullptr};CompositionDrawingSurface surface{nullptr};size_t bytes=0;
    ~Effect(){Reset();}
    void Reset(){if(target)target.Root(nullptr);root=nullptr;body=nullptr;content=nullptr;line=nullptr;surface=nullptr;target=nullptr;bytes=0;source=nullptr;if(hwnd){DestroyWindow(hwnd);hwnd=nullptr;}}
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;if(m==WM_ERASEBKGND)return 1;return DefWindowProcW(h,m,w,l);}
    void Start(Graphics &g,Frame const &f){
        Reset();source=f.source;float w=static_cast<float>(f.rect.right-f.rect.left),h=static_cast<float>(f.rect.bottom-f.rect.top);
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"MagicNativeCaptionClose";RegisterClassW(&wc);
        hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"",WS_POPUP,f.rect.left,f.rect.top,static_cast<int>(w),static_cast<int>(h),nullptr,nullptr,wc.hInstance,nullptr);if(!hwnd)throw_last_error();
        BOOL disable=TRUE;DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));
        check_hresult(g.compositor.as<abiComposition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd,FALSE,reinterpret_cast<abiComposition::Desktop::IDesktopWindowTarget**>(put_abi(target))));
        auto c=g.compositor;root=c.CreateContainerVisual();root.Size({w,h});target.Root(root);body=c.CreateContainerVisual();body.Size({w,h});body.CenterPoint({w/2,h/2,0});root.Children().InsertAtTop(body);
        auto geometry=c.CreateRoundedRectangleGeometry();geometry.Size({w,h});geometry.CornerRadius({f.radius,f.radius});body.Clip(c.CreateGeometricClip(geometry));
        auto base=c.CreateSpriteVisual();base.Size({w,h});auto fill=c.CreateLinearGradientBrush();fill.StartPoint({0,0});fill.EndPoint({0,1});fill.ColorStops().Append(c.CreateColorGradientStop(0,{255,255,255,255}));fill.ColorStops().Append(c.CreateColorGradientStop(1,{255,244,245,248}));base.Brush(fill);body.Children().InsertAtBottom(base);
        surface=g.composition.CreateDrawingSurface({static_cast<float>(f.width),static_cast<float>(f.height)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto drawing=surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> dc;POINT offset{};check_hresult(drawing->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),dc.put_void(),&offset));
        try{dc->SetDpi(96,96);dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));dc->Clear(D2D1::ColorF(0,0));auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));com_ptr<ID2D1Bitmap1> bitmap;
            check_hresult(dc->CreateBitmap({f.width,f.height},f.pixels.data(),f.width*4,&props,bitmap.put()));dc->DrawBitmap(bitmap.get());check_hresult(drawing->EndDraw());}catch(...){drawing->EndDraw();throw;}
        bytes=f.pixels.size();content=c.CreateSpriteVisual();content.Size({w,h});auto brush=c.CreateSurfaceBrush(surface);brush.Stretch(CompositionStretch::Fill);content.Brush(brush);body.Children().InsertAtTop(content);
        auto sheen=c.CreateSpriteVisual();sheen.Size({w,h});auto shine=c.CreateLinearGradientBrush();shine.StartPoint({0,0});shine.EndPoint({0,1});
        struct ShineStop{float at;BYTE alpha;};
        for(auto stop:{ShineStop{0.f,153},ShineStop{.16f,0},ShineStop{.84f,0},ShineStop{1.f,77}})shine.ColorStops().Append(c.CreateColorGradientStop(stop.at,{stop.alpha,255,255,255}));sheen.Brush(shine);body.Children().InsertAtTop(sheen);
        line=c.CreateSpriteVisual();line.Size({w,2});line.Offset({0,h/2-1,0});line.CenterPoint({w/2,1,0});auto band=c.CreateLinearGradientBrush();band.StartPoint({0,0});band.EndPoint({1,0});
        band.ColorStops().Append(c.CreateColorGradientStop(0,{0,226,239,255}));band.ColorStops().Append(c.CreateColorGradientStop(.12f,{217,226,239,255}));band.ColorStops().Append(c.CreateColorGradientStop(.5f,{250,255,255,255}));band.ColorStops().Append(c.CreateColorGradientStop(.88f,{217,226,239,255}));band.ColorStops().Append(c.CreateColorGradientStop(1,{0,226,239,255}));line.Brush(band);root.Children().InsertAtTop(line);
        auto glow=c.CreateDropShadow();glow.Color({255,190,220,255});glow.BlurRadius(14);glow.Opacity(.6f);glow.SourcePolicy(CompositionDropShadowSourcePolicy::InheritFromVisualContent);line.Shadow(glow);
        auto linear=c.CreateLinearEasingFunction();auto scale=c.CreateVector3KeyFrameAnimation();auto alpha=c.CreateScalarKeyFrameAnimation(),contentAlpha=c.CreateScalarKeyFrameAnimation(),lineAlpha=c.CreateScalarKeyFrameAnimation(),sheenAlpha=c.CreateScalarKeyFrameAnimation();auto lineScale=c.CreateVector3KeyFrameAnimation();
        // A finite keyframe table follows the approved HTML curve. The compositor
        // owns all frames; the process only wakes once to release the surface.
        for(int i=0;i<=40;i++){float p=i/40.f;scale.InsertKeyFrame(p,{1,Height(p),1},linear);alpha.InsertKeyFrame(p,Opacity(p),linear);contentAlpha.InsertKeyFrame(p,Content(p),linear);sheenAlpha.InsertKeyFrame(p,(1-Content(p))*.85f,linear);lineAlpha.InsertKeyFrame(p,Line(p),linear);lineScale.InsertKeyFrame(p,{.72f+.28f*Clamp((p-.58f)/.24f),1,1},linear);}
        for(auto a:{alpha,contentAlpha,lineAlpha,sheenAlpha})a.Duration(std::chrono::milliseconds(DurationMs));scale.Duration(std::chrono::milliseconds(DurationMs));lineScale.Duration(std::chrono::milliseconds(DurationMs));
        body.StartAnimation(L"Scale",scale);body.StartAnimation(L"Opacity",alpha);content.StartAnimation(L"Opacity",contentAlpha);sheen.StartAnimation(L"Opacity",sheenAlpha);line.StartAnimation(L"Opacity",lineAlpha);line.StartAnimation(L"Scale",lineScale);
        // Top of ordinary windows, underneath topmost shell UI and safety prompts.
        SetWindowPos(hwnd,HWND_TOP,f.rect.left,f.rect.top,static_cast<int>(w),static_cast<int>(h),SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
};
struct Controller {
    static constexpr UINT InputMessage=WM_APP+201,ReadyMessage=WM_APP+202,ConfirmMessage=WM_APP+203,MoveMessage=WM_APP+204;
    struct Job {
        Frame frame;unsigned generation=0;DWORD capturedAt=0,downTime=0,upTime=0,hiddenTime=0;POINT down{},up{};
        bool pressed=false,released=false,hidden=false,valid=false;
        std::atomic<bool> canceled=false,ready=false,suppressed=false;
        HRESULT restoreResult=S_OK;
        std::mutex transitionMutex;
        // The DWM flag is documented as set-only. FALSE returns the surviving
        // window to system-managed transitions; it does not change a global
        // animation preference. Never hold this lease during hover/preparation.
        bool Suppress(){
            std::lock_guard lock(transitionMutex);
            if(canceled||!ready||!valid||!Alive(frame))return false;
            BOOL disable=TRUE;
            if(FAILED(DwmSetWindowAttribute(frame.source,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable))))return false;
            suppressed=true;return true;
        }
        void Restore(){
            std::lock_guard lock(transitionMutex);
            if(suppressed.exchange(false)&&SameWindow(frame.source,frame.pid)){
                BOOL disable=FALSE;restoreResult=DwmSetWindowAttribute(frame.source,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));
            }
        }
        void Cancel(){canceled=true;Restore();}
        ~Job(){Restore();}
    };
    struct Input{POINT point;DWORD time;bool down;std::shared_ptr<Job> release{};};
    App &app;HWND receiver=nullptr;std::thread observer,worker;DWORD observerId=0;HHOOK hook=nullptr;std::atomic<bool> stop=false,workerDone=true;
    static inline thread_local Controller *hookOwner=nullptr;
    std::shared_ptr<Job> pending;std::atomic<std::shared_ptr<Job>> armed;std::atomic<bool> gateEnabled=false;
    // The input observer alone owns these three fields. The fixture drives the
    // same methods without installing the observer; no synthetic global clicks.
    POINT hookDown{};DWORD hookDownTime=0,lastMoveTime=0;HWND hookSource=nullptr;
    unsigned generation=0;bool locked=false;Effect effect;unsigned played=0;HWND lastProbeTarget=nullptr;DWORD lastProbeTime=0;
    std::atomic<unsigned> prepared=0,notReadyAtRelease=0,suppressionFailed=0;
    std::atomic<long long> maxReleaseUs=0;
    explicit Controller(App &a,bool observe=true):app(a){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"MagicCaptionCloseObserver";RegisterClassW(&wc);
        receiver=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,this);if(!receiver)return;WTSRegisterSessionNotification(receiver,NOTIFY_FOR_THIS_SESSION);
        if(!observe)return;HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready)return;
        try{observer=std::thread([this,ready]{MSG message{};PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);observerId=GetCurrentThreadId();hookOwner=this;hook=SetWindowsHookExW(WH_MOUSE_LL,Mouse,GetModuleHandleW(nullptr),0);SetEvent(ready);
            while(!stop&&GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}if(hook)UnhookWindowsHookEx(hook);hook=nullptr;hookOwner=nullptr;
        });WaitForSingleObject(ready,INFINITE);}catch(...){ }CloseHandle(ready);
    }
    ~Controller(){stop=true;if(observerId)PostThreadMessageW(observerId,WM_QUIT,0,0);if(observer.joinable())observer.join();Reset();if(worker.joinable())worker.join();if(receiver){MSG msg{};for(auto id:{InputMessage,MoveMessage})while(PeekMessageW(&msg,receiver,id,id,PM_REMOVE))delete reinterpret_cast<Input*>(msg.lParam);WTSUnRegisterSessionNotification(receiver);DestroyWindow(receiver);}}
    void BeforePress(POINT point,DWORD time){hookDown=point;hookDownTime=time;hookSource=GetAncestor(WindowFromPoint(point),GA_ROOT);}
    std::shared_ptr<Job> BeforeRelease(POINT point,DWORD time){
        // This is the ONLY synchronous work added to a matching release: cheap
        // cached geometry checks and a single per-window DWM attribute update.
        // UIA, hit-test IPC, capture and compositor work never run in the hook.
        auto started=std::chrono::steady_clock::now();auto job=armed.load();HWND source=hookSource;hookSource=nullptr;
        if(!job||!gateEnabled||job->canceled||!job->ready||!job->valid||job->frame.source!=source||
            !Recent(job->capturedAt,time,FrameAgeMs)||!ReleaseMatches(job->frame.button,hookDown,point,time-hookDownTime)){
            if(source)++notReadyAtRelease;return {};
        }
        RECT current{};
        if(GetForegroundWindow()!=source||GetAncestor(WindowFromPoint(point),GA_ROOT)!=source||!GetWindowRect(source,&current)||!EqualRect(&current,&job->frame.windowRect))return {};
        bool ok=job->Suppress();
        auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
        auto old=maxReleaseUs.load();while(us>old&&!maxReleaseUs.compare_exchange_weak(old,us)){}
        if(!ok){++suppressionFailed;return {};}return job;
    }
    static LRESULT CALLBACK Mouse(int code,WPARAM w,LPARAM l){
        auto self=hookOwner;
        if(code==HC_ACTION&&self&&(w==WM_MOUSEMOVE||w==WM_LBUTTONDOWN||w==WM_LBUTTONUP)){
            auto data=reinterpret_cast<MSLLHOOKSTRUCT*>(l);
            if(!(data->flags&(LLMHF_INJECTED|LLMHF_LOWER_IL_INJECTED))){
                if(w==WM_MOUSEMOVE&&Recent(self->lastMoveTime,data->time,HoverMessageMs-1))return CallNextHookEx(nullptr,code,w,l);
                if(w==WM_MOUSEMOVE)self->lastMoveTime=data->time;
                if(w==WM_LBUTTONDOWN)self->BeforePress(data->pt,data->time);
                auto release=w==WM_LBUTTONUP?self->BeforeRelease(data->pt,data->time):std::shared_ptr<Job>{};
                auto value=new(std::nothrow) Input{data->pt,data->time,w==WM_LBUTTONDOWN,release};
                if(!value||!PostMessageW(self->receiver,w==WM_MOUSEMOVE?MoveMessage:InputMessage,0,reinterpret_cast<LPARAM>(value))){if(release)release->Cancel();delete value;}
            }
        }
        return CallNextHookEx(nullptr,code,w,l);
    }
    bool Allowed()const{HIGHCONTRASTW high{sizeof(high)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(high),&high,0);return !locked&&!app.stopping&&!app.deviceRecoveryPending&&!app.graphics.warp&&NativeMotion(app.model)&&!(high.dwFlags&HCF_HIGHCONTRASTON)&&!GetSystemMetrics(SM_REMOTESESSION);}
    void Cancel(){if(pending)pending->Cancel();armed.store(nullptr);pending.reset();if(receiver){KillTimer(receiver,1);KillTimer(receiver,3);}}
    void Reset(){gateEnabled=false;Cancel();effect.Reset();if(receiver)KillTimer(receiver,2);}
    void Prepare(Input const &input,bool pressed){
        if(!workerDone||!Recent(input.time,GetTickCount(),150))return;
        HWND target=GetAncestor(WindowFromPoint(input.point),GA_ROOT);if(!target||target!=GetForegroundWindow())return;
        RECT bounds{};float dpi=GetDpiForWindow(target)/96.f;
        if(!GetWindowRect(target,&bounds)||!CaptionArea(bounds,input.point,dpi))return;
        if(!pressed&&target==lastProbeTarget&&Recent(lastProbeTime,input.time,250))return;
        gateEnabled=Allowed();if(!gateEnabled)return;if(worker.joinable())worker.join();
        lastProbeTarget=target;lastProbeTime=input.time;
        auto job=std::make_shared<Job>();job->frame.source=target;job->frame.down=input.point;job->pressed=pressed;job->down=input.point;job->downTime=input.time;job->generation=++generation;pending=job;workerDone=false;
        try{worker=std::thread([this,job]{
            auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            try{job->valid=!job->canceled&&Probe(job->frame)&&!job->canceled&&Capture(job->frame)&&!job->canceled;}catch(...){job->valid=false;}
            if(SUCCEEDED(com))CoUninitialize();job->capturedAt=GetTickCount();job->ready.store(true,std::memory_order_release);
            if(job->valid&&!job->canceled){armed.store(job);if(job->canceled){auto expected=job;armed.compare_exchange_strong(expected,nullptr);}else ++prepared;}
            workerDone=true;PostMessageW(receiver,ReadyMessage,job->generation,0);
        });}catch(...){workerDone=true;Cancel();return;}
        if(!SetTimer(receiver,1,pressed?ClickMs+ConfirmMs:FrameAgeMs,nullptr))Cancel();
    }
    void Move(Input const &input){
        if(pending&&pending->pressed)return;
        if(pending){
            if(!pending->ready){if(std::abs(input.point.x-pending->frame.down.x)<12&&std::abs(input.point.y-pending->frame.down.y)<12)return;}
            else if(!pending->canceled&&PtInRect(&pending->frame.button,input.point)&&Recent(pending->capturedAt,input.time,FrameAgeMs))return;
            Cancel();
        }
        Prepare(input,false);
    }
    void Down(Input const &input){
        gateEnabled=Allowed();if(!gateEnabled||!Recent(input.time,GetTickCount(),150)){Cancel();return;}
        if(pending&&!pending->pressed&&!pending->canceled){
            bool ready=pending->ready.load(std::memory_order_acquire);
            if((ready&&pending->valid&&PtInRect(&pending->frame.button,input.point)&&Recent(pending->capturedAt,input.time,FrameAgeMs))||
                (!ready&&std::abs(input.point.x-pending->frame.down.x)<12&&std::abs(input.point.y-pending->frame.down.y)<12)){
                pending->pressed=true;pending->down=input.point;pending->downTime=input.time;SetTimer(receiver,1,ClickMs+ConfirmMs,nullptr);return;
            }
        }
        Cancel();Prepare(input,true);
    }
    void Up(Input const &input){
        if(!pending||input.release!=pending||!pending->suppressed){if(input.release)input.release->Cancel();Cancel();return;}
        pending->released=true;pending->up=input.point;pending->upTime=input.time;
        if(!Recent(pending->downTime,input.time,ClickMs)){Cancel();return;}
        SetTimer(receiver,1,ConfirmMs,nullptr);TryStart();
    }
    void Event(DWORD event,HWND window,LONG object,LONG child,DWORD time){
        if(effect.source==window&&event==EVENT_OBJECT_SHOW&&object==OBJID_WINDOW){effect.Reset();KillTimer(receiver,2);}
        if(!pending)return;auto target=pending->frame.source;
        if(event==EVENT_SYSTEM_MINIMIZESTART&&window==target){Cancel();return;}
        if(event==EVENT_OBJECT_SHOW&&object==OBJID_WINDOW&&GetWindow(window,GW_OWNER)==target){Cancel();return;}
        if((event==EVENT_OBJECT_HIDE||event==EVENT_OBJECT_DESTROY)&&window==target&&object==OBJID_WINDOW&&child==CHILDID_SELF){pending->hidden=true;pending->hiddenTime=time;PostMessageW(receiver,ConfirmMessage,0,0);}
        // A closing app may activate its successor BEFORE it sends HIDE. A
        // prepared, released click must survive that ordering until confirmation.
        if(event==EVENT_SYSTEM_FOREGROUND&&window!=target&&!pending->suppressed){Cancel();}
        if(event==EVENT_OBJECT_LOCATIONCHANGE&&window==target&&object==OBJID_WINDOW&&!pending->suppressed)Cancel();
    }
    void TryStart(){
        auto job=pending;if(!job||!job->ready.load(std::memory_order_acquire)||!job->released||!job->hidden)return;
        DWORD now=GetTickCount();DWORD pid=0;GetWindowThreadProcessId(job->frame.source,&pid);
        bool gone=!IsWindow(job->frame.source)||(pid==job->frame.pid&&!IsWindowVisible(job->frame.source)&&!IsIconic(job->frame.source));
        if(!job->valid||job->canceled||!job->suppressed||!Allowed()||!gone||!ReleaseMatches(job->frame.button,job->down,job->up,job->upTime-job->downTime)||!Recent(job->upTime,job->hiddenTime,ConfirmMs)||!Recent(job->hiddenTime,now,150)){Cancel();return;}
        if(NativeRenderScope::busy){SetTimer(receiver,3,16,nullptr);return;}
        try{effect.Start(app.graphics,job->frame);++played;if(!SetTimer(receiver,2,DurationMs+16,nullptr))effect.Reset();}catch(...){effect.Reset();}Cancel();
    }
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){auto self=reinterpret_cast<Controller*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Controller*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self)try{
            if(m==InputMessage){std::unique_ptr<Input> input(reinterpret_cast<Input*>(l));if(input->down)self->Down(*input);else self->Up(*input);return 0;}
            if(m==MoveMessage){std::unique_ptr<Input> input(reinterpret_cast<Input*>(l));self->Move(*input);return 0;}
            if(m==ReadyMessage){if(self->pending&&self->pending->generation==w&&self->pending->ready.load(std::memory_order_acquire)){if(!self->pending->valid)self->Cancel();else self->TryStart();}return 0;}
            if(m==ConfirmMessage){self->TryStart();return 0;}
            if(m==WM_TIMER){KillTimer(h,w);if(w==1)self->Cancel();if(w==2)self->effect.Reset();if(w==3)self->TryStart();return 0;}
            if(m==WM_WTSSESSION_CHANGE){self->locked=w==WTS_SESSION_LOCK;if(self->locked)self->Reset();return 0;}
            if(m==WM_DISPLAYCHANGE||m==WM_DPICHANGED){self->Reset();return 0;}
        }catch(...){self->Reset();}return DefWindowProcW(h,m,w,l);
    }
};
}
