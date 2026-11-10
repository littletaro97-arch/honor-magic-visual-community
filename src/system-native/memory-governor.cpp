#include "memory-governor.h"
#include <winternl.h>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace magic::system
{
namespace
{
using NtSetSystemInformationFn = NTSTATUS(NTAPI *)(int, PVOID, ULONG);
using NtQuerySystemInformationFn = NTSTATUS(NTAPI *)(int, PVOID, ULONG, PULONG);
using NtSuspendProcessFn = NTSTATUS(NTAPI *)(HANDLE);
using NtResumeProcessFn = NTSTATUS(NTAPI *)(HANDLE);
using NtCreateProcessStateChangeFn = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG);
using NtChangeProcessStateFn = NTSTATUS(NTAPI *)(HANDLE, HANDLE, ULONG, PVOID, SIZE_T, ULONG);
using NtCloseFn = NTSTATUS(NTAPI *)(HANDLE);

// Access rights are native object rights; no phnt header is part of this build.
constexpr ACCESS_MASK ProcessStateAllAccess = 0x001F0000 | STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE;
constexpr ULONG ProcessStateChangeSuspend = 0;
constexpr ULONG ProcessStateChangeResume = 1;
constexpr LONG StatusPrivilegeNotHeld = 0xC0000061L;
constexpr int SystemMemoryListInformation = 80;
constexpr ULONG MemoryEmptyWorkingSets = 2;
constexpr ULONG MemoryPurgeStandbyList = 4;
constexpr DWORD MaxFrozenProcesses = 256;
constexpr size_t MaxImageLength = 32768;
constexpr size_t MaxCreatedLength = 20;
constexpr size_t MaxTriggerLength = 32;

// Native entry points are optional by OS version; their absence is a structured result.
struct NativeApi
{
    // ntdll remains loaded for the governor lifetime; dynamically resolved pointers are process-wide.
    HMODULE ntdll = nullptr;
    NtSetSystemInformationFn setSystemInformation = nullptr;
    NtQuerySystemInformationFn querySystemInformation = nullptr;
    NtSuspendProcessFn suspendProcess = nullptr;
    NtResumeProcessFn resumeProcess = nullptr;
    NtCreateProcessStateChangeFn createProcessStateChange = nullptr;
    NtChangeProcessStateFn changeProcessState = nullptr;
    NtCloseFn close = nullptr;

    NativeApi()
    {
        // Resolve every optional entry point once; a missing Win11 symbol selects fallback.
        ntdll = LoadLibraryExW(L"ntdll.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ntdll)
            return;
        setSystemInformation = reinterpret_cast<NtSetSystemInformationFn>(
            GetProcAddress(ntdll, "NtSetSystemInformation"));
        querySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
        suspendProcess = reinterpret_cast<NtSuspendProcessFn>(GetProcAddress(ntdll, "NtSuspendProcess"));
        resumeProcess = reinterpret_cast<NtResumeProcessFn>(GetProcAddress(ntdll, "NtResumeProcess"));
        createProcessStateChange = reinterpret_cast<NtCreateProcessStateChangeFn>(
            GetProcAddress(ntdll, "NtCreateProcessStateChange"));
        changeProcessState = reinterpret_cast<NtChangeProcessStateFn>(
            GetProcAddress(ntdll, "NtChangeProcessState"));
        close = reinterpret_cast<NtCloseFn>(GetProcAddress(ntdll, "NtClose"));
    }
    ~NativeApi()
    {
        if (ntdll)
            FreeLibrary(ntdll);
    }
};

// State-change handles can be kernel objects; CloseHandle handles both object kinds here.
struct OwnedHandle
{
    HANDLE value = nullptr;

    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE h) : value(h) {}
    // RAII applies to both process handles and native state-change handles.
    ~OwnedHandle() { Reset(); }
    OwnedHandle(OwnedHandle const &) = delete;
    OwnedHandle &operator=(OwnedHandle const &) = delete;
    // Move-only RAII permits the frozen-handle table to own entries without copies.
    OwnedHandle(OwnedHandle &&other) noexcept : value(other.Detach()) {}
    OwnedHandle &operator=(OwnedHandle &&other) noexcept
    {
        if (this != &other)
            Reset(other.Detach());
        return *this;
    }
    void Reset(HANDLE replacement = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = replacement;
    }
    // Detach transfers ownership without closing; native close paths use the returned handle.
    HANDLE Detach()
    {
        HANDLE prior = value;
        value = nullptr;
        return prior;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct MemorySample
{
    uint64_t total = 0, available = 0;
};

struct PrivilegeState
{
    bool increaseQuotaPresent = false, profileSingleProcessPresent = false;
    bool increaseQuotaEnabled = false, profileSingleProcessEnabled = false;
    DWORD lookupError = ERROR_SUCCESS, adjustError = ERROR_SUCCESS;
};

struct FrozenProcess
{
    OwnedHandle process;
    OwnedHandle state;
    std::wstring mode;
    uint64_t created = 0;
    // Identity is retained for the structured resume receipt.
    FrozenProcess() = default;
    FrozenProcess(FrozenProcess &&) = default;
    FrozenProcess &operator=(FrozenProcess &&) = default;
};

// JSON doubles are exact here only for bounded integral values or well-defined byte deltas.
DWORD BoundedPid(JsonObject const &arguments, wchar_t const *key)
{
    double value = arguments.GetNamedNumber(key, 0);
    if (!std::isfinite(value) || value < 1 || value > double(std::numeric_limits<DWORD>::max()) ||
        value != std::floor(value))
        return 0;
    return static_cast<DWORD>(value);
}

// The decimal parser rejects overflow rather than relying on JSON Number conversion.
uint64_t ParseCreated(std::wstring const &text)
{
    if (text.empty() || text.size() > MaxCreatedLength ||
        !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        throw std::invalid_argument("Invalid process creation time");
    uint64_t result = 0;
    for (wchar_t c : text)
    {
        uint64_t digit = uint64_t(c - L'0');
        if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            throw std::invalid_argument("Invalid process creation time");
        result = result * 10 + digit;
    }
    return result;
}

// A decimal string carries FILETIME without loss through the JSON number layer.
uint64_t RequiredCreated(JsonObject const &arguments, wchar_t const *key)
{
    return ParseCreated(RequiredText(arguments, key, MaxCreatedLength));
}

std::wstring RequiredImage(JsonObject const &arguments, wchar_t const *key)
{
    auto value = RequiredText(arguments, key, MaxImageLength);
    if (value.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Invalid process image path");
    return value;
}

// All normal protocol outcomes are returned; exceptions remain inside the module seam.
JsonObject Error(wchar_t const *reason, DWORD error = ERROR_SUCCESS)
{
    JsonObject result;
    Flag(result, L"ok", false);
    Text(result, L"reason", reason);
    Number(result, L"win32Error", double(error));
    return result;
}

JsonObject Success(wchar_t const *status = L"ok")
{
    JsonObject result;
    Flag(result, L"ok", true);
    Text(result, L"status", status);
    return result;
}

JsonObject MemoryValues(MemorySample const &sample)
{
    JsonObject result;
    Number(result, L"totalPhys", double(sample.total));
    Number(result, L"availablePhys", double(sample.available));
    return result;
}

// One bounded before/after sample is enough to describe the clean receipt.
MemorySample SampleMemory()
{
    MEMORYSTATUSEX status{sizeof(status)};
    if (!GlobalMemoryStatusEx(&status))
        throw std::invalid_argument("GlobalMemoryStatusEx failed");
    return {status.ullTotalPhys, status.ullAvailPhys};
}

// Privileges are adjusted only in this process token and remain one atomic request.
PrivilegeState EnablePrivileges()
{
    PrivilegeState result;
    OwnedHandle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token.value))
    {
        result.lookupError = result.adjustError = GetLastError();
        return result;
    }
    LUID increase{}, profile{};
    if (!LookupPrivilegeValueW(nullptr, SE_INCREASE_QUOTA_NAME, &increase))
        result.lookupError = GetLastError();
    if (!LookupPrivilegeValueW(nullptr, SE_PROF_SINGLE_PROCESS_NAME, &profile))
        result.lookupError = GetLastError();

    DWORD bytes = 0;
    if (GetTokenInformation(token.value, TokenPrivileges, nullptr, 0, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        result.lookupError = GetLastError();
        return result;
    }
    std::vector<BYTE> buffer(bytes);
    auto *privileges = reinterpret_cast<TOKEN_PRIVILEGES *>(buffer.data());
    if (!GetTokenInformation(token.value, TokenPrivileges, buffer.data(), bytes, &bytes))
    {
        result.lookupError = GetLastError();
        return result;
    }
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i)
    {
        auto const &entry = privileges->Privileges[i];
        if (entry.Luid.LowPart == increase.LowPart && entry.Luid.HighPart == increase.HighPart)
        {
            result.increaseQuotaPresent = true;
            result.increaseQuotaEnabled = (entry.Attributes & SE_PRIVILEGE_ENABLED) != 0;
        }
        if (entry.Luid.LowPart == profile.LowPart && entry.Luid.HighPart == profile.HighPart)
        {
            result.profileSingleProcessPresent = true;
            result.profileSingleProcessEnabled = (entry.Attributes & SE_PRIVILEGE_ENABLED) != 0;
        }
    }

    // TOKEN_PRIVILEGES has ANYSIZE_ARRAY; allocate storage for both requested privileges.
    std::vector<BYTE> desiredBytes(sizeof(TOKEN_PRIVILEGES) + sizeof(LUID_AND_ATTRIBUTES));
    auto *desired = reinterpret_cast<TOKEN_PRIVILEGES *>(desiredBytes.data());
    desired->PrivilegeCount = 2;
    desired->Privileges[0] = {increase, SE_PRIVILEGE_ENABLED};
    desired->Privileges[1] = {profile, SE_PRIVILEGE_ENABLED};
    SetLastError(ERROR_SUCCESS);
    BOOL adjusted = AdjustTokenPrivileges(token.value, FALSE, desired, DWORD(desiredBytes.size()), nullptr, nullptr);
    result.adjustError = adjusted ? GetLastError() : (GetLastError() ? GetLastError() : ERROR_NOT_ALL_ASSIGNED);
    if (adjusted && result.adjustError == ERROR_SUCCESS)
    {
        result.increaseQuotaEnabled = result.increaseQuotaPresent;
        result.profileSingleProcessEnabled = result.profileSingleProcessPresent;
    }
    else
    {
        result.increaseQuotaEnabled = false;
        result.profileSingleProcessEnabled = false;
    }
    return result;
}

// Presence, enabled state and Win32 lookup/adjust errors stay separate evidence.
void InsertPrivileges(JsonObject const &result, PrivilegeState const &state)
{
    JsonObject privileges;
    Flag(privileges, L"increaseQuotaPresent", state.increaseQuotaPresent);
    Flag(privileges, L"increaseQuotaEnabled", state.increaseQuotaEnabled);
    Flag(privileges, L"profileSingleProcessPresent", state.profileSingleProcessPresent);
    Flag(privileges, L"profileSingleProcessEnabled", state.profileSingleProcessEnabled);
    Number(privileges, L"lookupError", double(state.lookupError));
    Number(privileges, L"adjustError", double(state.adjustError));
    Flag(privileges, L"ready", state.increaseQuotaEnabled && state.profileSingleProcessEnabled);
    result.Insert(L"privileges", privileges);
}

// One user-mode command has a four-byte SYSTEM_MEMORY_LIST_COMMAND payload.
LONG CallMemoryCommand(NativeApi const &api, ULONG command)
{
    if (!api.setSystemInformation)
        return static_cast<LONG>(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    return api.setSystemInformation(SystemMemoryListInformation, &command, sizeof(command));
}

// The optional query is a bounded probe; its status is reported without changing the request.
// The query balances the user-mode dynamic-resolution mechanism without changing the result.
LONG QueryMemoryList(NativeApi const &api)
{
    if (!api.querySystemInformation)
        return static_cast<LONG>(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    BYTE buffer[256]{};
    ULONG returned = 0;
    return api.querySystemInformation(SystemMemoryListInformation, buffer, sizeof(buffer), &returned);
}

// NTSTATUS is an unsigned 32-bit decimal Number at the protocol seam.
void InsertStatus(JsonObject const &result, wchar_t const *key, LONG status)
{
    Number(result, key, double(static_cast<uint32_t>(status)));
}

// The parent handle proves identity; the snapshot proves direct-child lineage.
bool IsDirectChild(DWORD pid, DWORD parentPid, HANDLE parent)
{
    if (!parent || pid == parentPid || pid == GetCurrentProcessId())
        return false;
    OwnedHandle parentProbe{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid)};
    if (!parentProbe)
        return false;
    FILETIME parentTime{}, probeTime{}, unused{};
    std::wstring parentImage(MaxImageLength, L'\0'), probeImage(MaxImageLength, L'\0');
    DWORD parentLength = DWORD(parentImage.size()), probeLength = DWORD(probeImage.size());
    if (!GetProcessTimes(parent, &parentTime, &unused, &unused, &unused) ||
        !GetProcessTimes(parentProbe.value, &probeTime, &unused, &unused, &unused) ||
        Ticks(parentTime) != Ticks(probeTime) ||
        !QueryFullProcessImageNameW(parent, 0, parentImage.data(), &parentLength) ||
        !QueryFullProcessImageNameW(parentProbe.value, 0, probeImage.data(), &probeLength) ||
        parentLength >= MaxImageLength || probeLength >= MaxImageLength)
        return false;
    parentImage.resize(parentLength);
    probeImage.resize(probeLength);
    if (parentImage != probeImage)
        return false;
    DWORD parentSession = 0, childSession = 0;
    if (!ProcessIdToSessionId(parentPid, &parentSession) ||
        !ProcessIdToSessionId(pid, &childSession) || parentSession != childSession)
        return false;
    OwnedHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snapshot)
        return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.value, &entry))
        return false;
    do
    {
        if (entry.th32ProcessID == pid)
            return entry.th32ParentProcessID == parentPid;
    } while (Process32NextW(snapshot.value, &entry));
    return false;
}

// Exact image and creation-time comparison rejects stale handles after PID reuse.
bool MatchesIdentity(HANDLE process, std::wstring const &image, uint64_t created)
{
    FILETIME c{}, e{}, k{}, u{};
    std::wstring actual(MaxImageLength, L'\0');
    DWORD length = DWORD(actual.size());
    if (!GetProcessTimes(process, &c, &e, &k, &u) ||
        !QueryFullProcessImageNameW(process, 0, actual.data(), &length) || length >= MaxImageLength)
        return false;
    actual.resize(length);
    return actual == image && Ticks(c) == created;
}

void InsertIdentity(JsonObject const &result, DWORD pid, uint64_t created)
{
    Number(result, L"pid", double(pid));
    Text(result, L"created", std::to_wstring(created));
}

// Extended information and reserved slots are currently unused by this Win11 interface.
LONG CallProcessStateChange(NativeApi const &api, HANDLE state, HANDLE process, ULONG type)
{
    return api.changeProcessState(state, process, type, nullptr, 0, 0);
}
} // namespace

struct MemoryGovernor::Impl
{
    NativeApi api;
    std::mutex gate;
    std::unordered_map<DWORD, FrozenProcess> frozen;
    uint64_t cleanCount = 0;
};

MemoryGovernor::MemoryGovernor() : impl(std::make_unique<Impl>()) {}
MemoryGovernor::~MemoryGovernor() { ResumeAll(); }

// The interface exposes only capability and ownership state, never raw handles.
JsonObject MemoryGovernor::Health() const
{
    std::scoped_lock lock(impl->gate);
    auto result = Success(L"available");
    Flag(result, L"setSystemInformation", impl->api.setSystemInformation != nullptr);
    Flag(result, L"querySystemInformation", impl->api.querySystemInformation != nullptr);
    Flag(result, L"processStateChange", impl->api.createProcessStateChange != nullptr && impl->api.changeProcessState != nullptr);
    Flag(result, L"suspendFallback", impl->api.suspendProcess != nullptr && impl->api.resumeProcess != nullptr);
    Number(result, L"frozen", double(impl->frozen.size()));
    Number(result, L"cleanCount", double(impl->cleanCount));
    Text(result, L"scope", L"direct-child-processes");
    return result;
}

// Clean is one confirmed transaction: sample, privileges, two commands, sample, receipt.
JsonObject MemoryGovernor::Clean(JsonObject const &arguments)
{
    if (!arguments.GetNamedBoolean(L"confirmed", false))
        return Error(L"confirmation-required");
    auto result = Success(L"completed-with-status");
    auto trigger = RequiredText(arguments, L"trigger", MaxTriggerLength);
    auto before = SampleMemory();
    result.Insert(L"before", MemoryValues(before));
    auto privileges = EnablePrivileges();
    InsertPrivileges(result, privileges);

    // NTSTATUS is data at the protocol seam; even privilege failures keep the full receipt.
    try
    {
        LONG query = QueryMemoryList(impl->api);
        LONG purge = CallMemoryCommand(impl->api, MemoryPurgeStandbyList);
        LONG empty = CallMemoryCommand(impl->api, MemoryEmptyWorkingSets);
        InsertStatus(result, L"queryStatus", query);
        InsertStatus(result, L"purgeStandbyListStatus", purge);
        InsertStatus(result, L"emptyWorkingSetsStatus", empty);
        bool privilegeNotHeld = purge == StatusPrivilegeNotHeld || empty == StatusPrivilegeNotHeld;
        Flag(result, L"privilegeNotHeld", privilegeNotHeld);
        JsonObject diagnostics;
        Flag(diagnostics, L"privilege-not-held", privilegeNotHeld);
        result.Insert(L"diagnostics", diagnostics);
        if (purge == 0 && empty == 0)
            Text(result, L"status", L"ok");
    }
    catch (winrt::hresult_error const &error)
    {
        Flag(result, L"ok", false);
        Text(result, L"reason", L"clean-native-call-failed");
        Text(result, L"error", std::wstring(error.message()).substr(0, 256));
        Number(result, L"win32Error", double(error.code().value));
    }
    auto after = SampleMemory();

    {
        std::scoped_lock lock(impl->gate);
        ++impl->cleanCount;
    }
    Text(result, L"trigger", trigger);
    Text(result, L"scope", L"system-memory-lists");
    result.Insert(L"after", MemoryValues(after));
    Number(result, L"availableDeltaBytes",
           double(after.available >= before.available ? after.available - before.available
                                                      : -double(before.available - after.available)));
    Number(result, L"commandLength", sizeof(ULONG));
    Number(result, L"informationClass", SystemMemoryListInformation);
    Number(result, L"purgeStandbyListCommand", MemoryPurgeStandbyList);
    Number(result, L"emptyWorkingSetsCommand", MemoryEmptyWorkingSets);
    return result;
}

// Direct-child lineage plus exact image/creation identity is required before freezing.
JsonObject MemoryGovernor::Freeze(JsonObject const &arguments, DWORD parentPid, HANDLE parent)
{
    DWORD pid = BoundedPid(arguments, L"pid");
    if (!pid)
        return Error(L"invalid-process-id");
    auto image = RequiredImage(arguments, L"image");
    uint64_t created = RequiredCreated(arguments, L"created");
    {
        std::scoped_lock lock(impl->gate);
        if (impl->frozen.contains(pid))
        {
            auto result = Success(L"already-frozen");
            InsertIdentity(result, pid, created);
            Text(result, L"mode", impl->frozen.at(pid).mode);
            return result;
        }
        if (impl->frozen.size() >= MaxFrozenProcesses)
            return Error(L"freeze-table-full");
    }
    if (!IsDirectChild(pid, parentPid, parent))
        return Error(L"not-a-direct-child");

    OwnedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION |
                                        PROCESS_SUSPEND_RESUME | SYNCHRONIZE,
                                    FALSE, pid)};
    if (!process)
        return Error(L"open-process-failed", GetLastError());
    if (!MatchesIdentity(process.value, image, created))
        return Error(L"identity-mismatch");
    if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
        return Error(L"process-exited");

    std::wstring mode;
    OwnedHandle state;
    LONG status = -1;
    if (impl->api.createProcessStateChange && impl->api.changeProcessState && impl->api.close)
    {
        HANDLE candidate = nullptr;
        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(&attributes, nullptr, 0, nullptr, nullptr);
        status = impl->api.createProcessStateChange(&candidate, ProcessStateAllAccess, &attributes,
                                                    process.value, 0);
        if (status == 0)
        {
            state.Reset(candidate);
            status = CallProcessStateChange(impl->api, state.value, process.value, ProcessStateChangeSuspend);
            if (status == 0)
                mode = L"state-change";
        }
    }
    // Closing a created state object releases its kernel-owned freeze before fallback.
    if (mode.empty() && state && impl->api.close)
        (void)impl->api.close(state.Detach());
    else if (mode.empty())
        state.Reset();
    if (mode.empty() && impl->api.suspendProcess)
    {
        LONG fallback = impl->api.suspendProcess(process.value);
        if (fallback == 0)
        {
            mode = L"suspend-process";
            status = fallback;
        }
        else
            status = fallback;
    }
    if (mode.empty())
    {
        auto result = Error(L"freeze-interface-unavailable");
        InsertIdentity(result, pid, created);
        InsertStatus(result, L"status", status);
        return result;
    }

    {
        std::scoped_lock lock(impl->gate);
        if (impl->frozen.contains(pid))
        {
            // A concurrent winner must not leave this second freeze unowned; undo it here.
            if (mode == L"state-change" && state && impl->api.close)
                (void)impl->api.close(state.Detach());
            else if (mode == L"suspend-process" && impl->api.resumeProcess)
                (void)impl->api.resumeProcess(process.value);
            else
                state.Reset();
            process.Reset();
            auto result = Success(L"already-frozen");
            InsertIdentity(result, pid, created);
            Text(result, L"mode", impl->frozen.at(pid).mode);
            return result;
        }
        FrozenProcess entry;
        entry.process.Reset(process.Detach());
        entry.state.Reset(state.Detach());
        entry.mode = mode;
        entry.created = created;
        impl->frozen.emplace(pid, std::move(entry));
    }
    auto result = Success(L"frozen");
    InsertIdentity(result, pid, created);
    Text(result, L"mode", mode);
    InsertStatus(result, L"status", status);
    return result;
}

// Resume removes table ownership only after a resume is attempted or close is guaranteed.
JsonObject MemoryGovernor::Resume(JsonObject const &arguments)
{
    DWORD pid = BoundedPid(arguments, L"pid");
    if (!pid)
        return Error(L"invalid-process-id");
    FrozenProcess entry;
    {
        std::scoped_lock lock(impl->gate);
        auto it = impl->frozen.find(pid);
        if (it == impl->frozen.end())
        {
            auto result = Error(L"not-frozen");
            InsertIdentity(result, pid, 0);
            return result;
        }
        entry = std::move(it->second);
        impl->frozen.erase(it);
    }
    LONG status = 0;
    if (entry.mode == L"state-change")
    {
        // Explicit resume is requested; closing the state object also reverses outstanding state.
        if (impl->api.changeProcessState)
            status = CallProcessStateChange(impl->api, entry.state.value, entry.process.value,
                                            ProcessStateChangeResume);
        if (impl->api.close)
            (void)impl->api.close(entry.state.Detach());
        else
            entry.state.Reset();
    }
    else if (impl->api.resumeProcess)
        status = impl->api.resumeProcess(entry.process.value);
    else
        status = static_cast<LONG>(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));

    auto result = status == 0 ? Success(L"resumed") : Error(L"resume-status", ERROR_SUCCESS);
    InsertIdentity(result, pid, entry.created);
    Text(result, L"mode", entry.mode);
    InsertStatus(result, L"status", status);
    return result;
}

// Endpoint shutdown and destruction both reverse every owned freeze.
void MemoryGovernor::ResumeAll()
{
    std::unordered_map<DWORD, FrozenProcess> pending;
    {
        std::scoped_lock lock(impl->gate);
        pending.swap(impl->frozen);
    }
    for (auto &[pid, entry] : pending)
    {
        if (entry.mode == L"state-change")
        {
            // Closing the state object is the kernel-owned automatic undo path.
            if (entry.state.value && impl->api.close)
                (void)impl->api.close(entry.state.Detach());
            else
                entry.state.Reset();
        }
        else if (entry.process.value && impl->api.resumeProcess)
            (void)impl->api.resumeProcess(entry.process.value);
        entry.process.Reset();
    }
}
} // namespace magic::system
