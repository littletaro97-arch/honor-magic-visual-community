#pragma once
// Transient helper: one decoded alpha frame shared by all monitor surfaces.
// The packed PNG sequence is prepared offline; no runtime codec or WebView.
struct NativeTransition
{
    Graphics graphics;
    struct Screen{HWND hwnd=nullptr;RECT rect{};Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};ContainerVisual root{nullptr};SpriteVisual cover{nullptr},label{nullptr},logo{nullptr};CompositionDrawingSurface text{nullptr},logoSurface{nullptr};UINT32 logoPixelWidth=0,logoPixelHeight=0;};
    struct Frame{uint64_t offset;uint32_t bytes;};
    std::vector<Frame> frames;std::ifstream sequence;int currentFrame=-1;uint32_t frameWidth=0,frameHeight=0,fps=30;
    std::vector<std::unique_ptr<Screen>> screens;
    std::wstring marker,nonce,media;
    bool entrance=false,revealing=false,black=false,covered=false,skip=false;ULONGLONG began=0,revealAt=0;
    CompositionScopedBatch blackout{nullptr};winrt::event_token blackoutToken{};
    Windows::Foundation::IAsyncAction blackoutCommit{nullptr};
    ULONGLONG finalCoverAt=0;bool finalCoverQueued=false;
    HANDLE opaque=nullptr,parent=nullptr;com_ptr<ID2D1Bitmap1> logoBitmap;
    void Trace(char const *phase){if(marker.empty())return;try{std::ofstream file{std::filesystem::path(marker+L".trace"),std::ios::app};file<<GetTickCount64()<<" "<<phase<<"\n";}catch(...){}}
    ~NativeTransition(){if(blackout)blackout.Completed(blackoutToken);for(auto &s:screens)if(s->hwnd)DestroyWindow(s->hwnd);if(parent)CloseHandle(parent);if(opaque)CloseHandle(opaque);}
    void Fade(Visual const &visual,float from,float to,int milliseconds)
    {auto a=graphics.compositor.CreateScalarKeyFrameAnimation();a.InsertKeyFrame(0,from);a.InsertKeyFrame(1,to,graphics.compositor.CreateCubicBezierEasingFunction({.22f,.61f},{.36f,1}));a.Duration(std::chrono::milliseconds(milliseconds));visual.StartAnimation(L"Opacity",a);}
    void SignalOpaque(){Trace("opaque-complete");covered=true;if(opaque){SetEvent(opaque);CloseHandle(opaque);opaque=nullptr;}}
    void BeginBlackout(int duration){
        finalCoverAt=GetTickCount64()+duration;finalCoverQueued=false;black=true;if(blackout){blackout.Completed(blackoutToken);blackout=nullptr;}
        blackout=graphics.compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
        for(auto &s:screens)Fade(s->cover,0,1,duration);
        HWND receiver=screens.front()->hwnd;blackoutToken=blackout.Completed([receiver](auto const&,auto const&){PostMessageW(receiver,WM_APP+182,0,0);});blackout.End();blackoutCommit=graphics.compositor.RequestCommitAsync();Trace("blackout-committed");
    }
    void Reveal(int duration){if(revealing)return;if(!covered){if(!skip){skip=true;BeginBlackout(1);}return;}revealing=true;revealAt=GetTickCount64()+duration;for(auto &s:screens)Fade(s->root,1,0,duration);}
    void LoadSequence(){
        auto file=std::filesystem::path(media).parent_path()/L"entrance-alpha.frames";
        auto length=std::filesystem::file_size(file);if(length<24||length>256ull*1024*1024)throw std::runtime_error("Alpha sequence size");
        sequence.open(file,std::ios::binary);char magic[8]{};sequence.read(magic,8);if(memcmp(magic,"MAGICAF1",8))throw std::runtime_error("Alpha sequence format");
        uint32_t count=0;sequence.read(reinterpret_cast<char*>(&count),4);sequence.read(reinterpret_cast<char*>(&frameWidth),4);sequence.read(reinterpret_cast<char*>(&frameHeight),4);sequence.read(reinterpret_cast<char*>(&fps),4);
        if(count<1||count>256||frameWidth<1||frameWidth>4096||frameHeight<1||frameHeight>4096||fps<1||fps>60||static_cast<uint64_t>(frameWidth)*frameHeight*4>16*1024*1024)throw std::runtime_error("Alpha frame bounds");
        uint64_t minimum=24ull+count*12;for(uint32_t i=0;i<count;i++){Frame frame{};sequence.read(reinterpret_cast<char*>(&frame.offset),8);sequence.read(reinterpret_cast<char*>(&frame.bytes),4);if(!sequence||frame.offset<minimum||frame.bytes<8||frame.bytes>16*1024*1024||frame.offset>length||frame.bytes>length-frame.offset)throw std::runtime_error("Alpha frame table");frames.push_back(frame);minimum=frame.offset+frame.bytes;}
    }
    void ReadFrame(int index){
        if(index==currentFrame||frames.empty())return;auto frame=frames.at(index);std::vector<BYTE> png(frame.bytes);sequence.seekg(frame.offset);sequence.read(reinterpret_cast<char*>(png.data()),frame.bytes);if(!sequence)throw std::runtime_error("Alpha frame read");
        com_ptr<IStream> stream;stream.attach(SHCreateMemStream(png.data(),frame.bytes));com_ptr<IWICBitmapDecoder> decoder;com_ptr<IWICBitmapFrameDecode> image;
        check_hresult(graphics.wic->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.put()));check_hresult(decoder->GetFrame(0,image.put()));UINT w=0,h=0;image->GetSize(&w,&h);if(w!=frameWidth||h!=frameHeight)throw std::runtime_error("Alpha frame dimensions");
        com_ptr<IWICFormatConverter> converter;check_hresult(graphics.wic->CreateFormatConverter(converter.put()));check_hresult(converter->Initialize(image.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
        com_ptr<ID2D1DeviceContext> c;check_hresult(graphics.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,c.put()));logoBitmap=nullptr;check_hresult(c->CreateBitmapFromWicBitmap(converter.get(),nullptr,logoBitmap.put()));currentFrame=index;
    }
    void DrawFrame(){for(auto &s:screens){if(!s->logoSurface||!logoBitmap)continue;auto drawing=s->logoSurface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};check_hresult(drawing->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));c->SetUnitMode(D2D1_UNIT_MODE_PIXELS);c->Clear(D2D1::ColorF(0,0));/* Scale through the transform and let DrawImage resample once, cubic: the frame is smaller than the panel, and the plain bitmap path only offers nearest or linear. */float sx=static_cast<float>(s->logoPixelWidth)/frameWidth,sy=static_cast<float>(s->logoPixelHeight)/frameHeight;c->SetTransform(D2D1::Matrix3x2F::Scale(sx,sy)*D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));c->DrawImage(logoBitmap.get(),nullptr,nullptr,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,D2D1_COMPOSITE_MODE_SOURCE_OVER);check_hresult(drawing->EndDraw());}}
    void Tick(){
        auto elapsed=GetTickCount64()-began;if(revealing){if(GetTickCount64()>=revealAt)PostQuitMessage(0);return;}
        if((GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0){Reveal(250);return;}
        std::ifstream file{std::filesystem::path(marker)};char line[128]{};file.getline(line,sizeof(line));std::string status=line;bool ready=status==to_string(nonce)+":ready";
        if(elapsed>=30000||(parent&&WaitForSingleObject(parent,0)==WAIT_OBJECT_0&&!ready)){Trace("owner-ended-or-timeout");PostQuitMessage(2);return;}
        if(entrance&&!frames.empty()){int next=std::min({static_cast<int>(frames.size())-1,static_cast<int>(fps*44/10),static_cast<int>(elapsed*fps/1000)});if(next!=currentFrame){ReadFrame(next);DrawFrame();}}
        if(!black&&elapsed>=(entrance?2400ull:0ull))BeginBlackout(entrance?800:450);
        if(status==to_string(nonce)+":failed"){Trace("failed-marker");PostQuitMessage(2);return;}
        // On systems that do not deliver the batch event, commit an explicit
        // final cover and wait for that real compositor commit plus DWM present.
        // Time only schedules the final value; it never releases the handshake.
        if(black&&!covered&&GetTickCount64()>=finalCoverAt){
            if(!finalCoverQueued){for(auto &s:screens){s->cover.StopAnimation(L"Opacity");s->cover.Opacity(1);}blackoutCommit=graphics.compositor.RequestCommitAsync();finalCoverQueued=true;Trace("final-cover-enqueued");}
            else if(blackoutCommit.Status()==Windows::Foundation::AsyncStatus::Completed){check_hresult(DwmFlush());Trace("final-cover-presented");SignalOpaque();}
        }
        if(ready&&covered&&elapsed>=(entrance?4200ull:1050ull))Reveal(entrance?550:400);
    }
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
        auto self=reinterpret_cast<NativeTransition*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<NativeTransition*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self){if(m==WM_APP+182){self->SignalOpaque();if(self->skip)self->Reveal(250);return 0;}if(m==WM_TIMER){try{self->Tick();}catch(...){self->Trace("tick-error");PostQuitMessage(3);}return 0;}if(m==WM_DISPLAYCHANGE||m==WM_DPICHANGED){self->Reveal(250);return 0;}if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;if(m==WM_KEYDOWN&&w==VK_ESCAPE){self->Reveal(250);return 0;}if(m==WM_LBUTTONUP){RECT r{};GetClientRect(h,&r);POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};float scale=std::min(3.f,GetDpiForWindow(h)/96.f);RECT skipButton{static_cast<LONG>((r.right-320*scale)/2),static_cast<LONG>(r.bottom-90*scale),static_cast<LONG>((r.right+320*scale)/2),static_cast<LONG>(r.bottom-40*scale)};if(PtInRect(&skipButton,point)){POINT screen=point;ClientToScreen(h,&screen);bool overDock=false;HWND dock=nullptr;while((dock=FindWindowExW(nullptr,dock,L"MagicCompositionNativeBar",L"Magic 视界 · 原生 Dock"))!=nullptr){RECT bounds{};if(IsWindowVisible(dock)&&GetWindowRect(dock,&bounds)&&PtInRect(&bounds,screen)){overDock=true;break;}}if(!overDock)self->Reveal(250);}return 0;}if(m==WM_CLOSE){self->Reveal(250);return 0;}if(m==WM_ERASEBKGND)return 1;}
        return DefWindowProcW(h,m,w,l);
    }
    void Add(RECT rect,bool){
        auto s=std::make_unique<Screen>();s->rect=rect;float width=static_cast<float>(rect.right-rect.left),height=static_cast<float>(rect.bottom-rect.top);
        s->hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"MagicNativeTransition",L"Magic 视界过渡",WS_POPUP,rect.left,rect.top,static_cast<int>(width),static_cast<int>(height),nullptr,nullptr,GetModuleHandleW(nullptr),this);if(!s->hwnd)throw_last_error();
        check_hresult(graphics.compositor.as<abiComposition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(s->hwnd,FALSE,reinterpret_cast<abiComposition::Desktop::IDesktopWindowTarget**>(put_abi(s->target))));s->root=graphics.compositor.CreateContainerVisual();s->root.Size({width,height});s->target.Root(s->root);
        s->cover=graphics.compositor.CreateSpriteVisual();s->cover.Size({width,height});s->cover.Brush(graphics.compositor.CreateColorBrush(Windows::UI::Color{255,0,0,0}));s->cover.Opacity(0);s->root.Children().InsertAtBottom(s->cover);
        if(entrance&&!frames.empty()){
            // The sequence is smaller than the panel. Draw it into the panel's own
            // pixels once, through the cubic path: handing a small surface to the
            // compositor and letting it stretch is what made the reveal read as soft.
            float logoScale=std::min(3.f,GetDpiForWindow(s->hwnd)/96.f),fit=std::min(width/frameWidth,height/frameHeight);
            s->logoPixelWidth=static_cast<UINT32>(frameWidth*fit*logoScale+.5f);s->logoPixelHeight=static_cast<UINT32>(frameHeight*fit*logoScale+.5f);
            s->logoSurface=graphics.composition.CreateDrawingSurface({static_cast<float>(s->logoPixelWidth),static_cast<float>(s->logoPixelHeight)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);s->logo=graphics.compositor.CreateSpriteVisual();s->logo.Size({frameWidth*fit,frameHeight*fit});s->logo.Offset({(width-frameWidth*fit)/2,(height-frameHeight*fit)/2,0});s->logo.Brush(graphics.compositor.CreateSurfaceBrush(s->logoSurface));s->root.Children().InsertAtTop(s->logo);
        }
        float uiScale=std::min(3.f,GetDpiForWindow(s->hwnd)/96.f);s->text=graphics.composition.CreateDrawingSurface({640*uiScale,110*uiScale},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        com_ptr<ID2D1DeviceContext> c;POINT offset{};auto drawing=s->text.as<abiComposition::ICompositionDrawingSurfaceInterop>();check_hresult(drawing->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));c->SetDpi(96*uiScale,96*uiScale);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x/uiScale,offset.y/uiScale));c->Clear(D2D1::ColorF(0,0));NativePaint p(c.get(),graphics,true);p.Text(entrance?L"正在准备 Magic 视界":L"正在恢复 Windows 桌面",{0,0,640,45},20,D2D1::ColorF(0xffffff),500,DWRITE_TEXT_ALIGNMENT_CENTER);p.Text(L"跳过动画 · Esc",{0,56,640,100},13,D2D1::ColorF(0xffffff,.7f),400,DWRITE_TEXT_ALIGNMENT_CENTER);check_hresult(drawing->EndDraw());
        s->label=graphics.compositor.CreateSpriteVisual();s->label.Size({640*uiScale,110*uiScale});s->label.Offset({(width-640*uiScale)/2,height-140*uiScale,0});s->label.Brush(graphics.compositor.CreateSurfaceBrush(s->text));s->root.Children().InsertAtTop(s->label);ShowWindow(s->hwnd,SW_SHOWNOACTIVATE);screens.push_back(std::move(s));
    }
    static BOOL CALLBACK Monitor(HMONITOR h,HDC,LPRECT rect,LPARAM l){MONITORINFO info{sizeof(info)};GetMonitorInfoW(h,&info);try{reinterpret_cast<NativeTransition*>(l)->Add(*rect,(info.dwFlags&MONITORINFOF_PRIMARY)!=0);return TRUE;}catch(...){return FALSE;}}
    int Run(bool enter,std::wstring file,std::wstring token,std::wstring video,DWORD owner,bool reduce){
        if(token.size()==32){auto name=L"Local\\MagicTransition-"+token+L"-opaque";opaque=OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str());}
        BOOL animation=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animation,0);SYSTEM_POWER_STATUS power{};GetSystemPowerStatus(&power);if(!animation||(reduce&&power.SystemStatusFlag==1)){SignalOpaque();return 0;}
        entrance=enter;marker=std::move(file);nonce=std::move(token);media=std::move(video);Trace(opaque?"event-open":"event-open-failed");if(nonce.size()!=32||!opaque)return 2;parent=OpenProcess(SYNCHRONIZE,FALSE,owner);if(!parent)return 2;graphics.Start();if(entrance)try{LoadSequence();ReadFrame(0);Trace("alpha-first-frame");}catch(...){frames.clear();Trace("alpha-unavailable");}
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"MagicNativeTransition";RegisterClassW(&wc);
        began=GetTickCount64();EnumDisplayMonitors(nullptr,nullptr,Monitor,reinterpret_cast<LPARAM>(this));if(screens.empty())return 2;DrawFrame();Trace("surfaces-ready");SetTimer(screens.front()->hwnd,1,16,nullptr);MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;
    }
};
