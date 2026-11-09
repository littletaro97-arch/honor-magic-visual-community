#include "providers.h"
#include "../tray-capture/protocol.h"
#include <shellapi.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <shlobj.h>
namespace magic::system
{
namespace
{
std::wstring TokenText(GUID const &id)
{
    wchar_t s[40]{};
    StringFromGUID2(id, s, 40);
    return s;
}
std::wstring NewToken()
{
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    auto s = TokenText(id);
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'{' || c == L'}' || c == L'-'; }), s.end());
    return s;
}
std::wstring CaptureAssetKey(std::vector<BYTE> const &bytes)
{
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    Check(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE);
    BYTE digest[32]{}; DWORD length = sizeof(digest);
    bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
              CryptHashData(hash, bytes.data(), DWORD(bytes.size()), 0) &&
              CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest);
    DWORD error = GetLastError(); if (hash) CryptDestroyHash(hash); CryptReleaseContext(provider, 0);
    SetLastError(error); Check(ok);
    std::wstring key; wchar_t pair[3]{}; for (BYTE value : digest) { swprintf_s(pair, L"%02x", value); key += pair; }
    return key;
}
std::wstring EncodeIcon(BYTE const *premultiplied,ULONG width,ULONG height)
{
    if(!width||!height||width>MAGIC_TRAY_ICON_SIZE||height>MAGIC_TRAY_ICON_SIZE)
        throw std::invalid_argument("Invalid tray raster dimensions");
    std::vector<BYTE> rgba(premultiplied, premultiplied + size_t(width)*height*4);
    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        unsigned a = rgba[i + 3];
        if (a && a < 255)
            for (size_t n = 0; n < 3; n++)
                rgba[i + n] = BYTE(std::min(255u, unsigned(rgba[i + n]) * 255 / a));
    }
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          __uuidof(IWICImagingFactory), factory.put_void()));
    winrt::com_ptr<IStream> stream;
    winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> options;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
    winrt::check_hresult(frame->Initialize(options.get()));
    winrt::check_hresult(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA)
        throw std::runtime_error("Unexpected tray encoder format");
    winrt::check_hresult(
        frame->WritePixels(height, width * 4, DWORD(rgba.size()), rgba.data()));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
    STATSTG stat{};
    winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > 65536)
        throw std::runtime_error("Tray PNG exceeds bound");
    HGLOBAL memory = nullptr;
    winrt::check_hresult(GetHGlobalFromStream(stream.get(), &memory));
    auto bytes = static_cast<BYTE *>(GlobalLock(memory));
    if (!bytes)
        winrt::throw_last_error();
    DWORD count = 0;
    CryptBinaryToStringW(bytes, stat.cbSize.LowPart, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &count);
    std::wstring text(count, L'\0');
    BOOL ok = CryptBinaryToStringW(bytes, stat.cbSize.LowPart, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text.data(),
                                   &count);
    GlobalUnlock(memory);
    Check(ok != FALSE);
    text.resize(count);
    while (!text.empty() && !text.back())
        text.pop_back();
    return text;
}
bool RemoveStaleProperty(HWND window)
{
    auto atom = ATOM(reinterpret_cast<ULONG_PTR>(GetPropW(window, MAGIC_TRAY_PROPERTY)));
    if (!atom)
        return true;
    wchar_t name[100]{};
    if (!GlobalGetAtomNameW(atom, name, 100) || std::wstring(name).rfind(L"Local\\MagicNativeTray-", 0) != 0)
        return false;
    Handle mapping{OpenFileMappingW(FILE_MAP_READ, FALSE, name)};
    bool stale = false;
    if (!mapping.value)
        stale = GetLastError() == ERROR_FILE_NOT_FOUND;
    else
    {
        auto shared =
            static_cast<MagicTrayShared const *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(MagicTrayShared)));
        if (!shared)
            return false;
        if (shared->signature == MAGIC_TRAY_SIGNATURE && shared->version == MAGIC_TRAY_VERSION &&
            shared->bytes == sizeof(MagicTrayShared))
        {
            Handle owner{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shared->ownerPid)};
            if (!owner.value)
                stale = GetLastError() == ERROR_INVALID_PARAMETER;
            else
                stale = WaitForSingleObject(owner, 0) == WAIT_OBJECT_0 || Created(owner) != shared->ownerCreated;
        }
        UnmapViewOfFile(shared);
    }
    if (stale && reinterpret_cast<ULONG_PTR>(GetPropW(window, MAGIC_TRAY_PROPERTY)) == atom)
    {
        RemovePropW(window, MAGIC_TRAY_PROPERTY);
        GlobalDeleteAtom(atom);
        return true;
    }
    return false;
}
struct WindowTarget
{
    HWND window;
    DWORD pid, thread;
};
std::vector<WindowTarget> Targets(HWND fixture, DWORD parent)
{
    std::vector<WindowTarget> targets;
    if (fixture)
    {
        DWORD pid = 0;
        DWORD thread = GetWindowThreadProcessId(fixture, &pid);
        wchar_t cls[128]{};
        GetClassNameW(fixture, cls, 128);
        bool ownedClass=std::wstring(cls).rfind(L"MagicTrayFixture-",0)==0||wcscmp(cls,L"Qt51514MagicOwnedTrayIconMessageWindowClass")==0;
        if (IsUserAnAdmin() || pid != parent || !ownedClass)
            throw std::invalid_argument("Tray fixture is not an owned ordinary test window");
        targets.push_back({fixture, pid, thread});
        return targets;
    }
    for (auto cls : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
    {
        HWND window = nullptr;
        while ((window = FindWindowExW(nullptr, window, cls, nullptr)) != nullptr && targets.size() < 16)
        {
            DWORD pid = 0;
            DWORD thread = GetWindowThreadProcessId(window, &pid);
            Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
            wchar_t dir[32768]{};
            UINT n = GetWindowsDirectoryW(dir, 32768);
            if (process.value && n &&
                _wcsicmp(Image(process).c_str(), (std::wstring(dir, n) + L"\\explorer.exe").c_str()) == 0)
                targets.push_back({window, pid, thread});
        }
    }
    return targets;
}
} // namespace
struct TrayProvider::Impl
{
    struct Cached
    {
        MagicTrayEntry data{};
        std::wstring png, path;
        uint64_t processCreated = 0, imageHash = 0;
    };
    std::shared_ptr<ChangeState> signal;
    DWORD parent;
    std::mutex gate;
    std::map<std::wstring, Cached> cache;
    Handle mapping, event, stopped{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        response{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    MagicTrayShared *shared = nullptr;
    ATOM atom = 0;
    std::vector<WindowTarget> targets;
    std::vector<HHOOK> hooks;
    HMODULE library = nullptr;
    std::wstring captureDirectory, capturePath;
    Handle captureFile;
    std::thread worker;
    HWND fixture = nullptr;
    LONG resynced = 0;
    std::atomic<LONG> dropped{0};
    bool active = false, desired = false;
    unsigned reconnects = 0;
    Impl(std::shared_ptr<ChangeState> s, DWORD p) : signal(std::move(s)), parent(p)
    {
    }
    void Drain()
    {
        bool notify = false;
        for (;;)
        {
            MagicTrayEntry item{};
            bool found = false;
            if (InterlockedCompareExchange(&shared->lock, 1, 0) == 0)
            {
                LONG read = shared->readIndex, write = shared->writeIndex;
                if (read != write)
                {
                    item = shared->entries[ULONG(read) % MAGIC_TRAY_CAPACITY];
                    InterlockedExchange(&shared->readIndex, read + 1);
                    found = true;
                }
                InterlockedExchange(&shared->lock, 0);
            }
            if (!found)
                break;
            std::wstring id = TokenText(item.token);
            if (item.ownerHwnd == 0 || !item.ownerPid)
                continue;
            std::scoped_lock lock(gate);
            if (item.event == NIM_DELETE)
            {
                notify |= cache.erase(id) != 0;
                continue;
            }
            if (!cache.contains(id) && cache.size() >= MAGIC_TRAY_ICONS)
                continue;
            auto &row = cache[id];
            notify |= memcmp(&row.data, &item, sizeof(item)) != 0;
            row.data = item;
            if (!row.processCreated)
            {
                Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.ownerPid)};
                if (process.value)
                    try
                    {
                        row.processCreated = Created(process);
                        row.path = Image(process);
                    }
                    catch (...)
                    {
                    }
            }
        }
        dropped = shared->dropped;
        if (shared->completed == shared->request)
            SetEvent(response);
        if (notify)
            signal->Notify(512);
    }
    void Stop()
    {
        for (auto const &t : targets)
            if (reinterpret_cast<ULONG_PTR>(GetPropW(t.window, MAGIC_TRAY_PROPERTY)) == atom)
                RemovePropW(t.window, MAGIC_TRAY_PROPERTY);
        if (active && atom)
        {
            UINT release = RegisterWindowMessageW(MAGIC_TRAY_RELEASE);
            std::set<DWORD> released;
            for (auto const &t : targets)
                if (released.insert(t.thread).second)
                {
                    DWORD_PTR result = 0;
                    SendMessageTimeoutW(t.window, release, atom, 0, SMTO_ABORTIFHUNG, 1000, &result);
                }
        }
        for (auto hook : hooks)
            UnhookWindowsHookEx(hook);
        hooks.clear();
        SetEvent(stopped);
        if (worker.joinable())
            worker.join();
        if (shared)
            UnmapViewOfFile(shared);
        shared = nullptr;
        mapping.value ? CloseHandle(mapping.value) : 0;
        mapping.value = nullptr;
        event.value ? CloseHandle(event.value) : 0;
        event.value = nullptr;
        if (atom)
            GlobalDeleteAtom(atom);
        atom = 0;
        if (library)
            FreeLibrary(library);
        library = nullptr;
        if (captureFile.value) CloseHandle(captureFile.value);
        captureFile.value = nullptr;
        // Image sections in another process may outlive UnhookWindowsHookEx.
        // Retain one content-addressed asset instead of leaking a new directory
        // per restart or racing Explorer's asynchronous module release.
        capturePath.clear(); captureDirectory.clear();
        targets.clear();
        {
            std::scoped_lock lock(gate);
            cache.clear();
        }
        active = false;
    }
    ~Impl()
    {
        Stop();
    }
    void Start(HWND test, bool initialize)
    {
        if (active)
        {
            if (test != fixture)
                throw std::invalid_argument("Tray capture already owns another target");
            return;
        }
        try
        {
            fixture = test;
            targets = Targets(test, parent);
            if (targets.empty())
                throw std::runtime_error("Windows taskbar unavailable");
            DWORD targetPid = targets[0].pid;
            for (auto const &t : targets)
                if (t.pid != targetPid)
                    throw std::runtime_error("Mixed taskbar processes are not supported");
            auto name = L"Local\\MagicNativeTray-" + NewToken();
            auto eventName = name + L"-changed";
            mapping.value =
                CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(MagicTrayShared), name.c_str());
            Check(mapping.value != nullptr && GetLastError() != ERROR_ALREADY_EXISTS);
            shared =
                static_cast<MagicTrayShared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MagicTrayShared)));
            Check(shared != nullptr);
            ZeroMemory(shared, sizeof(*shared));
            shared->signature = MAGIC_TRAY_SIGNATURE;
            shared->version = MAGIC_TRAY_VERSION;
            shared->bytes = sizeof(*shared);
            shared->targetPid = targetPid;
            shared->ownerPid = GetCurrentProcessId();
            shared->ownerCreated = Created(GetCurrentProcess());
            wcscpy_s(shared->eventName, eventName.c_str());
            event.value = CreateEventW(nullptr, FALSE, FALSE, eventName.c_str());
            Check(event.value != nullptr && GetLastError() != ERROR_ALREADY_EXISTS);
            atom = GlobalAddAtomW(name.c_str());
            Check(atom != 0);
            auto path = Image(GetCurrentProcess());
            path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"MagicTrayCapture.dll";
            // Explorer is outside our MSIX package identity and cannot load a
            // package-private DLL (ERROR_ACCESS_DENIED). Materialize our verified
            // adjacent asset into a content-addressed per-user cache. Keep a read
            // lock for the entire hook lifetime so its bytes cannot be replaced.
            Handle source{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            Check(source.value != INVALID_HANDLE_VALUE);
            LARGE_INTEGER length{}; BY_HANDLE_FILE_INFORMATION info{};
            Check(GetFileInformationByHandle(source, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                  GetFileSizeEx(source, &length) && length.QuadPart > 0 && length.QuadPart <= 2 * 1024 * 1024);
            std::vector<BYTE> bytes(static_cast<size_t>(length.QuadPart)); DWORD read = 0;
            Check(ReadFile(source, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
            wchar_t temporary[32768]{}; DWORD temporaryLength = GetTempPathW(32768, temporary);
            Check(temporaryLength > 0 && temporaryLength < 32768);
            auto directory = std::wstring(temporary, temporaryLength) + L"MagicTrayCapture-" + CaptureAssetKey(bytes);
            Check(CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
            DWORD attributes = GetFileAttributesW(directory.c_str());
            Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
            captureDirectory = directory;
            capturePath = captureDirectory + L"\\MagicTrayCapture.dll";
            {
                Handle output{CreateFileW(capturePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr)};
                if (output.value == INVALID_HANDLE_VALUE) Check(GetLastError() == ERROR_FILE_EXISTS);
                else { DWORD written = 0; Check(WriteFile(output, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size()); }
            }
            captureFile.value = CreateFileW(capturePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            Check(captureFile.value != INVALID_HANDLE_VALUE);
            std::vector<BYTE> verified(bytes.size());
            Check(GetFileInformationByHandle(captureFile, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && GetFileSizeEx(captureFile, &length) && length.QuadPart == bytes.size() &&
                  ReadFile(captureFile, verified.data(), DWORD(verified.size()), &read, nullptr) && read == verified.size() && verified == bytes);
            path = capturePath;
            library =
                LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            Check(library != nullptr);
            auto hook = reinterpret_cast<HOOKPROC>(GetProcAddress(library, "MagicTrayCaptureProc"));
            Check(hook != nullptr);
            std::set<DWORD> threads;
            ResetEvent(stopped);
            worker = std::thread([this] {
                HANDLE waits[]{stopped, event};
                while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                    Drain();
            });
            for (auto const &t : targets)
            {
                if (GetPropW(t.window, MAGIC_TRAY_PROPERTY) && !RemoveStaleProperty(t.window))
                    throw std::runtime_error("Another native tray capture owns the taskbar");
                Check(SetPropW(t.window, MAGIC_TRAY_PROPERTY, reinterpret_cast<HANDLE>(ULONG_PTR(atom))) != FALSE);
                if (threads.insert(t.thread).second)
                {
                    auto h = SetWindowsHookExW(WH_CALLWNDPROCRET, hook, library, t.thread);
                    Check(h != nullptr);
                    hooks.push_back(h);
                }
            }
            active = true;
            if (initialize && !fixture)
            {
                UINT created = RegisterWindowMessageW(L"TaskbarCreated");
                Check(created != 0);
                SendNotifyMessageW(HWND_BROADCAST, created, 0, 0);
            }
        }
        catch (...)
        {
            Stop();
            throw;
        }
    }
};
TrayProvider::TrayProvider(std::shared_ptr<ChangeState> state, DWORD parent)
    : impl(std::make_unique<Impl>(std::move(state), parent))
{
}
TrayProvider::~TrayProvider() = default;
JsonObject TrayProvider::Subscribe(JsonObject const &args)
{
    if (!args.GetNamedBoolean(L"enabled", false))
    {
        impl->desired = false;
        impl->Stop();
    }
    else
    {
        HWND fixture = nullptr;
        if (args.HasKey(L"fixtureWindow"))
            fixture = reinterpret_cast<HWND>(std::stoull(RequiredText(args, L"fixtureWindow", 32)));
        if (!fixture && !args.GetNamedBoolean(L"production", false))
            throw std::invalid_argument("Production tray capture requires explicit selection");
        impl->Start(fixture, args.GetNamedBoolean(L"initializeExisting", false));
        impl->desired = true;
    }
    return Health();
}
JsonObject TrayProvider::Health() const
{
    JsonObject result;
    Flag(result, L"subscribed", impl->active);
    Number(result, L"nativeHooks", double(impl->hooks.size()));
    {
        std::scoped_lock lock(impl->gate);
        Number(result, L"cachedIcons", double(impl->cache.size()));
    }
    Number(result, L"droppedMessages", impl->dropped.load());
    Number(result, L"reconnections", impl->reconnects);
    Number(result, L"iconCapacity", MAGIC_TRAY_ICONS);
    Number(result, L"sharedBytes", impl->active ? sizeof(MagicTrayShared) : 0);
    return result;
}
JsonObject TrayProvider::Read()
{
    if (impl->desired && !impl->fixture)
    {
        auto current = Targets(nullptr, impl->parent);
        bool changed = current.size() != impl->targets.size();
        if (!changed)
            for (size_t i = 0; i < current.size(); ++i)
                changed |= current[i].window != impl->targets[i].window || current[i].pid != impl->targets[i].pid ||
                           current[i].thread != impl->targets[i].thread;
        if (changed)
        {
            impl->Stop();
            ++impl->reconnects;
            if (!current.empty())
                impl->Start(nullptr, true);
        }
    }
    if (impl->active && impl->dropped.load() != impl->resynced)
    {
        impl->resynced = impl->dropped.load();
        impl->shared->action = 0;
        InterlockedIncrement(&impl->shared->request);
        DWORD_PTR sent = 0;
        if (!SendMessageTimeoutW(impl->targets[0].window, RegisterWindowMessageW(MAGIC_TRAY_COMMAND), impl->atom, 0,
                                 SMTO_ABORTIFHUNG, 1000, &sent))
            throw std::runtime_error("Tray resynchronization timed out");
    }
    JsonObject result = Health();
    JsonArray rows;
    std::scoped_lock lock(impl->gate);
    for (auto &[id, row] : impl->cache)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(row.data.ownerHwnd), &pid);
        if (pid != row.data.ownerPid)
            continue;
        if (row.data.state & NIS_HIDDEN)
            continue;
        JsonObject item;
        Text(item, L"id", id);
        Text(item, L"tooltip", row.data.tooltip);
        Text(item, L"executable", row.path);
        Text(item, L"stableId", row.path + L":" + TokenText(row.data.iconGuid) + L":" + std::to_wstring(row.data.id));
        NOTIFYICONIDENTIFIER identity{sizeof(identity)};
        identity.hWnd = reinterpret_cast<HWND>(row.data.ownerHwnd);
        identity.uID = row.data.id;
        identity.guidItem = row.data.iconGuid;
        RECT iconRect{};
        bool promoted = false;
        if (SUCCEEDED(Shell_NotifyIconGetRect(&identity, &iconRect)))
            for (auto const &t : impl->targets)
            {
                RECT taskbar{};
                if (GetWindowRect(t.window, &taskbar) && iconRect.right > taskbar.left &&
                    iconRect.left < taskbar.right && iconRect.bottom > taskbar.top && iconRect.top < taskbar.bottom)
                {
                    promoted = true;
                    break;
                }
            }
        Flag(item, L"promoted", promoted);
        Text(item, L"ownerHwnd", std::to_wstring(row.data.ownerHwnd));
        Number(item, L"pid", pid);
        Flag(item, L"actionAvailable", row.processCreated != 0);
        Number(item, L"callbackVersion", row.data.version == MAGIC_TRAY_VERSION_UNKNOWN ? -1 : static_cast<double>(row.data.version));
        uint64_t hash = 1469598103934665603ULL;
        hash = (hash ^ row.data.pixelWidth) * 1099511628211ULL;
        hash = (hash ^ row.data.pixelHeight) * 1099511628211ULL;
        const size_t pixelBytes = row.data.pixelWidth>0&&row.data.pixelHeight>0&&
            row.data.pixelWidth<=MAGIC_TRAY_ICON_SIZE&&row.data.pixelHeight<=MAGIC_TRAY_ICON_SIZE
            ? size_t(row.data.pixelWidth)*row.data.pixelHeight*4 : 0;
        for (size_t pixelIndex=0;pixelIndex<pixelBytes;++pixelIndex)
        {
            hash ^= row.data.pixels[pixelIndex];
            hash *= 1099511628211ULL;
        }
        if (row.data.hasPixels && pixelBytes && hash != row.imageHash)
        {
            row.png = EncodeIcon(row.data.pixels,row.data.pixelWidth,row.data.pixelHeight);
            row.imageHash = hash;
        }
        Text(item, L"imageVersion", std::to_wstring(row.imageHash));
        Text(item, L"png", row.png);
        rows.Append(item);
    }
    Flag(result, L"available", impl->active);
    result.Insert(L"icons", rows);
    return result;
}
JsonObject TrayProvider::Action(JsonObject const &args)
{
    if (!impl->active)
        throw std::invalid_argument("Tray capture is not active");
    auto id = RequiredText(args, L"iconId", 64);
    auto verb = RequiredText(args, L"action", 32);
    ULONG action = verb == L"activate"       ? 1
                   : verb == L"context"      ? 2
                   : verb == L"double-click" ? 3
                   : verb == L"middle"       ? 4
                                             : 0;
    if (!action)
        throw std::invalid_argument("Unsupported tray action");
    MagicTrayEntry offered{}; uint64_t ownerCreated = 0;
    {
        std::scoped_lock lock(impl->gate);
        auto item = impl->cache.find(id);
        if (item == impl->cache.end())
            throw std::invalid_argument("Tray icon expired");
        offered = item->second.data;
        ownerCreated = item->second.processCreated;
    }
    DWORD livePid = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(offered.ownerHwnd), &livePid);
    Handle owner{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, offered.ownerPid)};
    if (livePid != offered.ownerPid || !owner.value || !ownerCreated || Created(owner) != ownerCreated || WaitForSingleObject(owner, 0) != WAIT_TIMEOUT)
        throw std::invalid_argument("Tray owner expired");
    bool ownerForegroundGranted = AllowSetForegroundWindow(offered.ownerPid) != FALSE;
    auto number = [&](wchar_t const *key) {
        double n = args.GetNamedNumber(key, 0);
        if (!std::isfinite(n) || n < -32768 || n > 32767)
            throw std::invalid_argument("Tray anchor outside Win32 callback bounds");
        return LONG(n);
    };
    ResetEvent(impl->response);
    impl->shared->action = action;
    impl->shared->actionToken = offered.token;
    impl->shared->x = number(L"x");
    impl->shared->y = number(L"y");
    LONG request = InterlockedIncrement(&impl->shared->request);
    UINT message = RegisterWindowMessageW(MAGIC_TRAY_COMMAND);
    DWORD_PTR sent = 0;
    Check(SendMessageTimeoutW(impl->targets[0].window, message, impl->atom, 0, SMTO_ABORTIFHUNG, 1000, &sent) != 0);
    if (WaitForSingleObject(impl->response, 1500) != WAIT_OBJECT_0 || impl->shared->completed != request)
        throw std::runtime_error("Tray action acknowledgement timed out");
    LONG error = impl->shared->result;
    if (error != ERROR_SUCCESS)
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    JsonObject result;
    Flag(result, L"accepted", true);
    Flag(result, L"ownerForegroundGranted", ownerForegroundGranted);
    return result;
}
} // namespace magic::system
