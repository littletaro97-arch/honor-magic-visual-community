#include "providers.h"
#include <msctf.h>
#include <imm.h>
#include "../window-action-helper/window-action.h"
namespace magic::system
{
namespace
{
std::wstring GuidText(GUID const &guid)
{
    wchar_t text[40]{};
    Check(StringFromGUID2(guid, text, 40) != 0);
    return text;
}
std::wstring ProfileId(TF_INPUTPROCESSORPROFILE const &p)
{
    return std::to_wstring(p.dwProfileType) + L":" + std::to_wstring(p.langid) + L":" + GuidText(p.clsid) + L":" +
           GuidText(p.guidProfile) + L":" + std::to_wstring(reinterpret_cast<uintptr_t>(p.hkl));
}
std::wstring LanguageName(LANGID language)
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{}, name[256]{};
    if (LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0))
        GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, 256);
    return name;
}
struct Profiles
{
    winrt::com_ptr<ITfInputProcessorProfiles> descriptions;
    winrt::com_ptr<ITfInputProcessorProfileMgr> manager;
    std::vector<TF_INPUTPROCESSORPROFILE> rows;
    Profiles()
    {
        winrt::check_hresult(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                              __uuidof(ITfInputProcessorProfiles), descriptions.put_void()));
        manager = descriptions.as<ITfInputProcessorProfileMgr>();
        winrt::com_ptr<IEnumTfInputProcessorProfiles> enumeration;
        winrt::check_hresult(manager->EnumProfiles(0, enumeration.put()));
        if (!enumeration)
            throw std::runtime_error("Windows returned no input profile enumerator");
        TF_INPUTPROCESSORPROFILE profile{};
        ULONG fetched = 0;
        HRESULT enumerationResult = S_OK;
        while ((enumerationResult = enumeration->Next(1, &profile, &fetched)) == S_OK && fetched)
        {
            if (rows.size() >= 128)
                throw std::runtime_error("Input profile count exceeds bound");
            if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR ||
                profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT)
                rows.push_back(profile);
        }
        winrt::check_hresult(enumerationResult);
    }
};
} // namespace
JsonObject InputProfiles()
{
    JsonObject result;
    JsonArray rows;
    try
    {
        Profiles profiles;
        for (auto const &p : profiles.rows)
        {
            JsonObject row;
            Text(row, L"id", ProfileId(p));
            Number(row, L"languageId", p.langid);
            Text(row, L"languageName", LanguageName(p.langid));
            Text(row, L"kind", p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR ? L"text-service" : L"keyboard-layout");
            Text(row, L"layoutId", std::to_wstring(reinterpret_cast<uintptr_t>(p.hkl)));
            Flag(row, L"enabled", (p.dwFlags & TF_IPP_FLAG_ENABLED) != 0);
            // TSF active flags describe the querying thread, not an arbitrary foreground app.
            Flag(row, L"activeInBackendThread", (p.dwFlags & TF_IPP_FLAG_ACTIVE) != 0);
            std::wstring name;
            if (p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
            {
                BSTR description = nullptr;
                HRESULT hr = profiles.descriptions->GetLanguageProfileDescription(p.clsid, p.langid, p.guidProfile,
                                                                                  &description);
                if (SUCCEEDED(hr) && description)
                    name.assign(description, std::min<UINT>(SysStringLen(description), 512));
                SysFreeString(description);
            }
            if (name.empty())
                name = LanguageName(p.langid);
            Text(row, L"name", name);
            rows.Append(row);
        }
        Flag(result, L"available", true);
    }
    catch (winrt::hresult_error const &error)
    {
        Flag(result, L"available", false);
        Number(result, L"errorCode", error.code().value);
    }
    Text(result, L"selectionScope", L"current-desktop-explicit");
    Flag(result, L"foregroundProfileSelectionKnown", false);
    result.Insert(L"profiles", rows);
    return result;
}
JsonObject InputProfileSelect(JsonObject const &args, Broker &broker)
{
    // Session-wide activation is never inferred from a foreground-only request.
    if (RequiredText(args, L"scope", 32) != L"current-desktop")
        throw std::invalid_argument("Input profile activation requires explicit current-desktop scope");
    auto id = RequiredText(args, L"profileId", 256);
    auto window = broker.ResolveWindow(RequiredText(args, L"windowId"));
    if (window.hwnd != GetForegroundWindow())
        throw std::invalid_argument("Foreground window changed");
    Profiles profiles;
    for (auto const &p : profiles.rows)
        if (ProfileId(p) == id && (p.dwFlags & TF_IPP_FLAG_ENABLED))
        {
            if (window.hwnd != GetForegroundWindow())
                throw std::invalid_argument("Foreground window changed");
            HRESULT hr =
                profiles.manager->ActivateProfile(p.dwProfileType, p.langid, p.clsid, p.guidProfile,
                                                  p.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT ? p.hkl : nullptr,
                                                  TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
            winrt::check_hresult(hr);
            JsonObject result;
            Flag(result, L"accepted", hr == S_OK);
            Text(result, L"scope", L"current-desktop");
            broker.wake->Notify(256);
            return result;
        }
    throw std::invalid_argument("Input profile is not enabled or no longer available");
}
JsonObject InputLayouts(HWND foreground)
{
    int count = GetKeyboardLayoutList(0, nullptr);
    if (count < 0 || count > 128)
        throw std::runtime_error("Keyboard layout count exceeds bound");
    std::vector<HKL> layouts(size_t(count), nullptr);
    count = GetKeyboardLayoutList(count, layouts.data());
    if(!foreground)foreground = GetForegroundWindow();
    DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    HKL current = GetKeyboardLayout(thread);
    JsonArray rows;
    for (int i = 0; i < count; i++)
    {
        auto layout = layouts[size_t(i)];
        JsonObject row;
        Text(row, L"id", std::to_wstring(reinterpret_cast<uintptr_t>(layout)));
        LANGID language = LOWORD(reinterpret_cast<uintptr_t>(layout));
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0);
        Text(row, L"locale", locale);
        Text(row, L"name", LanguageName(language));
        Flag(row, L"selected", layout == current);
        rows.Append(row);
    }
    JsonObject result;
    Flag(result, L"available", true);
    Text(result, L"foregroundWindow", std::to_wstring(reinterpret_cast<uintptr_t>(foreground)));
    // Read-only IMM compatibility messages; failure leaves mode unknown.
    if (auto ime = ImmGetDefaultIMEWnd(foreground))
    {
        DWORD_PTR open = 0, conversion = 0;
        if (SendMessageTimeoutW(ime, WM_IME_CONTROL, WPARAM(0x0005), 0, SMTO_ABORTIFHUNG, 100, &open) &&
            SendMessageTimeoutW(ime, WM_IME_CONTROL, WPARAM(0x0001), 0, SMTO_ABORTIFHUNG, 100, &conversion))
        {
            Flag(result, L"imeAvailable", true);
            Flag(result, L"imeOpen", open != 0);
            Flag(result, L"imeNative", (conversion & IME_CMODE_NATIVE) != 0);
        }
    }
    Text(result, L"scope", L"foreground-window-keyboard-layout");
    result.Insert(L"layouts", rows);
    return result;
}
JsonObject InputSelect(JsonObject const &args, Broker &broker)
{
    auto layoutId = RequiredText(args, L"layoutId", 32);
    // Validation does not need the two read-only IME queries in InputLayouts.
    int count=GetKeyboardLayoutList(0,nullptr);
    if(count<0 || count>128)throw std::runtime_error("Keyboard layout count exceeds bound");
    std::vector<HKL> offered(size_t(count),nullptr);
    count=GetKeyboardLayoutList(count,offered.data());
    HKL selected = nullptr;
    for(int i=0;i<count;i++)if(std::to_wstring(reinterpret_cast<uintptr_t>(offered[size_t(i)]))==layoutId)selected=offered[size_t(i)];
    if (!selected)
        throw std::invalid_argument("Keyboard layout is not available");
    auto id=RequiredText(args,L"windowId");
    auto window = broker.ResolveInputWindow(id);
    if(window.hwnd!=GetForegroundWindow()){
        auto activated=magic::window_action::Perform(window.hwnd,L"activate");
        if(!activated.accepted)throw std::invalid_argument("Input target could not be activated");
    }
    if (window.hwnd != GetForegroundWindow())
        throw std::invalid_argument("Foreground window changed");
    window=broker.ResolveInputWindow(id);
    DWORD thread=GetWindowThreadProcessId(window.hwnd,nullptr);
    GUITHREADINFO focus{sizeof(focus)};HWND receiver=window.hwnd;
    if(GetGUIThreadInfo(thread,&focus) && focus.hwndFocus && GetAncestor(focus.hwndFocus,GA_ROOT)==window.hwnd &&
        GetWindowThreadProcessId(focus.hwndFocus,nullptr)==thread)receiver=focus.hwndFocus;
    if(window.hwnd!=GetForegroundWindow())throw std::invalid_argument("Foreground window changed");
    SetLastError(0);ULONGLONG begin=GetTickCount64();
    DWORD_PTR processed=0;
    bool delivered = SendMessageTimeoutW(receiver, WM_INPUTLANGCHANGEREQUEST, INPUTLANGCHANGE_SYSCHARSET,
        reinterpret_cast<LPARAM>(selected),SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&processed)!=0;
    DWORD error=delivered?0:GetLastError();
    bool accepted=delivered && GetKeyboardLayout(thread)==selected;
    broker.wake->Notify(256);
    JsonObject result;
    Flag(result, L"accepted", accepted);
    Flag(result,L"delivered",delivered);Number(result,L"deliveryMs",double(GetTickCount64()-begin));
    Number(result,L"win32Error",error);
    return result;
}
} // namespace magic::system
