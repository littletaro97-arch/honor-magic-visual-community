#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../shared/native-ui-font.h"
#include <appmodel.h>
#include <shellapi.h>
#include <dispatcherqueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <string>
#include <atomic>
#include "permission-presentation.h"
using namespace winrt::Windows::UI::Notifications::Management;
namespace {
HWND mainWindow=nullptr,requestButton=nullptr,statusText=nullptr;PermissionPresentation presentation;
winrt::Windows::Foundation::IAsyncOperation<UserNotificationListenerAccessStatus> pending{nullptr};
std::atomic<LONG> lastError{0};std::atomic<int> access{0};int result=3;
void State(){auto status=UserNotificationListener::Current().GetAccessStatus();access=int(status);SetWindowTextW(statusText,status==UserNotificationListenerAccessStatus::Allowed?L"已允许。Magic 视界可以在通知面板中显示和清除通知。":status==UserNotificationListenerAccessStatus::Denied?L"权限未允许。可以在 Windows 设置中为 Magic 视界单独开启。":L"允许 Magic 视界在自己的通知面板中显示来自其他应用的通知。\r\n你可以随时在 Windows 设置中撤销权限。");if(status==UserNotificationListenerAccessStatus::Allowed){SetWindowTextW(requestButton,L"启用通知面板");}}
LRESULT CALLBACK Procedure(HWND h,UINT message,WPARAM w,LPARAM l){
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(h,&ps);RECT bounds{};GetClientRect(h,&bounds);try{if(presentation.target)presentation.Paint(h,dc,bounds,0);}catch(...){presentation.Fallback(h,dc,bounds,0,L"");}EndPaint(h,&ps);return 0;}
    if(message==WM_DRAWITEM){auto item=reinterpret_cast<DRAWITEMSTRUCT*>(l);wchar_t text[1024]{};GetWindowTextW(item->hwndItem,text,1024);try{if(presentation.target)presentation.Paint(h,item->hDC,item->rcItem,static_cast<int>(item->CtlID),text,item->itemState|(GetPropW(item->hwndItem,L"MagicDialogHover")?ODS_HOTLIGHT:0));}catch(...){presentation.Fallback(h,item->hDC,item->rcItem,static_cast<int>(item->CtlID),text);}return TRUE;}
    if(message==WM_DPICHANGED){auto r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOACTIVATE|SWP_NOZORDER);presentation.Layout(h);return 0;}
    if(message==WM_SIZE&&presentation.target){presentation.Layout(h);return 0;}
    if(message==WM_SETTINGCHANGE||message==WM_THEMECHANGED||message==WM_DWMCOMPOSITIONCHANGED){presentation.Apply(h);return 0;}
    if(message==WM_POWERBROADCAST&&w==PBT_POWERSETTINGCHANGE){presentation.Power(h,reinterpret_cast<POWERBROADCAST_SETTING*>(l));return TRUE;}
    if(message==WM_COMMAND&&LOWORD(w)==1){
        if(access==int(UserNotificationListenerAccessStatus::Allowed)){result=0;DestroyWindow(h);return 0;}
        if(pending)return 0;
        try{EnableWindow(requestButton,FALSE);pending=UserNotificationListener::Current().RequestAccessAsync();pending.Completed([h](auto const&operation,auto status){try{if(status==winrt::Windows::Foundation::AsyncStatus::Completed)access=int(operation.GetResults());else lastError=operation.ErrorCode().value;}catch(winrt::hresult_error const&error){lastError=error.code().value;}PostMessageW(h,WM_APP+1,0,0);});SetTimer(h,1,180000,nullptr);}
        catch(winrt::hresult_error const&error){lastError=error.code().value;PostMessageW(h,WM_APP+1,0,0);}return 0;
    }
    if(message==WM_COMMAND&&LOWORD(w)==2){ShellExecuteW(h,L"open",L"ms-settings:privacy-notifications",nullptr,nullptr,SW_SHOWNORMAL);return 0;}
    if(message==WM_APP+1){KillTimer(h,1);pending=nullptr;EnableWindow(requestButton,TRUE);if(lastError){SetWindowTextW(statusText,L"Windows 未完成授权请求。请使用下方的 Windows 通知隐私设置入口。权限不会自动开启。");}else try{State();if(access==int(UserNotificationListenerAccessStatus::Allowed)){result=0;DestroyWindow(h);}}catch(...){SetWindowTextW(statusText,L"暂时无法读取权限状态，请使用 Windows 设置查看。");}return 0;}
    if(message==WM_TIMER&&w==1){if(pending)pending.Cancel();KillTimer(h,1);EnableWindow(requestButton,TRUE);SetWindowTextW(statusText,L"授权请求已超时，可关闭后重新打开。权限不会自动开启。");return 0;}
    if(message==WM_DESTROY){if(pending)pending.Cancel();PostQuitMessage(result);return 0;}
    return DefWindowProcW(h,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    try{
        UINT32 length=0;auto identity=GetCurrentPackageFamilyName(&length,nullptr);
        if(identity!=APPMODEL_ERROR_NO_PACKAGE){if(identity!=ERROR_INSUFFICIENT_BUFFER||length>512)return 3;std::wstring family(length,L'\0');if(GetCurrentPackageFamilyName(&length,family.data())!=ERROR_SUCCESS||family.rfind(L"MagicCommunity.Native_",0)!=0)return 3;}
        winrt::init_apartment(winrt::apartment_type::single_threaded);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::Windows::System::DispatcherQueueController queue{nullptr};DispatcherQueueOptions options{sizeof(options),DQTYPE_THREAD_CURRENT,DQTAT_COM_STA};winrt::check_hresult(CreateDispatcherQueueController(options,reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        WNDCLASSW cls{};cls.hInstance=instance;cls.lpszClassName=L"MagicIndependentNotificationPermission";cls.lpfnWndProc=Procedure;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=nullptr;if(!RegisterClassW(&cls))return 3;
        UINT dpi=GetDpiForSystem();RECT rect{0,0,MulDiv(480,dpi,96),MulDiv(222,dpi,96)};DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
        AdjustWindowRectExForDpi(&rect,style,FALSE,WS_EX_DLGMODALFRAME,dpi);int width=rect.right-rect.left,height=rect.bottom-rect.top;
        mainWindow=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,L"Magic 视界社区版 · 通知权限",style,(GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,nullptr,nullptr,instance,nullptr);if(!mainWindow)return 3;
        statusText=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,0,0,mainWindow,reinterpret_cast<HMENU>(3),instance,nullptr);
        requestButton=CreateWindowExW(0,L"BUTTON",L"请求 Windows 授权",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,mainWindow,reinterpret_cast<HMENU>(1),instance,nullptr);
        auto settings=CreateWindowExW(0,L"BUTTON",L"Windows 通知隐私设置",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,mainWindow,reinterpret_cast<HMENU>(2),instance,nullptr);
        for(auto button:{requestButton,settings})SetWindowSubclass(button,PermissionPresentation::Control,1,0);
        presentation.Initialize(mainWindow);State();ShowWindow(mainWindow,SW_SHOW);SetForegroundWindow(mainWindow);SetFocus(requestButton);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){PostMessageW(mainWindow,WM_CLOSE,0,0);continue;}
            if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN){auto focused=GetFocus();if((focused==requestButton||focused==settings)&&IsWindowEnabled(focused))SendMessageW(focused,BM_CLICK,0,0);continue;}
            if(!IsDialogMessageW(mainWindow,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
        queue.ShutdownQueueAsync();return result;
    }catch(...){return 3;}
}
