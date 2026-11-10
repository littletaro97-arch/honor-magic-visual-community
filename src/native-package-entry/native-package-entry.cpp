#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include "../shared/trusted-win32-install.h"
bool InstallationInProgress(){
    SetLastError(0);HANDLE handle=OpenMutexW(SYNCHRONIZE,FALSE,L"Global\\MagicCommunity.Desktop.Maintenance");
    if(handle){CloseHandle(handle);return true;}
    return GetLastError()==ERROR_ACCESS_DENIED;
}
struct InstalledActivity {
    HANDLE handle=nullptr;
    InstalledActivity(){PSECURITY_DESCRIPTOR sd=nullptr;
        if(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;WD)",SDDL_REVISION_1,&sd,nullptr)){
            SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};handle=CreateMutexW(&sa,FALSE,L"Global\\MagicCommunity.Desktop.Active");LocalFree(sd);
            if(!handle)handle=OpenMutexW(SYNCHRONIZE,FALSE,L"Global\\MagicCommunity.Desktop.Active");
        }
    }
    ~InstalledActivity(){if(handle)CloseHandle(handle);}
};
static int ReportStartupState(winrt::Windows::ApplicationModel::StartupTaskState state){
    using winrt::Windows::ApplicationModel::StartupTaskState;
    // ASCII on purpose: the parent reads this pipe as UTF-8, so writing UTF-16
    // here arrives as "d\0i\0s\0..." and reads as an unknown state.
    const char* name=state==StartupTaskState::Enabled?"enabled":state==StartupTaskState::EnabledByPolicy?"enabled-by-policy":state==StartupTaskState::Disabled?"disabled":"disabled-by-policy";
    std::string line(name);line.push_back('\n');
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    if(output&&output!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(output,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);}
    // The state itself is the answer, so a readable answer is always a success.
    return 0;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR arguments,int){
    try{
        if(InstallationInProgress())return ERROR_INSTALL_ALREADY_RUNNING;
        UINT32 n=0;auto identity=GetCurrentPackageFamilyName(&n,nullptr);bool unpackaged=identity==APPMODEL_ERROR_NO_PACKAGE;
        if(!unpackaged){if(identity!=ERROR_INSUFFICIENT_BUFFER||n>512)return 3;std::wstring family(n,L'\0');if(GetCurrentPackageFamilyName(&n,family.data())||family.rfind(L"MagicCommunity.Native_",0)!=0)return 3;}
        wchar_t buffer[32768]{};DWORD length=GetModuleFileNameW(nullptr,buffer,32768);if(!length||length>=32768)return 3;auto directory=std::filesystem::path(std::wstring(buffer,length)).parent_path();
        if(unpackaged&&!magic::installed::Tree(directory)){MessageBoxW(nullptr,L"安装位置或文件权限不正确。请重新运行完整安装包；不要直接运行解压后的程序。",L"Magic 视界社区版",MB_OK|MB_ICONERROR);return 3;}
        std::ifstream file(directory/L"native-launch.json",std::ios::binary);std::string text((std::istreambuf_iterator<char>(file)),{});if(text.empty()||text.size()>8192)return 3;
        int setupArgc=0;auto setupArgv=CommandLineToArgvW(GetCommandLineW(),&setupArgc);
        std::wstring startup;
        bool logon=false,configure=false;
        if(setupArgv){
            if(setupArgc==3&&std::wstring(setupArgv[1])==L"--configure-startup")startup=setupArgv[2];
            for(int i=1;i<setupArgc;i++)if(std::wstring_view(setupArgv[i])==L"--logon")logon=true;
            for(int i=1;i<setupArgc;i++)if(std::wstring_view(setupArgv[i])==L"--configure")configure=true;
            LocalFree(setupArgv);
        }
        // MTA on purpose: an STA without a message pump blocks IAsyncOperation::get()
        // until the caller's timeout. The consent prompt is drawn by the shell.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto document=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(text));bool installed=document.GetNamedNumber(L"schemaVersion",1)==2 && document.GetNamedString(L"dataRootMode",L"")==L"per-user";
        if(installed&&!startup.empty()){
            if(unpackaged)return 3; // Unpackaged startup is an opt-in profile flag.
            auto task=winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"MagicCommunityStartup").get();
            if(startup==L"disable")task.Disable();
            else if(startup==L"enable"){try{task.RequestEnableAsync().get();}catch(...){ReportStartupState(task.State());return 5;}}
            else if(startup!=L"state")return 3;
            return ReportStartupState(task.State());
        }
        auto root=std::wstring(document.GetNamedString(L"root",L""));
        if(installed){
            if(unpackaged&&logon){
                PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))return 0;
                auto registration=std::filesystem::path(local)/L"MagicCommunity/Deployment/installation.json";CoTaskMemFree(local);
                try{std::ifstream input(registration);std::string data((std::istreambuf_iterator<char>(input)),{});if(data.size()>16384)return 0;
                    auto profile=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(data));
                    if(profile.GetNamedString(L"packageFamily",L"")!=L"MagicCommunity.Unpackaged"||profile.GetNamedString(L"state",L"")!=L"installed"||!profile.GetNamedBoolean(L"startupRequested",false))return 0;
                }catch(...){return 0;}
            }
            if(unpackaged){
                UINT32 count=0,size=0;auto status=GetPackagesByPackageFamily(L"MagicCommunity.Native_x52pfh0dth434",&count,nullptr,&size,nullptr);
                if((status==ERROR_INSUFFICIENT_BUFFER||status==ERROR_SUCCESS)&&count){if(!logon)MessageBoxW(nullptr,L"当前账户仍安装着旧 MSIX 版。请先退出并从 Windows 设置卸载旧版，保留用户数据，再打开此版本。",L"Magic 视界社区版",MB_OK|MB_ICONINFORMATION);return 4;}
            }
            InstalledActivity activity;if(!activity.handle)return 3;
            auto executable=(directory/L"MagicCommunity.exe").wstring();
            auto command=L"\""+executable+L"\" --installed-desktop --desktop";
            if(logon)command+=L" --logon";
            if(configure)command+=L" --configure-installation";
            SetEnvironmentVariableW(L"DOTNET_ROOT",(directory/L"dotnet").c_str());SetEnvironmentVariableW(L"DOTNET_ROOT_X64",(directory/L"dotnet").c_str());
            STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION child{};
            if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&start,&child))return 3;
            CloseHandle(child.hThread);WaitForSingleObject(child.hProcess,INFINITE);DWORD result=3;GetExitCodeProcess(child.hProcess,&result);CloseHandle(child.hProcess);return int(result);
        }
        if(root.size()<3||root[1]!=L':'||root[2]!=L'\\'||root.find(L'\"')!=root.npos)return 3;
        std::ifstream head(std::filesystem::path(root)/L".git"/L"HEAD");std::string branch;std::getline(head,branch);if(!branch.empty()&&branch.back()=='\r')branch.pop_back();if(branch!="ref: refs/heads/main")return 3;
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool probe=false;
        if(argv){for(int i=1;i<argc;i++)if(std::wstring(argv[i])==L"--identity-probe")probe=true;LocalFree(argv);}
        // Packaged activation may append server arguments or omit launch arguments.
        // Never enter a legacy desktop when the independent selection is absent.
        try{std::ifstream selected(std::filesystem::path(root)/L"runtime"/L"system"/L"selection.json",std::ios::binary);std::string selectionText((std::istreambuf_iterator<char>(selected)),{});if(selectionText.size()>8192)return 3;auto selection=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(selectionText));if(selection.GetNamedString(L"protocol",L"")!=L"Magic.System.Native/1"||!selection.GetNamedBoolean(L"enabled",false))probe=true;}catch(...){probe=true;}
        std::wstring executable=(directory/(probe?L"MagicSystemNative.exe":L"MagicCommunity.exe")).wstring();
        auto report=std::wstring(document.GetNamedString(L"probeReport",L""));if(probe&&(report.empty()||report.find(L'\"')!=report.npos))return 3;
        std::wstring command=L"\""+executable+(probe?L"\" --identity-probe \""+report+L"\"":L"\" --root \""+root+L"\" --desktop");
        SetEnvironmentVariableW(L"DOTNET_ROOT",(directory/L"dotnet").c_str());SetEnvironmentVariableW(L"DOTNET_ROOT_X64",(directory/L"dotnet").c_str());SetEnvironmentVariableW(L"DOTNET_CLI_TELEMETRY_OPTOUT",L"1");
        STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION child{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,probe?CREATE_NO_WINDOW:0,nullptr,directory.c_str(),&start,&child))return 3;CloseHandle(child.hThread);WaitForSingleObject(child.hProcess,INFINITE);DWORD result=3;GetExitCodeProcess(child.hProcess,&result);CloseHandle(child.hProcess);return int(result);
    }catch(...){return 3;}
}
