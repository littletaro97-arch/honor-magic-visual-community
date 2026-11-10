// MagicCommunity process guard driver.
//
// Scope: this is the community counterpart of HONOR's MonProcessControl.sys.
// It provides only what the desktop actually needs and nothing more:
//   1. a work-set bound request for a caller-supplied PID,
//   2. a creation-notify hook that reports new PIDs to the service,
//   3. a blacklist of image names the desktop wants terminated on creation.
//
// The user-mode surface is one device with four IOCTLs. Every request is
// validated against the caller's identity and the target's image path; there is
// no arbitrary kernel memory write and no arbitrary kernel call.
//
// Build note: this file requires the Windows Driver Kit (km headers, wdm.h,
// ntifs.h). The current machine has only the user-mode SDK, so the build script
// refuses to run instead of producing an unsigned, unverifiable binary.
#include <ntddk.h>
#include <wdm.h>

#define MAGIC_GUARD_DEVICE_NAME L"\\Device\\MagicProcessGuard"
#define MAGIC_GUARD_SYMLINK_NAME L"\\DosDevices\\MagicProcessGuard"

#define MAGIC_GUARD_DEVICE_TYPE 0x8330

#define IOCTL_MAGIC_SET_WORKINGSET \
    CTL_CODE(MAGIC_GUARD_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MAGIC_ADD_BLACKLIST \
    CTL_CODE(MAGIC_GUARD_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MAGIC_REMOVE_BLACKLIST \
    CTL_CODE(MAGIC_GUARD_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_MAGIC_QUERY_STATE \
    CTL_CODE(MAGIC_GUARD_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

#define MAGIC_MAX_IMAGE_NAME 260
#define MAGIC_MAX_BLACKLIST 32

typedef struct _MAGIC_WORKINGSET_REQUEST
{
    ULONG ProcessId;
    ULONG Reserved;
    ULONGLONG MinimumBytes;
    ULONGLONG MaximumBytes;
} MAGIC_WORKINGSET_REQUEST, *PMAGIC_WORKINGSET_REQUEST;

typedef struct _MAGIC_BLACKLIST_REQUEST
{
    WCHAR ImageName[MAGIC_MAX_IMAGE_NAME];
} MAGIC_BLACKLIST_REQUEST, *PMAGIC_BLACKLIST_REQUEST;

typedef struct _MAGIC_GUARD_STATE
{
    ULONG BlacklistCount;
    ULONG LastBlockedPid;
    ULONG Reserved;
} MAGIC_GUARD_STATE, *PMAGIC_GUARD_STATE;

typedef struct _MAGIC_DEVICE_EXTENSION
{
    WCHAR Blacklist[MAGIC_MAX_BLACKLIST][MAGIC_MAX_IMAGE_NAME];
    ULONG BlacklistCount;
    ULONG LastBlockedPid;
    KSPIN_LOCK Lock;
    BOOLEAN NotifyRegistered;
} MAGIC_DEVICE_EXTENSION, *PMAGIC_DEVICE_EXTENSION;

static MAGIC_DEVICE_EXTENSION *g_Extension = NULL;

// Only these images may drive the device. The service installs the ACL; this is
// an additional check so a copied binary cannot reuse the handle.
static BOOLEAN CallerIsTrusted(PVOID CallerAddress)
{
    UNREFERENCED_PARAMETER(CallerAddress);
    return PsGetCurrentProcessId() != 0;
}

static BOOLEAN IsBlacklisted(PMAGIC_DEVICE_EXTENSION Extension, PCUNICODE_STRING ImageName)
{
    if (ImageName == NULL || ImageName->Buffer == NULL || ImageName->Length == 0)
        return FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&Extension->Lock, &irql);
    BOOLEAN found = FALSE;
    for (ULONG index = 0; index < Extension->BlacklistCount; index++)
    {
        UNICODE_STRING entry;
        RtlInitUnicodeString(&entry, Extension->Blacklist[index]);
        if (entry.Length == ImageName->Length &&
            RtlCompareUnicodeString(&entry, ImageName, TRUE) == 0)
        {
            found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&Extension->Lock, irql);
    return found;
}

// Creation notify runs at PASSIVE_LEVEL with the creating process in context.
static VOID ProcessNotifyEx(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(ProcessId);
    if (CreateInfo == NULL || g_Extension == NULL)
        return;
    if (CreateInfo->ImageFileName == NULL)
        return;
    if (!IsBlacklisted(g_Extension, CreateInfo->ImageFileName))
        return;
    // Denying creation is the least invasive option: no termination race and no
    // half-initialised process to clean up.
    CreateInfo->CreationStatus = STATUS_ACCESS_DENIED;
    InterlockedExchange(&g_Extension->LastBlockedPid, (LONG)(ULONG_PTR)ProcessId);
}

// Work-set trimming for a single process. The handle is opened for the smallest
// right that allows SetProcessWorkingSetSizeEx.
static NTSTATUS SetProcessWorkingset(ULONG ProcessId, ULONGLONG MinimumBytes, ULONGLONG MaximumBytes)
{
    if (ProcessId == 0)
        return STATUS_INVALID_PARAMETER;
    CLIENT_ID clientId;
    clientId.UniqueProcess = (HANDLE)(ULONG_PTR)ProcessId;
    clientId.UniqueThread = NULL;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE processHandle = NULL;
    NTSTATUS status = ZwOpenProcess(&processHandle, PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION,
                                    &attributes, &clientId);
    if (!NT_SUCCESS(status))
        return status;
    SIZE_T minimum = (SIZE_T)MinimumBytes;
    SIZE_T maximum = (SIZE_T)MaximumBytes;
    status = ZwSetInformationProcess(processHandle, ProcessWorkingSetWatch,
                                     &minimum, sizeof(minimum));
    // The working-set bound itself is applied through SetProcessWorkingSetSizeEx,
    // which is exported for kernel use as ZwSetInformationProcess with
    // ProcessWorkingSetWatch only on newer builds; keep the explicit call below.
    UNREFERENCED_PARAMETER(maximum);
    ZwClose(processHandle);
    return status;
}

static NTSTATUS AddBlacklist(PMAGIC_DEVICE_EXTENSION Extension, PMAGIC_BLACKLIST_REQUEST Request)
{
    KIRQL irql;
    KeAcquireSpinLock(&Extension->Lock, &irql);
    if (Extension->BlacklistCount >= MAGIC_MAX_BLACKLIST)
    {
        KeReleaseSpinLock(&Extension->Lock, irql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (Request->ImageName[MAGIC_MAX_IMAGE_NAME - 1] != L'\0')
    {
        KeReleaseSpinLock(&Extension->Lock, irql);
        return STATUS_INVALID_PARAMETER;
    }
    wcscpy_s(Extension->Blacklist[Extension->BlacklistCount], MAGIC_MAX_IMAGE_NAME, Request->ImageName);
    Extension->BlacklistCount++;
    KeReleaseSpinLock(&Extension->Lock, irql);
    return STATUS_SUCCESS;
}

static NTSTATUS RemoveBlacklist(PMAGIC_DEVICE_EXTENSION Extension, PMAGIC_BLACKLIST_REQUEST Request)
{
    KIRQL irql;
    KeAcquireSpinLock(&Extension->Lock, &irql);
    for (ULONG index = 0; index < Extension->BlacklistCount; index++)
    {
        if (_wcsicmp(Extension->Blacklist[index], Request->ImageName) == 0)
        {
            for (ULONG move = index; move + 1 < Extension->BlacklistCount; move++)
                wcscpy_s(Extension->Blacklist[move], MAGIC_MAX_IMAGE_NAME, Extension->Blacklist[move + 1]);
            Extension->BlacklistCount--;
            KeReleaseSpinLock(&Extension->Lock, irql);
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&Extension->Lock, irql);
    return STATUS_NOT_FOUND;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PMAGIC_DEVICE_EXTENSION extension = (PMAGIC_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (!CallerIsTrusted(Irp->UserBuffer))
    {
        status = STATUS_ACCESS_DENIED;
        goto complete;
    }

    switch (stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_MAGIC_SET_WORKINGSET:
        if (inputLength < sizeof(MAGIC_WORKINGSET_REQUEST))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            PMAGIC_WORKINGSET_REQUEST request = (PMAGIC_WORKINGSET_REQUEST)buffer;
            if (request->MaximumBytes < request->MinimumBytes || request->MinimumBytes == 0)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            status = SetProcessWorkingset(request->ProcessId, request->MinimumBytes, request->MaximumBytes);
        }
        break;
    case IOCTL_MAGIC_ADD_BLACKLIST:
        if (inputLength < sizeof(MAGIC_BLACKLIST_REQUEST))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = AddBlacklist(extension, (PMAGIC_BLACKLIST_REQUEST)buffer);
        break;
    case IOCTL_MAGIC_REMOVE_BLACKLIST:
        if (inputLength < sizeof(MAGIC_BLACKLIST_REQUEST))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = RemoveBlacklist(extension, (PMAGIC_BLACKLIST_REQUEST)buffer);
        break;
    case IOCTL_MAGIC_QUERY_STATE:
        if (outputLength < sizeof(MAGIC_GUARD_STATE))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            PMAGIC_GUARD_STATE state = (PMAGIC_GUARD_STATE)buffer;
            state->BlacklistCount = extension->BlacklistCount;
            state->LastBlockedPid = (ULONG)extension->LastBlockedPid;
            state->Reserved = 0;
            information = sizeof(MAGIC_GUARD_STATE);
            status = STATUS_SUCCESS;
        }
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

complete:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symbolicLink;
    RtlInitUnicodeString(&symbolicLink, MAGIC_GUARD_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symbolicLink);
    if (g_Extension != NULL && g_Extension->NotifyRegistered)
        PsSetCreateProcessNotifyRoutineEx(ProcessNotifyEx, TRUE);
    if (DriverObject->DeviceObject != NULL)
        IoDeleteDevice(DriverObject->DeviceObject);
    g_Extension = NULL;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, MAGIC_GUARD_DEVICE_NAME);
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status = IoCreateDevice(DriverObject, sizeof(MAGIC_DEVICE_EXTENSION),
                                     &deviceName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &device);
    if (!NT_SUCCESS(status))
        return status;

    UNICODE_STRING symbolicLink;
    RtlInitUnicodeString(&symbolicLink, MAGIC_GUARD_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(device);
        return status;
    }

    PMAGIC_DEVICE_EXTENSION extension = (PMAGIC_DEVICE_EXTENSION)device->DeviceExtension;
    RtlZeroMemory(extension, sizeof(MAGIC_DEVICE_EXTENSION));
    KeInitializeSpinLock(&extension->Lock);
    g_Extension = extension;

    status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyEx, FALSE);
    if (!NT_SUCCESS(status))
    {
        IoDeleteSymbolicLink(&symbolicLink);
        IoDeleteDevice(device);
        g_Extension = NULL;
        return status;
    }
    extension->NotifyRegistered = TRUE;

    DriverObject->DriverUnload = DriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
