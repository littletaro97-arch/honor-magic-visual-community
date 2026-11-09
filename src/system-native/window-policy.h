#pragma once
#include <cstdint>
#include <string_view>
namespace magic::system {
inline bool TaskbarWindowStyle(std::uint32_t extended,bool owned){
    constexpr std::uint32_t tool=0x80,noActivate=0x08000000,appWindow=0x40000;
    return !(extended&tool)&&(!(extended&noActivate)||(extended&appWindow))&&(!owned||(extended&appWindow));
}
inline bool SecurityApplication(std::wstring_view id){
    // Verified from the installed Windows package manifest, not a translated
    // title or executable basename shared by unrelated applications.
    return id==L"Microsoft.LockApp_cw5n1h2txyewy!WindowsDefaultLockScreen";
}
inline bool ShellSurfaceApplication(std::wstring_view id){
    // The Start menu is a shell surface, not an application for the Dock.
    // Match its verified package identity, never a localized window title.
    return id==L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy!App";
}
enum class WindowSessionChange { None,Clear,Refresh };
inline WindowSessionChange WindowSessionPolicy(unsigned message,std::uintptr_t parameter){
    if(message==0x02B1){
        if(parameter==7||parameter==2||parameter==4||parameter==6)return WindowSessionChange::Clear;
        if(parameter==8||parameter==1||parameter==3||parameter==5)return WindowSessionChange::Refresh;
    }
    if(message==0x0218){
        if(parameter==4)return WindowSessionChange::Clear;
        if(parameter==7||parameter==18)return WindowSessionChange::Refresh;
    }
    return WindowSessionChange::None;
}
}
