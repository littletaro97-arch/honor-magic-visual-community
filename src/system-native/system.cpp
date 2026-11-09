#include "system.h"
#include "metrics.h"
#include "../window-action-helper/window-action.h"
#include <appmodel.h>
#include <wtsapi32.h>
#include "window-policy.h"
#pragma comment(lib,"wtsapi32.lib")
namespace magic::system
{
constexpr UINT ConfigureMessage = WM_APP + 1, EventMessage = WM_APP + 2, RefreshMessage = WM_APP + 3,
               ShortcutsMessage = WM_APP + 4, InputLanguageMessage = WM_APP + 5, StartFallbackMessage = WM_APP + 6;
constexpr int StartHotkey = 1;
Broker::Broker(DWORD delegatePid) : metrics(std::make_unique<Metrics>()), foregroundDelegate(delegatePid)
{
    Check(changed.value && stop.value);
    current = this;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MagicNativeSystemEvents";
    Check(RegisterClassW(&wc) != 0);
    relay = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    Check(relay != nullptr);
}
Broker::~Broker()
{
    ConfigureShortcuts(false);
    Configure(false, false);
    if (relay)
        DestroyWindow(relay);
    UnregisterClassW(L"MagicNativeSystemEvents", GetModuleHandleW(nullptr));
    current = nullptr;
}
void Broker::ConfigureMetrics(bool gpu, bool storage)
{
    std::scoped_lock lock(samplingGate);
    metrics->Configure(gpu, storage);
}
void Broker::ConfigureShortcuts(bool enabled)
{
    DWORD_PTR result = 0;
    Check(SendMessageTimeoutW(relay, ShortcutsMessage, enabled, 0, SMTO_ABORTIFHUNG, 4000, &result) != 0);
    if (result != 1)
        throw std::runtime_error("Native start shortcut hook unavailable");
}
void Broker::SetShortcutModes(bool win,bool winA)
{
    DWORD_PTR result=0;
    Check(SendMessageTimeoutW(relay,ShortcutsMessage,(win?1:0)|(winA?2:0),1,SMTO_ABORTIFHUNG,4000,&result)!=0);
    if(result!=1)throw std::runtime_error("Native shortcut configuration rejected");
}
LRESULT CALLBACK Broker::KeyboardEvent(int code, WPARAM message, LPARAM value)
{
    auto self = current;
    if (code != HC_ACTION || !self || !value)
        return CallNextHookEx(nullptr, code, message, value);
    auto const &key = *reinterpret_cast<KBDLLHOOKSTRUCT const *>(value);
    if (key.flags & LLKHF_INJECTED)
        return CallNextHookEx(nullptr, code, message, value);
    bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN, up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (key.vkCode == VK_LWIN || key.vkCode == VK_RWIN)
    {
        if (down)
        {
            unsigned bit = key.vkCode == VK_LWIN ? 1 : 2;
            if (self->winKeys && !(self->winKeys & bit))
                self->winCombo = true;
            bool first = !self->winKeys;
            self->winKeys |= bit;
            self->winHeld = true;
            if (first) self->capturedChord=false;
            if (first)
                self->winCombo = (GetAsyncKeyState(0x41) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                                 (GetAsyncKeyState(VK_SHIFT) & 0x8000);
        }
        else if (up && self->winHeld)
        {
            self->winKeys &= ~(key.vkCode == VK_LWIN ? 1 : 2);
            self->winHeld = self->winKeys != 0;
            if (self->winHeld)
                return CallNextHookEx(nullptr, code, message, value);
            if (!self->winCombo && (self->shortcutModes.load() & 1))
            {
                INPUT inputs[3]{};
                for (auto &input : inputs)
                {
                    input.type = INPUT_KEYBOARD;
                    input.ki.dwExtraInfo = 0x4d47434b;
                }
                inputs[0].ki.wVk = VK_NONAME;
                inputs[1].ki.wVk = VK_NONAME;
                inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
                inputs[2].ki.wVk = WORD(key.vkCode);
                inputs[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                if (self->pendingShortcutModes.size() >= 8)
                    return CallNextHookEx(nullptr, code, message, value);
                self->pendingShortcutModes.push_back(1);
                if (SendInput(3, inputs, sizeof(INPUT)) == 3)
                {
                    if (!self->startHotkeyRegistered && !PostMessageW(self->relay, StartFallbackMessage, 0, 0))
                        self->pendingShortcutModes.pop_back();
                    return 1;
                }
                self->pendingShortcutModes.pop_back();
            }
        }
    }
    else if(key.vkCode=='A' && self->swallowA)
    {
        if(up)self->swallowA=false;
        return 1;
    }
    else if(self->winHeld && down)
    {
        bool chord=key.vkCode=='A' && (self->shortcutModes.load()&2) && self->capturedChord;
        bool first=key.vkCode=='A' && !self->winCombo && (self->shortcutModes.load()&2) &&
            !(GetAsyncKeyState(VK_CONTROL)&0x8000) && !(GetAsyncKeyState(VK_MENU)&0x8000) && !(GetAsyncKeyState(VK_SHIFT)&0x8000);
        self->winCombo=true;
        if(first)
        {
            // Mark the already-forwarded Win as used, so releasing it cannot open
            // Windows Start after the physical A press has been consumed.
            INPUT mask[2]{};
            for(auto &input:mask){input.type=INPUT_KEYBOARD;input.ki.wVk=VK_NONAME;input.ki.dwExtraInfo=0x4d47434b;}
            mask[1].ki.dwFlags=KEYEVENTF_KEYUP;
            if (self->pendingShortcutModes.size() >= 8)
                return CallNextHookEx(nullptr, code, message, value);
            self->pendingShortcutModes.push_back(2);
            if (SendInput(2, mask, sizeof(INPUT)) != 2)
            {
                self->pendingShortcutModes.pop_back();
                return CallNextHookEx(nullptr, code, message, value);
            }
            if (!self->startHotkeyRegistered && !PostMessageW(self->relay, StartFallbackMessage, 0, 0))
            {
                self->pendingShortcutModes.pop_back();
                return CallNextHookEx(nullptr, code, message, value);
            }
            self->capturedChord = true;
        }
        if(first||chord){self->swallowA=true;return 1;}
        self->capturedChord=false;
    }
    if (up && (key.vkCode == VK_LSHIFT || key.vkCode == VK_RSHIFT || key.vkCode == VK_SPACE))
        PostMessageW(self->relay, InputLanguageMessage, 0, 0);
    return CallNextHookEx(nullptr, code, message, value);
}
void Broker::Configure(bool w, bool s)
{
    wantWindows = w;
    wantSystem = s;
    DWORD_PTR result = 0;
    Check(SendMessageTimeoutW(relay, ConfigureMessage, 0, 0, SMTO_ABORTIFHUNG, 4000, &result) != 0);
    if (result != 1)
        throw std::runtime_error("Event subscription failed");
}
void Broker::ConfigureOnThread()
{
    if (wantWindows && hooks.empty())
    {
        for (auto range : std::vector<std::pair<DWORD, DWORD>>{{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                                                               {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
                                                               {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
                                                               {EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE},
                                                               {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}})
        {
            auto hook = SetWinEventHook(range.first, range.second, nullptr, WindowEvent, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (!hook)
                winrt::throw_last_error();
            hooks.push_back(hook);
        }
        hookCount = unsigned(hooks.size());
        windowSessionEvents=WTSRegisterSessionNotification(relay,NOTIFY_FOR_THIS_SESSION)!=FALSE;
        Refresh();
    }
    else if (!wantWindows && !hooks.empty())
    {
        for (auto hook : hooks)
            UnhookWinEvent(hook);
        hooks.clear();
        if(windowSessionEvents)WTSUnRegisterSessionNotification(relay);
        windowSessionEvents=false;
        hookCount = 0;
        KillTimer(relay, 1);
        pending.clear();
        std::scoped_lock lock(gate);
        windows.clear();inputTarget={};inputThread=0;inputUntil=0;
        Signal();
    }
    if (wantSystem && !systemListeners)
    {
        systemListeners = true;
        ac = RegisterPowerSettingNotification(relay, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
        battery =
            RegisterPowerSettingNotification(relay, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
        saver = RegisterPowerSettingNotification(relay, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (NotifyIpInterfaceChange(AF_UNSPEC, NetworkEvent, this, FALSE, &network) != NO_ERROR)
            network = nullptr;
        powerCount = unsigned(bool(ac)) + unsigned(bool(battery)) + unsigned(bool(saver));
        networkActive = network != nullptr;
    }
    else if (!wantSystem)
    {
        for (auto n : {ac, battery, saver})
            if (n)
                UnregisterPowerSettingNotification(n);
        ac = battery = saver = nullptr;
        if (network)
        {
            CancelMibChangeNotify2(network);
            network = nullptr;
        }
        powerCount = 0;
        networkActive = false;
        systemListeners = false;
    }
}
LRESULT CALLBACK Broker::WindowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    auto *self = reinterpret_cast<Broker *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<Broker *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self)
        try
        {
            if (message == ShortcutsMessage)
            {
                bool subscribed = l ? self->shortcutsSubscribed : w != 0;
                unsigned modes = l ? static_cast<unsigned>(w) & 3u : self->shortcutModes.load();
                bool needed = subscribed && modes;
                if (needed && !self->keyboardHook)
                {
                    self->keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardEvent, GetModuleHandleW(nullptr), 0);
                    if (!self->keyboardHook) return 0;
                    // The registered mask receives USER32's foreground grant for
                    // either explicitly enabled community shortcut.
                    self->startHotkeyRegistered = self->foregroundDelegate &&
                        RegisterHotKey(hwnd, StartHotkey, MOD_WIN | MOD_NOREPEAT, VK_NONAME);
                }
                else if (!needed && self->keyboardHook)
                {
                    if (!UnhookWindowsHookEx(self->keyboardHook)) return 0;
                    if (self->startHotkeyRegistered) UnregisterHotKey(hwnd, StartHotkey);
                    self->startHotkeyRegistered = false;
                    self->keyboardHook = nullptr;
                    self->swallowA = false;
                    KillTimer(hwnd, 2);
                }
                if (modes != self->shortcutModes.load() || subscribed != self->shortcutsSubscribed)
                {
                    self->pendingShortcutModes.clear();
                    self->winHeld = self->winCombo = self->capturedChord = false;
                    self->winKeys = 0;
                }
                self->shortcutsSubscribed = subscribed;
                self->shortcutModes.store(modes);
                self->shortcutHookActive.store(self->keyboardHook != nullptr);
                return 1;
            }
            bool registeredStart = message == WM_HOTKEY && w == StartHotkey &&
                (LOWORD(l) & ~MOD_NOREPEAT) == MOD_WIN && HIWORD(l) == VK_NONAME;
            if (registeredStart || message == StartFallbackMessage)
            {
                if (self->keyboardHook && !self->pendingShortcutModes.empty() &&
                    registeredStart == self->startHotkeyRegistered)
                {
                    unsigned mode = self->pendingShortcutModes.front();
                    self->pendingShortcutModes.erase(self->pendingShortcutModes.begin());
                    if (!self->shortcutsSubscribed || !(self->shortcutModes.load() & mode)) return 0;
                    if (self->foregroundDelegate) AllowSetForegroundWindow(self->foregroundDelegate);
                    if (mode == 2) self->quickSettingsEpoch.fetch_add(1);
                    else self->startMenuEpoch.fetch_add(1);
                    self->wake->Notify(1024);
                }
                return 0;
            }
            if(message==InputLanguageMessage){SetTimer(hwnd,2,75,nullptr);return 0;}
            if (message == WM_TIMER && w == 2)
            {
                KillTimer(hwnd, 2);
                self->wake->Notify(256);
                return 0;
            }
            if (message == RegisterWindowMessageW(L"TaskbarCreated"))
            {
                self->wake->Notify(512);
                return 0;
            }
            if (message == ConfigureMessage)
            {
                self->ConfigureOnThread();
                return 1;
            }
            if(self->wantWindows){
                auto change=WindowSessionPolicy(message,w);
                if(change==WindowSessionChange::Clear){
                    KillTimer(hwnd,1);self->pending.clear();self->movingWindow=0;
                    {std::scoped_lock lock(self->gate);self->windows.clear();self->authorization={};self->inputTarget={};self->inputThread=0;self->inputUntil=0;}
                    self->Signal();return TRUE;
                }
                if(change==WindowSessionChange::Refresh){self->Refresh();return TRUE;}
            }
            if (message == RefreshMessage)
            {
                self->Refresh();
                return 1;
            }
            if (message == EventMessage)
            {
                if (!self->wantWindows)
                    return 0;
                self->events.fetch_add(1);
                if (w == EVENT_SYSTEM_MOVESIZESTART)
                {
                    self->movingWindow = static_cast<uintptr_t>(l);
                    self->Signal();
                }
                if (w == EVENT_SYSTEM_MOVESIZEEND ||
                    (w == EVENT_OBJECT_DESTROY && self->movingWindow == static_cast<uintptr_t>(l)))
                {
                    self->movingWindow = 0;
                    self->Signal();
                }
                {
                    std::scoped_lock identityLock(self->gate);
                    HWND target = reinterpret_cast<HWND>(l);
                    if((w==EVENT_OBJECT_DESTROY || w==EVENT_OBJECT_CREATE) && self->inputTarget.hwnd==target){
                        self->inputTarget={};self->inputThread=0;self->inputUntil=0;
                    }
                    if (w == EVENT_OBJECT_DESTROY && self->authorization.hwnd == target)
                    {
                        self->authorization = {};
                        self->Signal();
                    }
                    else if (w == EVENT_OBJECT_CREATE || w == EVENT_OBJECT_SHOW || w == EVENT_SYSTEM_FOREGROUND)
                    {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(target, &pid);
                        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
                        if (process.value)
                        {
                            try
                            {
                                auto path = Image(process);
                                wchar_t dir[32768]{};
                                UINT count = GetSystemDirectoryW(dir, 32768);
                                if (count &&
                                    _wcsicmp(path.c_str(), (std::wstring(dir, count) + L"\\consent.exe").c_str()) == 0)
                                {
                                    self->authorization.hwnd = target;
                                    self->authorization.pid = pid;
                                    self->authorization.created = Created(process);
                                    self->Signal();
                                }
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                }
                bool first = self->pending.empty();
                if (w == EVENT_SYSTEM_FOREGROUND)
                    self->Signal();
                if (w == EVENT_OBJECT_DESTROY || w == EVENT_OBJECT_CREATE)
                {
                    std::scoped_lock lock(self->gate);
                    if (self->windows.erase(reinterpret_cast<HWND>(l)))
                        self->Signal();
                }
                if (self->pending.size() < 512)
                    self->pending.insert(reinterpret_cast<HWND>(l));
                else
                    self->overflow = true;
                if (first)
                    SetTimer(hwnd, 1, 75, nullptr);
                return 0;
            }
            if (message == WM_TIMER && w == 1)
            {
                KillTimer(hwnd, 1);
                self->Flush();
                return 0;
            }
            if ((message == WM_POWERBROADCAST && self->wantSystem) ||
                (message == WM_DISPLAYCHANGE && (self->wantSystem || self->wantWindows)))
            {
                self->Signal();
                return TRUE;
            }
        }
        catch (...)
        {
            self->RequestStop();
            return 0;
        }
    return DefWindowProcW(hwnd, message, w, l);
}
void CALLBACK Broker::WindowEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD)
{
    if (!current || !hwnd)
        return;
    if (event >= EVENT_OBJECT_CREATE && (object != OBJID_WINDOW || child != CHILDID_SELF))
        return;
    if (event >= EVENT_OBJECT_CREATE && event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_NAMECHANGE &&
        event != EVENT_OBJECT_LOCATIONCHANGE && event != EVENT_OBJECT_STATECHANGE &&
        event != EVENT_OBJECT_CLOAKED && event != EVENT_OBJECT_UNCLOAKED)
        return;
    PostMessageW(current->relay, EventMessage, event, reinterpret_cast<LPARAM>(hwnd));
}
void CALLBACK Broker::NetworkEvent(PVOID context, MIB_IPINTERFACE_ROW *, MIB_NOTIFICATION_TYPE)
{
    static_cast<Broker *>(context)->Signal();
}
bool Broker::ReadWindow(HWND hwnd, WindowInfo &out) const
{
    if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd || hwnd == GetShellWindow())
        return false;
    DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!TaskbarWindowStyle(style,GetWindow(hwnd,GW_OWNER)!=nullptr))
        return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return false;
    GetWindowThreadProcessId(hwnd, &out.pid);
    if (!out.pid || out.pid == GetCurrentProcessId())
        return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, out.pid)};
    if (!process.value) { identityReadFailures.fetch_add(1); return false; }
    try
    {
        out.path = Image(process);
        UINT32 count = 0;
        if (GetApplicationUserModelId(process, &count, nullptr) == ERROR_INSUFFICIENT_BUFFER && count <= 512)
        {
            std::wstring app(count, L'\0');
            if (GetApplicationUserModelId(process, &count, app.data()) == ERROR_SUCCESS)
            {
                app.resize(wcslen(app.c_str()));
                out.aumid = app;
            }
        }
        out.created = Created(process);
        if(SecurityApplication(out.aumid)||ShellSurfaceApplication(out.aumid))return false;
    }
    catch (...)
    {
        return false;
    }
    wchar_t title[513]{};
    GetWindowTextW(hwnd, title, 513);
    out.title = title;
    if (out.title.empty())
        return false;
    out.hwnd = hwnd;
    out.id = std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)) + L":" + std::to_wstring(out.pid) + L":" +
             std::to_wstring(out.created);
    GetWindowRect(hwnd, &out.rect);
    out.dpi = GetDpiForWindow(hwnd);
    out.minimized = IsIconic(hwnd) != FALSE;
    return true;
}
BOOL CALLBACK Broker::Enumerate(HWND hwnd, LPARAM context)
{
    auto *self = reinterpret_cast<Broker *>(context);
    WindowInfo row;
    if (self->ReadWindow(hwnd, row))
    {
        if (self->collecting->size() < 512)
        {
            row.id += L":" + std::to_wstring(++self->windowGeneration);
            (*self->collecting)[hwnd] = std::move(row);
        }
        else
            self->windowLimitReached = true;
    }
    return TRUE;
}
void Broker::Refresh()
{
    std::map<HWND, WindowInfo> next;
    collecting = &next;
    overflow = false;
    windowLimitReached = false;
    enumerations.fetch_add(1);
    EnumWindows(Enumerate, reinterpret_cast<LPARAM>(this));
    collecting = nullptr;
    {
        std::scoped_lock lock(gate);
        windows.swap(next);
    }
    Signal();
}
void Broker::Flush()
{
    if (overflow)
    {
        pending.clear();
        Refresh();
        return;
    }
    bool changedState = false;
    for (HWND hwnd : pending)
    {
        WindowInfo row;
        bool valid = ReadWindow(hwnd, row);
        std::scoped_lock lock(gate);
        auto it = windows.find(hwnd);
        if (valid)
        {
            bool same = it != windows.end() && it->second.pid == row.pid && it->second.created == row.created &&
                        it->second.title == row.title && it->second.path == row.path &&
                        EqualRect(&it->second.rect, &row.rect) && it->second.dpi == row.dpi &&
                        it->second.minimized == row.minimized;
            if (!same && (it != windows.end() || windows.size() < 512))
            {
                if (it != windows.end() && it->second.pid == row.pid && it->second.created == row.created)
                    row.id = it->second.id;
                else
                    row.id += L":" + std::to_wstring(++windowGeneration);
                windows[hwnd] = std::move(row);
                changedState = true;
            }
            else if (!same)
                windowLimitReached = true;
        }
        else if (it != windows.end())
        {
            windows.erase(it);
            changedState = true;
        }
    }
    pending.clear();
    if (changedState)
        Signal();
}
JsonObject Broker::Health()
{
    JsonObject o;
    Text(o, L"protocol", Protocol);
    Text(o, L"implementation", L"win32-native");
    Number(o, L"pid", GetCurrentProcessId());
    Number(o, L"revision", double(Revision()));
    Flag(o, L"windowsSubscribed", wantWindows);
    Flag(o,L"windowSessionEvents",windowSessionEvents);
    Number(o, L"startMenuEpoch", double(startMenuEpoch.load()));
    Number(o, L"quickSettingsEpoch", double(quickSettingsEpoch.load()));
    Number(o, L"shortcutModes", shortcutModes.load());
    Flag(o, L"shortcutHookActive",shortcutHookActive.load());
    Flag(o, L"systemSubscribed", wantSystem);
    Number(o, L"windowEvents", double(events.load()));
    Number(o, L"windowIdentityReadFailures", double(identityReadFailures.load()));
    Number(o, L"windowEnumerations", double(enumerations.load()));
    Number(o, L"systemSamples", double(samples.load()));
    Number(o, L"nativeHooks", hookCount);
    Number(o, L"powerRegistrations", powerCount);
    Flag(o, L"networkNotificationActive", networkActive);
    JsonArray implemented;
    for (auto s : {L"system-memory",
                   L"system-cpu",
                   L"power",
                   L"network-interfaces",
                   L"displays",
                   L"window-tracking",
                   L"window-actions",
                   L"audio-output-volume",
                   L"media-sessions",
                   L"keyboard-layouts",
                   L"wifi-interfaces",
                   L"wifi-profile-actions",
                   L"bluetooth-paired-devices",
                   L"notifications-access",
                   L"tsf-profile-enumeration",
                   L"native-config-storage",
                   L"native-tray-capture-actions",
                   L"notification-independent-identity-consent",
                   L"audio-default-device-compat",
                   L"audio-input-volume-settings",
                   L"application-mixer",
                   L"bluetooth-pair-connect-radio",
                   L"wifi-user-network-create",
                   L"gpu-metrics-on-demand",
                   L"fixed-volume-storage",
                   L"network-rates",
                   L"configuration-launch-resolution"})
        implemented.Append(JsonValue::CreateStringValue(s));
    o.Insert(L"implemented", implemented);
    Flag(o, L"manualAcceptanceRequired", true);
    JsonArray pendingCapabilities;
    for (auto s : {L"tsf-foreground-profile-events", L"third-party-plugins"})
        pendingCapabilities.Append(JsonValue::CreateStringValue(s));
    o.Insert(L"unavailable", pendingCapabilities);
    return o;
}
static BOOL CALLBACK Monitor(HMONITOR monitor, HDC, LPRECT, LPARAM list)
{
    auto *rows = reinterpret_cast<JsonArray *>(list);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;
    JsonObject o;
    Text(o, L"id", info.szDevice);
    Number(o, L"left", info.rcMonitor.left);
    Number(o, L"top", info.rcMonitor.top);
    Number(o, L"width", info.rcMonitor.right - info.rcMonitor.left);
    Number(o, L"height", info.rcMonitor.bottom - info.rcMonitor.top);
    Number(o, L"workTop", info.rcWork.top);
    Number(o, L"workBottom", info.rcWork.bottom);
    UINT x = 96, y = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y);
    Number(o, L"dpi", x);
    Flag(o, L"primary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0);
    rows->Append(o);
    return TRUE;
}
JsonObject Broker::Snapshot(bool sampleSystem)
{
    std::scoped_lock sampling(samplingGate);
    JsonObject o = Health();
    JsonArray rows;
    HWND foreground = GetForegroundWindow();
    size_t estimatedBytes = 0;
    bool truncated = windowLimitReached;
    {
        std::scoped_lock lock(gate);
        for (auto const &[hwnd, row] : windows)
        {
            estimatedBytes += 6 * (row.title.size() + row.path.size() + row.id.size() + row.aumid.size()) + 512;
            if (estimatedBytes > 750000)
            {
                truncated = true;
                break;
            }
            JsonObject w;
            Text(w, L"id", row.id);
            Text(w, L"hwnd", std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)));
            Text(w, L"processCreated", std::to_wstring(row.created));
            Number(w, L"pid", row.pid);
            Text(w, L"title", row.title);
            Text(w, L"executable", row.path);
            Text(w, L"aumid", row.aumid);
            Number(w, L"dpi", row.dpi);
            Number(w, L"left", row.rect.left);
            Number(w, L"top", row.rect.top);
            Number(w, L"right", row.rect.right);
            Number(w, L"bottom", row.rect.bottom);
            Flag(w, L"minimized", row.minimized);
            Flag(w, L"focused", foreground == hwnd);
            rows.Append(w);
        }
    }
    o.Insert(L"windows", rows);
    Flag(o, L"windowsTruncated", truncated);
    Flag(o, L"windowMoving", movingWindow.load() != 0);
    {
        std::scoped_lock identityLock(gate);
        Flag(o, L"uacPending", authorization.hwnd && IsWindow(authorization.hwnd));
    }
    JsonArray monitors;
    EnumDisplayMonitors(nullptr, nullptr, Monitor, reinterpret_cast<LPARAM>(&monitors));
    o.Insert(L"displays", monitors);
    if (sampleSystem)
    {
        samples.fetch_add(1);
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        Check(GlobalMemoryStatusEx(&memory) != FALSE);
        JsonObject data;
        Number(data, L"memoryTotalBytes", double(memory.ullTotalPhys));
        Number(data, L"memoryAvailableBytes", double(memory.ullAvailPhys));
        Number(data, L"memoryLoadPercent", memory.dwMemoryLoad);
        FILETIME idle{}, kernel{}, user{};
        Check(GetSystemTimes(&idle, &kernel, &user) != FALSE);
        auto i = Ticks(idle), k = Ticks(kernel), u = Ticks(user);
        if (priorKernel && k + u > priorKernel + priorUser)
        {
            double busy = 1.0 - double(i - priorIdle) / double(k - priorKernel + u - priorUser);
            Number(data, L"cpuPercent", std::clamp(busy * 100.0, 0.0, 100.0));
        }
        else
            data.Insert(L"cpuPercent", JsonValue::CreateNullValue());
        priorIdle = i;
        priorKernel = k;
        priorUser = u;
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power))
        {
            JsonObject p;
            Number(p, L"acLineStatus", power.ACLineStatus);
            if (power.BatteryLifePercent <= 100)
                Number(p, L"batteryPercent", power.BatteryLifePercent);
            else
                p.Insert(L"batteryPercent", JsonValue::CreateNullValue());
            Number(p, L"batteryFlags", power.BatteryFlag);
            Number(p, L"powerSaver", power.SystemStatusFlag);
            ReadPowerMode(p,power);
            auto tick=GetTickCount64();
            if(!batteryDetailsTick || tick-batteryDetailsTick>=300000){
                batteryDetails=ReadBatteryDetails();batteryDetailsTick=tick;
            }
            p.Insert(L"batteries",batteryDetails);
            data.Insert(L"power", p);
        }
        data.Insert(L"metrics", metrics->Sample());
        JsonArray interfaces;
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> networkNow;
        double receivedRate = 0, sentRate = 0;
        ULONGLONG nowTick = GetTickCount64();
        MIB_IF_TABLE2 *table = nullptr;
        if (GetIfTable2(&table) == NO_ERROR)
        {
            for (ULONG n = 0; n < table->NumEntries && interfaces.Size() < 128; n++)
            {
                auto const &item = table->Table[n];
                if (item.Type == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;
                JsonObject itemJson;
                Text(itemJson, L"name", item.Alias);
                Number(itemJson, L"index", item.InterfaceIndex);
                Flag(itemJson, L"up", item.OperStatus == IfOperStatusUp);
                Text(itemJson, L"receivedBytes", std::to_wstring(item.InOctets));
                Text(itemJson, L"sentBytes", std::to_wstring(item.OutOctets));
                if (item.OperStatus == IfOperStatusUp && item.InterfaceAndOperStatusFlags.HardwareInterface)
                {
                    auto key = item.InterfaceLuid.Value;
                    networkNow[key] = {item.InOctets, item.OutOctets};
                    auto prior = priorNetwork.find(key);
                    if (networkTick && nowTick > networkTick && prior != priorNetwork.end())
                    {
                        if (item.InOctets >= prior->second.first)
                            receivedRate +=
                                double(item.InOctets - prior->second.first) * 1000 / (nowTick - networkTick);
                        if (item.OutOctets >= prior->second.second)
                            sentRate += double(item.OutOctets - prior->second.second) * 1000 / (nowTick - networkTick);
                    }
                }
                interfaces.Append(itemJson);
            }
            FreeMibTable(table);
        }
        data.Insert(L"networkInterfaces", interfaces);
        if (networkTick)
        {
            Number(data, L"receivedBytesPerSecond", receivedRate);
            Number(data, L"sentBytesPerSecond", sentRate);
        }
        priorNetwork = std::move(networkNow);
        networkTick = nowTick;
        o.Insert(L"system", data);
    }
    return o;
}
namespace {
bool ReadInputIdentity(HWND hwnd, WindowInfo &out, DWORD &thread)
{
    if(!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd))return false;
    thread=GetWindowThreadProcessId(hwnd,&out.pid);
    if(!thread || !out.pid)return false;
    DWORD cloaked=0;DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
    if(cloaked)return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,out.pid)};
    if(!process.value)return false;
    try{
        out.created=Created(process);
        UINT32 count=0;
        if(GetApplicationUserModelId(process,&count,nullptr)==ERROR_INSUFFICIENT_BUFFER && count<=512){
            std::wstring app(count,L'\0');
            if(GetApplicationUserModelId(process,&count,app.data())==ERROR_SUCCESS){
                app.resize(wcslen(app.c_str()));if(SecurityApplication(app))return false;
            }
        }
    }catch(...){return false;}
    DWORD finalPid=0;
    if(GetWindowThreadProcessId(hwnd,&finalPid)!=thread || finalPid!=out.pid)return false;
    out.hwnd=hwnd;return true;
}
}
JsonObject Broker::CaptureInputTarget(DWORD popupProcess)
{
    JsonObject result;Flag(result,L"available",false);
    HWND foreground=GetForegroundWindow();DWORD pid=0;
    GetWindowThreadProcessId(foreground,&pid);
    wchar_t cls[96]{};GetClassNameW(foreground,cls,96);
    bool ownPopup=pid==popupProcess && (wcscmp(cls,L"MagicCompositionPopupWindow")==0 ||
        wcscmp(cls,L"MagicCompositionInputHost")==0 || wcscmp(cls,L"MagicCompositionNativeBar")==0 ||
        wcscmp(cls,L"MagicNativeDwmPreview")==0);
    if(ownPopup){
        std::wstring id;{std::scoped_lock lock(gate);id=inputTarget.id;}
        if(!id.empty())try{auto target=ResolveInputWindow(id);Text(result,L"id",target.id);Flag(result,L"available",true);return result;}catch(std::invalid_argument const&){}
    }else{
        WindowInfo target;DWORD thread=0;
        if(ReadInputIdentity(foreground,target,thread) && foreground==GetForegroundWindow()){
            std::scoped_lock lock(gate);
            target.id=L"input:"+std::to_wstring(reinterpret_cast<uintptr_t>(target.hwnd))+L":"+
                std::to_wstring(target.pid)+L":"+std::to_wstring(target.created)+L":"+std::to_wstring(++inputGeneration);
            inputTarget=target;inputThread=thread;inputUntil=GetTickCount64()+120000;
            Text(result,L"id",target.id);Flag(result,L"available",true);return result;
        }
    }
    Text(result,L"status",L"input_target_unavailable");return result;
}
WindowInfo Broker::ResolveInputWindow(std::wstring const &id)
{
    WindowInfo selected;DWORD thread=0;
    {
        std::scoped_lock lock(gate);
        if(id.empty() || inputTarget.id!=id || GetTickCount64()>inputUntil)
            throw std::invalid_argument("Input target expired; reopen the keyboard panel");
        selected=inputTarget;thread=inputThread;
    }
    WindowInfo now;DWORD nowThread=0;
    if(!ReadInputIdentity(selected.hwnd,now,nowThread) || now.pid!=selected.pid ||
        now.created!=selected.created || nowThread!=thread)
        throw std::invalid_argument("Input target expired; reopen the keyboard panel");
    now.id=selected.id;return now;
}
WindowInfo Broker::ResolveWindow(std::wstring const &id)
{
    WindowInfo selected;
    {
        std::scoped_lock lock(gate);
        auto found =
            std::find_if(windows.begin(), windows.end(), [&](auto const &item) { return item.second.id == id; });
        if (found == windows.end())
            throw std::invalid_argument("Window identity expired");
        selected = found->second;
    }
    WindowInfo now;
    if (!ReadWindow(selected.hwnd, now) || now.pid != selected.pid || now.created != selected.created)
        throw std::invalid_argument("Window identity expired");
    now.id = selected.id;
    return now;
}
JsonObject Broker::Action(JsonObject const &request)
{
    auto action = RequiredText(request, L"action", 32);
    auto now = ResolveWindow(RequiredText(request, L"windowId"));
    JsonObject o;
    Text(o,L"action",action);
    if(action==L"inspect") {
        Flag(o,L"accepted",true);Text(o,L"hwnd",std::to_wstring(reinterpret_cast<uintptr_t>(now.hwnd)));
        Text(o,L"pid",std::to_wstring(now.pid));Text(o,L"created",std::to_wstring(now.created));Text(o,L"path",now.path);
        return o;
    }
    if(action!=L"close" && action!=L"minimize" && action!=L"restore" && action!=L"activate")throw std::invalid_argument("Unsupported window action");
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,now.pid)};
    HANDLE rawToken=nullptr;TOKEN_ELEVATION elevation{};DWORD count=0;
    bool known=process.value&&OpenProcessToken(process,TOKEN_QUERY,&rawToken);
    Handle token{rawToken};
    known=known&&GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&count);
    if(!known || elevation.TokenIsElevated) {
        Flag(o,L"accepted",false);Flag(o,L"needsElevation",true);
        Text(o,L"status",L"elevation_required");return o;
    }
    auto result=magic::window_action::Perform(now.hwnd,action);
    Flag(o,L"accepted",result.accepted);Flag(o,L"needsElevation",result.error==ERROR_ACCESS_DENIED);
    Flag(o,L"restored",result.restored);Flag(o,L"activated",result.activated);Flag(o,L"minimized",result.minimized);
    Flag(o,L"closeRequested",action==L"close"&&result.accepted);
    Number(o,L"win32Error",result.error);Text(o,L"status",result.accepted?(action==L"close"?L"close_requested":L"completed"):L"state_not_reached");
    return o;
}
void Broker::Pump(HANDLE parent)
{
    HANDLE waits[]{parent, stop};
    bool done = false;
    while (!done)
    {
        DWORD result = MsgWaitForMultipleObjectsEx(2, waits, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1)
            break;
        if (result == WAIT_FAILED)
            winrt::throw_last_error();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                done = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    SetEvent(stop);
}
} // namespace magic::system
