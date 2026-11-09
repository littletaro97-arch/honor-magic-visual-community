#include "../../../src/system-native/window-policy.h"
#include "../../../src/composition-trial/native-notification-hints.h"
#include <cstdio>
int main(){
    int checks=0;bool pass=true;
    auto check=[&](bool value,char const *name){++checks;if(!value){pass=false;std::fprintf(stderr,"failed: %s\n",name);}};
    using namespace magic::system;
    check(ShellSurfaceApplication(L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy!App"),"verified-start-identity-excluded");
    check(!ShellSurfaceApplication(L"Example.App!App"),"ordinary-application-remains");
    check(!ShellSurfaceApplication(L"StartMenuExperienceHost.exe"),"basename-does-not-exclude-ordinary-app");
    check(!ShellSurfaceApplication(L"开始"),"localized-title-does-not-exclude-ordinary-app");
    check(!ShellSurfaceApplication(L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy!Other"),"different-identity-remains");
    check(!ShellSurfaceApplication(L""),"unpackaged-ordinary-app-remains");
    check(SecurityApplication(L"Microsoft.LockApp_cw5n1h2txyewy!WindowsDefaultLockScreen"),"lock-screen-boundary-preserved");
    check(!SecurityApplication(L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy!App"),"start-exclusion-does-not-change-input-security-policy");
    using NativeNotificationHints::SessionStatus;
    check(!SessionStatus(false,L"",L"not-authorized"),"no-unrequested-authorization-toast-on-start");
    check(!SessionStatus(false,L"not-authorized",L"not-authorized"),"no-repeated-authorization-toast");
    check(!SessionStatus(false,L"listening",L"not-authorized"),"revoked-access-is-quiet");
    check(!SessionStatus(false,L"temporary-unavailable",L"not-authorized"),"unavailable-transition-is-quiet");
    check(!SessionStatus(false,L"",L""),"empty-unauthorized-status-is-quiet");
    check(SessionStatus(true,L"not-authorized",L"listening"),"authorized-session-change-remains-visible");
    check(!SessionStatus(true,L"listening",L"listening"),"authorized-duplicate-status-is-quiet");
    check(!SessionStatus(true,L"listening",L""),"empty-authorized-status-is-quiet");
    check(SessionStatus(true,L"listening",L"restore-error"),"authorized-session-errors-remain-visible");
    std::printf("{\"allPass\":%s,\"checks\":%d,\"scope\":\"Pure notification hint and Dock identity policies; no permission or user configuration changes\"}\n",pass?"true":"false",checks);
    return pass?0:1;
}
