#pragma once
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

// An unpackaged helper has no OS package identity. The machine installation
// record chooses the root; administrator-owned files and ancestors grant trust.
// Never infer trust from a request-supplied or per-user data path.
namespace magic::installed {
inline std::filesystem::path Root() {
    HKEY record=nullptr;
    auto status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{80B5D4B1-E952-4B68-B38A-368DC145B09F}_is1",0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&record);
    wchar_t installed[32768]{};DWORD bytes=sizeof(installed);
    if(status==ERROR_SUCCESS){
        status=RegGetValueW(record,nullptr,L"InstallLocation",RRF_RT_REG_SZ,nullptr,installed,&bytes);RegCloseKey(record);
        if(status!=ERROR_SUCCESS)return {};
        std::wstring path=installed;while(path.size()>3&&path.back()==L'\\')path.pop_back();
        if(path.size()<4||!iswalpha(path[0])||path[1]!=L':'||path[2]!=L'\\'||path.find(L'%')!=std::wstring::npos)return {};
        wchar_t canonical[32768]{};auto length=GetFullPathNameW(path.c_str(),32768,canonical,nullptr);
        return length>3&&length<32768?std::filesystem::path(canonical):std::filesystem::path{};
    }
    if(status!=ERROR_FILE_NOT_FOUND&&status!=ERROR_PATH_NOT_FOUND)return {};
    PWSTR value=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&value)))return {};
    std::filesystem::path result=std::filesystem::path(value)/L"MagicCommunityDesktop";
    CoTaskMemFree(value);return result;
}
inline bool Privileged(PSID sid) {
    if(!sid||!IsValidSid(sid))return false;
    if(IsWellKnownSid(sid,WinLocalSystemSid)||IsWellKnownSid(sid,WinBuiltinAdministratorsSid))return true;
    PSID installer=nullptr;
    if(!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&installer))return false;
    bool result=EqualSid(sid,installer)!=FALSE;LocalFree(installer);return result;
}
inline bool FixedVolumeRoot(std::filesystem::path const& path) {
    auto value=path.native();
    if(value.size()!=3||!iswalpha(value[0])||value[1]!=L':'||value[2]!=L'\\'||GetDriveTypeW(value.c_str())!=DRIVE_FIXED)return false;
    wchar_t target[32768]{};
    if(!QueryDosDeviceW(value.substr(0,2).c_str(),target,32768))return false;
    std::wstring device=target,prefix=L"\\Device\\HarddiskVolume";
    if(device.size()<=prefix.size()||_wcsnicmp(device.c_str(),prefix.c_str(),prefix.size()))return false;
    return device.find_first_not_of(L"0123456789",prefix.size())==std::wstring::npos;
}
inline DWORD UnsafeWriteMask(bool ancestor,bool volumeRoot) {
    DWORD authority=FILE_DELETE_CHILD|WRITE_DAC|WRITE_OWNER|GENERIC_ALL;
    if(ancestor&&volumeRoot)return authority;
    return authority|DELETE|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|GENERIC_WRITE|(ancestor?0:FILE_WRITE_DATA|FILE_APPEND_DATA);
}
inline bool Protected(std::filesystem::path const& path,bool ancestor=false) {
    DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    PSECURITY_DESCRIPTOR descriptor=nullptr;PSID owner=nullptr;PACL acl=nullptr;
    if(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
    bool valid=Privileged(owner)&&acl;
    // Same root-specific rule as setup and Win32Installation. Ordinary parent
    // directories, installed files and reparse points retain all protections.
    DWORD writes=UnsafeWriteMask(ancestor,ancestor&&FixedVolumeRoot(path));
    for(DWORD i=0;valid&&i<acl->AceCount;i++) {
        void* item=nullptr;if(!GetAce(acl,i,&item)){valid=false;break;}
        auto header=static_cast<ACE_HEADER*>(item);
        if(header->AceFlags&INHERIT_ONLY_ACE)continue;
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE) {
            auto ace=static_cast<ACCESS_ALLOWED_ACE*>(item);
            if((ace->Mask&writes)&&!Privileged(&ace->SidStart))valid=false;
        } else if(header->AceType!=ACCESS_DENIED_ACE_TYPE)valid=false;
    }
    LocalFree(descriptor);return valid;
}
inline bool Tree(std::filesystem::path directory) {
    try {
        auto expected=Root();auto text=directory.native();
        if(text.rfind(L"\\\\?\\",0)==0)directory=text.substr(4);
        if(expected.empty()||_wcsicmp(directory.c_str(),expected.c_str()))return false;
        if(!Protected(expected))return false;
        for(auto ancestor=expected.parent_path();!ancestor.empty();){
            if(!Protected(ancestor,true))return false;auto next=ancestor.parent_path();if(next==ancestor)break;ancestor=next;
        }
        size_t count=0;
        for(auto const& file:std::filesystem::recursive_directory_iterator(expected))
            if(++count>8192||!Protected(file.path()))return false;
        return true;
    } catch(...) {return false;}
}
}
