#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>
#include <objbase.h>
#include <dwmapi.h>

/* One message wait, no polling. This process owns hiding/restoring the original
   list window and exits on adapter/host/Explorer exit or owned view destruction. */
static const wchar_t *diagnostics;
static void trace_guard(const wchar_t *phase,int code,HWND view,DWORD host) {
    if(!diagnostics)return;
    HANDLE file=CreateFileW(diagnostics,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return;
    SYSTEMTIME now;GetSystemTime(&now);char line[512];DWORD owner=0;GetWindowThreadProcessId(view,&owner);
    int size=_snprintf_s(line,sizeof(line),_TRUNCATE,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu phase=%ls code=%d hwnd=%llu valid=%d visible=%d actualPid=%lu expectedPid=%lu error=%lu\r\n",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,GetCurrentProcessId(),phase,code,(unsigned long long)(UINT_PTR)view,IsWindow(view),IsWindowVisible(view),owner,host,GetLastError());
    DWORD written;if(size>0)WriteFile(file,line,(DWORD)size,&written,NULL);CloseHandle(file);
}
static HWND original, views[64];
static HWND shownDesktopOwner;
static DWORD explorerPid, guardHostPid, threadId;
static int viewCount, originallyVisible, layerQueued;
#define CHECK_LAYER (WM_APP+1)
static void queue_layer(void) {
    if(!layerQueued){layerQueued=1;if(!PostThreadMessageW(threadId,CHECK_LAYER,0,0))layerQueued=0;}
}
/* Owned top-level surfaces preserve DWM alpha and Show Desktop, but Explorer
   activation can raise an owned group. Correct only an actual inversion, on
   window events; never activate or restack an application window. Let Windows
   lower the verified Explorer owner with its owned surfaces, so the surfaces
   cannot be stranded below the native desktop. */
/* Do not use WS_CAPTION as an application identity: borderless games and
   custom frames have no caption. Ignore noninteractive overlays and desktops. */
static int application_window(HWND hwnd,HWND foreground) {
    DWORD pid=0,cloaked=0;wchar_t cls[80];
    GetWindowThreadProcessId(hwnd,&pid);
    if(!pid||pid==guardHostPid||!IsWindowVisible(hwnd)||IsIconic(hwnd)||GetAncestor(hwnd,GA_ROOT)!=hwnd)return 0;
    if(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return 0;
    LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    if(ex&WS_EX_NOACTIVATE)return 0;
    if((ex&WS_EX_TOOLWINDOW)&&hwnd!=foreground)return 0;
    if(GetClassNameW(hwnd,cls,80)&&(!wcscmp(cls,L"Progman")||!wcscmp(cls,L"WorkerW")||
        !wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd")))return 0;
    return 1;
}
/* Win+D raises Explorer's desktop over windows without WS_MINIMIZEBOX. That
   ordering is intentional, not an inversion. Remember only a foreground
   transition to our verified Explorer owner; clicking a Magic desktop alone
   must not suppress the late Settings/uncloak repair. No shortcut interception,
   game messages, timer or foreground stealing is needed. */
static void observe_desktop_foreground(HWND foreground) {
    HWND desktopOwner=GetAncestor(original,GA_ROOT);
    DWORD pid=0;GetWindowThreadProcessId(desktopOwner,&pid);
    if(!desktopOwner||pid!=explorerPid){shownDesktopOwner=NULL;return;}
    HWND shell=GetShellWindow();DWORD shellPid=0;GetWindowThreadProcessId(shell,&shellPid);
    if(foreground==desktopOwner||(shell&&foreground==shell&&shellPid==explorerPid)){
        shownDesktopOwner=desktopOwner;return;
    }
    if(!foreground)return; /* A transient null foreground is not an app switch. */
    for(int i=0;i<viewCount;i++)if(foreground==views[i]) {
        GetWindowThreadProcessId(foreground,&pid);
        if(pid==guardHostPid&&GetParent(foreground)==desktopOwner)return;
    }
    shownDesktopOwner=NULL;
}
static int lower_surfaces(void) {
    HWND desktopOwner=GetAncestor(original,GA_ROOT);
    HWND foreground=GetAncestor(GetForegroundWindow(),GA_ROOT);
    if(!desktopOwner)return 0;
    for(int i=0;i<viewCount;i++) {
        DWORD pid=0;GetWindowThreadProcessId(views[i],&pid);
        if(!IsWindow(views[i])||pid!=guardHostPid||GetParent(views[i])!=desktopOwner)return 0;
    }
    observe_desktop_foreground(foreground);
    if(shownDesktopOwner==desktopOwner)return 1;
    /* A changing GetWindow chain can cycle. One shared budget bounds the entire
       event, including multimonitor views; partial scans never prove absence.
       No calls into a game's window procedure, activation or periodic polling. */
    int remaining=2048;
    for(int i=0;i<viewCount;i++) {
        HWND view=views[i],seen[2048];int count=0,covered=0,ownerBelow=0;
        RECT surface;if(!GetWindowRect(view,&surface))continue;
        HWND below=GetWindow(view,GW_HWNDNEXT);
        for(;below;below=GetWindow(below,GW_HWNDNEXT)) {
            if(--remaining<0)return 1;
            for(int j=0;j<count;j++)if(seen[j]==below)return 1;
            seen[count++]=below;
            if(below==desktopOwner){ownerBelow=1;continue;}
            if(!application_window(below,foreground))continue;
            RECT rect,overlap;
            if(GetWindowRect(below,&rect)&&IntersectRect(&overlap,&surface,&rect)){covered=1;break;}
        }
        if(covered||(!below&&!ownerBelow)) {
            /* Queue only our verified surface's owned group. ASYNC avoids
               blocking this recovery guard on a stalled renderer input queue.
               Keep owner ordering; NOOWNERZORDER strands it below Explorer. */
            if(!SetWindowPos(view,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS))
                trace_guard(L"layer-deferred",0,view,guardHostPid);
            return 1;
        }
    }
    return 1;
}
static int same_time(HANDLE process, unsigned long long expected) {
    FILETIME created,exit,kernel,user;
    if(!GetProcessTimes(process,&created,&exit,&kernel,&user)) return 0;
    return (((unsigned long long)created.dwHighDateTime<<32)|created.dwLowDateTime)==expected;
}
static int original_valid(void) {
    wchar_t cls[80],path[32768],expected[32768]; DWORD pid=0,size=32768;
    if(!IsWindow(original)||!GetClassNameW(original,cls,80)||wcscmp(cls,L"SysListView32")) return 0;
    HWND parent=GetParent(original);
    if(!parent||!GetClassNameW(parent,cls,80)||wcscmp(cls,L"SHELLDLL_DefView")) return 0;
    GetWindowThreadProcessId(original,&pid); if(pid!=explorerPid) return 0;
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); if(!p) return 0;
    int ok=QueryFullProcessImageNameW(p,0,path,&size); CloseHandle(p);
    if(!ok||!GetWindowsDirectoryW(expected,32750)) return 0;
    wcscat_s(expected,32768,L"\\explorer.exe");
    return !_wcsicmp(path,expected);
}
static void restore(void) {
    for(int i=0;i<viewCount;i++) {DWORD pid=0;GetWindowThreadProcessId(views[i],&pid);if(pid==guardHostPid&&IsWindow(views[i]))ShowWindow(views[i],SW_HIDE);}
    if(original_valid()) ShowWindow(original,originallyVisible?SW_SHOWNA:SW_HIDE);
}
static void CALLBACK changed(HWINEVENTHOOK hook,DWORD event,HWND hwnd,LONG object,LONG child,DWORD eventThread,DWORD time) {
    (void)hook;(void)eventThread;(void)time;
    if(event==EVENT_SYSTEM_FOREGROUND){
        /* Capture the transition before the coalesced check: focus may already
           have moved from Explorer to one of our desktop views by then. */
        observe_desktop_foreground(GetAncestor(hwnd,GA_ROOT));queue_layer();return;
    }
    /* Top-level Z-order changes are reported on the desktop's CLIENT object,
       not necessarily on either reordered window. Settings can activate while
       still cloaked and be reordered again after its frame is revealed. */
    if(event==EVENT_OBJECT_REORDER&&hwnd==GetDesktopWindow()&&object==OBJID_CLIENT&&child==CHILDID_SELF){queue_layer();return;}
    if(object!=OBJID_WINDOW||child!=CHILDID_SELF)return;
    for(int i=0;i<viewCount;i++)if(hwnd==views[i]){
        if(event==EVENT_OBJECT_DESTROY){trace_guard(L"window-destroyed",0,hwnd,guardHostPid);PostThreadMessageW(threadId,WM_QUIT,0,0);}
        else if(event==EVENT_OBJECT_REORDER||event==EVENT_OBJECT_SHOW)queue_layer();
        return;
    }
    if(event==EVENT_OBJECT_SHOW||event==EVENT_OBJECT_REORDER||event==EVENT_OBJECT_UNCLOAKED){
        if(hwnd==GetAncestor(original,GA_ROOT)){queue_layer();return;}
        if(application_window(hwnd,GetAncestor(GetForegroundWindow(),GA_ROOT)))queue_layer();
    }
}
/* Session notification lease: a second instance of this existing tiny guard
   waits on process handles. No timer, hooks, notification deletion or registry policy. */
typedef struct QuietSettings QuietSettings;
typedef struct QuietVtable {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(QuietSettings*,REFIID,void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(QuietSettings*);
    ULONG (STDMETHODCALLTYPE *Release)(QuietSettings*);
    HRESULT (STDMETHODCALLTYPE *GetProfile)(QuietSettings*,LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *SetProfile)(QuietSettings*,LPCWSTR);
} QuietVtable;
struct QuietSettings { const QuietVtable *lpVtbl; };
static const GUID quietClass={0xf53321fa,0x34f8,0x4b7f,{0xb9,0xa3,0x36,0x18,0x77,0xcb,0x94,0xcf}};
static const GUID quietInterface={0x6bff4732,0x81ec,0x4ffb,{0xae,0x67,0xb6,0xc1,0xbc,0x29,0x63,0x1f}};
static const wchar_t quietProfile[]=L"Microsoft.QuietHoursProfile.AlarmsOnly";
static int notification_guard(int argc,LPWSTR *argv) {
    if(argc!=9)return 20;
    HANDLE waits[3]={OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)wcstoul(argv[2],NULL,10)),
        OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)wcstoul(argv[4],NULL,10)),
        OpenEventW(SYNCHRONIZE,FALSE,argv[6])};
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[7]);
    int valid=waits[0]&&waits[1]&&waits[2]&&ready&&
        same_time(waits[0],_wcstoui64(argv[3],NULL,10))&&same_time(waits[1],_wcstoui64(argv[5],NULL,10));
    int result=21;
    HRESULT initialized=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    QuietSettings *settings=NULL;
    if(valid&&SUCCEEDED(initialized)&&SUCCEEDED(CoCreateInstance(&quietClass,NULL,
        CLSCTX_INPROC_SERVER|CLSCTX_LOCAL_SERVER,&quietInterface,(void**)&settings))) {
        LPWSTR before=NULL;
        HRESULT hr=settings->lpVtbl->GetProfile(settings,&before);
        int matches=SUCCEEDED(hr)&&before&&!wcscmp(before,argv[8]);
        CoTaskMemFree(before);
        if(matches&&WaitForMultipleObjects(3,waits,FALSE,0)==WAIT_TIMEOUT&&
            SUCCEEDED(settings->lpVtbl->SetProfile(settings,quietProfile))) {
            SetEvent(ready);
            WaitForMultipleObjects(3,waits,FALSE,INFINITE);
            LPWSTR current=NULL;result=22;
            if(SUCCEEDED(settings->lpVtbl->GetProfile(settings,&current))) {
                /* A later explicit user change wins over our temporary choice. */
                result=current&&!wcscmp(current,quietProfile)?
                    (SUCCEEDED(settings->lpVtbl->SetProfile(settings,argv[8]))?0:23):0;
            }
            CoTaskMemFree(current);
        }
        settings->lpVtbl->Release(settings);
    }
    if(SUCCEEDED(initialized))CoUninitialize();
    if(ready)CloseHandle(ready);
    for(int i=0;i<3;i++)if(waits[i])CloseHandle(waits[i]);
    return result;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    (void)instance;(void)previous;(void)command;(void)show;
    int argc=0; LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv&&argc>1&&!wcscmp(argv[1],L"--notifications")){int result=notification_guard(argc,argv);LocalFree(argv);return result;}
    if(argv&&argc>3&&!wcscmp(argv[argc-2],L"--diagnostics")){diagnostics=argv[argc-1];argc-=2;}
    int validateOnly=argv&&argc>1&&!wcscmp(argv[argc-1],L"--validate-only");if(validateOnly)argc--;
    if(!argv||argc<12) {if(argv)LocalFree(argv);return 2;}
    DWORD adapter=(DWORD)wcstoul(argv[1],NULL,10),host=(DWORD)wcstoul(argv[3],NULL,10);
    guardHostPid=host;
    explorerPid=(DWORD)wcstoul(argv[5],NULL,10);
    original=(HWND)(UINT_PTR)_wcstoui64(argv[7],NULL,10);
    originallyVisible=wcstol(argv[8],NULL,10)==1;
    viewCount=argc-11; if(viewCount<1||viewCount>64){LocalFree(argv);return 2;}
    HANDLE processes[4]={OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,adapter),OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,host),OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,explorerPid),OpenEventW(SYNCHRONIZE,FALSE,argv[9])};
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[10]);
    int valid=ready&&processes[0]&&processes[1]&&processes[2]&&processes[3]&&adapter!=host&&host!=explorerPid;
    int failure=3;
    if(valid){failure=4;valid=same_time(processes[0],_wcstoui64(argv[2],NULL,10))&&same_time(processes[1],_wcstoui64(argv[4],NULL,10))&&same_time(processes[2],_wcstoui64(argv[6],NULL,10));}
    if(valid){failure=5;valid=original_valid();}
    for(int i=0;i<viewCount&&valid;i++) {
        views[i]=(HWND)(UINT_PTR)_wcstoui64(argv[11+i],NULL,10);DWORD pid=0;
        GetWindowThreadProcessId(views[i],&pid);
        if(validateOnly){wchar_t cls[80]=L"";GetClassNameW(GetParent(views[i]),cls,80);fwprintf(stdout,L"view=%llu parent=%llu expected=%llu class=%ls style=%llx visible=%d\n",(unsigned long long)(UINT_PTR)views[i],(unsigned long long)(UINT_PTR)GetParent(views[i]),(unsigned long long)(UINT_PTR)((GetWindowLongPtrW(views[i],GWL_STYLE)&WS_CHILD)?GetParent(original):GetAncestor(original,GA_ROOT)),cls,(unsigned long long)GetWindowLongPtrW(views[i],GWL_STYLE),IsWindowVisible(views[i]));fflush(stdout);}
        HWND expectedParent=(GetWindowLongPtrW(views[i],GWL_STYLE)&WS_CHILD)?GetParent(original):GetAncestor(original,GA_ROOT);
        DWORD ownerPid=0;GetWindowThreadProcessId(expectedParent,&ownerPid);
        trace_guard(L"validate-window",0,views[i],host);
        failure=60;valid=IsWindow(views[i]);
        if(valid){failure=61;valid=IsWindowVisible(views[i]);}
        if(valid){failure=62;valid=pid==host;}
        if(valid){failure=63;valid=ownerPid==explorerPid&&GetParent(views[i])==expectedParent;}
    }
    threadId=GetCurrentThreadId(); MSG message; PeekMessageW(&message,NULL,0,0,PM_NOREMOVE);
    HWINEVENTHOOK hook=NULL,foregroundHook=NULL,orderHook=NULL,revealHook=NULL;
    if(valid) {failure=7;hook=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_REORDER,NULL,changed,host,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);valid=hook!=NULL;}
    if(!validateOnly&&valid){
        failure=8;foregroundHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,NULL,changed,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        /* Cross-process app frames and desktop-container reorder notifications
           are required even without a new foreground transition. Coalesce them
           into a check, then lower only our verified surfaces on an inversion.
           No timer, foreground activation, or foreign-window restacking. */
        orderHook=SetWinEventHook(EVENT_OBJECT_SHOW,EVENT_OBJECT_REORDER,NULL,changed,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        revealHook=SetWinEventHook(EVENT_OBJECT_UNCLOAKED,EVENT_OBJECT_UNCLOAKED,NULL,changed,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        valid=foregroundHook&&orderHook&&revealHook&&lower_surfaces();
    }
    if(!validateOnly&&valid&&WaitForMultipleObjects(4,processes,FALSE,0)==WAIT_TIMEOUT) {
        ShowWindow(original,SW_HIDE);
        if(!IsWindowVisible(original)) {
            trace_guard(L"ready",0,views[0],host);SetEvent(ready);
            for(;;) {
                DWORD result=MsgWaitForMultipleObjects(4,processes,FALSE,INFINITE,QS_ALLINPUT);
                if(result!=WAIT_OBJECT_0+4){trace_guard(L"wait-ended",(int)result,views[0],host);break;}
                int quit=0;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){if(message.message==WM_QUIT){quit=1;break;}if(message.message==CHECK_LAYER){layerQueued=0;if(!lower_surfaces()){trace_guard(L"layer-invalid",0,views[0],host);quit=1;break;}continue;}TranslateMessage(&message);DispatchMessageW(&message);}
                if(quit)break;
            }
        }
        restore();
    }
    if(hook)UnhookWinEvent(hook);
    if(foregroundHook)UnhookWinEvent(foregroundHook);
    if(orderHook)UnhookWinEvent(orderHook);
    if(revealHook)UnhookWinEvent(revealHook);
    if(ready)CloseHandle(ready);
    for(int i=0;i<4;i++)if(processes[i])CloseHandle(processes[i]);
    trace_guard(L"exit",valid?0:failure,viewCount?views[0]:NULL,host);
    LocalFree(argv);return valid?0:failure;
}
