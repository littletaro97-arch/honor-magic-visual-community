#pragma once
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <appmodel.h>
namespace magic::system {
inline std::wstring ProcessApplicationId(HANDLE process){
    UINT32 count=0;
    if(GetApplicationUserModelId(process,&count,nullptr)!=ERROR_INSUFFICIENT_BUFFER||count>512)return {};
    std::wstring id(count,L'\0');
    if(GetApplicationUserModelId(process,&count,id.data())!=ERROR_SUCCESS)return {};
    id.resize(wcslen(id.c_str()));return id;
}
// Resolve the app behind a Windows frame without changing the frame's action
// HWND/PID/creation identity. No title matching and no mapping all hosts to Settings.
inline std::wstring HostedApplicationId(HWND window,DWORD ownerPid,std::wstring const &ownerPath){
    wchar_t system[MAX_PATH]{},name[128]{};
    UINT n=GetSystemDirectoryW(system,MAX_PATH);
    if(!n||n>=MAX_PATH||_wcsicmp(ownerPath.c_str(),(std::wstring(system)+L"\\ApplicationFrameHost.exe").c_str()))return {};
    if(!GetClassNameW(window,name,128)||wcscmp(name,L"ApplicationFrameWindow"))return {};
    winrt::com_ptr<IPropertyStore> store;
    if(SUCCEEDED(SHGetPropertyStoreForWindow(window,__uuidof(IPropertyStore),store.put_void()))){
        PROPVARIANT value{};std::wstring id;
        if(SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID,&value))&&value.vt==VT_LPWSTR&&value.pwszVal){
            size_t length=wcsnlen(value.pwszVal,513);if(length<=512)id.assign(value.pwszVal,length);
        }
        PropVariantClear(&value);if(!id.empty())return id;
    }
    struct Search{DWORD owner;unsigned count=0;std::wstring id;};Search search{ownerPid};
    EnumChildWindows(window,[](HWND child,LPARAM context)->BOOL{
        auto &s=*reinterpret_cast<Search*>(context);if(++s.count>64)return FALSE;
        wchar_t type[128]{};DWORD pid=0;
        if(!GetClassNameW(child,type,128)||wcscmp(type,L"Windows.UI.Core.CoreWindow"))return TRUE;
        GetWindowThreadProcessId(child,&pid);if(!pid||pid==s.owner)return TRUE;
        winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)};
        if(process)s.id=ProcessApplicationId(process.get());
        return s.id.empty();
    },reinterpret_cast<LPARAM>(&search));
    return search.id;
}
}
