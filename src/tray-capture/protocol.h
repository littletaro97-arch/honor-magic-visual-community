#pragma once
#include <windows.h>
#define MAGIC_TRAY_SIGNATURE 0x59415254u
#define MAGIC_TRAY_VERSION 2u
#define MAGIC_TRAY_VERSION_UNKNOWN 0xffffffffu
#define MAGIC_TRAY_CAPACITY 256u
#define MAGIC_TRAY_ICONS 128u
#define MAGIC_TRAY_ICON_SIZE 64u
#define MAGIC_TRAY_PROPERTY L"MagicCommunity.NativeTrayCapture.2"
#define MAGIC_TRAY_RELEASE L"MagicCommunity.NativeTrayRelease.2"
#define MAGIC_TRAY_COMMAND L"MagicCommunity.NativeTrayCommand.2"
#define MAGIC_TRAY_BYTES (MAGIC_TRAY_ICON_SIZE*MAGIC_TRAY_ICON_SIZE*4u)
// Fixed-width IPC; process-local pointers never cross this boundary.
typedef struct MagicTrayEntry {
    ULONG event, flags, id, callbackMessage, version, state, stateMask;
    ULONG ownerPid; ULONGLONG ownerHwnd; GUID iconGuid, token;
    WCHAR tooltip[128]; ULONG hasPixels, pixelWidth, pixelHeight; BYTE pixels[MAGIC_TRAY_BYTES];
} MagicTrayEntry;
typedef struct MagicTrayShared {
    ULONG signature,version,bytes,targetPid,ownerPid;
    ULONGLONG ownerCreated;
    volatile LONG lock,writeIndex,readIndex,dropped,request,completed,result;
    ULONG action;GUID actionToken;LONG x,y;
    WCHAR eventName[100];
    MagicTrayEntry entries[MAGIC_TRAY_CAPACITY];
} MagicTrayShared;
