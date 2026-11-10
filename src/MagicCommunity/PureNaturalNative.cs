using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace MagicCommunity;

internal readonly record struct PrivilegeState(string Name, bool Held, int Error);
internal readonly record struct MemorySample(long TotalBytes, long AvailableBytes);

internal readonly record struct CleanResult(bool Executed, MemorySample Before, MemorySample After,
    IReadOnlyList<PrivilegeState> Privileges, int PurgeStatus, int EmptyWorkingSetsStatus)
{
    internal long AvailableDeltaBytes => After.AvailableBytes - Before.AvailableBytes;
    internal bool PrivilegeNotHeld => PurgeStatus == PureNaturalNative.StatusPrivilegeNotHeld ||
                                      EmptyWorkingSetsStatus == PureNaturalNative.StatusPrivilegeNotHeld;
    internal bool Effective => PurgeStatus == 0 || EmptyWorkingSetsStatus == 0;
}

// Deep module: the only place that owns the two memory-list privileges, the
// native system-information call and process suspend primitives. Callers pass
// no handles and receive value receipts, so scheduling and diagnostics share
// one implementation instead of duplicating privilege handling.
internal static class PureNaturalNative
{
    internal const int SystemMemoryListInformation = 80;
    internal const int MemoryPurgeStandbyList = 4;
    internal const int MemoryEmptyWorkingSets = 2;
    internal const int StatusPrivilegeNotHeld = unchecked((int)0xC0000061);
    private const uint TokenQuery = 0x0008, TokenAdjustPrivileges = 0x0020, SePrivilegeEnabled = 0x00000002;
    private const uint ProcessSuspendResume = 0x0800;

    internal static MemorySample ReadMemory()
    {
        var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        if (!GlobalMemoryStatusEx(ref status))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "GlobalMemoryStatusEx failed.");
        return new MemorySample((long)status.TotalPhysical, (long)status.AvailablePhysical);
    }

    internal static IReadOnlyList<PrivilegeState> EnablePrivileges()
    {
        var states = new List<PrivilegeState>(2);
        using var process = Process.GetCurrentProcess();
        if (!OpenProcessToken(process.Handle, TokenQuery | TokenAdjustPrivileges, out IntPtr token))
        {
            int error = Marshal.GetLastWin32Error();
            states.Add(new PrivilegeState("SeIncreaseQuotaPrivilege", false, error));
            states.Add(new PrivilegeState("SeProfileSingleProcessPrivilege", false, error));
            return states;
        }
        try
        {
            foreach (string name in new[] { "SeIncreaseQuotaPrivilege", "SeProfileSingleProcessPrivilege" })
            {
                if (!LookupPrivilegeValueW(null, name, out Luid luid))
                {
                    states.Add(new PrivilegeState(name, false, Marshal.GetLastWin32Error()));
                    continue;
                }
                var request = new TokenPrivileges { Count = 1, Luid = luid, Attributes = SePrivilegeEnabled };
                bool adjusted = AdjustTokenPrivileges(token, false, ref request,
                    (uint)Marshal.SizeOf<TokenPrivileges>(), IntPtr.Zero, out _);
                int error = Marshal.GetLastWin32Error();
                states.Add(new PrivilegeState(name, adjusted && error == 0, adjusted && error == 0 ? 0 : error));
            }
        }
        finally { CloseHandle(token); }
        return states;
    }

    // One confirmed transaction: privileges, both list commands, before/after
    // memory. STATUS_PRIVILEGE_NOT_HELD stays in the receipt instead of throwing.
    internal static CleanResult Clean()
    {
        var privileges = EnablePrivileges();
        var before = ReadMemory();
        int purge = MemoryPurgeStandbyList;
        int purgeStatus = NtSetSystemInformation(SystemMemoryListInformation, ref purge, sizeof(int));
        int empty = MemoryEmptyWorkingSets;
        int emptyStatus = NtSetSystemInformation(SystemMemoryListInformation, ref empty, sizeof(int));
        Thread.Sleep(500);
        var after = ReadMemory();
        return new CleanResult(true, before, after, privileges, purgeStatus, emptyStatus);
    }

    internal static bool Suspend(int pid)
    {
        IntPtr handle = OpenProcess(ProcessSuspendResume, false, pid);
        if (handle == IntPtr.Zero) return false;
        try { return NtSuspendProcess(handle) == 0; }
        finally { CloseHandle(handle); }
    }

    internal static bool Resume(int pid)
    {
        IntPtr handle = OpenProcess(ProcessSuspendResume, false, pid);
        if (handle == IntPtr.Zero) return false;
        try { return NtResumeProcess(handle) == 0; }
        finally { CloseHandle(handle); }
    }

    // Direct children of this process only. Scheduling never receives a name or
    // an arbitrary PID, so it cannot target user applications.
    internal static IReadOnlyList<int> Children(int parentPid)
    {
        var result = new List<int>();
        IntPtr snapshot = CreateToolhelp32Snapshot(Th32csSnapProcess, 0);
        if (snapshot == IntPtr.Zero || snapshot == new IntPtr(-1)) return result;
        try
        {
            var entry = new ProcessEntry32 { Size = (uint)Marshal.SizeOf<ProcessEntry32>() };
            if (!Process32FirstW(snapshot, ref entry)) return result;
            do
            {
                if (entry.ParentProcessId == parentPid && entry.ProcessId != parentPid && result.Count < 32)
                    result.Add((int)entry.ProcessId);
            } while (Process32NextW(snapshot, ref entry));
        }
        finally { CloseHandle(snapshot); }
        return result;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        public uint Length, MemoryLoad;
        public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile, TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Luid { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)] private struct TokenPrivileges { public uint Count; public Luid Luid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ProcessEntry32
    {
        public uint Size, Usage, ProcessId, DefaultHeapId;
        public nuint ModuleId;
        public uint Threads, ParentProcessId;
        public int PriorityClass, Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string ExeFile;
    }

    private const uint Th32csSnapProcess = 0x00000002;

    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx value);
    [DllImport("ntdll.dll")] private static extern int NtSetSystemInformation(int informationClass, ref int information, int length);
    [DllImport("ntdll.dll")] private static extern int NtSuspendProcess(IntPtr process);
    [DllImport("ntdll.dll")] private static extern int NtResumeProcess(IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool LookupPrivilegeValueW(string? system, string name, out Luid luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state,
        uint length, IntPtr previous, out uint returned);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32FirstW(IntPtr snapshot, ref ProcessEntry32 entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32NextW(IntPtr snapshot, ref ProcessEntry32 entry);
}
