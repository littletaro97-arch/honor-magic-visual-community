using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MagicCommunity;

internal readonly record struct GuardState(uint BlacklistCount, uint LastBlockedPid);

// Client for \\.\MagicProcessGuard. The driver is optional: every caller gets
// null when the device is absent, so the desktop keeps working on machines
// without the signed driver installed.
internal static class ProcessGuardClient
{
    internal const string DevicePath = @"\\.\MagicProcessGuard";
    private const uint DeviceType = 0x8330;
    private const uint IoctlSetWorkingSet = (DeviceType << 16) | (0x800 << 2) | 0;
    private const uint IoctlAddBlacklist = (DeviceType << 16) | (0x801 << 2) | 0;
    private const uint IoctlRemoveBlacklist = (DeviceType << 16) | (0x802 << 2) | 0;
    private const uint IoctlQueryState = (DeviceType << 16) | (0x803 << 2) | 2;
    private const int MaxImageName = 260;

    internal static bool Available()
    {
        try
        {
            using SafeFileHandle handle = CreateFileW(DevicePath, 0, FileShare.ReadWrite, IntPtr.Zero,
                FileMode.Open, 0, IntPtr.Zero);
            return !handle.IsInvalid;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    // Work-set bound for one PID. Returns false when the driver is absent or the
    // request was rejected; the caller decides whether that is fatal.
    internal static bool SetWorkingSet(int pid, ulong minimumBytes, ulong maximumBytes, out int win32Error)
    {
        win32Error = 0;
        if (pid <= 0 || minimumBytes == 0 || maximumBytes < minimumBytes) return false;
        using SafeFileHandle handle = CreateFileW(DevicePath, 0x40000000 /* GENERIC_WRITE */,
            FileShare.ReadWrite, IntPtr.Zero, FileMode.Open, 0, IntPtr.Zero);
        if (handle.IsInvalid) { win32Error = Marshal.GetLastWin32Error(); return false; }
        var request = new WorkingSetRequest { ProcessId = (uint)pid, MinimumBytes = minimumBytes, MaximumBytes = maximumBytes };
        bool ok = DeviceIoControl(handle, IoctlSetWorkingSet, ref request,
            (uint)Marshal.SizeOf<WorkingSetRequest>(), IntPtr.Zero, 0, out _, IntPtr.Zero);
        if (!ok) win32Error = Marshal.GetLastWin32Error();
        return ok;
    }

    internal static bool AddBlacklist(string imageName, out int win32Error)
    {
        win32Error = 0;
        if (string.IsNullOrWhiteSpace(imageName) || imageName.Length >= MaxImageName) return false;
        using SafeFileHandle handle = CreateFileW(DevicePath, 0x40000000, FileShare.ReadWrite,
            IntPtr.Zero, FileMode.Open, 0, IntPtr.Zero);
        if (handle.IsInvalid) { win32Error = Marshal.GetLastWin32Error(); return false; }
        var request = new BlacklistRequest();
        request.ImageName = imageName;
        bool ok = DeviceIoControl(handle, IoctlAddBlacklist, ref request,
            (uint)Marshal.SizeOf<BlacklistRequest>(), IntPtr.Zero, 0, out _, IntPtr.Zero);
        if (!ok) win32Error = Marshal.GetLastWin32Error();
        return ok;
    }

    internal static bool RemoveBlacklist(string imageName, out int win32Error)
    {
        win32Error = 0;
        if (string.IsNullOrWhiteSpace(imageName) || imageName.Length >= MaxImageName) return false;
        using SafeFileHandle handle = CreateFileW(DevicePath, 0x40000000, FileShare.ReadWrite,
            IntPtr.Zero, FileMode.Open, 0, IntPtr.Zero);
        if (handle.IsInvalid) { win32Error = Marshal.GetLastWin32Error(); return false; }
        var request = new BlacklistRequest();
        request.ImageName = imageName;
        bool ok = DeviceIoControl(handle, IoctlRemoveBlacklist, ref request,
            (uint)Marshal.SizeOf<BlacklistRequest>(), IntPtr.Zero, 0, out _, IntPtr.Zero);
        if (!ok) win32Error = Marshal.GetLastWin32Error();
        return ok;
    }

    internal static GuardState? QueryState()
    {
        using SafeFileHandle handle = CreateFileW(DevicePath, 0x80000000 /* GENERIC_READ */,
            FileShare.ReadWrite, IntPtr.Zero, FileMode.Open, 0, IntPtr.Zero);
        if (handle.IsInvalid) return null;
        var state = default(GuardStateRaw);
        bool ok = DeviceIoControl(handle, IoctlQueryState, IntPtr.Zero, 0,
            ref state, (uint)Marshal.SizeOf<GuardStateRaw>(), out uint returned, IntPtr.Zero);
        if (!ok || returned < Marshal.SizeOf<GuardStateRaw>()) return null;
        return new GuardState(state.BlacklistCount, state.LastBlockedPid);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WorkingSetRequest
    {
        public uint ProcessId;
        public uint Reserved;
        public ulong MinimumBytes;
        public ulong MaximumBytes;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct BlacklistRequest
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = MaxImageName)]
        public string ImageName;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct GuardStateRaw
    {
        public uint BlacklistCount;
        public uint LastBlockedPid;
        public uint Reserved;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, FileShare share,
        IntPtr security, FileMode mode, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code, IntPtr input,
        uint inputSize, ref GuardStateRaw output, uint outputSize, out uint returned, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code,
        ref WorkingSetRequest input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code,
        ref BlacklistRequest input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
}
