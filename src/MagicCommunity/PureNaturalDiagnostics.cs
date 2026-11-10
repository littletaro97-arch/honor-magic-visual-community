using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
namespace MagicCommunity;

// Runtime evidence module. It executes only bounded, locally owned actions and
// returns structured before/after observations for the pure natural engine.
internal static class PureNaturalDiagnostics
{
    private const int SystemMemoryListInformation = 80;
    private const int MemoryPurgeStandbyList = 4;
    private const int MemoryEmptyWorkingSets = 2;
    private const uint TokenQuery = 0x0008;
    private const uint TokenAdjustPrivileges = 0x0020;
    private const uint SePrivilegeEnabled = 0x00000002;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint ProcessSetInformation = 0x0200;
    private const uint ProcessSuspendResume = 0x0800;
    private const uint ProcessStateChangeState = 0x0001;

    internal static async Task<object> Run(string root, string? backendPath, CancellationToken cancellation)
    {
        var policy = PureNaturalConfig.Load(root);
        var systemBefore = ReadSystemMemory();
        var workingSet = MeasureWorkingSetTrim();
        var systemClean = CleanSystemMemoryLists();
        var directFreeze = ExerciseOwnedProcessFreeze();
        var backend = string.IsNullOrWhiteSpace(backendPath)
            ? new { ok = true, skipped = true, reason = "No --native-backend supplied; P1/P2 direct Windows API path executed only." }
            : await ExerciseNativeBackend(backendPath!, cancellation);
        var operations = ReadOperationsPlan(root, policy);
        var processGroup = ReadProcessGroup(root);
        var systemAfter = ReadSystemMemory();
        long availableDelta = systemAfter.AvailablePhysicalBytes - systemBefore.AvailablePhysicalBytes;
        bool p0Effective = workingSet.WorkingSetDeltaBytes > 1024 * 1024;
        bool p1Effective = systemClean.PurgeStatus == "0x00000000" || systemClean.EmptyWorkingSetsStatus == "0x00000000";
        bool p2Effective = directFreeze.FreezeSucceeded && directFreeze.ResumeSucceeded && directFreeze.CpuDeltaDuringFreezeMs < 150;
        bool allPass = workingSet.ApiSucceeded && systemClean.Executed && directFreeze.FreezeSucceeded && directFreeze.ResumeSucceeded &&
            (backend is not null && (backend.GetType().GetProperty("ok")?.GetValue(backend) is true));
        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass,
            effective = new
            {
                p0WorkingSet = p0Effective,
                p1SystemMemoryLists = p1Effective,
                p2OwnedProcessFreeze = p2Effective,
                overall = p0Effective || p1Effective || p2Effective
            },
            policy,
            systemBefore,
            systemAfter,
            systemAvailableDeltaBytes = availableDelta,
            p0 = workingSet,
            p1 = systemClean,
            p2 = directFreeze,
            nativeBackend = backend,
            p3p4 = operations,
            processGroup,
            scope = "Local owned fixture. P0/P1/P2 execute bounded memory and child-process actions; P3/P4 are read-only status and dry-run plans. No service, driver, third-party process, user file or network target is changed."
        };
    }

    private static MemorySnapshot ReadSystemMemory()
    {
        var value = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        if (!GlobalMemoryStatusEx(ref value)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "GlobalMemoryStatusEx failed.");
        return new MemorySnapshot(DateTimeOffset.Now, (long)value.TotalPhysical, (long)value.AvailablePhysical,
            (long)value.TotalPageFile, (long)value.AvailablePageFile, (long)value.MemoryLoad);
    }

    private static WorkingSetObservation MeasureWorkingSetTrim()
    {
        const int megabytes = 384;
        byte[][] pressure = new byte[megabytes][];
        for (int i = 0; i < pressure.Length; i++)
        {
            pressure[i] = new byte[1024 * 1024];
            pressure[i][0] = (byte)i;
        }
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        using var process = Process.GetCurrentProcess();
        process.Refresh();
        long beforeWorkingSet = process.WorkingSet64, beforePrivate = process.PrivateMemorySize64;
        var beforeCounters = ReadCounters(process.Handle);
        bool apiSucceeded = PureNaturalEngine.TrimCurrentProcess();
        Thread.Sleep(300);
        process.Refresh();
        var afterCounters = ReadCounters(process.Handle);
        long afterWorkingSet = process.WorkingSet64, afterPrivate = process.PrivateMemorySize64;
        long delta = beforeWorkingSet - afterWorkingSet;
        GC.KeepAlive(pressure);
        Array.Clear(pressure);
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        GC.WaitForPendingFinalizers();
        return new WorkingSetObservation(apiSucceeded, megabytes, beforeWorkingSet, afterWorkingSet, delta,
            beforePrivate, afterPrivate, beforeCounters.PageFaults, afterCounters.PageFaults,
            beforeCounters.PrivateUsage, afterCounters.PrivateUsage);
    }

    private static Counters ReadCounters(IntPtr process)
    {
        var value = new MemoryCounters { Length = (uint)Marshal.SizeOf<MemoryCounters>() };
        if (!K32GetProcessMemoryInfo(process, ref value, value.Length)) return new Counters(false, 0, 0, 0);
        return new Counters(true, value.PageFaultCount, (long)value.PrivateUsage, (long)value.WorkingSetSize);
    }

    // Delegates to the shared native module so scheduling and diagnostics cannot
    // diverge on privileges, command order or NTSTATUS handling.
    private static SystemCleanObservation CleanSystemMemoryLists()
    {
        var clean = PureNaturalNative.Clean();
        var privileges = clean.Privileges.Select(state => (object)new
        {
            name = state.Name,
            enabled = state.Held,
            status = state.Error == 0 ? "ok" : "win32-error"
        }).ToArray();
        return new SystemCleanObservation(clean.Executed, clean.Effective ? "ok" : "completed-with-status",
            privileges,
            new MemorySample(clean.Before.TotalBytes, clean.Before.AvailableBytes),
            new MemorySample(clean.After.TotalBytes, clean.After.AvailableBytes),
            clean.AvailableDeltaBytes, Hex(clean.PurgeStatus), Hex(clean.EmptyWorkingSetsStatus),
            clean.PrivilegeNotHeld, clean.Effective);
    }

    private static DirectFreezeObservation ExerciseOwnedProcessFreeze()
    {
        Process? child = null;
        try
        {
            child = StartOwnedCpuChild();
            Thread.Sleep(500);
            var identity = ReadIdentity(child);
            var freeze = FreezeOwned(child.Id, identity.Image, identity.Created);
            child.Refresh();
            double cpuBefore = child!.TotalProcessorTime.TotalMilliseconds;
            Thread.Sleep(800);
            child!.Refresh();
            double cpuDelta = Math.Max(0, child.TotalProcessorTime.TotalMilliseconds - cpuBefore);
            var resume = ResumeOwned(freeze);
            bool alive = !child.HasExited;
            return new DirectFreezeObservation(freeze.Success, resume.Success, freeze.Mode, freeze.SuspendStatus,
                freeze.StateStatus, resume.ResumeStatus, cpuDelta, alive, freeze.Reason, new { identity.Image, identity.Created, identity.Pid });
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            return new DirectFreezeObservation(false, false, "exception", -1, -1, -1, -1, false,
                error.GetType().Name + ": " + error.Message, null);
        }
        finally
        {
            if (child is not null)
            {
                EmergencyResume(child.Id);
                try { if (!child.HasExited) child.Kill(entireProcessTree: true); } catch (Exception error) when (error is not OutOfMemoryException) { }
                try { child.WaitForExit(2000); } catch (Exception error) when (error is not OutOfMemoryException) { }
                child.Dispose();
            }
        }
    }

    private static async Task<object> ExerciseNativeBackend(string executable, CancellationToken cancellation)
    {
        Process? child = null;
        try
        {
            executable = Path.GetFullPath(executable);
            if (!File.Exists(executable)) return new { ok = false, skipped = false, reason = "native backend executable missing", executable };
            string hash;
            using (var stream = File.OpenRead(executable)) hash = Convert.ToHexString(SHA256.HashData(stream));
            // The native backend validates direct-child lineage, so the fixture process must be
            // launched by this host before the named-pipe backend is started.
            child = StartOwnedCpuChild();
            Thread.Sleep(500);
            await using var client = await NativeSystemClient.Start(executable, hash, cancellation, null);
            async Task<object> Call(string operation, object arguments)
            {
                try
                {
                    var result = await client.Request(operation, arguments, cancellation);
                    bool accepted = result.ValueKind == JsonValueKind.Object &&
                        result.TryGetProperty("ok", out var ok) && ok.ValueKind is JsonValueKind.True or JsonValueKind.False && ok.GetBoolean();
                    return new { ok = accepted, transportOk = true, result };
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    return new { ok = false, transportOk = false, error = error.GetType().Name + ": " + error.Message };
                }
            }
            var health = await Call("memory-health", new { });
            var clean = await Call("memory-clean", new { confirmed = true, trigger = "pure-natural-runtime-test" });
            var identity = ReadIdentity(child!);
            child!.Refresh();
            double cpuBefore = child.TotalProcessorTime.TotalMilliseconds;
            var freeze = await Call("process-freeze", new { pid = identity.Pid, image = identity.Image, created = identity.Created });
            Thread.Sleep(800);
            child.Refresh();
            double cpuDelta = Math.Max(0, child.TotalProcessorTime.TotalMilliseconds - cpuBefore);
            var resume = await Call("process-resume", new { pid = identity.Pid });
            bool cleanOk = clean.GetType().GetProperty("ok")?.GetValue(clean) is true;
            bool freezeOk = freeze.GetType().GetProperty("ok")?.GetValue(freeze) is true;
            bool resumeOk = resume.GetType().GetProperty("ok")?.GetValue(resume) is true;
            return new
            {
                ok = cleanOk && freezeOk && resumeOk,
                skipped = false,
                executable,
                sha256 = hash,
                health,
                clean,
                freeze,
                resume,
                cpuDeltaDuringFreezeMs = cpuDelta,
                childAliveAfterResume = child is not null && !child.HasExited,
                identity = new { identity.Pid, identity.Created }
            };
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            return new { ok = false, skipped = false, error = error.GetType().Name + ": " + error.Message, executable };
        }
        finally
        {
            if (child is not null)
            {
                EmergencyResume(child.Id);
                try { if (!child.HasExited) child.Kill(entireProcessTree: true); } catch (Exception error) when (error is not OutOfMemoryException) { }
                try { child.WaitForExit(2000); } catch (Exception error) when (error is not OutOfMemoryException) { }
                child.Dispose();
            }
        }
    }

    private static Process StartOwnedCpuChild()
    {
        string powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell/v1.0/powershell.exe");
        string executable = File.Exists(powershell) ? powershell : "pwsh.exe";
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("-NoProfile");
        start.ArgumentList.Add("-Command");
        start.ArgumentList.Add("$end=[DateTime]::UtcNow.AddSeconds(20);while([DateTime]::UtcNow -lt $end){}");
        return Process.Start(start) ?? throw new IOException("Owned process fixture did not start.");
    }

    private static OwnedIdentity ReadIdentity(Process process)
    {
        process.Refresh();
        var imagePath = new StringBuilder(32768);
        uint imageSize = (uint)imagePath.Capacity;
        if (!QueryFullProcessImageNameW(process.Handle, 0, imagePath, ref imageSize))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Owned process image unavailable.");
        string image = imagePath.ToString();
        string created = process.StartTime.ToFileTimeUtc().ToString(CultureInfo.InvariantCulture);
        return new OwnedIdentity(process.Id, image, created);
    }

    private static FreezeRecord FreezeOwned(int pid, string image, string created)
    {
        IntPtr processHandle = OpenProcess(ProcessQueryLimitedInformation | ProcessSetInformation | ProcessSuspendResume, false, pid);
        if (processHandle == IntPtr.Zero) return new FreezeRecord(false, "open-failed", -1, -1, IntPtr.Zero, IntPtr.Zero, "OpenProcess failed.");
        if (!VerifyOwned(processHandle, image, created))
        {
            NtClose(processHandle);
            return new FreezeRecord(false, "identity-failed", -1, -1, IntPtr.Zero, IntPtr.Zero, "Image or creation identity differs.");
        }
        int stateStatus = NtCreateProcessStateChange(out IntPtr stateHandle, ProcessStateChangeState, IntPtr.Zero, processHandle, 0u);
        if (stateStatus == 0)
        {
            int changeStatus = NtChangeProcessState(stateHandle, processHandle, 0, IntPtr.Zero, UIntPtr.Zero, 0u);
            if (changeStatus == 0) return new FreezeRecord(true, "state-change", 0, stateStatus, processHandle, stateHandle, "frozen");
            NtClose(stateHandle);
            stateStatus = changeStatus;
        }
        int suspendStatus = NtSuspendProcess(processHandle);
        if (suspendStatus == 0) return new FreezeRecord(true, "nt-suspend", suspendStatus, stateStatus, processHandle, IntPtr.Zero, "frozen-fallback");
        NtClose(processHandle);
        return new FreezeRecord(false, "suspend-failed", suspendStatus, stateStatus, IntPtr.Zero, IntPtr.Zero, "NtSuspendProcess failed.");
    }

    private static ResumeRecord ResumeOwned(FreezeRecord freeze)
    {
        if (!freeze.Success) return new ResumeRecord(false, -1, -1, "not-frozen");
        int resumeStatus = -1, fallbackStatus = -1;
        if (freeze.StateHandle != IntPtr.Zero)
        {
            resumeStatus = NtChangeProcessState(freeze.StateHandle, freeze.ProcessHandle, 1, IntPtr.Zero, UIntPtr.Zero, 0u);
            NtClose(freeze.StateHandle);
            if (resumeStatus != 0) fallbackStatus = NtResumeProcess(freeze.ProcessHandle);
            NtClose(freeze.ProcessHandle);
            return new ResumeRecord(resumeStatus == 0 || fallbackStatus == 0, resumeStatus, fallbackStatus,
                resumeStatus == 0 ? "state-change-resumed" : "state-object-closed-plus-nt-resume");
        }
        fallbackStatus = NtResumeProcess(freeze.ProcessHandle);
        NtClose(freeze.ProcessHandle);
        return new ResumeRecord(fallbackStatus == 0, resumeStatus, fallbackStatus, "nt-resume");
    }

    private static void EmergencyResume(int pid)
    {
        IntPtr handle = OpenProcess(ProcessSuspendResume, false, pid);
        if (handle == IntPtr.Zero) return;
        try { NtResumeProcess(handle); } finally { NtClose(handle); }
    }

    private static bool VerifyOwned(IntPtr process, string image, string created)
    {
        var path = new StringBuilder(32768);
        uint size = (uint)path.Capacity;
        if (!QueryFullProcessImageNameW(process, 0, path, ref size)) return false;
        if (!GetProcessTimes(process, out long creation, out _, out _, out _)) return false;
        return StringComparer.OrdinalIgnoreCase.Equals(path.ToString(), image) &&
            creation.ToString(CultureInfo.InvariantCulture) == created;
    }

    private static object ReadProcessGroup(string root)
    {
        string executable = Path.Combine(root, "tools", "p0", "measure_process_group.py");
        var candidates = Process.GetProcessesByName("MagicCommunity")
            .Concat(Process.GetProcessesByName("MagicSystemNative"))
            .Concat(Process.GetProcessesByName("MagicNativeEntry"))
            .Concat(Process.GetProcessesByName("MagicCompositionHost"))
            .DistinctBy(process => process.Id)
            .Select(process => process.Id)
            .Take(16)
            .ToArray();
        if (!File.Exists(executable) || candidates.Length == 0)
            return new { ok = true, skipped = true, reason = "Sampler or community process group is absent.", executable, roots = candidates };
        string output = Path.Combine(root, "artifacts", "process-group-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff") + ".json");
        var start = new ProcessStartInfo("python") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        start.ArgumentList.Add(executable);
        start.ArgumentList.Add("--roots");
        foreach (int pid in candidates) start.ArgumentList.Add(pid.ToString(CultureInfo.InvariantCulture));
        start.ArgumentList.Add("--phase"); start.ArgumentList.Add("pure-natural-runtime");
        start.ArgumentList.Add("--output"); start.ArgumentList.Add(output);
        start.ArgumentList.Add("--warmup"); start.ArgumentList.Add("0");
        start.ArgumentList.Add("--samples"); start.ArgumentList.Add("3");
        try
        {
            using var process = Process.Start(start) ?? throw new IOException("Process group sampler did not start.");
            string stdout = process.StandardOutput.ReadToEnd();
            string stderr = process.StandardError.ReadToEnd();
            bool exited = process.WaitForExit(30000);
            int exitCode = exited ? process.ExitCode : -1;
            if (!exited)
            {
                try { process.Kill(entireProcessTree: true); } catch (Exception error) when (error is not OutOfMemoryException) { }
            }
            if (exitCode != 0 || !File.Exists(output))
                return new { ok = false, skipped = false, reason = "Process group sampler failed.", exitCode, stdout, stderr, output };
            using var document = JsonDocument.Parse(File.ReadAllBytes(output));
            var value = document.RootElement;
            var samples = value.GetProperty("samples").EnumerateArray().ToArray();
            return new
            {
                ok = true,
                skipped = false,
                executable,
                output,
                roots = value.GetProperty("root_names").Clone(),
                sampleCount = samples.Length,
                allComplete = samples.All(sample => sample.GetProperty("complete").GetBoolean()),
                firstTotals = samples.First().GetProperty("observed_totals").Clone(),
                lastTotals = samples.Last().GetProperty("observed_totals").Clone(),
                limitations = value.GetProperty("limitations").Clone()
            };
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            return new { ok = false, skipped = false, reason = error.GetType().Name + ": " + error.Message, executable, output, roots = candidates };
        }
    }

    private static object ReadOperationsPlan(string root, PureNaturalPolicy policy)
    {
        var services = new List<object>();
        foreach (string name in policy.ServiceNames)
        {
            string sc = Path.Combine(Environment.SystemDirectory, "sc.exe");
            var start = new ProcessStartInfo(sc) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
            start.ArgumentList.Add("query"); start.ArgumentList.Add(name);
            try
            {
                using var process = Process.Start(start) ?? throw new IOException("sc.exe did not start.");
                string output = process.StandardOutput.ReadToEnd();
                process.WaitForExit(3000);
                var match = Regex.Match(output, @"(?im)^\s*STATE\s*:\s*\d+\s+([A-Z_]+)");
                services.Add(new { name, exitCode = process.HasExited ? process.ExitCode : -1, state = match.Success ? match.Groups[1].Value : "unknown", action = "read-only" });
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            {
                services.Add(new { name, exitCode = -1, state = "unavailable", error = error.GetType().Name, action = "read-only" });
            }
        }
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var candidates = policy.MigrationCandidates.Select(name =>
        {
            string path = Path.Combine(home, name);
            int boundedEntries = 0;
            try { if (Directory.Exists(path)) boundedEntries = Directory.EnumerateFileSystemEntries(path).Take(32).Count(); }
            catch (Exception error) when (error is not OutOfMemoryException) { boundedEntries = -1; }
            return new { name, exists = Directory.Exists(path), boundedEntries, action = "plan-only" };
        }).ToArray();
        var drive = new DriveInfo("C:");
        var plan = PureNaturalConfig.Plan(root, policy);
        return new
        {
            plan,
            services,
            migration = new
            {
                candidates,
                cDriveAvailableBytes = drive.IsReady ? drive.AvailableFreeSpace : 0,
                cDriveTotalBytes = drive.IsReady ? drive.TotalSize : 0,
                action = "plan-only"
            },
            scope = "No service state, user directory, junction or registry value is modified."
        };
    }

    private static string Hex(int value) => "0x" + ((uint)value).ToString("X8", CultureInfo.InvariantCulture);

    private sealed record MemorySnapshot(DateTimeOffset At, long TotalPhysicalBytes, long AvailablePhysicalBytes,
        long TotalPageFileBytes, long AvailablePageFileBytes, long MemoryLoadPercent);
    private sealed record Counters(bool Available, long PageFaults, long PrivateUsage, long WorkingSet);
    private sealed record WorkingSetObservation(bool ApiSucceeded, int FixtureMegabytes, long WorkingSetBeforeBytes,
        long WorkingSetAfterBytes, long WorkingSetDeltaBytes, long PrivateBeforeBytes, long PrivateAfterBytes,
        long PageFaultsBefore, long PageFaultsAfter, long PrivateUsageBefore, long PrivateUsageAfter);
    private sealed record SystemCleanObservation(bool Executed, string Result, IReadOnlyList<object> Privileges,
        MemorySample Before, MemorySample After, long AvailableDeltaBytes, string PurgeStatus, string EmptyWorkingSetsStatus,
        bool PrivilegeNotHeld, bool Effective);
    private sealed record DirectFreezeObservation(bool FreezeSucceeded, bool ResumeSucceeded, string Mode,
        int SuspendStatus, int StateStatus, int ResumeStatus, double CpuDeltaDuringFreezeMs, bool ChildAliveAfterResume,
        string Reason, object? Identity);
    private sealed record OwnedIdentity(int Pid, string Image, string Created);
    private sealed record FreezeRecord(bool Success, string Mode, int SuspendStatus, int StateStatus,
        IntPtr ProcessHandle, IntPtr StateHandle, string Reason);
    private sealed record ResumeRecord(bool Success, int ResumeStatus, int FallbackStatus, string Reason);

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        public uint Length, MemoryLoad;
        public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile, TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryCounters
    {
        public uint Length, PageFaultCount;
        public nuint PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
            QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct Luid { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)]
    private struct TokenPrivileges { public uint Count; public Luid Luid; public uint Attributes; }

    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx value);
    [DllImport("kernel32.dll", EntryPoint = "K32GetProcessMemoryInfo", SetLastError = true)]
    private static extern bool K32GetProcessMemoryInfo(IntPtr process, ref MemoryCounters counters, uint length);
    [DllImport("ntdll.dll")] private static extern int NtSetSystemInformation(int informationClass, ref int information, int length);
    [DllImport("ntdll.dll")] private static extern int NtClose(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("ntdll.dll")] private static extern int NtCreateProcessStateChange(out IntPtr stateHandle, uint desiredAccess,
        IntPtr objectAttributes, IntPtr processHandle, uint reserved);
    [DllImport("ntdll.dll")] private static extern int NtChangeProcessState(IntPtr stateHandle, IntPtr processHandle,
        int stateChangeType, IntPtr extendedInformation, nuint extendedInformationLength, uint reserved);
    [DllImport("ntdll.dll")] private static extern int NtSuspendProcess(IntPtr processHandle);
    [DllImport("ntdll.dll")] private static extern int NtResumeProcess(IntPtr processHandle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder path, ref uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool LookupPrivilegeValueW(string? system, string name, out Luid luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state,
        uint length, IntPtr previous, out uint returned);
}
