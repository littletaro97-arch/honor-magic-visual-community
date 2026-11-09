#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include "protocol.h"
#pragma pack(push,4)
typedef struct Nid32 {DWORD size,hwnd,id,flags,message,icon;WCHAR tip[128];DWORD state,stateMask;WCHAR info[256];UINT version;WCHAR title[64];DWORD infoFlags;GUID guid;DWORD balloon;} Nid32;
#pragma pack(pop)
typedef struct PrivateIcon {BOOL used;MagicTrayEntry data;ULONGLONG ownerCreated;} PrivateIcon;
static PrivateIcon icons[MAGIC_TRAY_ICONS];
static SRWLOCK localLock=SRWLOCK_INIT;
static UINT releaseMessage;static ATOM attachedAtom;static HANDLE mapping,changed;static MagicTrayShared* shared;static UINT command;
static ULONGLONG Created(DWORD pid){HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);FILETIME c,e,k,u;ULONGLONG value=0;if(p){if(GetProcessTimes(p,&c,&e,&k,&u))value=((ULONGLONG)c.dwHighDateTime<<32)|c.dwLowDateTime;CloseHandle(p);}return value;}
static void Detach(void){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(changed)CloseHandle(changed);shared=NULL;mapping=NULL;changed=NULL;attachedAtom=0;ZeroMemory(icons,sizeof(icons));}
static BOOL Attach(HWND window){ATOM atom=(ATOM)(ULONG_PTR)GetPropW(window,MAGIC_TRAY_PROPERTY);WCHAR name[100];HANDLE owner;BOOL valid=FALSE;if(!atom)return FALSE;if(atom==attachedAtom&&shared)return TRUE;Detach();if(!GlobalGetAtomNameW(atom,name,100)||wcsncmp(name,L"Local\\MagicNativeTray-",22))return FALSE;
    mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name);if(!mapping)return FALSE;shared=(MagicTrayShared*)MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(MagicTrayShared));if(!shared){Detach();return FALSE;}
    if(shared->signature!=MAGIC_TRAY_SIGNATURE||shared->version!=MAGIC_TRAY_VERSION||shared->bytes!=sizeof(MagicTrayShared)||shared->targetPid!=GetCurrentProcessId()){Detach();return FALSE;}
    owner=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,shared->ownerPid);if(owner){valid=WaitForSingleObject(owner,0)==WAIT_TIMEOUT&&Created(shared->ownerPid)==shared->ownerCreated;CloseHandle(owner);}if(!valid){Detach();return FALSE;}
    shared->eventName[99]=0;changed=OpenEventW(EVENT_MODIFY_STATE,FALSE,shared->eventName);if(!changed){Detach();return FALSE;}attachedAtom=atom;command=RegisterWindowMessageW(MAGIC_TRAY_COMMAND);releaseMessage=RegisterWindowMessageW(MAGIC_TRAY_RELEASE);return TRUE;}
static BOOL Push(MagicTrayEntry const* item){LONG write,read;if(!shared)return FALSE;if(InterlockedCompareExchange(&shared->lock,1,0)!=0){InterlockedIncrement(&shared->dropped);return FALSE;}write=shared->writeIndex;read=shared->readIndex;if((ULONG)(write-read)>=MAGIC_TRAY_CAPACITY){InterlockedExchange(&shared->lock,0);InterlockedIncrement(&shared->dropped);SetEvent(changed);return FALSE;}shared->entries[(ULONG)write%MAGIC_TRAY_CAPACITY]=*item;InterlockedExchange(&shared->writeIndex,write+1);InterlockedExchange(&shared->lock,0);SetEvent(changed);return TRUE;}
static BOOL HasGuid(GUID const* id){static GUID empty;return memcmp(id,&empty,sizeof(GUID))!=0;}
static BOOL Same(MagicTrayEntry const* a,MagicTrayEntry const* b){if(HasGuid(&a->iconGuid)&&HasGuid(&b->iconGuid))return memcmp(&a->iconGuid,&b->iconGuid,sizeof(GUID))==0;return a->ownerHwnd==b->ownerHwnd&&a->id==b->id;}
static BOOL Pixels(HICON icon,BYTE* out,ULONG* pixelWidth,ULONG* pixelHeight){
    ICONINFO source;BITMAP size;BITMAPINFO info;HDC dc;HBITMAP bitmap,old;BYTE* pixels;
    ULONG n,bytes;BOOL good=FALSE;int width=0,height=0;
    *pixelWidth=*pixelHeight=0;ZeroMemory(out,MAGIC_TRAY_BYTES);ZeroMemory(&source,sizeof(source));ZeroMemory(&size,sizeof(size));
    if(!icon||!GetIconInfo(icon,&source))return FALSE;
    if(source.hbmColor&&GetObject(source.hbmColor,sizeof(size),&size)){width=size.bmWidth;height=size.bmHeight;}
    else if(source.hbmMask&&GetObject(source.hbmMask,sizeof(size),&size)){width=size.bmWidth;height=size.bmHeight/2;}
    if(source.hbmColor)DeleteObject(source.hbmColor);if(source.hbmMask)DeleteObject(source.hbmMask);
    if(width<1||height<1)return FALSE;
    /* Preserve the supplied raster; do not enlarge 16/32px status images first.
       Higher-resolution application-provided state images keep up to 64px. */
    if(width>MAGIC_TRAY_ICON_SIZE||height>MAGIC_TRAY_ICON_SIZE){
        int largest=max(width,height);width=max(1,MulDiv(width,MAGIC_TRAY_ICON_SIZE,largest));height=max(1,MulDiv(height,MAGIC_TRAY_ICON_SIZE,largest));
    }
    bytes=(ULONG)width*(ULONG)height*4u;
    ZeroMemory(&info,sizeof(info));info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height*2;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    dc=CreateCompatibleDC(NULL);if(!dc)return FALSE;
    bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void**)&pixels,NULL,0);
    if(!bitmap){DeleteDC(dc);return FALSE;}old=(HBITMAP)SelectObject(dc,bitmap);
    ZeroMemory(pixels,bytes);memset(pixels+bytes,255,bytes);
    if(DrawIconEx(dc,0,0,icon,width,height,0,NULL,DI_NORMAL)&&DrawIconEx(dc,0,height,icon,width,height,0,NULL,DI_NORMAL)){
        GdiFlush();for(n=0;n<bytes;n+=4){
            int b=pixels[bytes+n]-pixels[n],g=pixels[bytes+n+1]-pixels[n+1],r=pixels[bytes+n+2]-pixels[n+2];
            int transparent=max(0,max(b,max(g,r)));out[n]=pixels[n];out[n+1]=pixels[n+1];out[n+2]=pixels[n+2];out[n+3]=(BYTE)(255-min(255,transparent));
        }
        *pixelWidth=(ULONG)width;*pixelHeight=(ULONG)height;good=TRUE;
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return good;
}
static BOOL Decode(COPYDATASTRUCT const* data,MagicTrayEntry* out,HICON* icon){BYTE const* bytes;DWORD signature,message,size;ULONG count;*icon=NULL;ZeroMemory(out,sizeof(*out));if(!data||data->dwData!=1||data->cbData<32||data->cbData>65536||!data->lpData)return FALSE;bytes=(BYTE const*)data->lpData;memcpy(&signature,bytes,4);memcpy(&message,bytes+4,4);memcpy(&size,bytes+8,4);if(signature!=0x34753423||message>NIM_SETVERSION||size<24||size>sizeof(NOTIFYICONDATAW)||size+8>data->cbData)return FALSE;out->event=message;
    BOOL nativeLayout=(size==sizeof(NOTIFYICONDATAW)||size==NOTIFYICONDATAW_V3_SIZE||size==NOTIFYICONDATAW_V2_SIZE||size==NOTIFYICONDATAW_V1_SIZE);
    /* Win32 V3 (952 bytes) overlaps Win64 V2. Resolve the owner before
       selecting a layout; GUID-only Win32 deletes have no owner HWND. */
    if(size==offsetof(Nid32,balloon)&&size==NOTIFYICONDATAW_V2_SIZE){NOTIFYICONDATAW native;Nid32 wow;DWORD pid=0;ZeroMemory(&native,sizeof(native));ZeroMemory(&wow,sizeof(wow));memcpy(&native,bytes+8,min(size,sizeof(native)));memcpy(&wow,bytes+8,min(size,sizeof(wow)));GetWindowThreadProcessId(native.hWnd,&pid);if(!pid&&(IsWindow((HWND)(ULONG_PTR)wow.hwnd)||(wow.flags&NIF_GUID)))nativeLayout=FALSE;}
    if(nativeLayout){NOTIFYICONDATAW n;ZeroMemory(&n,sizeof(n));memcpy(&n,bytes+8,min(size,sizeof(n)));out->ownerHwnd=(ULONGLONG)(ULONG_PTR)n.hWnd;out->id=n.uID;out->flags=n.uFlags;out->callbackMessage=n.uCallbackMessage;*icon=n.hIcon;out->version=n.uVersion;out->state=n.dwState;out->stateMask=n.dwStateMask;if(n.uFlags&NIF_GUID)out->iconGuid=n.guidItem;count=min(128,(size>offsetof(NOTIFYICONDATAW,szTip)?(size-(ULONG)offsetof(NOTIFYICONDATAW,szTip))/sizeof(WCHAR):0));if(count)memcpy(out->tooltip,n.szTip,count*sizeof(WCHAR));}
    else{Nid32 n;ZeroMemory(&n,sizeof(n));memcpy(&n,bytes+8,min(size,sizeof(n)));out->ownerHwnd=n.hwnd;out->id=n.id;out->flags=n.flags;out->callbackMessage=n.message;*icon=(HICON)(ULONG_PTR)n.icon;out->version=n.version;out->state=n.state;out->stateMask=n.stateMask;if(n.flags&NIF_GUID)out->iconGuid=n.guid;count=min(128,(size>offsetof(Nid32,tip)?(size-(ULONG)offsetof(Nid32,tip))/sizeof(WCHAR):0));if(count)memcpy(out->tooltip,n.tip,count*sizeof(WCHAR));}
    out->tooltip[127]=0;GetWindowThreadProcessId((HWND)(ULONG_PTR)out->ownerHwnd,&out->ownerPid);return (out->ownerPid&&IsWindow((HWND)(ULONG_PTR)out->ownerHwnd))||HasGuid(&out->iconGuid);}
// A capture attached after Qt started can first see a duplicate NIM_ADD or
// NIM_MODIFY without its original NIM_SETVERSION. Qt 5 initializes uVersion=4 on its
// ordinary updates too. This is a bounded compatibility hint, not a
// general interpretation of the uTimeout/uVersion union on NIM_MODIFY.
static BOOL LateQtVersion4(MagicTrayEntry const* item){
    WCHAR name[128];ULONG required=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
    if((item->event!=NIM_MODIFY&&item->event!=NIM_ADD)||item->version!=NOTIFYICON_VERSION_4||(item->flags&required)!=required||(item->flags&NIF_INFO)||item->id>0xffff)return FALSE;
    if(!GetClassNameW((HWND)(ULONG_PTR)item->ownerHwnd,name,128))return FALSE;
    return wcsncmp(name,L"Qt5",3)==0&&wcsstr(name,L"TrayIconMessageWindowClass")!=NULL;
}
static void Capture(CWPRETSTRUCT const* call){MagicTrayEntry item;HICON icon;ULONG i,freeSlot=MAGIC_TRAY_ICONS;PrivateIcon* target=NULL;if(call->message!=WM_COPYDATA||!call->lParam||!Decode((COPYDATASTRUCT*)call->lParam,&item,&icon))return;
    // Existing clients may re-add the same identifier after TaskbarCreated.
    // Require a current Shell-owned identifier for unsuccessful re-adds.
    if(!call->lResult){NOTIFYICONIDENTIFIER identity;RECT rect;ZeroMemory(&identity,sizeof(identity));identity.cbSize=sizeof(identity);identity.hWnd=(HWND)(ULONG_PTR)item.ownerHwnd;identity.uID=item.id;identity.guidItem=item.iconGuid;if(item.event!=NIM_ADD||FAILED(Shell_NotifyIconGetRect(&identity,&rect)))return;}
    for(i=0;i<MAGIC_TRAY_ICONS;i++){if(icons[i].used&&Same(&icons[i].data,&item)){target=&icons[i];break;}if(!icons[i].used&&freeSlot==MAGIC_TRAY_ICONS)freeSlot=i;}
    if(target&&item.event!=NIM_DELETE&&item.ownerHwnd&&((ULONGLONG)target->data.ownerHwnd!=item.ownerHwnd||target->data.ownerPid!=item.ownerPid||target->ownerCreated!=Created(item.ownerPid))){MagicTrayEntry removed=target->data;removed.event=NIM_DELETE;Push(&removed);ZeroMemory(target,sizeof(*target));target=NULL;freeSlot=i;}
    if(!target){if(!item.ownerPid||item.event==NIM_DELETE||freeSlot==MAGIC_TRAY_ICONS){if(freeSlot==MAGIC_TRAY_ICONS)InterlockedIncrement(&shared->dropped);return;}target=&icons[freeSlot];ZeroMemory(target,sizeof(*target));target->used=TRUE;target->data=item;target->data.version=MAGIC_TRAY_VERSION_UNKNOWN;target->ownerCreated=Created(item.ownerPid);if(BCryptGenRandom(NULL,(PUCHAR)&target->data.token,sizeof(GUID),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){ZeroMemory(target,sizeof(*target));InterlockedIncrement(&shared->dropped);return;}}
    if(item.event==NIM_DELETE){item=target->data;item.event=NIM_DELETE;Push(&item);ZeroMemory(target,sizeof(*target));return;}
    target->data.event=item.event;target->data.flags=item.flags;
    if(item.flags&NIF_MESSAGE)target->data.callbackMessage=item.callbackMessage;
    if(item.flags&NIF_TIP)memcpy(target->data.tooltip,item.tooltip,sizeof(item.tooltip));
    if(item.flags&NIF_STATE){target->data.state=(target->data.state&~item.stateMask)|(item.state&item.stateMask);target->data.stateMask=item.stateMask;}
    if(item.event==NIM_ADD&&call->lResult)target->data.version=0;
    if(item.event==NIM_SETVERSION)target->data.version=item.version;
    else if(target->data.version==MAGIC_TRAY_VERSION_UNKNOWN&&LateQtVersion4(&item))target->data.version=NOTIFYICON_VERSION_4;
    if((item.flags&NIF_ICON)&&icon)target->data.hasPixels=Pixels(icon,target->data.pixels,&target->data.pixelWidth,&target->data.pixelHeight);
    Push(&target->data);
}
static BOOL PostCallback(PrivateIcon const* item,UINT event,POINT point){
    BOOL modern=item->data.version!=MAGIC_TRAY_VERSION_UNKNOWN&&item->data.version>=NOTIFYICON_VERSION_4;
    WPARAM w=modern?MAKEWPARAM((WORD)point.x,(WORD)point.y):item->data.id;
    LPARAM l=modern?MAKELPARAM(event,(WORD)item->data.id):event;
    return PostMessageW((HWND)(ULONG_PTR)item->data.ownerHwnd,item->data.callbackMessage,w,l);
}
static void Command(void){
    LONG request=shared->request;ULONG action=shared->action,i;GUID token=shared->actionToken;POINT point={shared->x,shared->y};LONG result=ERROR_NOT_FOUND;
    if(action==0){for(i=0;i<MAGIC_TRAY_ICONS;i++)if(icons[i].used)Push(&icons[i].data);result=ERROR_SUCCESS;}
    else if(action<=4)for(i=0;i<MAGIC_TRAY_ICONS;i++){
        PrivateIcon* item=&icons[i];DWORD pid=0;HWND owner=(HWND)(ULONG_PTR)item->data.ownerHwnd;BOOL ok=FALSE;
        if(!item->used||memcmp(&item->data.token,&token,sizeof(GUID)))continue;
        GetWindowThreadProcessId(owner,&pid);if(pid!=item->data.ownerPid||!item->ownerCreated||Created(pid)!=item->ownerCreated){result=ERROR_INVALID_WINDOW_HANDLE;break;}
        if(item->data.callbackMessage<WM_USER||item->data.callbackMessage>0xffff){result=ERROR_INVALID_DATA;break;}
        SetLastError(0);
        /* Version 4 changes packing and adds selection notifications; it does
           not replace physical mouse callbacks. Mouse-driven clients consume
           LBUTTONUP, while selection-driven clients consume NIN_SELECT. Keep
           one ordered gesture for both, including the double-click release. */
        if(item->data.version!=MAGIC_TRAY_VERSION_UNKNOWN&&item->data.version>=NOTIFYICON_VERSION_4&&(action==1||action==3))
            ok=PostCallback(item,action==1?WM_LBUTTONDOWN:WM_LBUTTONDBLCLK,point)&&PostCallback(item,WM_LBUTTONUP,point)&&PostCallback(item,NIN_SELECT,point);
        else if(item->data.version!=MAGIC_TRAY_VERSION_UNKNOWN&&item->data.version>=NOTIFYICON_VERSION_4&&action==2)ok=PostCallback(item,WM_RBUTTONDOWN,point)&&PostCallback(item,WM_RBUTTONUP,point)&&PostCallback(item,WM_CONTEXTMENU,point);
        else {
            UINT down=action==1?WM_LBUTTONDOWN:action==2?WM_RBUTTONDOWN:action==3?WM_LBUTTONDBLCLK:WM_MBUTTONDOWN;
            UINT up=action==3&&item->data.version==NOTIFYICON_VERSION_4?NIN_SELECT:action==1||action==3?WM_LBUTTONUP:action==2?WM_RBUTTONUP:WM_MBUTTONUP;
            ok=PostCallback(item,down,point)&&PostCallback(item,up,point);
        }
        result=ok?ERROR_SUCCESS:GetLastError();break;
    }
    InterlockedExchange(&shared->result,result);InterlockedExchange(&shared->completed,request);SetEvent(changed);
}
__declspec(dllexport) LRESULT CALLBACK MagicTrayCaptureProc(int code,WPARAM w,LPARAM l){
    if(code>=0&&l&&TryAcquireSRWLockExclusive(&localLock)){__try {CWPRETSTRUCT const* call=(CWPRETSTRUCT*)l;if(releaseMessage&&call->message==releaseMessage&&(ATOM)call->wParam==attachedAtom)Detach();else if(Attach(call->hwnd)){if(call->message==command&&(ATOM)call->wParam==attachedAtom)Command();else Capture(call);}}__except(EXCEPTION_EXECUTE_HANDLER){if(shared)InterlockedIncrement(&shared->dropped);}ReleaseSRWLockExclusive(&localLock);}
    return CallNextHookEx(NULL,code,w,l);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID reserved){(void)reserved;if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(module);else if(reason==DLL_PROCESS_DETACH)Detach();return TRUE;}
