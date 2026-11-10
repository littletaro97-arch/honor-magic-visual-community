// MagicCommunity memory service: hosts the privilege-gated system memory-list
// clean so the desktop UI never needs an elevated token. The service runs as
// LocalSystem, exposes one authenticated named pipe, and accepts only the
// operations below. No generic "run this command" surface exists.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <string>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{
constexpr wchar_t ServiceName[] = L"MagicCommunityMemoryService";
constexpr wchar_t PipeLeaf[] = L"MagicCommunityMemory-v1";
std::wstring PipeNameValue()
{
    std::wstring name;
    name.push_back(L'\\');
    name.push_back(L'\\');
    name.push_back(L'.');
    name.push_back(L'\\');
    name.append(L"pipe");
    name.push_back(L'\\');
    name.append(PipeLeaf);
    return name;
}
constexpr int SystemMemoryListInformation = 80;
constexpr ULONG MemoryPurgeStandbyList = 4;
constexpr ULONG MemoryEmptyWorkingSets = 2;
constexpr LONG StatusPrivilegeNotHeld = 0xC0000061L;
constexpr LONG StatusProcedureNotFound = 0xC000007AL;
constexpr DWORD MaxRequest = 4096;

SERVICE_STATUS_HANDLE g_status{};
SERVICE_STATUS g_state{};
HANDLE g_stop{};
HANDLE g_cleanNow{};

using NtSetSystemInformationFn = LONG(NTAPI *)(int, PVOID, ULONG);

// Errors are reported as numbers so the client can distinguish "not admin" from
// "native call failed" without parsing localized text.
struct Receipt
{
    LONG purgeStatus;
    LONG emptyStatus;
    DWORD privilegeError;
    bool privilegeEnabled;
    unsigned long long availableBefore;
    unsigned long long availableAfter;
};

NtSetSystemInformationFn ResolveSetInformation()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return nullptr;
    return reinterpret_cast<NtSetSystemInformationFn>(GetProcAddress(ntdll, "NtSetSystemInformation"));
}

bool EnablePrivileges(DWORD *error)
{
    *error = ERROR_SUCCESS;
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        *error = GetLastError();
        return false;
    }
    const wchar_t *names[]{L"SeIncreaseQuotaPrivilege", L"SeProfileSingleProcessPrivilege"};
    bool all = true;
    for (const wchar_t *name : names)
    {
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, name, &luid))
        {
            *error = GetLastError();
            all = false;
            continue;
        }
        TOKEN_PRIVILEGES request{};
        request.PrivilegeCount = 1;
        request.Privileges[0].Luid = luid;
        request.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!AdjustTokenPrivileges(token, FALSE, &request, sizeof(request), nullptr, nullptr))
        {
            *error = GetLastError();
            all = false;
            continue;
        }
        DWORD adjustError = GetLastError();
        if (adjustError != ERROR_SUCCESS)
        {
            *error = adjustError;
            all = false;
        }
    }
    CloseHandle(token);
    return all;
}

unsigned long long AvailablePhysical()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
        return 0;
    return status.ullAvailPhys;
}

Receipt Clean()
{
    Receipt receipt{};
    receipt.availableBefore = AvailablePhysical();
    receipt.privilegeEnabled = EnablePrivileges(&receipt.privilegeError);
    auto setInformation = ResolveSetInformation();
    if (!setInformation)
    {
        receipt.purgeStatus = StatusProcedureNotFound;
        receipt.emptyStatus = StatusProcedureNotFound;
    }
    else
    {
        ULONG purge = MemoryPurgeStandbyList;
        receipt.purgeStatus = setInformation(SystemMemoryListInformation, &purge, sizeof(purge));
        ULONG empty = MemoryEmptyWorkingSets;
        receipt.emptyStatus = setInformation(SystemMemoryListInformation, &empty, sizeof(empty));
    }
    Sleep(400);
    receipt.availableAfter = AvailablePhysical();
    return receipt;
}

std::wstring FormatReceipt(Receipt const &r)
{
    wchar_t buffer[512]{};
    swprintf_s(buffer, L"{\"ok\":true,\"purgeStatus\":%ld,\"emptyStatus\":%ld,"
                       L"\"privilegeEnabled\":%s,\"privilegeError\":%lu,"
                       L"\"availableBefore\":%llu,\"availableAfter\":%llu,"
                       L"\"statusPrivilegeNotHeld\":%s}",
               r.purgeStatus, r.emptyStatus,
               r.privilegeEnabled ? L"true" : L"false", r.privilegeError,
               r.availableBefore, r.availableAfter,
               (r.purgeStatus == StatusPrivilegeNotHeld || r.emptyStatus == StatusPrivilegeNotHeld) ? L"true" : L"false");
    return buffer;
}

// Only SYSTEM, Administrators and the interactive user may connect. The client
// is additionally checked for a same-session token before the clean runs.
bool CreatePipeSecurity(SECURITY_ATTRIBUTES *attributes, PSECURITY_DESCRIPTOR *descriptor)
{
    // SY = LocalSystem, BA = Builtin Administrators, IU = interactive logon.
    const wchar_t *sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, descriptor, nullptr))
        return false;
    attributes->nLength = sizeof(SECURITY_ATTRIBUTES);
    attributes->lpSecurityDescriptor = *descriptor;
    attributes->bInheritHandle = FALSE;
    return true;
}

bool SameSession(HANDLE pipe)
{
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid))
        return false;
    DWORD session = 0;
    if (!ProcessIdToSessionId(clientPid, &session))
        return false;
    DWORD serverSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &serverSession);
    // Session 0 is the service session; the client must be in a user session.
    return session != 0 || clientPid == GetCurrentProcessId();
}

// P3: stop a host service on request. The allow-list is fixed in the binary so
// the pipe cannot be used to stop arbitrary services.
constexpr wchar_t const *StoppableServices[] = {L"SysMain", L"whesvc"};

bool IsStoppable(std::wstring const &name)
{
    for (auto const *allowed : StoppableServices)
        if (_wcsicmp(name.c_str(), allowed) == 0)
            return true;
    return false;
}

struct StopResult
{
    DWORD queryBefore = 0;
    DWORD stopStatus = 0;
    DWORD queryAfter = 0;
    DWORD win32Error = 0;
};

DWORD QueryState(SC_HANDLE manager, wchar_t const *name)
{
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS);
    if (!service)
        return 0;
    SERVICE_STATUS status{};
    DWORD state = QueryServiceStatus(service, &status) ? status.dwCurrentState : 0;
    CloseServiceHandle(service);
    return state;
}

StopResult StopHostService(std::wstring const &name)
{
    StopResult result{};
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
    {
        result.win32Error = GetLastError();
        return result;
    }
    wchar_t const *target = _wcsicmp(name.c_str(), L"SysMain") == 0 ? L"SysMain" : L"whesvc";
    result.queryBefore = QueryState(manager, target);
    SC_HANDLE service = OpenServiceW(manager, target, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service)
    {
        result.win32Error = GetLastError();
        CloseServiceHandle(manager);
        return result;
    }
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status))
        result.win32Error = GetLastError();
    // Wait briefly so the receipt reflects the settled state.
    for (int i = 0; i < 20; i++)
    {
        SERVICE_STATUS current{};
        if (!QueryServiceStatus(service, &current))
            break;
        if (current.dwCurrentState == SERVICE_STOPPED)
            break;
        Sleep(250);
    }
    CloseServiceHandle(service);
    result.queryAfter = QueryState(manager, target);
    CloseServiceHandle(manager);
    return result;
}

std::wstring FormatStop(std::wstring const &name, StopResult const &r)
{
    wchar_t buffer[512]{};
    swprintf_s(buffer, L"{\"ok\":%s,\"service\":\"%s\",\"before\":%lu,\"after\":%lu,"
                       L"\"win32Error\":%lu,\"stopped\":%s}",
               (r.queryAfter == SERVICE_STOPPED ? L"true" : L"false"),
               name.c_str(), r.queryBefore, r.queryAfter, r.win32Error,
               (r.queryAfter == SERVICE_STOPPED ? L"true" : L"false"));
    return buffer;
}

bool DirectoryExists(std::wstring const &path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool PathExists(std::wstring const &path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsReparsePoint(std::wstring const &path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// P4: move one known user directory to another drive and leave a junction.
// Only the three shell folders below are accepted, and the target must be an
// existing local directory on a fixed drive. The receipt lists what moved so a
// caller can roll back.
constexpr wchar_t const *MigratableFolders[] = {L"Desktop", L"Downloads", L"Documents"};

bool IsMigratable(std::wstring const &name)
{
    for (auto const *allowed : MigratableFolders)
        if (_wcsicmp(name.c_str(), allowed) == 0)
            return true;
    return false;
}

struct MigrateResult
{
    DWORD win32Error = 0;
    bool moved = false;
    bool junctioned = false;
    bool rolledBack = false;
    std::wstring source;
    std::wstring destination;
};

MigrateResult MigrateFolder(std::wstring const &name, std::wstring const &targetRoot, std::wstring const &userProfile)
{
    MigrateResult result{};
    if (!IsMigratable(name) || targetRoot.empty())
    {
        result.win32Error = ERROR_INVALID_PARAMETER;
        return result;
    }
    result.source = userProfile + L"\\" + name;
    result.destination = targetRoot + L"\\" + name;
    bool targetAbsolute = targetRoot.size() > 2 && targetRoot[1] == L':';
    if (!DirectoryExists(targetRoot) || !targetAbsolute)
    {
        result.win32Error = ERROR_PATH_NOT_FOUND;
        return result;
    }
    if (!DirectoryExists(result.source) || IsReparsePoint(result.source))
    {
        result.win32Error = ERROR_DIRECTORY;
        return result;
    }
    if (PathExists(result.destination))
    {
        result.win32Error = ERROR_ALREADY_EXISTS;
        return result;
    }
    if (!MoveFileExW(result.source.c_str(), result.destination.c_str(), MOVEFILE_COPY_ALLOWED))
    {
        result.win32Error = GetLastError();
        return result;
    }
    result.moved = true;
    std::wstring command = L"cmd.exe /c mklink /J \"" + result.source + L"\" \"" + result.destination + L"\"";
    STARTUPINFOW startInfo{};
    startInfo.cb = sizeof(startInfo);
    PROCESS_INFORMATION processInfo{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startInfo, &processInfo))
    {
        result.win32Error = GetLastError();
    }
    else
    {
        WaitForSingleObject(processInfo.hProcess, 20000);
        DWORD exitCode = 1;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        if (exitCode == 0)
            result.junctioned = true;
        else
            result.win32Error = exitCode;
    }
    if (!result.junctioned)
    {
        // A failed junction must not leave the folder detached.
        if (MoveFileExW(result.destination.c_str(), result.source.c_str(), MOVEFILE_COPY_ALLOWED))
            result.rolledBack = true;
    }
    return result;
}

std::wstring FormatMigrate(std::wstring const &name, MigrateResult const &r)
{
    wchar_t buffer[1024]{};
    swprintf_s(buffer, L"{\"ok\":%s,\"folder\":\"%s\",\"moved\":%s,\"junctioned\":%s,"
                       L"\"rolledBack\":%s,\"win32Error\":%lu,\"source\":\"%s\",\"destination\":\"%s\"}",
               (r.junctioned ? L"true" : L"false"), name.c_str(),
               (r.moved ? L"true" : L"false"), (r.junctioned ? L"true" : L"false"),
               (r.rolledBack ? L"true" : L"false"), r.win32Error,
               r.source.c_str(), r.destination.c_str());
    return buffer;
}

bool ServeOne(HANDLE pipe)
{
    if (!SameSession(pipe))
    {
        const char denied[] = "{\"ok\":false,\"error\":\"session-rejected\"}";
        DWORD written = 0;
        WriteFile(pipe, denied, DWORD(sizeof(denied) - 1), &written, nullptr);
        return false;
    }
    char request[MaxRequest]{};
    DWORD read = 0;
    if (!ReadFile(pipe, request, MaxRequest - 1, &read, nullptr) || read == 0)
        return false;
    std::string command(request, read);
    std::string response;
    if (command.find("clean") != std::string::npos)
    {
        std::wstring text = FormatReceipt(Clean());
        response.assign(text.begin(), text.end());
    }
    else if (command.find("migrate-folder:") == 0)
    {
        // NUL-delimited payload; paths may contain '|' or spaces.
        std::string payload(command.begin() + 15, command.end());
        while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
            payload.pop_back();
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= payload.size())
        {
            size_t zero = payload.find('\0', start);
            if (zero == std::string::npos)
            {
                parts.push_back(payload.substr(start));
                break;
            }
            parts.push_back(payload.substr(start, zero - start));
            start = zero + 1;
        }
        while (parts.size() < 3)
            parts.push_back(std::string());
        auto toWide = [](std::string const &text) {
            int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
            std::wstring value(size_t(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), value.data(), size);
            return value;
        };
        std::wstring name = toWide(parts[0]);
        if (!IsMigratable(name))
        {
            response = "{\"ok\":false,\"error\":\"folder-not-allowed\"}";
        }
        else
        {
            std::wstring text = FormatMigrate(name, MigrateFolder(name, toWide(parts[1]), toWide(parts[2])));
            response.assign(text.begin(), text.end());
        }
    }
    else if (command.find("stop-service:") == 0)
    {
        std::wstring name(command.begin() + 13, command.end());
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        if (!IsStoppable(name))
        {
            response = "{\"ok\":false,\"error\":\"service-not-allowed\"}";
        }
        else
        {
            std::wstring text = FormatStop(name, StopHostService(name));
            response.assign(text.begin(), text.end());
        }
    }
    else if (command.find("health") != std::string::npos)
    {
        response = "{\"ok\":true,\"service\":\"MagicCommunityMemoryService\",\"version\":1}";
    }
    else
    {
        response = "{\"ok\":false,\"error\":\"unknown-command\"}";
    }
    DWORD written = 0;
    if (!WriteFile(pipe, response.data(), DWORD(response.size()), &written, nullptr))
        return false;
    return true;
}

DWORD WINAPI PipeLoop(void *)
{
    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
    bool securityReady = CreatePipeSecurity(&attributes, &descriptor);
    wchar_t logPath[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"MAGIC_MEMORY_SERVICE_LOG", logPath, MAX_PATH) == 0)
    {
        GetModuleFileNameW(nullptr, logPath, MAX_PATH);
        wchar_t *slash = wcsrchr(logPath, L'\\');
        if (slash)
            *(slash + 1) = 0;
        wcscat_s(logPath, L"memory-service.log");
    }
    auto trace = [&](wchar_t const *text) {
        FILE *file = nullptr;
        if (_wfopen_s(&file, logPath, L"a") == 0 && file)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            fwprintf(file, L"%04d-%02d-%02d %02d:%02d:%02d %s\n",
                     now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, text);
            fclose(file);
        }
    };
    if (!securityReady)
    {
        wchar_t detail[96]{};
        swprintf_s(detail, L"security-failed err=%lu", GetLastError());
        trace(detail);
        if (descriptor)
            LocalFree(descriptor);
        descriptor = nullptr;
    }
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0)
    {
        HANDLE pipe = CreateNamedPipeW(PipeNameValue().c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, MaxRequest, MaxRequest, 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            wchar_t detail[64]{};
            swprintf_s(detail, L"create-failed err=%lu", GetLastError());
            trace(detail);
            Sleep(500);
            continue;
        }
        trace(L"listen");
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
            connected = TRUE;
        if (!connected)
        {
            trace(L"connect-failed");
            CloseHandle(pipe);
            Sleep(200);
            continue;
        }
        trace(L"connected");
        while (ServeOne(pipe))
        {
        }
        trace(L"served");
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (descriptor)
        LocalFree(descriptor);
    return 0;
}

void SetState(DWORD state, DWORD exitCode = NO_ERROR)
{
    static DWORD checkpoint = 1;
    g_state.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_state.dwCurrentState = state;
    g_state.dwWin32ExitCode = exitCode;
    g_state.dwWaitHint = state == SERVICE_START_PENDING ? 5000 : 0;
    g_state.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_state.dwCheckPoint = state == SERVICE_RUNNING ? 0 : checkpoint++;
    if (g_status)
        SetServiceStatus(g_status, &g_state);
}

DWORD WINAPI Handler(DWORD control, DWORD, void *, void *)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        SetState(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
    }
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, wchar_t **)
{
    g_status = RegisterServiceCtrlHandlerExW(ServiceName, Handler, nullptr);
    if (!g_status)
        return;
    SetState(SERVICE_START_PENDING);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_cleanNow = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop)
    {
        SetState(SERVICE_STOPPED, GetLastError());
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, PipeLoop, nullptr, 0, nullptr);
    if (!thread)
    {
        SetState(SERVICE_STOPPED, GetLastError());
        return;
    }
    SetState(SERVICE_RUNNING);
    WaitForSingleObject(g_stop, INFINITE);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    SetState(SERVICE_STOPPED);
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc > 1 && _wcsicmp(argv[1], L"--console") == 0)
    {
        // Console mode exists for local verification only; it does not
        // register with the SCM and still requires the caller's own privileges.
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler([](DWORD) -> BOOL { SetEvent(g_stop); return TRUE; }, TRUE);
        wprintf(L"listening\n");
        PipeLoop(nullptr);
        return 0;
    }
    SERVICE_TABLE_ENTRYW table[]{const_cast<wchar_t *>(ServiceName), ServiceMain, nullptr, nullptr};
    if (!StartServiceCtrlDispatcherW(table))
        return GetLastError();
    return 0;
}
