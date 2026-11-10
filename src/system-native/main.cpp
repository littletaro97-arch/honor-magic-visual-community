#include "providers.h"
#include "configuration.h"
#include <iostream>
#include <fstream>
#include <filesystem>
namespace magic::system
{
struct Endpoint
{
    Broker &broker;
    std::shared_ptr<TrayProvider> tray;
    std::shared_ptr<AudioProvider> audio, microphone;
    std::shared_ptr<BrightnessProvider> brightness;
    std::shared_ptr<MixerProvider> mixer;
    std::shared_ptr<MediaProvider> media;
    std::shared_ptr<WifiProvider> wifi;
    std::shared_ptr<BluetoothProvider> bluetooth;
    std::shared_ptr<NotificationProvider> notifications;
    std::shared_ptr<ConfigurationProvider> configuration;
    std::shared_ptr<MemoryGovernor> memory;
    HANDLE parent;
    Handle pipe;
    DWORD parentPid;
    bool trusted = false;
    struct Timing
    {
        std::atomic<DWORD> interval{1000};
        std::atomic<uint64_t> lastSample{0};
        std::recursive_mutex notificationGate, wifiGate, bluetoothGate;
    };
    std::shared_ptr<Timing> timing;
    bool eventChannel = false, urgentChannel = false;
    int deviceChannel = 0;
    Endpoint(Broker &b, HANDLE p, DWORD pid, std::wstring const &name, std::wstring const &dataDirectory,
             Endpoint *owner = nullptr, bool urgent = false, int devices = 0)
        : broker(b), tray(owner ? owner->tray : std::make_shared<TrayProvider>(b.wake, pid)),
          audio(owner ? owner->audio : std::make_shared<AudioProvider>(b.wake)),
          brightness(owner ? owner->brightness : std::make_shared<BrightnessProvider>(b.wake)),
          microphone(owner ? owner->microphone : std::make_shared<AudioProvider>(b.wake, true)),
          mixer(owner ? owner->mixer : std::make_shared<MixerProvider>(b.wake)),
          media(owner ? owner->media : std::make_shared<MediaProvider>(b.wake)),
          wifi(owner ? owner->wifi : std::make_shared<WifiProvider>(b.wake)),
          bluetooth(owner ? owner->bluetooth : std::make_shared<BluetoothProvider>(b.wake)),
          notifications(owner ? owner->notifications : std::make_shared<NotificationProvider>(b.wake, dataDirectory)),
          configuration(owner ? owner->configuration : std::make_shared<ConfigurationProvider>(dataDirectory, b.wake)),
          memory(owner ? owner->memory : std::make_shared<MemoryGovernor>()),
          parent(p), parentPid(pid), timing(owner ? owner->timing : std::make_shared<Timing>()),
          eventChannel(owner != nullptr && !urgent && !devices), urgentChannel(urgent), deviceChannel(devices)
    {
        Handle token;
        Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE);
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<BYTE> user(bytes);
        Check(GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes) != FALSE);
        LPWSTR sid = nullptr;
        Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid) != FALSE);
        std::wstring dacl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                   nullptr) != FALSE);
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        pipe.value = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(),
                                      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                      65536, 65536, 0, &security);
        DWORD error = GetLastError();
        LocalFree(descriptor);
        if (pipe.value == INVALID_HANDLE_VALUE)
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    bool Complete(OVERLAPPED &ov, DWORD &count, DWORD timeout)
    {
        HANDLE waits[]{ov.hEvent, parent, broker.stop};
        DWORD result = WaitForMultipleObjects(3, waits, FALSE, timeout);
        if (result != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &ov);
            WaitForSingleObject(ov.hEvent, INFINITE);
            return false;
        }
        return GetOverlappedResult(pipe, &ov, &count, FALSE) != FALSE;
    }
    bool Connect()
    {
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED ov{};
        ov.hEvent = event;
        DWORD count = 0;
        if (ConnectNamedPipe(pipe, &ov))
            return true;
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
            return true;
        if (error != ERROR_IO_PENDING)
            return false;
        return Complete(ov, count, INFINITE);
    }
    bool Transfer(void *buffer, DWORD count, bool write)
    {
        BYTE *p = static_cast<BYTE *>(buffer);
        DWORD remaining = count;
        while (remaining)
        {
            Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            OVERLAPPED ov{};
            ov.hEvent = event;
            DWORD done = 0;
            BOOL result = write ? WriteFile(pipe, p, remaining, &done, &ov) : ReadFile(pipe, p, remaining, &done, &ov);
            if (!result)
            {
                if (GetLastError() != ERROR_IO_PENDING || !Complete(ov, done, write ? 5000 : INFINITE))
                    return false;
            }
            if (!done)
                return false;
            p += done;
            remaining -= done;
        }
        return true;
    }
    bool Read(JsonObject &value)
    {
        uint32_t length = 0;
        if (!Transfer(&length, 4, false) || !length || length > FrameLimit)
            return false;
        std::string data(length, '\0');
        if (!Transfer(data.data(), length, false))
            return false;
        try
        {
            value = JsonObject::Parse(winrt::to_hstring(data));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    bool Write(JsonObject const &value)
    {
        auto data = winrt::to_string(value.Stringify());
        if (data.empty() || data.size() > FrameLimit)
        {
            JsonObject bounded;
            Text(bounded, L"protocol", Protocol);
            Text(bounded, L"id", value.GetNamedString(L"id", L""));
            Flag(bounded, L"ok", false);
            Text(bounded, L"error", L"Response exceeds the bounded payload; use a narrower query");
            data = winrt::to_string(bounded.Stringify());
        }
        uint32_t length = uint32_t(data.size());
        return Transfer(&length, 4, true) && Transfer(data.data(), length, true);
    }
    void WaitForChanges(DWORD timeout)
    {
        // Pending read detects disconnect without an idle polling timer.
        // Clients serialize requests; pipelined bytes fail closed.
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED operation{};
        operation.hEvent = event;
        BYTE unexpected = 0;
        DWORD count = 0;
        if (ReadFile(pipe, &unexpected, 1, &count, &operation) || GetLastError() != ERROR_IO_PENDING)
        {
            broker.RequestStop();
            throw std::runtime_error("Client disconnected or pipelined a request");
        }
        HANDLE waits[]{broker.changed, parent, broker.stop, event};
        DWORD result = WaitForMultipleObjects(4, waits, FALSE, timeout);
        CancelIoEx(pipe, &operation);
        BOOL completed = GetOverlappedResult(pipe, &operation, &count, TRUE);
        DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        if (result == WAIT_OBJECT_0 + 3 || result == WAIT_OBJECT_0 + 1 || result == WAIT_OBJECT_0 + 2 ||
            result == WAIT_FAILED || completed || error != ERROR_OPERATION_ABORTED)
        {
            broker.RequestStop();
            throw std::runtime_error("Client or parent disconnected while awaiting changes");
        }
    }
    JsonObject Health()
    {
        auto health = broker.Health();
        health.Insert(L"tray", tray->Health());
        health.Insert(L"memory", memory->Health());
        health.Insert(L"audio", audio->Health());
        health.Insert(L"brightness", brightness->Health());
        health.Insert(L"microphone", microphone->Health());
        health.Insert(L"mixer", mixer->Health());
        health.Insert(L"media", media->Health());
        { std::unique_lock lock(timing->wifiGate, std::try_to_lock); if(lock.owns_lock())health.Insert(L"wifi", wifi->Health()); }
        { std::unique_lock lock(timing->bluetoothGate, std::try_to_lock); if(lock.owns_lock())health.Insert(L"bluetooth", bluetooth->Health()); }
        {
            std::unique_lock lock(timing->notificationGate, std::try_to_lock);
            if (lock.owns_lock())
                health.Insert(L"notifications", notifications->Health());
            else
            {
                JsonObject busy;
                Flag(busy, L"busy", true);
                health.Insert(L"notifications", busy);
            }
        }
        return health;
    }
    void Unsubscribe()
    {
        // Unsubscribe reverses freezes before unrelated providers are detached.
        memory->ResumeAll();
        {
            JsonObject args;
            Flag(args, L"enabled", false);
            tray->Subscribe(args);
        }
        audio->Subscribe(false);
        brightness->Subscribe(false);
        microphone->Subscribe(false);
        mixer->Subscribe(false);
        media->Subscribe(false);
        { std::scoped_lock lock(timing->wifiGate); wifi->Subscribe(false); }
        { std::scoped_lock lock(timing->bluetoothGate); bluetooth->Subscribe(false); }
        {
            std::scoped_lock lock(timing->notificationGate);
            notifications->Subscribe(false);
        }
        broker.ConfigureMetrics(false, false);
        broker.ConfigureShortcuts(false);
        broker.Configure(false, false);
        broker.wake->Notify(1);
    }
    void PollNotifications()
    {
        std::unique_lock lock(timing->notificationGate, std::try_to_lock);
        if (lock.owns_lock())
            notifications->PollIfDue();
    }
    DWORD NotificationDelay()
    {
        std::unique_lock lock(timing->notificationGate, std::try_to_lock);
        return lock.owns_lock() ? notifications->NextPollDelay() : INFINITE;
    }
    JsonObject Call(JsonObject const &request)
    {
        if (request.GetNamedString(L"protocol", L"") != Protocol)
            throw std::invalid_argument("Unsupported protocol");
        auto op = RequiredText(request, L"operation", 32);
        auto args = request.GetNamedObject(L"arguments", JsonObject{});
        // Memory governance is deliberately unavailable on urgent/event/device channels.
        bool memoryOperation = op == L"memory-health" || op == L"memory-clean" ||
                               op == L"process-freeze" || op == L"process-resume";
        if (memoryOperation && (eventChannel || urgentChannel || deviceChannel))
            throw std::invalid_argument("Memory operations require the ordinary control channel");
        int deviceOperation=op.rfind(L"wifi-",0)==0?1:op.rfind(L"bluetooth-",0)==0?2:0;
        if(deviceChannel!=deviceOperation)throw std::invalid_argument("Device operations require the independent device channel");
        std::unique_lock<std::recursive_mutex> deviceLock(deviceOperation==2?timing->bluetoothGate:timing->wifiGate,std::defer_lock);if(deviceOperation)deviceLock.lock();
        if (urgentChannel && op != L"window-action" && op != L"input-target" && op != L"input-read" && op != L"input-select" && op != L"input-profile-select" &&
            op != L"health-fast")
            throw std::invalid_argument("Urgent channel accepts only bounded interactive actions");
        if (op == L"health-fast")
            return broker.Health();
        if (eventChannel && op != L"next")
            throw std::invalid_argument("Event channel accepts only next");
        if (!eventChannel && op == L"next")
            throw std::invalid_argument("Use the independent event channel");
        // One shared governor across endpoint adapters owns all frozen process handles.
        if (op == L"memory-health")
            return memory->Health();
        if (op == L"memory-clean")
            return memory->Clean(args);
        if (op == L"process-freeze")
            return memory->Freeze(args, parentPid, parent);
        if (op == L"process-resume")
            return memory->Resume(args);
        if (op == L"configuration-catalog")
            return ConfigurationProvider::Catalog();
        if (op == L"configuration-launch-targets")
            return configuration->LaunchTargets();
        if (op == L"configuration-read")
            return configuration->Read();
        if (op == L"configuration-write")
            return configuration->Write(args);
        if (op == L"wifi-read")
            return wifi->Read(args.GetNamedBoolean(L"networkDetails", false));
        if (op == L"wifi-health")
            return wifi->Health();
        if (op == L"wifi-profiles")
            return wifi->Profiles(args);
        if (op == L"wifi-action")
            return wifi->Action(args);
        if (op == L"wifi-subscribe")
            return wifi->Subscribe(args.GetNamedBoolean(L"enabled", false));
        if (op == L"bluetooth-read")
            return bluetooth->Read();
        if (op == L"bluetooth-health")
            return bluetooth->Health();
        if (op == L"bluetooth-subscribe")
            return bluetooth->Subscribe(args.GetNamedBoolean(L"enabled", false));
        if (op == L"bluetooth-action")
            return bluetooth->Action(args);
        if (op == L"bluetooth-unpair")
            return bluetooth->Unpair(args);
        if (op.starts_with(L"notifications-"))
        {
            std::scoped_lock lock(timing->notificationGate);
            if (op == L"notifications-status")
                return notifications->Status();
            if (op == L"notifications-read")
                return notifications->Read();
            if (op == L"notifications-health")
                return notifications->Health();
            if (op == L"notifications-subscribe")
            {
                double interval = args.GetNamedNumber(L"intervalMs", 2000);
                if (interval < 2000 || interval > 60000 || interval != double(DWORD(interval)))
                    throw std::invalid_argument("Invalid notification interval");
                auto before = notifications->Health();
                auto result = notifications->Subscribe(args.GetNamedBoolean(L"enabled", false), DWORD(interval));
                auto after = notifications->Health();
                // A consumer may reassert demand after a notification event.
                // Only registration transitions may wake it; acknowledging the
                // same subscription must not manufacture the next event.
                if(before.GetNamedNumber(L"callbacks")!=after.GetNamedNumber(L"callbacks")||
                   before.GetNamedBoolean(L"fallbackPolling")!=after.GetNamedBoolean(L"fallbackPolling"))
                    broker.wake->Notify(16);
                return result;
            }
            if (op == L"notifications-remove")
                return notifications->Remove(args);
            if (op == L"notifications-clear")
                return notifications->Clear(args);
        }
        if (op == L"mixer-read")
            return mixer->Read();
        if (op == L"mixer-subscribe")
            return mixer->Subscribe(args.GetNamedBoolean(L"enabled", false));
        if (op == L"mixer-action")
            return mixer->Set(args);
        if (op == L"microphone-read")
            return microphone->Read();
        if (op == L"microphone-subscribe")
            return microphone->Subscribe(args.GetNamedBoolean(L"enabled", false));
        if (op == L"microphone-action")
            return microphone->Set(args);
        if (op == L"audio-default-device")
        {
            auto result = SetAudioDefault(args);
            broker.wake->Notify(2);
            return result;
        }
        if(op==L"brightness-read")return brightness->Read(args.GetNamedBoolean(L"refresh",false));
        if(op==L"brightness-subscribe")return brightness->Subscribe(args.GetNamedBoolean(L"enabled",false));
        if(op==L"brightness-action")return brightness->Set(args);
        if (op == L"audio-read")
            return audio->Read();
        if (op == L"audio-health")
            return audio->Health();
        if (op == L"audio-subscribe")
            return audio->Subscribe(args.GetNamedBoolean(L"enabled", false));
        if (op == L"audio-action")
            return audio->Set(args);
        if (op == L"media-read")
            return media->Read();
        if (op == L"media-health")
            return media->Health();
        if (op == L"media-subscribe")
            return media->Subscribe(args.GetNamedBoolean(L"enabled", false));
        if (op == L"media-action")
            return media->Command(args);
        if (op == L"media-artwork")
            return media->Artwork(args);
        if (op == L"tray-refresh")
        {
            broker.wake->Notify(512);
            return tray->Health();
        }
        if (op == L"tray-read")
            return tray->Read();
        if (op == L"tray-subscribe")
            return tray->Subscribe(args);
        if (op == L"tray-action")
            return tray->Action(args);
        if (op == L"tray-shell-inspect")
            return InspectShellTrayCompatibility();
        if (op == L"tray-inspect")
            return InspectWindowsTray();
        if (op == L"input-profiles")
            return InputProfiles();
        if (op == L"input-profile-select")
            return InputProfileSelect(args, broker);
        if (op == L"input-target")
            return broker.CaptureInputTarget(static_cast<DWORD>(args.GetNamedNumber(L"popupProcessId",0)));
        if (op == L"input-read")
            return InputLayouts(args.HasKey(L"windowId") ? broker.ResolveInputWindow(RequiredText(args,L"windowId")).hwnd : nullptr);
        if (op == L"input-select")
            return InputSelect(args, broker);
        if (op == L"health")
            return Health();
        if (op == L"shortcuts-config")
        {
            bool win=args.GetNamedBoolean(L"win"), winA=args.GetNamedBoolean(L"winA");
            if((win||winA)&&!args.GetNamedBoolean(L"production",false))
                throw std::invalid_argument("Shortcut capture requires explicit production selection");
            broker.SetShortcutModes(win,winA);
            return broker.Health();
        }
        if (op == L"shortcuts-subscribe")
        {
            if (args.GetNamedBoolean(L"enabled", false) && !args.GetNamedBoolean(L"production", false))
                throw std::invalid_argument("Shortcut capture requires explicit production selection");
            broker.ConfigureShortcuts(args.GetNamedBoolean(L"enabled", false));
            return broker.Health();
        }
        if (op == L"subscribe")
        {
            double n = args.GetNamedNumber(L"intervalMs", 1000);
            if (n < 1000 || n > 60000 || n != double(DWORD(n)))
                throw std::invalid_argument("Sample interval must be 1000..60000 ms");
            broker.ConfigureMetrics(args.GetNamedBoolean(L"gpu", false), args.GetNamedBoolean(L"storage", false));
            timing->interval = DWORD(n);
            bool system=args.GetNamedBoolean(L"system", false);
            // Reordering the bar republishes its subscription. Keep the sample
            // deadline; an unchanged demand must not manufacture another sample.
            if(!broker.SystemDemand()||!system)timing->lastSample = 0;
            broker.Configure(args.GetNamedBoolean(L"windows", false), system);
            return Health();
        }
        if (op == L"unsubscribe")
        {
            Unsubscribe();
            return Health();
        }
        if (op == L"snapshot"){
            bool system=args.GetNamedBoolean(L"includeSystem", true);auto snapshot=broker.Snapshot(system);
            if(system&&broker.SystemDemand())timing->lastSample=GetTickCount64();
            return snapshot;
        }
        if (op == L"window-action")
            return broker.Action(args);
        if (op == L"next")
        {
            double supplied = args.GetNamedNumber(L"afterRevision", 0), t = args.GetNamedNumber(L"timeoutMs", 5000);
            if (supplied < 0 || supplied > 9007199254740991.0 || t < 1 || t > 10000)
                throw std::invalid_argument("Invalid next cursor or timeout");
            PollNotifications();
            auto after = uint64_t(supplied);
            DWORD timeout = DWORD(t);
            bool due = broker.SystemDemand() && (GetTickCount64() - timing->lastSample >= timing->interval);
            if (broker.Revision() <= after && !due)
            {
                broker.ResetChanged();
                if (broker.Revision() <= after)
                {
                    if (broker.SystemDemand())
                        timeout = std::min(timeout, DWORD(timing->interval -
                                                          std::min<uint64_t>(timing->interval,
                                                                             GetTickCount64() - timing->lastSample)));
                    timeout = std::min(timeout, NotificationDelay());
                    WaitForChanges(timeout);
                }
            }
            PollNotifications();
            due = broker.SystemDemand() && (GetTickCount64() - timing->lastSample >= timing->interval);
            // A false response must never acknowledge a revision that arrived
            // after the decision, otherwise the consumer can skip that event.
            auto revision = broker.Revision();
            bool changed = revision > after || due;
            JsonObject result;
            Flag(result, L"changed", changed);
            Number(result, L"revision", double(revision));
            if (changed)
            {
                if (due)
                    timing->lastSample = GetTickCount64();
                unsigned mask = broker.wake->topics.exchange(0);
                JsonArray topics;
                if (mask & 1)
                    topics.Append(JsonValue::CreateStringValue(L"windows-system"));
                if (mask & 2)
                    topics.Append(JsonValue::CreateStringValue(L"audio"));
                if (mask & 4)
                    topics.Append(JsonValue::CreateStringValue(L"media"));
                if (mask & 32)
                    topics.Append(JsonValue::CreateStringValue(L"configuration"));
                if (mask & 16)
                    topics.Append(JsonValue::CreateStringValue(L"notifications"));
                if (mask & 1024)
                    topics.Append(JsonValue::CreateStringValue(L"shortcuts"));
                if (mask & 512)
                    topics.Append(JsonValue::CreateStringValue(L"tray"));
                if (mask & 256)
                    topics.Append(JsonValue::CreateStringValue(L"input"));
                if (mask & 64)
                    topics.Append(JsonValue::CreateStringValue(L"wifi"));
                if (mask & 128)
                    topics.Append(JsonValue::CreateStringValue(L"bluetooth"));
                if(mask & 2048)topics.Append(JsonValue::CreateStringValue(L"brightness"));
                result.Insert(L"changedTopics", topics);
                result.Insert(L"snapshot", broker.Snapshot(due));
            }
            return result;
        }
        if (op == L"stop")
        {
            Unsubscribe();
            JsonObject result;
            Flag(result, L"stopping", true);
            return result;
        }
        throw std::invalid_argument("Unsupported operation");
    }
    void Serve()
    {
        while (WaitForSingleObject(broker.stop, 0) == WAIT_TIMEOUT && WaitForSingleObject(parent, 0) == WAIT_TIMEOUT)
        {
            if (!Connect())
                break;
            ULONG pid = 0;
            if (!GetNamedPipeClientProcessId(pipe, &pid) || pid != parentPid)
            {
                DisconnectNamedPipe(pipe);
                continue;
            }
            trusted = true;
            JsonObject request;
            while (Read(request))
            {
                JsonObject response;
                Text(response, L"protocol", Protocol);
                Text(response, L"id", request.GetNamedString(L"id", L""));
                bool stopping = false;
                try
                {
                    auto result = Call(request);
                    Flag(response, L"ok", true);
                    response.Insert(L"result", result);
                    stopping = request.GetNamedString(L"operation", L"") == L"stop";
                }
                catch (winrt::hresult_error const &error)
                {
                    Flag(response, L"ok", false);
                    Text(response, L"error", std::wstring(error.message()).substr(0, 512));
                    Number(response, L"errorCode", error.code().value);
                }
                catch (std::exception const &e)
                {
                    Flag(response, L"ok", false);
                    Text(response, L"error", std::wstring(winrt::to_hstring(e.what())));
                }
                if (!Write(response) || stopping)
                    break;
            }
            DisconnectNamedPipe(pipe);
            trusted = false;
            // Losing an optional request channel must not shut down unrelated
            // providers or the bar. A new request gets a new authenticated pipe
            // connection; never replay the previous command here.
            if (!deviceChannel && !urgentChannel) break;
        }
        if(!deviceChannel && !urgentChannel)broker.RequestStop();
    }
};
} // namespace magic::system
int wmain(int argc, wchar_t **argv)
{
    using namespace magic::system;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (argc == 3 && std::wstring(argv[1]) == L"--identity-probe")
        {
            Broker probe;
            NotificationProvider notifications(probe.wake);
            auto status = notifications.Status();
            JsonObject result;
            Text(result, L"protocol", Protocol);
            Text(result, L"implementation", L"independent-win32");
            result.Insert(L"notificationStatus", status);
            Flag(result, L"authorizationRequested", false);
            Flag(result, L"desktopActivated", false);
            if (status.GetNamedBoolean(L"available", false))
            {
                auto subscription = notifications.Subscribe(true);
                result.Insert(L"subscription", subscription);
                auto read = notifications.Read();
                Number(result, L"notificationCount", read.GetNamedArray(L"notifications").Size());
                notifications.Subscribe(false);
                result.Insert(L"released", notifications.Health());
            }
            std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary | std::ios::trunc);
            output << winrt::to_string(result.Stringify());
            return output ? 0 : 3;
        }
        std::map<std::wstring, std::wstring> args;
        for (int i = 1; i < argc; i += 2)
        {
            if (i + 1 >= argc || !args.emplace(argv[i], argv[i + 1]).second)
                throw std::invalid_argument("Invalid arguments");
        }
        if (args.size() != 4 && (args.size() != 5 || !args.contains(L"--data")))
            throw std::invalid_argument("Use an authenticated parent launch");
        auto name = args.at(L"--pipe");
        if (name.size() != 50 || name.substr(0, 18) != L"MagicNativeSystem-" ||
            !std::all_of(name.begin() + 18, name.end(),
                         [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }))
            throw std::invalid_argument("Invalid pipe name");
        auto pidValue = std::stoull(args.at(L"--parent"));
        if (pidValue == 0 || pidValue > MAXDWORD || pidValue == GetCurrentProcessId())
            throw std::invalid_argument("Invalid parent");
        DWORD pid = DWORD(pidValue);
        Handle parent{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
        Check(parent.value != nullptr);
        DWORD currentSession = 0, parentSession = 0;
        Check(ProcessIdToSessionId(pid, &parentSession) &&
              ProcessIdToSessionId(GetCurrentProcessId(), &currentSession));
        if (currentSession != parentSession || Created(parent) != std::stoull(args.at(L"--parent-created")) ||
            _wcsicmp(Image(parent).c_str(), args.at(L"--parent-image").c_str()) != 0)
            throw std::invalid_argument("Parent identity differs");
        Broker broker(pid);
        Endpoint endpoint{broker, parent, pid, name, args.contains(L"--data") ? args.at(L"--data") : L""};
        Endpoint eventEndpoint{
            broker, parent, pid, name + L"-events", args.contains(L"--data") ? args.at(L"--data") : L"", &endpoint};
        Endpoint urgentEndpoint{
            broker,    parent, pid, name + L"-urgent", args.contains(L"--data") ? args.at(L"--data") : L"",
            &endpoint, true};
        Endpoint deviceEndpoint{broker,parent,pid,name+L"-devices",args.contains(L"--data")?args.at(L"--data"):L"",&endpoint,false,1};
        Endpoint bluetoothEndpoint{broker,parent,pid,name+L"-bluetooth",args.contains(L"--data")?args.at(L"--data"):L"",&endpoint,false,2};
        std::thread bluetoothTransport([&]{try{winrt::init_apartment(winrt::apartment_type::multi_threaded);bluetoothEndpoint.Serve();}catch(...){broker.RequestStop();}});
        std::thread deviceTransport([&]{try{winrt::init_apartment(winrt::apartment_type::multi_threaded);deviceEndpoint.Serve();}catch(...){broker.RequestStop();}});
        std::thread urgentTransport([&] {
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                urgentEndpoint.Serve();
            }
            catch (...)
            {
                broker.RequestStop();
            }
        });
        std::thread eventTransport([&] {
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                eventEndpoint.Serve();
            }
            catch (...)
            {
                broker.RequestStop();
            }
        });
        std::thread transport([&] {
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                endpoint.Serve();
            }
            catch (...)
            {
                broker.RequestStop();
            }
        });
        broker.Pump(parent);
        transport.join();
        eventTransport.join();
        urgentTransport.join();
        deviceTransport.join();
        bluetoothTransport.join();
        return 0;
    }
    catch (winrt::hresult_error const &e)
    {
        std::wcerr << e.message().c_str() << L"\n";
        return 1;
    }
    catch (std::exception const &e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
