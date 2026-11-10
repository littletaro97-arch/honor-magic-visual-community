#pragma once
#include <string>
#include <cwctype>
// The brand menu header shows the machine brand instead of a hard-coded product
// name, matching MyMagicBar's Brand helper. Read once and cached: the value is
// static for the lifetime of the process and the renderer must not touch the
// registry on every paint.
inline std::wstring const &NativeBrandName()
{
    static std::wstring name;
    static bool read = false;
    if (read) return name;
    read = true;
    wchar_t buffer[128]{};
    DWORD bytes = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer",
                     RRF_RT_REG_SZ, nullptr, buffer, &bytes) == ERROR_SUCCESS)
    {
        std::wstring value(buffer);
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.pop_back();
        for (auto &ch : value) ch = static_cast<wchar_t>(towupper(ch));
        // Placeholder strings that OEMs ship on unconfigured firmware.
        if (!value.empty() && value.find(L"TO BE FILLED") == std::wstring::npos &&
            value.find(L"DEFAULT STRING") == std::wstring::npos && value != L"SYSTEM MANUFACTURER" &&
            value != L"OEM" && value != L"UNKNOWN")
            name = value;
    }
    if (name.size() > 24) name.resize(24);
    if (name.empty()) name = L"PC";
    return name;
}
