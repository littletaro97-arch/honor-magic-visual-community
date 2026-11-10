#pragma once
#include "system.h"
#include "memory-governor.h"
#include <winrt/Windows.Foundation.h>

namespace magic::system
{
class TrayProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    TrayProvider(std::shared_ptr<ChangeState>, DWORD authorizedParent);
    ~TrayProvider();
    JsonObject Subscribe(JsonObject const &);
    JsonObject Read();
    JsonObject Action(JsonObject const &);
    JsonObject Health() const;
};
class MixerProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit MixerProvider(std::shared_ptr<ChangeState>);
    ~MixerProvider();
    JsonObject Read();
    JsonObject Set(JsonObject const &);
    JsonObject Subscribe(bool);
    JsonObject Health() const;
};
class BrightnessProvider{
    struct Impl;std::unique_ptr<Impl> impl;
  public: explicit BrightnessProvider(std::shared_ptr<ChangeState>);~BrightnessProvider();
    JsonObject Read(bool refresh=false);JsonObject Subscribe(bool);JsonObject Set(JsonObject const &);JsonObject Health()const;
};
class AudioProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit AudioProvider(std::shared_ptr<ChangeState>, bool capture = false);
    ~AudioProvider();
    JsonObject Read();
    JsonObject Set(JsonObject const &);
    JsonObject Subscribe(bool);
    JsonObject Health() const;
};
class MediaProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit MediaProvider(std::shared_ptr<ChangeState>);
    ~MediaProvider();
    JsonObject Read();
    JsonObject Command(JsonObject const &);
    JsonObject Artwork(JsonObject const &);
    JsonObject Subscribe(bool);
    JsonObject Health() const;
};
class WifiProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit WifiProvider(std::shared_ptr<ChangeState>);
    ~WifiProvider();
    JsonObject Read(bool networkDetails);
    JsonObject Profiles(JsonObject const &);
    JsonObject Action(JsonObject const &);
    JsonObject Subscribe(bool);
    JsonObject Health() const;
};
class BluetoothProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit BluetoothProvider(std::shared_ptr<ChangeState>);
    ~BluetoothProvider();
    JsonObject Read();
    JsonObject Unpair(JsonObject const &);
    JsonObject Action(JsonObject const &);
    JsonObject Subscribe(bool);
    JsonObject Health() const;
};
class NotificationProvider
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit NotificationProvider(std::shared_ptr<ChangeState>, std::wstring dataDirectory = L"");
    ~NotificationProvider();
    JsonObject Status();
    JsonObject Read();
    JsonObject Remove(JsonObject const &);
    JsonObject Clear(JsonObject const &);
    JsonObject Subscribe(bool, DWORD intervalMs = 2000);
    DWORD NextPollDelay() const;
    void PollIfDue();
    JsonObject Health() const;
};
bool AudioPolicyAvailable();
JsonObject SetAudioDefault(JsonObject const &);
JsonObject InspectWindowsTray();
JsonObject InspectShellTrayCompatibility();
JsonObject InputLayouts(HWND foreground = nullptr);
JsonObject InputProfiles();
JsonObject InputProfileSelect(JsonObject const &, Broker &);
JsonObject InputSelect(JsonObject const &, Broker &);

// Completion owns its event until WinRT releases the callback, including after
// timeout/cancellation. A late completion never writes to a closed stack handle.
template <typename T>
T Await(winrt::Windows::Foundation::IAsyncOperation<T> const &operation, std::shared_ptr<ChangeState> const &state,
        ULONGLONG deadline)
{
    auto done = std::make_shared<Handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(done->value != nullptr);
    operation.Completed([done](auto const &, auto) { SetEvent(*done); });
    HANDLE waits[]{*done, state->stop};
    ULONGLONG now = GetTickCount64();
    DWORD remaining = deadline > now ? DWORD(std::min<ULONGLONG>(deadline - now, 4000)) : 0;
    if (WaitForMultipleObjects(2, waits, FALSE, remaining) != WAIT_OBJECT_0)
    {
        operation.Cancel();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.GetResults();
}
} // namespace magic::system
