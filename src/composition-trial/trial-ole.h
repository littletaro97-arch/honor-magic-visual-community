#pragma once
#include "trial.h"
inline CLIPFORMAT DragFormat()
{
    static auto format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"MagicCommunity.CompositionTrial.v1"));
    return format;
}
inline bool ReadDrag(IDataObject *data, JsonObject &payload)
{
    FORMATETC format{DragFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium)))
        return false;
    bool result = false;
    if (medium.tymed == TYMED_HGLOBAL)
    {
        SIZE_T size = GlobalSize(medium.hGlobal);
        auto text = static_cast<wchar_t *>(GlobalLock(medium.hGlobal));
        if (text && size >= sizeof(wchar_t) && size <= 65536)
        {
            size_t count = size / sizeof(wchar_t);
            auto end = std::find(text, text + count, L'\0');
            if (end != text + count)
                try
                {
                    payload = JsonObject::Parse(hstring(text, static_cast<uint32_t>(end - text)));
                    result = true;
                }
                catch (...)
                {
                }
        }
        if (text)
            GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}
// External paths are accepted only by the installed adapter. Extraction is
// bounded; the authenticated broker validates membership and executes Shell work.
inline bool HasFiles(IDataObject *data)
{
    FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};return data&&SUCCEEDED(data->QueryGetData(&format));
}
inline JsonArray DropFiles(IDataObject *data)
{
    FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM medium{};JsonArray files;
    check_hresult(data->GetData(&format,&medium));
    try{
        if(medium.tymed!=TYMED_HGLOBAL)throw std::runtime_error("Unsupported file drop medium");
        UINT count=DragQueryFileW(static_cast<HDROP>(medium.hGlobal),0xffffffff,nullptr,0);if(count==0||count>100)throw std::runtime_error("File drop count exceeds limit");
        for(UINT i=0;i<count;++i){UINT length=DragQueryFileW(static_cast<HDROP>(medium.hGlobal),i,nullptr,0);if(length==0||length>32767)throw std::runtime_error("Invalid file drop path");std::wstring path(length+1,L'\0');DragQueryFileW(static_cast<HDROP>(medium.hGlobal),i,path.data(),length+1);path.resize(length);files.Append(Text(path));}
    }catch(...){ReleaseStgMedium(&medium);throw;}
    ReleaseStgMedium(&medium);return files;
}
// A transparent, input-free image follows OLE feedback events in physical screen
// coordinates. Its grab offset and dimensions are DIP; only target DPI scales them.
inline void HideOleDragImage(HWND hwnd,SpriteVisual const &visual){
    if(hwnd)ShowWindow(hwnd,SW_HIDE);
    if(visual)visual.IsVisible(false);
}
class TrialDragImage {
    Window &window;bool fromFolder=false;HWND hwnd=nullptr;float scale=0,grabX=0,grabY=0,width=0,height=0;
    std::vector<std::pair<Tile,D2D1_RECT_F>> tiles;
    Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};SpriteVisual visual{nullptr};CompositionDrawingSurface surface{nullptr};
    HMODULE dpiModule=nullptr;
    using MonitorDpi=HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*);MonitorDpi monitorDpi=nullptr;
    POINT start{},last{};int logged=0;bool ended=false;
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;if(m==WM_ERASEBKGND)return 1;return DefWindowProcW(h,m,w,l);}
    void Paint(){
        float raster=std::min(scale,std::sqrt((8*1024*1024.f)/(width*height*4)));
        int w=std::max(1,static_cast<int>(std::ceil(width*raster))),h=std::max(1,static_cast<int>(std::ceil(height*raster)));
        auto &g=window.app.graphics;surface=g.composition.CreateDrawingSurface({static_cast<float>(w),static_cast<float>(h)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto drawing=surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};check_hresult(drawing->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));
        c->SetDpi(96*raster,96*raster);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x/raster,offset.y/raster));c->Clear(D2D1::ColorF(0,0));
        bool prior=window.dragging;window.dragging=false;try{for(auto const &[tile,rect]:tiles)window.PaintTile(c.get(),tile,rect,0,fromFolder,true);check_hresult(drawing->EndDraw());}catch(...){window.dragging=prior;drawing->EndDraw();throw;}window.dragging=prior;
        visual.Brush(g.compositor.CreateSurfaceBrush(surface));visual.Size({width*scale,height*scale});visual.Opacity(.85f);
    }
public:
    TrialDragImage(Window &source,JsonObject const &payload):window(source),fromFolder(!Str(payload,L"folder").empty()){try{
        auto anchor=window.Bounds(Str(payload,L"anchor"));D2D1_RECT_F area=anchor;
        for(auto const &v:payload.GetNamedArray(L"keys")){auto key=std::wstring(v.GetString());if(auto tile=window.Find(key)){auto r=window.Bounds(key);tiles.push_back({*tile,r});area.left=std::min(area.left,r.left);area.top=std::min(area.top,r.top);area.right=std::max(area.right,r.right);area.bottom=std::max(area.bottom,r.bottom);}}
        width=area.right-area.left;height=area.bottom-area.top;if(width<=0||height<=0||width>16384||height>16384)return;
        grabX=anchor.left-area.left+static_cast<float>(Num(payload,L"grabX"));grabY=anchor.top-area.top+static_cast<float>(Num(payload,L"grabY"));
        for(auto &[tile,r]:tiles){r.left-=area.left;r.right-=area.left;r.top-=area.top;r.bottom-=area.top;}
        dpiModule=LoadLibraryW(L"shcore.dll");if(dpiModule)monitorDpi=reinterpret_cast<MonitorDpi>(GetProcAddress(dpiModule,"GetDpiForMonitor"));
        WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=Proc;type.lpszClassName=L"MagicOleDragImage";RegisterClassW(&type);
        GetCursorPos(&start);last=start;hwnd=CreateWindowExW(WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOPMOST,type.lpszClassName,L"",WS_POPUP,start.x,start.y,1,1,nullptr,nullptr,type.hInstance,nullptr);if(!hwnd)throw_last_error();
        auto &g=window.app.graphics;check_hresult(g.compositor.as<abiComposition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd,FALSE,reinterpret_cast<abiComposition::Desktop::IDesktopWindowTarget**>(put_abi(target))));visual=g.compositor.CreateSpriteVisual();target.Root(visual);Move();
        JsonObject row;Put(row,L"pointerX",static_cast<double>(start.x));Put(row,L"pointerY",static_cast<double>(start.y));Put(row,L"grabX",static_cast<double>(grabX));Put(row,L"grabY",static_cast<double>(grabY));Put(row,L"sourceScale",window.dpi/96);window.app.Telemetry(L"drag-anchor",row);
    }catch(...){if(hwnd)DestroyWindow(hwnd);if(dpiModule)FreeLibrary(dpiModule);throw;}
    }
    ~TrialDragImage(){if(hwnd)DestroyWindow(hwnd);if(dpiModule)FreeLibrary(dpiModule);}
    void Hide(){ended=true;HideOleDragImage(hwnd,visual);}
    void Move(){if(!hwnd||ended)return;POINT pointer{};GetCursorPos(&pointer);UINT dx=96,dy=96;float next=window.dpi/96;if(monitorDpi&&SUCCEEDED(monitorDpi(MonitorFromPoint(pointer,MONITOR_DEFAULTTONEAREST),0,&dx,&dy)))next=dx/96.f;
        if(next!=scale){scale=next;Paint();}
        int left=static_cast<int>(DragImageOrigin(pointer.x,grabX,scale)),top=static_cast<int>(DragImageOrigin(pointer.y,grabY,scale));
        SetWindowPos(hwnd,HWND_TOPMOST,left,top,static_cast<int>(std::ceil(width*scale)),static_cast<int>(std::ceil(height*scale)),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        // Bounded diagnostics only on actual pointer movement, never an idle sampler.
        if(logged<12&&(std::abs(pointer.x-last.x)+std::abs(pointer.y-last.y)>=static_cast<int>(100*scale))){JsonObject row;Put(row,L"pointerX",static_cast<double>(pointer.x));Put(row,L"pointerY",static_cast<double>(pointer.y));Put(row,L"originX",static_cast<double>(left));Put(row,L"originY",static_cast<double>(top));Put(row,L"scale",scale);window.app.Telemetry(L"drag-follow",row);last=pointer;++logged;}
    }
};
class TrialData final : public IDataObject, public IDropSource
{
    std::atomic<ULONG> refs = 1;
    std::wstring payload;
    std::vector<std::wstring> files;
    HRESULT terminal=S_OK;

  public:
    std::function<void()> feedback;
    std::function<void(HRESULT)> ending;
    TrialData(JsonObject const &value, std::vector<std::wstring> paths)
        : payload(value.Stringify()), files(std::move(paths))
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDataObject))
            *out = static_cast<IDataObject *>(this);
        else if (iid == __uuidof(IDropSource))
            *out = static_cast<IDropSource *>(this);
        if (!*out)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
    {
        return format && (format->tymed & TYMED_HGLOBAL) && format->dwAspect == DVASPECT_CONTENT &&
                       (format->cfFormat == DragFormat() || (format->cfFormat == CF_HDROP && !files.empty()))
                   ? S_OK
                   : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
    {
        if (!medium)
            return E_POINTER;
        *medium = {};
        HRESULT hr = QueryGetData(format);
        if (FAILED(hr))
            return hr;
        size_t bytes = 0;
        if (format->cfFormat == DragFormat())
            bytes = (payload.size() + 1) * sizeof(wchar_t);
        else
        {
            bytes = sizeof(DROPFILES) + sizeof(wchar_t);
            for (auto const &file : files)
                bytes += (file.size() + 1) * sizeof(wchar_t);
        }
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!memory)
            return E_OUTOFMEMORY;
        auto buffer = static_cast<BYTE *>(GlobalLock(memory));
        if (!buffer)
        {
            GlobalFree(memory);
            return E_OUTOFMEMORY;
        }
        if (format->cfFormat == DragFormat())
            memcpy(buffer, payload.c_str(), bytes);
        else
        {
            auto header = reinterpret_cast<DROPFILES *>(buffer);
            header->pFiles = sizeof(DROPFILES);
            header->fWide = TRUE;
            auto dst = reinterpret_cast<wchar_t *>(buffer + sizeof(DROPFILES));
            for (auto const &file : files)
            {
                memcpy(dst, file.c_str(), (file.size() + 1) * sizeof(wchar_t));
                dst += file.size() + 1;
            }
        }
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override
    {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override
    {
        if (out)
            out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
    {
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        FORMATETC formats[] = {{DragFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                               {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(files.empty() ? 1 : 2, formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if(terminal!=S_OK)return terminal;
        terminal=escape ? DRAGDROP_S_CANCEL : !(keys & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK;
        // End only the transient image before OLE enters the destination's
        // potentially synchronous Drop. Actual file completion is still OLE's.
        if(terminal!=S_OK&&ending)try{ending(terminal);}catch(...){}
        return terminal;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        if(terminal==S_OK&&feedback)try{feedback();}catch(...){feedback={};}
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};
class TrialDrop final : public IDropTarget
{
    std::atomic<ULONG> refs = 1;
    std::shared_ptr<ProviderState> state;
    JsonObject payload;
    bool accepted = false, external = false;
    DWORD offered = 0;
    int pageDirection=0;ULONGLONG pageEntered=0;bool pageRepeated=false;
    std::shared_ptr<bool> pagePending=std::make_shared<bool>(false);
    unsigned long long pageGeneration=0;
    void CancelPageTimer(Window &window){KillTimer(window.hwnd,97);window.dragPageDwell={};}
    void ArmPage(Window &window,UINT delay){
        com_ptr<TrialDrop> self;self.copy_from(this);auto sequence=pageGeneration;
        window.dragPageDwell=[self,sequence]{auto window=Live(self->state);if(!window||!self->accepted||self->pageGeneration!=sequence||!(GetAsyncKeyState(VK_LBUTTON)&0x8000))return;POINT p{};GetCursorPos(&p);ScreenToClient(window->hwnd,&p);self->PageHover(*window,p.x*96/window->dpi,p.y*96/window->dpi);};
        SetTimer(window.hwnd,97,std::max(10u,delay),nullptr);
    }
    void PageHover(Window &window,float x,float y){
        auto b=window.app.model.Find(window.boardId);int direction=x<36?-1:x>window.width-36?1:0;
        if(!b||!b->paged||b->pages.size()<2||!direction||y<36||y>window.height-36||!accepted||!window.AcceptDrag(payload)){CancelPageTimer(window);pageDirection=0;pageEntered=0;pageRepeated=false;return;}
        if(direction!=pageDirection){CancelPageTimer(window);pageDirection=direction;pageEntered=GetTickCount64();pageRepeated=false;ArmPage(window,600);return;}
        auto delay=pageRepeated?900ull:600ull,elapsed=GetTickCount64()-pageEntered;
        if(*pagePending)return;
        if(elapsed<delay){if(!window.dragPageDwell)ArmPage(window,static_cast<UINT>(delay-elapsed));return;}
        CancelPageTimer(window);
        auto at=std::find(b->pages.begin(),b->pages.end(),b->pageId);int index=static_cast<int>(at-b->pages.begin())+direction;
        if(index<0||index>=static_cast<int>(b->pages.size()))return;
        if(!window.folderId.empty())window.CloseFolder(true);
        JsonObject request;Put(request,L"op",std::wstring(L"drag-page"));Put(request,L"board",window.boardId);Put(request,L"page",b->pages[index]);request.Insert(L"drag",payload);
        *pagePending=true;auto pending=pagePending;com_ptr<TrialDrop> self;self.copy_from(this);auto sequence=pageGeneration;
        window.app.Request(request,[self,pending,sequence](JsonObject const&reply){*pending=false;if(self->pageGeneration!=sequence||!self->accepted)return;if(auto window=Live(self->state)){if(Bool(reply,L"ok")){self->pageEntered=GetTickCount64();self->pageRepeated=true;self->ArmPage(*window,900);}else self->CancelPageTimer(*window);}});
    }

  public:
    TrialDrop(std::shared_ptr<ProviderState> value) : state(std::move(value))
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDropTarget))
            return E_NOINTERFACE;
        *out = static_cast<IDropTarget *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override
    {
        auto window = Live(state);
        ++pageGeneration;if(window)CancelPageTimer(*window);
        pageDirection=0;pageEntered=0;pageRepeated=false;pagePending=std::make_shared<bool>(false);
        offered=effect?*effect:0;
        accepted = window && ReadDrag(data, payload) && window->AcceptDrag(payload);
        external=window&&!window->GridEditing()&&window->app.model.desktopRuntime&&!accepted&&HasFiles(data);
        return DragOver(keys, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD *effect) override
    {
        if (!effect)
            return E_POINTER;
        *effect = DROPEFFECT_NONE;
        auto window = Live(state);
        if (accepted && window)
        {
            POINT p{point.x, point.y};
            ScreenToClient(window->hwnd, &p);
            float x = p.x * 96 / window->dpi, y = p.y * 96 / window->dpi;
            PageHover(*window,x,y);
            window->DragPreview(window->FolderDropOpen() &&
                                DropScope(true, window->panelRect, x, y) == FolderDropScope::Desktop);
            if (!*pagePending&&window->AcceptDrag(payload)&&window->CanDrop(payload, x, y))
                *effect = DROPEFFECT_MOVE;
            window->DropPreview(&payload,x,y,*effect!=DROPEFFECT_NONE);
        }
        if(external&&window&&!window->GridEditing()){
            POINT p{point.x,point.y};ScreenToClient(window->hwnd,&p);float x=p.x*96/window->dpi,y=p.y*96/window->dpi;
            if(!window->FolderDropOpen()||Inside(window->contentRect,x,y)){
                DWORD preferred=keys&MK_CONTROL?DROPEFFECT_COPY:keys&MK_SHIFT?DROPEFFECT_MOVE:DROPEFFECT_COPY;
                *effect=offered&preferred?preferred:offered&DROPEFFECT_COPY?DROPEFFECT_COPY:offered&DROPEFFECT_MOVE?DROPEFFECT_MOVE:DROPEFFECT_NONE;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        ++pageGeneration;
        if (auto window = Live(state)){CancelPageTimer(*window);window->DragPreview(window->dragging&&!window->folderId.empty());window->DropPreview();window->Draw(false);}
        pageDirection=0;pageEntered=0;
        accepted = external = false;
        payload = JsonObject();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override
    {
        if (!effect)
            return E_POINTER;
        DWORD allowed=*effect&(DROPEFFECT_COPY|DROPEFFECT_MOVE);
        *effect = DROPEFFECT_NONE;
        auto window = Live(state);
        JsonObject fresh;
        if (!*pagePending&&window && ReadDrag(data, fresh) && window->AcceptDrag(fresh))
            window->Drop(fresh, point, effect);
        else if(external&&window&&!window->GridEditing()&&window->app.model.desktopRuntime){
            try{
                JsonObject request;Put(request,L"op",std::wstring(L"import"));Put(request,L"board",window->boardId);request.Insert(L"paths",DropFiles(data));Put(request,L"allowedEffects",static_cast<double>(allowed));
                if(window->FolderDropOpen())Put(request,L"folder",window->folderId);
                request.Insert(L"control",JsonValue::CreateBooleanValue((keys&MK_CONTROL)!=0));request.Insert(L"shift",JsonValue::CreateBooleanValue((keys&MK_SHIFT)!=0));
                // Wait for the Shell transaction before reporting completion to
                // the source. Pump only while this OLE Drop call is active.
                struct Completion{bool done=false,ok=false;DWORD effect=DROPEFFECT_NONE;};auto completion=std::make_shared<Completion>();
                window->app.Request(request,[completion](JsonObject const &response){completion->ok=Bool(response,L"ok");if(completion->ok)completion->effect=static_cast<DWORD>(Num(response.GetNamedObject(L"result"),L"effect"));completion->done=true;});
                MSG message{};while(!completion->done&&Live(state)){
                    if(!GetMessageW(&message,nullptr,0,0)){PostQuitMessage(static_cast<int>(message.wParam));break;}
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
                if(completion->done&&completion->ok&&(completion->effect==DROPEFFECT_COPY||completion->effect==DROPEFFECT_MOVE)&&(allowed&completion->effect))*effect=completion->effect;
            }catch(...){*effect=DROPEFFECT_NONE;}
        }
        ++pageGeneration;
        if (window){CancelPageTimer(*window);window->DragPreview(false);window->DropPreview();window->Draw(false);}
        pageDirection=0;pageEntered=0;
        accepted = external = false;
        payload = JsonObject();
        return S_OK;
    }
};
void InstallDrop(Window &window)
{
    auto target = new TrialDrop(window.providerState);
    HRESULT hr = RegisterDragDrop(window.hwnd, target);
    target->Release();
    check_hresult(hr);
}
void RemoveDrop(Window &window)
{
    if (window.hwnd)
        RevokeDragDrop(window.hwnd);
}
void OleDrag(Window &window, JsonObject const &payload, std::vector<std::wstring> const &paths)
{
    auto data = new TrialData(payload, paths);
    std::unique_ptr<TrialDragImage> image;try{image=std::make_unique<TrialDragImage>(window,payload);data->feedback=[&image]{if(image)try{image->Move();}catch(...){image.reset();throw;}};}catch(...){window.app.Telemetry(L"drag-image-unavailable");}
    long long released=0;double hideMs=0;HRESULT decision=S_OK;
    data->ending=[&](HRESULT action){released=Clock();decision=action;if(image)image->Hide();hideMs=Millis(released);};
    DWORD effect = 0;
    window.dragging = true;
    window.dragKeys.clear();for(auto const &key:payload.GetNamedArray(L"keys"))window.dragKeys.insert(std::wstring(key.GetString()));window.desktopStamp.valid=window.panelStamp.valid=false;window.Draw();
    HRESULT result=DoDragDrop(data, data, DROPEFFECT_MOVE | DROPEFFECT_COPY, &effect);
    // No file I/O or telemetry in QueryContinueDrag: measure in memory, then
    // record after the destination has returned control.
    double destinationMs=released?Millis(released):0;
    data->feedback={};data->ending={};image.reset();
    window.dragging = false;
    window.dragKeys.clear();window.DropPreview();window.desktopStamp.valid=window.panelStamp.valid=false;window.Draw();
    window.DragPreview(false);
    data->Release();
    JsonObject timing;Put(timing,L"imageHiddenMs",hideMs);Put(timing,L"destinationAfterReleaseMs",destinationMs);
    Put(timing,L"releaseDecision",static_cast<double>(decision));Put(timing,L"oleResult",static_cast<double>(result));Put(timing,L"effect",static_cast<double>(effect));
    window.app.Telemetry(L"drag-release-completed",timing);
    check_hresult(result);
}
