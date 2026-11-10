#include "providers.h"
#include "notification-removal.h"
#include <appmodel.h>
#include <fstream>
#include <filesystem>
#include <set>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.ApplicationModel.h>
namespace magic::system
{
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;
struct NotificationProvider::Impl
{
    using Offered = NotificationIdentity;
    std::wstring dataDirectory;
    std::shared_ptr<ChangeState> state;
    UserNotificationListener listener{nullptr};
    UserNotificationListener::NotificationChanged_revoker changed;
    std::shared_ptr<std::atomic<bool>> active = std::make_shared<std::atomic<bool>>(false);
    std::map<std::wstring, Offered> offered;
    bool fallback = false;
    DWORD interval = 2000;
    HRESULT subscriptionError = S_OK, pollError = S_OK;
    DWORD EffectiveInterval() const
    {
        SYSTEM_POWER_STATUS power{};
        return GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1 ? std::max(interval, DWORD(5000)) : interval;
    }
    ULONGLONG nextPoll = 0;
    uint64_t polls = 0;
    std::string fingerprint;
    static NotificationIdentity Identity(UserNotification const &value)
    {
        auto app = value.AppInfo();
        return {value.Id(), value.CreationTime().time_since_epoch().count(), app ? std::wstring(app.AppUserModelId()) : L""};
    }
    std::vector<NotificationIdentity> Snapshot()
    {
        auto values = Await(listener.GetNotificationsAsync(NotificationKinds::Toast), state, GetTickCount64() + 4000);
        std::vector<NotificationIdentity> result;
        for (auto const &value : values) result.push_back(Identity(value));
        return result;
    }
    JsonObject RemoveSnapshot(std::vector<NotificationIdentity> const &targets)
    {
        auto result = RemoveNotificationSnapshot(targets, [&](uint32_t id) -> NotificationLookup {
            try { auto value = listener.GetNotification(id); return {value ? std::optional(Identity(value)) : std::nullopt}; }
            catch (winrt::hresult_error const &e) { if(e.code()==E_OUTOFMEMORY)throw; return {{},e.code().value}; }
        }, [&](uint32_t id) -> int32_t {
            try { listener.RemoveNotification(id); return 0; }
            catch (winrt::hresult_error const &e) { if(e.code()==E_OUTOFMEMORY)throw; return e.code().value; }
        }, [&]() { return Snapshot(); });
        state->Notify(16);
        JsonObject response;
        Flag(response, L"accepted", result.remaining == 0);
        response.Insert(L"remaining", JsonValue::CreateNumberValue(result.remaining));
        response.Insert(L"removed", JsonValue::CreateNumberValue(result.removed));
        response.Insert(L"errorCode", JsonValue::CreateNumberValue(result.error));
        return response;
    }
    explicit Impl(std::shared_ptr<ChangeState> s, std::wstring directory)
        : dataDirectory(std::move(directory)), state(std::move(s))
    {
    }
    void Stop()
    {
        *active = false;
        fallback = false;
        nextPoll = 0;
        subscriptionError = pollError = S_OK;
        std::string{}.swap(fingerprint);
        changed.revoke();
        offered.clear();
        listener = nullptr;
    }
    ~Impl()
    {
        Stop();
    }
    bool Consent(std::wstring const &family)
    {
        if (dataDirectory.empty())
            return false;
        auto path = std::filesystem::path(dataDirectory) / L"notification-consent.json";
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4096)
            return false;
        std::string text(size_t(size.QuadPart), '\0');
        DWORD read = 0;
        if (!ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr) || read != text.size())
            return false;
        try
        {
            auto record = JsonObject::Parse(winrt::to_hstring(text));
            if (record.GetNamedString(L"protocol", L"") != L"Magic.Notification.Consent/1" ||
                record.GetNamedString(L"family", L"") != family ||
                record.GetNamedString(L"flow", L"") != L"explicit-user-interface")
                return false;
            Handle token;
            Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE);
            DWORD bytes = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
            std::vector<BYTE> user(bytes);
            Check(GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes) != FALSE);
            LPWSTR sid = nullptr;
            Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid) != FALSE);
            bool matches = record.GetNamedString(L"userSid", L"") == sid;
            LocalFree(sid);
            return matches;
        }
        catch (...)
        {
            return false;
        }
    }
    UserNotificationListenerAccessStatus Access()
    {
        if (!listener)
            listener = UserNotificationListener::Current();
        return listener.GetAccessStatus();
    }
    JsonObject Health() const
    {
        JsonObject result;
        Number(result, L"callbacks", changed ? 1 : 0);
        Number(result, L"cachedNotifications", double(offered.size()));
        Flag(result, L"fallbackPolling", fallback);
        Number(result, L"pollIntervalMs", EffectiveInterval());
        Number(result, L"subscriptionError", subscriptionError);
        Number(result, L"pollError", pollError);
        Number(result, L"polls", double(polls));
        return result;
    }
};
NotificationProvider::NotificationProvider(std::shared_ptr<ChangeState> state, std::wstring directory)
    : impl(std::make_unique<Impl>(std::move(state), std::move(directory)))
{
}
NotificationProvider::~NotificationProvider() = default;
JsonObject NotificationProvider::Health() const
{
    return impl->Health();
}
JsonObject NotificationProvider::Status()
{
    JsonObject result = impl->Health();
    UINT32 length = 0;
    LONG package = GetCurrentPackageFullName(&length, nullptr);
    bool packaged = package == ERROR_SUCCESS || package == ERROR_INSUFFICIENT_BUFFER;
    Flag(result, L"hasPackageIdentity", packaged);
    UINT32 familyLength = 0;
    bool independent = false;
    std::wstring currentFamily;
    if (GetCurrentPackageFamilyName(&familyLength, nullptr) == ERROR_INSUFFICIENT_BUFFER && familyLength < 512)
    {
        std::wstring family(familyLength, L'\0');
        if (GetCurrentPackageFamilyName(&familyLength, family.data()) == ERROR_SUCCESS)
        {
            independent = family.rfind(L"MagicCommunity.Native_", 0) == 0;
            family.resize(wcslen(family.c_str()));
            currentFamily = family;
        }
    }
    Flag(result, L"independentIdentity", independent);
    bool unpackaged = package == APPMODEL_ERROR_NO_PACKAGE;
    if (unpackaged)
        currentFamily = L"MagicCommunity.Unpackaged";
    Text(result, L"consentScope", currentFamily);
    if (!independent && !unpackaged)
    {
        impl->Stop();
        Flag(result, L"available", false);
        Text(result, L"access", L"independent-identity-required");
        Flag(result, L"authorizationRequested", false);
        return result;
    }
    try
    {
        auto access = impl->Access();
        if (access != UserNotificationListenerAccessStatus::Allowed)
            impl->Stop();
        bool consent = impl->Consent(currentFamily);
        Flag(result, L"communityConsent", consent);
        Flag(result, L"available", consent && access == UserNotificationListenerAccessStatus::Allowed);
        Flag(result, L"canClearAll", consent && access == UserNotificationListenerAccessStatus::Allowed);
        Text(result, L"windowsAccess",
             access == UserNotificationListenerAccessStatus::Allowed  ? L"allowed"
             : access == UserNotificationListenerAccessStatus::Denied ? L"denied"
                                                                      : L"unspecified");
        if (!consent)
        {
            impl->Stop();
            Text(result, L"access", L"community-consent-required");
            Flag(result, L"authorizationRequested", false);
            return result;
        }
        Text(result, L"access",
             access == UserNotificationListenerAccessStatus::Allowed  ? L"allowed"
             : access == UserNotificationListenerAccessStatus::Denied ? L"denied"
                                                                      : L"unspecified");
    }
    catch (winrt::hresult_error const &error)
    {
        Flag(result, L"available", false);
        Text(result, L"access", L"unavailable");
        Number(result, L"errorCode", error.code().value);
        Text(result, L"reason", std::wstring(error.message()).substr(0, 512));
        impl->Stop();
    }
    for (auto const &entry : impl->Health())
        result.Insert(entry.Key(), entry.Value());
    Flag(result, L"authorizationRequested", false);
    return result;
}
JsonObject NotificationProvider::Read()
{
    auto status = Status();
    JsonArray rows;
    if (!status.GetNamedBoolean(L"available", false))
    {
        rows.Clear();
        status.Insert(L"notifications", rows);
        return status;
    }
    auto all =
        Await(impl->listener.GetNotificationsAsync(NotificationKinds::Toast), impl->state, GetTickCount64() + 4000);
    std::map<std::wstring, Impl::Offered> offered;
    size_t budget = 0;
    bool truncated = false;
    for (auto const &notification : all)
    {
        if (offered.size() >= 256)
            break;
        try
        {
            auto created = notification.CreationTime().time_since_epoch().count();
            auto app = notification.AppInfo();
            std::wstring appId = app ? std::wstring(app.AppUserModelId()) : L"";
            std::wstring id = L"notification:" + std::to_wstring(notification.Id()) + L":" + std::to_wstring(created);
            offered.emplace(id, Impl::Offered{notification.Id(), created, appId});
            JsonObject row;
            Text(row, L"id", id);
            Text(row, L"createdFileTime", std::to_wstring(created));
            Text(row, L"appId", appId.substr(0, 512));
            if (app)
                Text(row, L"appName", std::wstring(app.DisplayInfo().DisplayName()).substr(0, 512));
            JsonArray text;
            // A toast carries one or more bindings, and ToastGeneric is only one of
            // them: an app that still ships a legacy template puts its words in a
            // binding this lookup never returns, so the row arrived with no text at
            // all and the centre had nothing to draw. Read every binding instead.
            auto visual = notification.Notification().Visual();
            std::set<std::wstring> seen;
            auto appendText = [&](winrt::Windows::UI::Notifications::NotificationBinding const &binding) {
                for (auto const &element : binding.GetTextElements())
                {
                    if (text.Size() >= 12)
                        return;
                    auto value = std::wstring(element.Text());
                    if (value.empty() || !seen.insert(value).second)
                        continue;
                    text.Append(JsonValue::CreateStringValue(winrt::hstring(value.substr(0, 1024))));
                }
            };
            if (auto generic = visual.GetBinding(KnownNotificationBindings::ToastGeneric()))
                appendText(generic);
            for (auto const &binding : visual.Bindings())
                appendText(binding);
            row.Insert(L"text", text);
            budget += winrt::to_string(row.Stringify()).size();
            if (budget > 750000)
            {
                offered.erase(id);
                truncated = true;
                break;
            }
            rows.Append(row);
        }
        catch (winrt::hresult_error const &)
        {
        }
    }
    impl->offered.swap(offered);
    Flag(status, L"truncated", truncated);
    status.Insert(L"notifications", rows);
    return status;
}
JsonObject NotificationProvider::Subscribe(bool enabled, DWORD intervalMs)
{
    if (!enabled)
    {
        impl->Stop();
        return impl->Health();
    }
    if (intervalMs < 2000 || intervalMs > 60000)
        throw std::invalid_argument("Notification fallback interval outside 2000..60000 ms");
    if (impl->fallback && impl->interval != intervalMs)
        impl->nextPoll = GetTickCount64() + std::max(intervalMs, DWORD(2000));
    impl->interval = intervalMs;
    auto status = Status();
    if (!status.GetNamedBoolean(L"available", false))
        return status;
    if (!impl->changed && !impl->fallback)
    {
        impl->active = std::make_shared<std::atomic<bool>>(true);
        auto state = impl->state;
        auto active = impl->active;
        try
        {
            impl->changed =
                impl->listener.NotificationChanged(winrt::auto_revoke, [state, active](auto const &, auto const &) {
                    if (*active)
                        state->Notify(16);
                });
        }
        catch (winrt::hresult_error const &error)
        {
            if (impl->Access() != UserNotificationListenerAccessStatus::Allowed || error.code() == E_OUTOFMEMORY)
                throw;
            impl->subscriptionError = error.code().value;
            impl->fallback = true;
            impl->nextPoll = GetTickCount64() + impl->EffectiveInterval();
        }
    }
    return impl->Health();
}
DWORD NotificationProvider::NextPollDelay() const
{
    if (!impl->fallback)
        return INFINITE;
    auto now = GetTickCount64();
    return impl->nextPoll > now ? DWORD(std::min<ULONGLONG>(impl->nextPoll - now, 60000)) : 0;
}
void NotificationProvider::PollIfDue()
{
    if (NextPollDelay() != 0)
        return;
    impl->nextPoll = GetTickCount64() + impl->EffectiveInterval();
    ++impl->polls;
    JsonObject snapshot;
    try
    {
        snapshot = Read();
        if (impl->pollError != S_OK) impl->state->Notify(16);
        impl->pollError = S_OK;
    }
    catch (winrt::hresult_error const &error)
    {
        if (error.code() == E_OUTOFMEMORY) throw;
        if (error.code() == E_ACCESSDENIED) { impl->Stop(); impl->state->Notify(16); return; }
        if (impl->pollError == S_OK) impl->state->Notify(16);
        impl->pollError = error.code().value;
        impl->nextPoll = GetTickCount64() + 30000;
        return;
    }
    if (!snapshot.GetNamedBoolean(L"available", false))
    {
        impl->state->Notify(16);
        return;
    }
    auto next = winrt::to_string(snapshot.GetNamedString(L"access", L"unavailable")) + ":" +
                winrt::to_string(snapshot.GetNamedArray(L"notifications", JsonArray{}).Stringify());
    if (next != impl->fingerprint)
    {
        impl->fingerprint = std::move(next);
        impl->state->Notify(16);
    }
}
JsonObject NotificationProvider::Remove(JsonObject const &args)
{
    auto id = RequiredText(args, L"notificationId", 128);
    if (!Status().GetNamedBoolean(L"available", false))
        winrt::throw_hresult(E_ACCESSDENIED);
    auto offered = impl->offered.find(id);
    if (offered == impl->offered.end())
        throw std::invalid_argument("Notification identity expired");
    if (impl->Access() != UserNotificationListenerAccessStatus::Allowed)
        throw std::invalid_argument("Notification access revoked");
    auto result = impl->RemoveSnapshot({offered->second});
    if (result.GetNamedBoolean(L"accepted", false)) impl->offered.erase(offered);
    return result;
}
JsonObject NotificationProvider::Clear(JsonObject const &args)
{
    // Offered only as an explicit all-notifications action; never on subscribe,
    // refresh, exit or dismissal of an individual notification.
    if (!args.GetNamedBoolean(L"confirmed", false))
        throw std::invalid_argument("Clearing notifications requires explicit confirmation");
    auto status = Status();
    if (!status.GetNamedBoolean(L"available", false) || !status.GetNamedBoolean(L"canClearAll", false))
        winrt::throw_hresult(E_ACCESSDENIED);
    // ClearNotifications fails with ERROR_NOT_FOUND on affected unpackaged
    // installations even though enumeration succeeds. Use the listener's
    // individual removal path and verify the actual Windows state instead.
    auto result = impl->RemoveSnapshot(impl->Snapshot());
    if (result.GetNamedBoolean(L"accepted", false)) impl->offered.clear();
    return result;
}
} // namespace magic::system
