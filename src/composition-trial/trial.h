#pragma once
#include <optional>
#define NOMINMAX
#include "native-popup-ids.h"
#include "trial-geometry.h"
#include "native-folder-cover.h"
#include <UIAutomation.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <condition_variable>
#include <cstdio>
#include <d2d1_3.h>
#include <d3d11_4.h>
#include <deque>
#include <dispatcherqueue.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <fstream>
#include <functional>
#include <imm.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <wincodec.h>
#include <wincrypt.h>
#include <windows.h>
#include "../shared/native-ui-font.h"
#include "../shared/native-ui-font-dwrite.h"
#include <wtsapi32.h>
#include <windows.ui.composition.interop.h>
#include <windowsx.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::UI::Composition;
using namespace Windows::Foundation::Numerics;
namespace abiComposition = ABI::Windows::UI::Composition;
constexpr UINT PacketMessage = WM_APP + 1, AnimationMessage = WM_APP + 2;
constexpr size_t PacketLimit = 1024 * 1024, IconLimit = 8 * 1024 * 1024, SurfaceLimit = 64 * 1024 * 1024;
#include "native-icon-cache.h"
inline std::wstring Str(JsonObject const &value, wchar_t const *key)
{
    auto v = value ? value.TryLookup(key) : nullptr;
    return v && v.ValueType() == JsonValueType::String ? std::wstring(v.GetString()) : L"";
}
inline double Num(JsonObject const &value, wchar_t const *key, double fallback = 0)
{
    auto v = value ? value.TryLookup(key) : nullptr;
    return v && v.ValueType() == JsonValueType::Number ? v.GetNumber() : fallback;
}
inline bool Bool(JsonObject const &value, wchar_t const *key, bool fallback = false)
{
    auto v = value ? value.TryLookup(key) : nullptr;
    return v && v.ValueType() == JsonValueType::Boolean ? v.GetBoolean() : fallback;
}
inline JsonValue Text(std::wstring const &text)
{
    return JsonValue::CreateStringValue(text);
}
inline void Put(JsonObject const &obj, wchar_t const *key, std::wstring const &value)
{
    obj.Insert(key, Text(value));
}
inline void Put(JsonObject const &obj, wchar_t const *key, double value)
{
    obj.Insert(key, JsonValue::CreateNumberValue(value));
}
inline float Clamp(float value, float low, float high)
{
    return std::max(low, std::min(high, value));
}
inline long long Clock()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
inline double Millis(long long from)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (Clock() - from) * 1000.0 / frequency.QuadPart;
}
// Native shell metrics in DIP. Shape-specific circles/tracks retain their geometry.
struct NativeMetrics {
    static constexpr float ControlRadius=6, PanelRadius=10, FloatingRadius=16;
    static constexpr float PopupInset=10, ToolbarPopupGap=10, WorkEdgeGap=12;
};
struct Tile
{
    std::wstring key, name, target, folder;
    std::vector<Tile> items;
    JsonObject card{nullptr};
    bool isVirtual = false, isFolder = false, showItemCount = true;
    std::wstring iconExecutable;
};
struct Place
{
    int x = 0, y = 0, w = 1, h = 1;
};
struct Board
{
    std::wstring id, name;
    std::vector<Tile> tiles;
    std::map<std::wstring, Place> positions;
    int x = 30, y = 30, width = 1280, height = 850;
    float projectionScale=0;
    float originX=14,originY=76,stepX=96,stepY=96;
    std::wstring pageId,sortMode=L"custom",gridNotice;
    bool sortDescending=false;
    std::vector<std::wstring> pages;
    bool paged=true;int pageLimit=128,gridColumns=128,gridRows=128;
    std::map<std::wstring,int> pageCounts;
    std::vector<std::pair<std::wstring,std::wstring>> unplaced;
    int PageStart()const{int n=std::min(14,static_cast<int>(pages.size()));auto found=std::find(pages.begin(),pages.end(),pageId);return std::clamp(static_cast<int>(std::distance(pages.begin(),found))-n/2,0,std::max(0,static_cast<int>(pages.size())-n));}
};
struct Model
{
    Board *overlay = nullptr;
    int recoveryCount = 0;
    std::wstring session, fixtureRoot;
    long long revision = 0, workspaceVersion = 0, cardCatalogVersion = 0;
    bool synthetic = false, components = false, desktopRuntime = false;
    bool saverKnown=false,saverActive=false,saverModern=false,recycleFull=false;
    std::wstring frontendVersion;
    JsonObject cardData{nullptr},weatherData{nullptr},preferences{nullptr},encodeckData{nullptr},todoData{nullptr},honorData{nullptr},mediaData{nullptr};
    JsonArray handoffData{};uint64_t handoffVersion=0;
    JsonArray cardDefinitions{};
    bool honorSignedIn=false;double iconIdentityVersion=0;
    std::vector<Board> boards;
    bool AcceptsSnapshot(JsonObject const &value)const{
        return session.empty()||(Str(value,L"session")==session&&static_cast<long long>(Num(value,L"revision",-1))>=revision);
    }
    void Read(JsonObject const &value)
    {
        session = Str(value, L"session");
        fixtureRoot = Str(value, L"fixtureRoot");
        revision = static_cast<long long>(Num(value, L"revision"));
        recoveryCount=static_cast<int>(Num(value,L"recoveryCount"));
        workspaceVersion=static_cast<long long>(Num(value,L"workspaceVersion"));cardCatalogVersion=static_cast<long long>(Num(value,L"cardCatalogVersion"));
        synthetic = Bool(value, L"syntheticIcons");
        components=Bool(value,L"components");desktopRuntime=Bool(value,L"desktopRuntime");
        if(auto flow=value.TryLookup(L"handoffData");flow&&flow.ValueType()==JsonValueType::Array){handoffData=flow.GetArray();++handoffVersion;}
        honorSignedIn=Bool(value,L"honorSignedIn");iconIdentityVersion=Num(value,L"iconIdentityVersion");
        if(auto services=value.TryLookup(L"honorData");services&&services.ValueType()==JsonValueType::Object)honorData=services.GetObject();
        if(auto tasks=value.TryLookup(L"todoData");tasks&&tasks.ValueType()==JsonValueType::Object)todoData=tasks.GetObject();
        frontendVersion=Str(value,L"frontendVersion");recycleFull=Bool(value,L"recycleBinFull");
        if(auto p=value.TryLookup(L"preferences");p&&p.ValueType()==JsonValueType::Object)preferences=p.GetObject();
        if(auto definitions=value.TryLookup(L"cardDefinitions");definitions&&definitions.ValueType()==JsonValueType::Array)cardDefinitions=definitions.GetArray();
        std::vector<Board> next;
        for (auto const &item : value.GetNamedArray(L"boards"))
        {
            auto obj = item.GetObject();
            Board b;
            b.gridColumns=static_cast<int>(Num(obj,L"gridColumns",128));b.gridRows=static_cast<int>(Num(obj,L"gridRows",128));
            b.id = Str(obj, L"id");
            b.name = Str(obj, L"name");b.paged=Bool(obj,L"paged",true);b.pageLimit=static_cast<int>(Num(obj,L"pageLimit",128));
            if(auto counts=obj.TryLookup(L"pageCounts");counts&&counts.ValueType()==JsonValueType::Object)for(auto const &count:counts.GetObject())b.pageCounts[count.Key().c_str()]=static_cast<int>(count.Value().GetNumber());
            if(auto pending=obj.TryLookup(L"unplaced");pending&&pending.ValueType()==JsonValueType::Array)for(auto const &v:pending.GetArray()){auto entry=v.GetObject();b.unplaced.push_back({Str(entry,L"key"),Str(entry,L"name")});}
            if(auto v=obj.TryLookup(L"projection");v&&v.ValueType()==JsonValueType::Object){auto p=v.GetObject();b.projectionScale=static_cast<float>(Num(p,L"scale"));b.originX=static_cast<float>(Num(p,L"originX",14));b.originY=static_cast<float>(Num(p,L"originY",76));b.stepX=static_cast<float>(Num(p,L"stepX",96));b.stepY=static_cast<float>(Num(p,L"stepY",96));}
            b.pageId=Str(obj,L"pageId");b.sortMode=Str(obj,L"sortMode");if(b.sortMode.empty())b.sortMode=L"custom";b.sortDescending=Bool(obj,L"sortDescending");b.gridNotice=Str(obj,L"gridNotice");
            if(auto v=obj.TryLookup(L"pages");v&&v.ValueType()==JsonValueType::Array)for(auto const &p:v.GetArray())b.pages.push_back(std::wstring(p.GetString()));
            auto screen = obj.TryLookup(L"screen");
            if (screen && screen.ValueType() == JsonValueType::Object)
            {
                auto s = screen.GetObject();
                b.x = static_cast<int>(Num(s, L"x"));
                b.y = static_cast<int>(Num(s, L"y"));
                b.width = static_cast<int>(Num(s, L"width", 1280));
                b.height = static_cast<int>(Num(s, L"height", 850));
            }
            for (auto const &p : obj.GetNamedArray(L"placements"))
            {
                auto s = p.GetObject();
                b.positions[Str(s, L"key")] = {static_cast<int>(Num(s, L"x")), static_cast<int>(Num(s, L"y")),
                                               static_cast<int>(Num(s, L"w", 1)),
                                               static_cast<int>(Num(s, L"h", 1))};
            }
            for (auto const &t : obj.GetNamedArray(L"tiles"))
            {
                auto s = t.GetObject();
                Tile tile{Str(s, L"key"), Str(s, L"name"), Str(s, L"target"), Str(s, L"folderId"), {}};
                tile.isVirtual=Bool(s,L"isVirtual");tile.isFolder=Bool(s,L"isFolder");tile.showItemCount=Bool(s,L"showItemCount",true);tile.iconExecutable=Str(s,L"iconExecutable");
                if(auto card=s.TryLookup(L"card");card&&card.ValueType()==JsonValueType::Object)tile.card=card.GetObject();
                auto children = s.TryLookup(L"items");
                if (children && children.ValueType() == JsonValueType::Array)
                    for (auto const &f : children.GetArray())
                    {
                        auto c = f.GetObject();
                        Tile child{Str(c,L"key"),Str(c,L"name"),Str(c,L"target"),L"",{}};
                        child.isVirtual=Bool(c,L"isVirtual");child.isFolder=Bool(c,L"isFolder");child.iconExecutable=Str(c,L"iconExecutable");tile.items.push_back(std::move(child));
                    }
                b.tiles.push_back(std::move(tile));
            }
            next.push_back(std::move(b));
        }
        boards = std::move(next);
    }
    Board *Find(std::wstring const &id)
    {
        if(overlay&&overlay->id==id)return overlay;
        auto i = std::find_if(boards.begin(), boards.end(), [&](auto const &x) { return x.id == id; });
        return i == boards.end() ? nullptr : &*i;
    }
};
// Synchronous COM/font/wallpaper calls can pump window messages. Never enter
// a second draw, replace the model or destroy visuals while render state is in use.
inline constexpr UINT DeferredRenderPackets=WM_APP+125;
struct NativeRenderScope {
    inline static thread_local bool busy=false,needsDrain=false;
    HWND relay=nullptr;bool owns=false;
    explicit NativeRenderScope(HWND target):relay(target),owns(!busy){if(owns)busy=true;}
    explicit operator bool()const{return owns;}
    ~NativeRenderScope(){if(owns){busy=false;if(needsDrain){needsDrain=false;if(relay)PostMessageW(relay,DeferredRenderPackets,0,0);}}}
};
class PipeClient
{
    friend int CheckTrayRouting(std::wstring const &); // Owned input and serialized commands; no connected peer.
    friend int CheckCenterPanels(std::wstring const &); // Owned fixture inspects serialized output without a connected peer.
    friend int CheckFolderDragReturn(std::wstring const &); // Owned OLE messages, no connected adapter or file operations.
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::thread reader, writer;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> outgoing;
    std::atomic<bool> stopping = false;
    HWND relay = nullptr;
    static void Exact(HANDLE handle, void *bytes, DWORD length, bool write)
    {
        auto p = static_cast<char *>(bytes);
        while (length)
        {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!operation.hEvent)
                throw_last_error();
            DWORD done = 0;
            BOOL ok = write ? WriteFile(handle, p, length, &done, &operation)
                            : ReadFile(handle, p, length, &done, &operation);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                ok = GetOverlappedResult(handle, &operation, &done, TRUE);
            CloseHandle(operation.hEvent);
            if (!ok || !done)
                throw std::runtime_error("Pipe disconnected");
            p += done;
            length -= done;
        }
    }

  public:
    void Start(std::wstring const &name, DWORD parent, HWND window)
    {
        relay = window;
        pipe = CreateFileW((L"\\\\.\\pipe\\" + name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            throw_last_error();
        ULONG server = 0;
        if (!GetNamedPipeServerProcessId(pipe, &server) || server != parent)
            throw std::runtime_error("Unexpected pipe server");
        reader = std::thread([this] {
            try
            {
                while (!stopping)
                {
                    DWORD length = 0;
                    Exact(pipe, &length, 4, false);
                    if (length < 2 || length > PacketLimit)
                        throw std::runtime_error("Packet bound");
                    auto packet = std::make_unique<std::string>(length, '\0');
                    Exact(pipe, packet->data(), length, false);
                    if (!PostMessageW(relay, PacketMessage, 0, reinterpret_cast<LPARAM>(packet.get())))
                        break;
                    packet.release();
                }
            }
            catch (...)
            {
                if (!stopping)
                    PostMessageW(relay, PacketMessage, 1, 0);
            }
        });
        writer = std::thread([this] {
            try
            {
                while (true)
                {
                    std::string packet;
                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, [&] { return stopping || !outgoing.empty(); });
                        if (stopping)
                            break;
                        packet = std::move(outgoing.front());
                        outgoing.pop_front();
                    }
                    DWORD length = static_cast<DWORD>(packet.size());
                    Exact(pipe, &length, 4, true);
                    Exact(pipe, packet.data(), length, true);
                }
            }
            catch (...)
            {
                if (!stopping)
                    PostMessageW(relay, PacketMessage, 1, 0);
            }
        });
    }
    void Send(JsonObject const &value)
    {
        auto packet = to_string(value.Stringify());
        if (packet.size() > PacketLimit)
            throw std::runtime_error("Packet bound");
        {
            std::lock_guard lock(mutex);
            if (stopping)
                return;
            // Observational telemetry has no reply callback. It must not crowd
            // out actual user actions while a peer is temporarily busy.
            if(outgoing.size()>=96&&Str(value,L"op")==L"telemetry")return;
            if (outgoing.size() >= 128)
                throw std::runtime_error("IPC queue bound");
            outgoing.push_back(std::move(packet));
        }
        changed.notify_one();
    }
    void Stop()
    {
        if (stopping.exchange(true))
            return;
        changed.notify_all();
        if (pipe != INVALID_HANDLE_VALUE)
            CancelIoEx(pipe, nullptr);
        if (reader.joinable())
            CancelSynchronousIo(reader.native_handle());
        if (writer.joinable())
            CancelSynchronousIo(writer.native_handle());
        if (reader.joinable())
            reader.join();
        if (writer.joinable())
            writer.join();
        if (pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }
    ~PipeClient()
    {
        Stop();
    }
};
struct Graphics
{
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID2D1Device> d2d;
    com_ptr<IDWriteFactory> text;
    NativeUiFont::DirectWriteFonts fonts;
    std::map<std::tuple<float, bool, int, int>, com_ptr<IDWriteTextFormat>> formats;
    com_ptr<IWICImagingFactory> wic;
    com_ptr<IDXGIAdapter3> adapter;
    Windows::System::DispatcherQueueController queue{nullptr};
    Compositor compositor{nullptr};
    CompositionGraphicsDevice composition{nullptr};
    std::map<int,CompositionEffectFactory> glassFactories;
    CompositionEffectFactory dissolveFactory{nullptr}; // Lazy shader graph only; no retained animation surfaces.
    HANDLE budgetEvent = nullptr;
    DWORD budgetCookie = 0;
    unsigned long long budgetEvents=0,budgetTrims=0;ULONGLONG lastBudgetTrim=0;
    int recoveries = 0;
    bool warp = false;
    void Device()
    {
        com_ptr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                       D3D11_SDK_VERSION, d3d.put(), &level, context.put());
        warp = FAILED(hr);
        if (warp)
            check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                            D3D11_SDK_VERSION, d3d.put(), &level, context.put()));
        com_ptr<ID2D1Factory1> factory;
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                        factory.put_void()));
        auto dxgi = d3d.as<IDXGIDevice>();
        check_hresult(factory->CreateDevice(dxgi.get(), d2d.put()));
        com_ptr<IDXGIAdapter> base;
        check_hresult(dxgi->GetAdapter(base.put()));
        adapter = base.try_as<IDXGIAdapter3>();
    }
    void Start()
    {
        DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
        check_hresult(CreateDispatcherQueueController(
            options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController **>(put_abi(queue))));
        compositor = Compositor();
        Device();
        auto interop = compositor.as<abiComposition::ICompositorInterop>();
        check_hresult(interop->CreateGraphicsDevice(
            d2d.get(),
            reinterpret_cast<abiComposition::ICompositionGraphicsDevice **>(put_abi(composition))));
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown **>(text.put())));
        fonts.Initialize(text.get());
        wic = create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        budgetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (adapter && budgetEvent)
            check_hresult(
                adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(budgetEvent, &budgetCookie));
    }
    void Recover()
    {
        if (++recoveries > 3)
            throw std::runtime_error("Device recovery bound exceeded");
        if (adapter && budgetCookie)
            adapter->UnregisterVideoMemoryBudgetChangeNotification(budgetCookie);
        budgetCookie = 0;
        adapter = nullptr;
        d2d = nullptr;
        d3d = nullptr;
        Device();
        check_hresult(composition.as<abiComposition::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(
            d2d.get()));
        if (adapter && budgetEvent)
            check_hresult(
                adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(budgetEvent, &budgetCookie));
    }
    bool BudgetPressure(){
        if(!adapter)return false;
        for(auto segment:{DXGI_MEMORY_SEGMENT_GROUP_LOCAL,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}){
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};if(SUCCEEDED(adapter->QueryVideoMemoryInfo(0,segment,&info))&&info.Budget>0&&info.CurrentUsage>info.Budget-info.Budget/8)return true;
        }return false;
    }
    bool MaintainBudget(){++budgetEvents;if(!BudgetPressure()||GetTickCount64()-lastBudgetTrim<2000)return false;lastBudgetTrim=GetTickCount64();++budgetTrims;return true;}
    JsonObject Metrics()
    {
        JsonObject out;
        Put(out, L"warp", warp ? 1.0 : 0.0);Put(out,L"d2dTextureCacheLimit",static_cast<double>(d2d?d2d->GetMaximumTextureMemory():0));
        Put(out,L"budgetEvents",static_cast<double>(budgetEvents));Put(out,L"budgetTrims",static_cast<double>(budgetTrims));
        if (adapter)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, other{};
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
            {
                Put(out, L"dxgiLocalUsage", static_cast<double>(local.CurrentUsage));
                Put(out, L"dxgiLocalBudget", static_cast<double>(local.Budget));
            }
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &other)))
                Put(out, L"dxgiNonLocalUsage", static_cast<double>(other.CurrentUsage));
        }
        return out;
    }
    IDWriteTextFormat *Format(float size, bool center, int weight=400, int face=0)
    {
        // Legacy card face hints now share the product-wide family.
        face=0;
        auto key = std::make_tuple(size, center, weight, face);
        auto found = formats.find(key);
        if (found != formats.end())
            return found->second.get();
        // Bounded shared typography for desktop and declarative cards.
        if (formats.size() >= 32)
            formats.clear();
        com_ptr<IDWriteTextFormat> format;
        check_hresult(text->CreateTextFormat(fonts.family.c_str(), fonts.collection.get(), static_cast<DWRITE_FONT_WEIGHT>(weight),
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                             L"zh-CN", format.put()));
        format->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        auto result = format.get();
        formats.emplace(key, std::move(format));
        return result;
    }
    void Trim()
    {
        com_ptr<ID3D11DeviceContext> context;d3d->GetImmediateContext(context.put());context->ClearState();context->Flush();
        auto device = d3d.try_as<IDXGIDevice3>();
        if (device)
            device->Trim();
    }
    ~Graphics()
    {
        if (adapter && budgetCookie)
            adapter->UnregisterVideoMemoryBudgetChangeNotification(budgetCookie);
        if (budgetEvent)
            CloseHandle(budgetEvent);
    }
};
#include "native-honor-icons.h"
struct App;
struct Window;
struct NativeFolderAvoidance;
struct NativeFolderMorph;
struct NativeFlyoutResize;
struct NativePopup;
struct NativeShell;
struct NativeRecovery;
void OpenDesktopRecovery(App &, HWND);
struct NativeMenus;
#include "native-label-background.h"
struct ProviderState
{
    HWND hwnd = nullptr;
    std::atomic<bool> alive = true;
    std::map<std::wstring, int> ids;
    int nextId = 0;
};
struct Icon
{
    com_ptr<ID2D1Bitmap1> bitmap;
    size_t bytes = 0;
    unsigned long long age = 0;
};
// Exactly one stamp per retained drawing surface. Selection is copied only
// after painting, with no per-file image/layout cache added here.
struct NativeCardTitle {ContainerVisual clip{nullptr};SpriteVisual visual{nullptr};CompositionDrawingSurface pixels{nullptr};std::wstring key;size_t bytes=0;};
struct SurfaceStamp
{
    bool valid = false;
    long long revision = -1;
    unsigned long long iconVersion = 0;
    int scroll = 0;
    std::wstring folder, focus, status;
    std::vector<std::wstring> selection;
    bool Matches(long long rev, unsigned long long icons, int row, std::wstring const &dir,
                 std::wstring const &focused, std::wstring const &message, std::set<std::wstring> const &selected) const
    {
        return valid && revision == rev && iconVersion == icons && scroll == row && folder == dir &&
            focus == focused && status == message && selection.size() == selected.size() &&
            std::equal(selection.begin(),selection.end(),selected.begin());
    }
    void Capture(long long rev, unsigned long long icons, int row, std::wstring const &dir,
                 std::wstring const &focused, std::wstring const &message, std::set<std::wstring> const &selected)
    {
        revision=rev;iconVersion=icons;scroll=row;folder=dir;focus=focused;status=message;
        selection.assign(selected.begin(),selected.end());valid=true;
    }
};
struct NativeHoverVisual;
struct Window
{
    App &app;
    HWND hwnd = nullptr, edit = nullptr, renameHost=nullptr;
    HFONT editFont = nullptr;
    std::shared_ptr<ProviderState> providerState = std::make_shared<ProviderState>();
    std::wstring boardId, folderId, focusKey, renameKey,
        status = L"隔离样本 · 双击打开 · F2 重命名 · Ctrl 多选 · 拖放 · F6 "
                 L"切换显示区 · F8 资源快照";
    std::set<std::wstring> selected, pendingIcons,pendingCreatedKeys;
    std::wstring pendingCreatedPage;
    std::map<std::wstring, Icon> icons;
    size_t iconBytes = 0;
    unsigned long long iconAge = 0, iconVersion = 0;
    SurfaceStamp desktopStamp, panelStamp;
    NativeLabelBackground labelBackground;
    SpriteVisual desktopNameVisual{nullptr};CompositionDrawingSurface desktopNameSurface{nullptr};
    std::wstring desktopNameKey,desktopNameStamp;size_t desktopNameBytes=0;unsigned long long desktopNameDraws=0;
    void PaintDesktopName(ID2D1DeviceContext*,Tile const&,D2D1_RECT_F,float);
    void SyncDesktopName(std::wstring const&,bool withinDraw=false);void ClearDesktopName();
    long long surfaceCacheHits = 0;
    long long redraws = 0, rootRedraws = 0, panelRedraws = 0, drawnTiles = 0, cardRedraws = 0;
    unsigned long long cardUpdatePixels = 0;
    com_ptr<ID2D1SolidColorBrush> paintBrush;
    bool dragOutPreview = false, sessionPaused = false, everShown = false;
    std::map<std::wstring,float> todoScrolls;std::map<std::wstring,std::wstring> todoSelectedDays;
    std::map<std::wstring,std::pair<std::wstring,int>> weatherHourPages;
    int pageEdge = 0;
    int AvailablePageEdge(int direction)const;
    std::function<void()> dragPageDwell;
    JsonObject gridEdit{nullptr};
    std::optional<Board> gridPreview;
    bool gridOpening=false,gridBusy=false,gridBarHidden=false,gridCancelPending=false;
    long long gridWorkspace=0;
    std::wstring gridPreviousFocus,gridError;
    std::vector<Tile> gridControls;
    std::set<std::wstring> gridEnabled;
    std::vector<std::pair<std::wstring,D2D1_RECT_F>> gridButtons;
    D2D1_RECT_F gridPanel{};
    ContainerVisual gridLayer{nullptr},gridDockLayer{nullptr};SpriteVisual gridVisual{nullptr},gridBackdrop{nullptr};CompositionDrawingSurface gridSurface{nullptr};
    size_t gridSurfaceBytes=0,gridDockBytes=0;std::wstring gridSurfaceKey;bool gridClosing=false,gridSceneReady=false,gridBarCollapsing=false;
    void SyncGridScene();void AnimateGrid(bool close=false);void FinishGridMotion();void ReleaseGridScene(bool restoreDock=true);void SyncGridDock(bool suppress);D2D1_RECT_F GridPanelBounds()const;
    bool GridEditing()const{return gridOpening||static_cast<bool>(gridEdit);}
    void BeginGridEdit();void GridRequest(std::wstring const &);void GridAction(std::wstring const &);
    void GridKey(UINT);bool GridPointer(UINT,float,float);
    void PaintGridLines(ID2D1DeviceContext *);void PaintGridEditor(ID2D1DeviceContext *);
    float RootUnit()const;
    void PaintPages(ID2D1DeviceContext *c);
    void PaintFolder(ID2D1DeviceContext *c);
    void SyncCardBackdrops();
    std::map<std::wstring,SpriteVisual> cardBackdrops;
    size_t cardBackdropBytes=0;float cardBlurRadius=0;
    ContainerVisual folderMotion{nullptr};
    SpriteVisual folderBackdrop{nullptr},pageBackdrop{nullptr};
    bool materialPreview=false;
    bool ClickPage(float x,float y);
    int folderLayoutCount=-1;
    int scrollRow = 0, columns = 5, animationGeneration = 0, renameGeneration = 0;
    bool freshOpen = true, closing = false, dragging = false, pressed = false, renaming = false,
         composing = false, renameCommitPending=false, benchmark = false;
    POINT pressedPoint{};
    std::wstring pressedKey;
    std::wstring renameClickKey;
    ULONGLONG selectedClickAt=0;
    bool renameClick=false;
    std::set<std::wstring> dragKeys,pendingDragKeys;
    std::shared_ptr<NativeFolderMorph> folderMorph;
    void ResetFolderMorph();
    void PaintIcon(ID2D1DeviceContext *,Tile const &,D2D1_RECT_F);
    std::shared_ptr<NativeFolderAvoidance> folderAvoidance;
    bool folderPreviewActive=false,folderPreviewCommitted=false;long long folderPreviewRevision=0;std::vector<std::wstring> folderPreviewOrder;std::set<std::wstring> folderPreviewKeys;std::wstring folderPreviewStamp;
    void ResetFolderAvoidance();bool PreviewFolderDrag(JsonObject const &,float,float);int FolderDropIndex(JsonObject const &,float,float);
    ShapeVisual dropVisual{nullptr};
    std::wstring dropStamp;
    void DropPreview(JsonObject const *drag=nullptr,float x=0,float y=0,bool valid=false);
    std::wstring dropPlanStamp;
    std::vector<Place> dropPlanPlaces;
    bool dropPlanAdjusted=false;
    std::vector<Place> DropPlaces(JsonObject const &drag,float x,float y,bool *adjusted=nullptr);
    float dpi = 96, width = 1000, height = 700;
    D2D1_RECT_F panelRect{}, contentRect{};
    int panelPixelWidth = 0, panelPixelHeight = 0, rootPixelWidth = 0, rootPixelHeight = 0;
    size_t estimatedSurfaceBytes = 0;
    Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
    ContainerVisual root{nullptr},pageLayer{nullptr};
    ContainerVisual outgoingPage{nullptr};size_t outgoingPageBytes=0;
    std::wstring presentedPage;int presentedPageIndex=-1;
    void ResetPageMotion();void SyncPageMotion();void StartPageTransition(int direction,bool stationary);
    SpriteVisual desktop{nullptr}, scrim{nullptr}, panel{nullptr},surfaceVeil{nullptr};
    std::shared_ptr<NativeHoverVisual> interactionHover;
    void SyncInteraction(float x,float y,bool leave=false);
    std::map<std::wstring,NativeCardTitle> mediaTitles;void ClearMediaTitles();void SyncMediaTitles();size_t AdditionalSurfaceBytes()const;bool MediaKey(UINT);bool MediaPointer(UINT,float,float);
    std::wstring mediaSeekKey,mediaSeekSession,mediaSeekTrack;double mediaSeekValue=0;int mediaControlFocus=0;
    void MediaSend(std::wstring const &,std::wstring const &,double=0,std::wstring const &session=L"",std::wstring const &track=L"");
    std::unique_ptr<NativePopup> fileTooltip;std::wstring fileHoverKey;uint64_t fileHoverEpoch=0;bool fileHoverWaiting=false;int fileHoverIndex=-1;
    com_ptr<IDWriteTextLayout> handoffHoverLayout;std::vector<std::pair<uint32_t,int>> handoffHoverMarkers;std::wstring handoffHoverKey;D2D1_RECT_F handoffHoverBounds{};
    std::wstring handoffPressKey,handoffPressAction;bool HandoffPointer(UINT,float,float);
    int HandoffAttachmentHit(Tile const &,float,float);
    std::map<std::wstring,int> honorServicePages,honorServiceFocus;bool HonorContext(POINT);void HonorPage(Tile const &,int);bool HonorKey(UINT);
    void FileHover(float x,float y);void HideFileHover(bool releaseLayout=false);void ShowFileHover();
    CompositionDrawingSurface desktopSurface{nullptr}, panelSurface{nullptr};
    CompositionScopedBatch batch{nullptr};
    winrt::event_token batchToken{};
    Window(App &owner, std::wstring board) : app(owner), boardId(std::move(board))
    {
    }
    ~Window();
    void Create(int index);
    void Initialize();
    void Layout();
    bool ProjectionReady();
    SIZE RootSurfacePixels();
    void Draw(bool rootChanged = true);
    ID2D1SolidColorBrush *ColorBrush(ID2D1DeviceContext *context, D2D1_COLOR_F color);
    void DragPreview(bool outside);
    bool FolderDropOpen()const{return !folderId.empty()&&!closing&&!dragOutPreview;}
    Tile const *DropTarget(float x,float y);
    std::wstring DropFolder(JsonObject const &drag,float x,float y);
    bool CanDrop(JsonObject const &drag, float x, float y);
    void DrawSurface(CompositionDrawingSurface surface, bool folder);
    void DrawIfChanged(CompositionDrawingSurface surface, bool folder);
    void PaintTile(ID2D1DeviceContext *context, Tile const &tile, D2D1_RECT_F rect, int index,bool inFolder=false,bool detached=false);
    void PaintCard(ID2D1DeviceContext *context, Tile const &tile, D2D1_RECT_F rect);
    void PaintCardContent(ID2D1DeviceContext *context, Tile const &tile, D2D1_RECT_F rect);
    void CardAction(std::wstring const &key, std::wstring const &kind, int index=0);
    void UpdateCardClock();
    void DrawCards(bool resources,std::wstring const &filter=L"");
    bool registering=false, registerAgain=false, frameReported=false;
    int registerAttempts=0;
    void Register(bool reset=true);
    void VerifyDesktopFrame();
    void TextDraw(ID2D1DeviceContext *context, std::wstring const &text, D2D1_RECT_F rect, float size,
                  D2D1_COLOR_F color, bool center = false);
    std::vector<Tile> const &Tiles();
    Tile const *Find(std::wstring const &key);
    D2D1_RECT_F Bounds(std::wstring const &key);
    D2D1_RECT_F RootBounds(std::wstring const &key);
    std::wstring Hit(float x, float y);
    void Apply();
    void Select(std::wstring const &key, bool control = false, bool extend = false);
    void Focus(std::wstring const &key);
    void Activate(std::wstring const &key);
    void OpenFolder(std::wstring const &key);
    void CloseFolder(bool immediate = false);
    void Animate(bool close);
    void AnimationDone(int generation);
    void Key(UINT key);
    void Scroll(int delta);
    void Menu(POINT screen);
    static LRESULT CALLBACK RenameHostProc(HWND,UINT,WPARAM,LPARAM);
    NativeFolderCover::Layout<D2D1_RECT_F> CoverLayout(Tile const &,D2D1_RECT_F);
    D2D1_RECT_F NameBounds(Tile const &,D2D1_RECT_F,bool,float unit=1);
    Tile const *RenameItem();
    D2D1_RECT_F RenameBounds();
    void Rename(std::wstring key=L"");
    void ResizeRename();
    bool marquee=false,marqueeControl=false;
    std::set<std::wstring> marqueeBase;
    ShapeVisual marqueeVisual{nullptr};
    void MarqueePreview(float x=0,float y=0);
    std::wstring renameOriginal;
    void SaveRename();
    void EndRename();
    void MoveTo(std::wstring const &destination, bool out = false);
    void BeginDrag(std::wstring const &key);
    JsonObject DragPayload(std::wstring const &key);
    void Drop(JsonObject const &drag, POINTL point, DWORD *effects);
    void CommitDrop(JsonObject const &request,JsonObject const &drag,bool leaving);
    bool AcceptDrag(JsonObject const &drag);
    void RequestIcons();
    void ReceiveIcons(JsonObject const &result);
    void ClearIcons();
    void ReleaseSurfaces(bool force=false);
    bool surfaceReleasePending=false;
    bool displayChangePending=false,dpiChangePending=false,applyingDisplayChange=false;
    RECT pendingDpiRect{};
    void QueueDisplayChange(RECT const *dpiRect=nullptr);
    void RefreshDisplay();
    void Snapshot(std::wstring const &phase);
    void NotifyFocus();
    void NotifySelection();
    bool IsOnscreen(std::wstring const &key);
    void CancelBenchmark();
    LRESULT Message(UINT message, WPARAM w, LPARAM l);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w, LPARAM l);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
};
struct App
{
    ~App();
    Model model;
    std::wstring activeDragLease,lastPacketKind;
    JsonObject pendingShellData{nullptr};
    std::deque<std::unique_ptr<std::string>> deferredRenderPackets;size_t deferredRenderBytes=0;
    bool shutdownRequested=false,deviceRecoveryPending=false;
    void QueueRenderWork(){if(NativeRenderScope::busy)NativeRenderScope::needsDrain=true;else if(relay)PostMessageW(relay,DeferredRenderPackets,0,0);}
    Graphics graphics;
    NativeHonorBitmapCache honor;
    std::map<std::wstring,Icon> handoffImages;std::map<std::wstring,std::wstring> handoffImageKeys;uint64_t handoffImageAge=0;
    std::map<std::wstring,Icon> honorServiceIcons;std::map<std::wstring,std::wstring> honorServicePng;
    com_ptr<ID2D1Bitmap1> clipboardBitmap;
    std::wstring clipboardPng;
    PipeClient pipe;
    HWND relay = nullptr;
    std::vector<HPOWERNOTIFY> powerNotifications;
    void SubscribePower();void RefreshRenderPolicy();
    DWORD parent = 0;
    HANDLE parentHandle = nullptr;
    int cycles = 0, tick = 0, popupCycles = 0;
    bool allDisplays = false, started = false, graphicsReady=false, stopping = false, operationErrorVisible=false;
    long long nextId = 0, inputEpoch=0;
    unsigned long long waitWakes=0;std::map<UINT,unsigned long long> messageCounts;std::map<std::wstring,unsigned long long> packetCounts;
    void CountMessage(UINT id){auto found=messageCounts.find(id);if(found!=messageCounts.end())++found->second;else if(messageCounts.size()<64)messageCounts.emplace(id,1);}
    std::vector<std::unique_ptr<Window>> windows;
    std::unique_ptr<NativePopup> popup;
    static constexpr UINT_PTR RetiringPopupTimer=77;
    std::vector<std::unique_ptr<NativePopup>> retiringPopups; // At most one noninteractive closing panel.
    void RetirePopup(std::wstring const &nextKind);void ClearRetiringPopups();
    std::unique_ptr<NativeShell> shell;
    std::unique_ptr<NativeRecovery> recovery;
    std::unique_ptr<NativeMenus> menus;
    std::map<long long, std::function<void(JsonObject const &)>> callbacks;
    void Request(JsonObject request, std::function<void(JsonObject const &)> callback = {})
    {
        Put(request, L"id", static_cast<double>(++nextId));
        Put(request, L"session", model.session);
        Put(request, L"revision", static_cast<double>(model.revision));
        if (callback)
            callbacks[nextId] = std::move(callback);
        pipe.Send(request);
    }
    void Telemetry(std::wstring const &phase, JsonObject const &detail = JsonObject{})
    {
        if (!model.fixtureRoot.empty())
        {
            JsonObject record;
            Put(record, L"phase", phase);
            record.Insert(L"detail", detail);
            auto bytes = to_string(record.Stringify()) + "\n";
            HANDLE file = CreateFileW((model.fixtureRoot + L"\\native-renderer.jsonl").c_str(),
                                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
                CloseHandle(file);
            }
        }
        JsonObject request;
        Put(request, L"op", std::wstring(L"telemetry"));
        Put(request, L"phase", phase);
        request.Insert(L"detail", detail);
        Request(request);
    }
    void Packet(std::string const &bytes);
    void StartWindows();
    void BenchmarkTick();
    void PopupBenchmarkTick();
    void PopupSnapshot(std::wstring const &phase);
    void Shutdown();
    void Recover();
    static LRESULT CALLBACK RelayProc(HWND hwnd, UINT message, WPARAM w, LPARAM l);
};
inline bool Inside(D2D1_RECT_F rect, float x, float y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}
inline Window *Live(std::shared_ptr<ProviderState> const &state)
{
    return state && state->alive && IsWindow(state->hwnd)
               ? reinterpret_cast<Window *>(GetWindowLongPtrW(state->hwnd, GWLP_USERDATA))
               : nullptr;
}
IRawElementProviderSimple *MakeProvider(std::shared_ptr<ProviderState> const &,
                                        std::wstring const &key = L"");
void InstallDrop(Window &window);
void RemoveDrop(Window &window);
void OleDrag(Window &window, JsonObject const &payload, std::vector<std::wstring> const &files);
