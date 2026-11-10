using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

namespace MagicCommunity;

// P1/P2 automatic scheduling. Official MagicAnimation schedules only its own
// working-set trim; the system memory-list clean and process freeze are one-shot
// actions there. This module adds an opt-in timer for both, sharing the same
// native primitives as the diagnostics so behaviour cannot drift.
internal sealed class PureNaturalScheduler : IDisposable
{
    internal const string DisableVariable = "MAGICCOMMUNITY_DISABLE_PURE_NATURAL_SCHEDULE";
    internal const long Never = long.MinValue;

    private readonly PureNaturalPolicy policy;
    private readonly Func<long> now;
    private readonly Func<CleanResult> clean;
    private readonly Func<MemoryServiceReceipt?> serviceClean;
    private readonly Func<IReadOnlyList<int>> children;
    private readonly Func<int, bool> suspend;
    private readonly Func<int, bool> resume;
    private readonly Action<string, object> log;
    private readonly List<Timer> timers = [];
    private readonly HashSet<int> frozen = [];
    private long lastClean = Never, lastFreeze = Never;
    private int cleaning, freezing, disposed;

    private PureNaturalScheduler(PureNaturalPolicy policy, Func<long> now, Func<CleanResult> clean,
        Func<IReadOnlyList<int>> children, Func<int, bool> suspend, Func<int, bool> resume, Action<string, object> log,
        Func<MemoryServiceReceipt?> serviceClean)
    {
        this.serviceClean = serviceClean;
        this.policy = policy;
        this.now = now;
        this.clean = clean;
        this.children = children;
        this.suspend = suspend;
        this.resume = resume;
        this.log = log;
    }

    internal static PureNaturalScheduler? Start(PureNaturalPolicy policy, Action<string, object> log,
        Func<long>? clock = null, Func<CleanResult>? clean = null, Func<IReadOnlyList<int>>? children = null,
        Func<int, bool>? suspend = null, Func<int, bool>? resume = null,
        Func<MemoryServiceReceipt?>? serviceClean = null)
    {
        bool disabled = Environment.GetEnvironmentVariable(DisableVariable) == "1";
        bool any = policy.SystemMemoryClean || policy.OwnedProcessFreeze;
        if (!any || disabled)
        {
            log("pure_natural_schedule", new { enabled = false, reason = disabled ? "environment-disabled" : "policy-disabled", policy.SystemMemoryClean, policy.OwnedProcessFreeze });
            return null;
        }
        var scheduler = new PureNaturalScheduler(policy, clock ?? (() => Environment.TickCount64),
            clean ?? PureNaturalNative.Clean,
            children ?? (() => PureNaturalNative.Children(Environment.ProcessId)),
            suspend ?? PureNaturalNative.Suspend,
            resume ?? PureNaturalNative.Resume,
            log,
            serviceClean ?? MemoryServiceClient.Clean);
        int parent = Environment.ProcessId;
        if (policy.SystemMemoryClean)
            scheduler.timers.Add(new Timer(_ => scheduler.CleanTick(), null, policy.SystemCleanIntervalMs, policy.SystemCleanIntervalMs));
        if (policy.OwnedProcessFreeze)
            scheduler.timers.Add(new Timer(_ => scheduler.FreezeTick(parent), null, policy.OwnedFreezeIntervalMs, policy.OwnedFreezeIntervalMs));
        log("pure_natural_schedule", new
        {
            enabled = true,
            systemClean = policy.SystemMemoryClean,
            systemCleanIntervalMs = policy.SystemCleanIntervalMs,
            ownedFreeze = policy.OwnedProcessFreeze,
            ownedFreezeIntervalMs = policy.OwnedFreezeIntervalMs,
            explicitPids = policy.FreezePids,
            memoryServiceInstalled = MemoryServiceClient.Installed(),
            memoryServiceHealthy = MemoryServiceClient.Installed() && MemoryServiceClient.Healthy()
        });
        return scheduler;
    }

    internal int FrozenCount { get { lock (frozen) return frozen.Count; } }

    internal void CleanTick()
    {
        if (Volatile.Read(ref disposed) != 0) return;
        if (Interlocked.Exchange(ref cleaning, 1) != 0) return;
        try
        {
            long tick = now();
            long previous = Interlocked.Read(ref lastClean);
            if (previous != Never && tick - previous < policy.SystemCleanIntervalMs) return;
            // Prefer the LocalSystem service so the desktop keeps its normal
            // token; fall back to the in-process call when the service is absent.
            var serviced = serviceClean();
            if (serviced is { } receipt)
            {
                Interlocked.Exchange(ref lastClean, tick);
                log("pure_natural_clean", new
                {
                    trigger = "schedule",
                    source = "service",
                    purgeStatus = Hex(receipt.PurgeStatus),
                    emptyWorkingSetsStatus = Hex(receipt.EmptyStatus),
                    availableDeltaBytes = receipt.AvailableDeltaBytes,
                    privilegeNotHeld = receipt.PrivilegeNotHeld,
                    effective = receipt.Effective
                });
                return;
            }
            var result = clean();
            Interlocked.Exchange(ref lastClean, tick);
            log("pure_natural_clean", new
            {
                trigger = "schedule",
                source = "in-process",
                purgeStatus = Hex(result.PurgeStatus),
                emptyWorkingSetsStatus = Hex(result.EmptyWorkingSetsStatus),
                availableDeltaBytes = result.AvailableDeltaBytes,
                privilegeNotHeld = result.PrivilegeNotHeld,
                effective = result.Effective
            });
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            log("pure_natural_clean_failed", new { type = error.GetType().Name, message = error.Message });
        }
        finally { Interlocked.Exchange(ref cleaning, 0); }
    }

    // Only direct children of this process are frozen; explicit PIDs must also
    // be present in that child list, so a policy file cannot target user apps.
    internal void FreezeTick(int parentPid)
    {
        if (Volatile.Read(ref disposed) != 0) return;
        if (Interlocked.Exchange(ref freezing, 1) != 0) return;
        try
        {
            long tick = now();
            long previous = Interlocked.Read(ref lastFreeze);
            if (previous != Never && tick - previous < policy.OwnedFreezeIntervalMs) return;
            var owned = new HashSet<int>(children());
            var chosen = new List<int>();
            foreach (int pid in policy.FreezePids)
                if (owned.Contains(pid)) chosen.Add(pid);
            foreach (int pid in owned)
                if (!chosen.Contains(pid)) chosen.Add(pid);
            int suspended = 0;
            lock (frozen)
            {
                foreach (int pid in chosen)
                {
                    if (frozen.Contains(pid)) continue;
                    if (suspend(pid)) { frozen.Add(pid); suspended++; }
                }
            }
            Interlocked.Exchange(ref lastFreeze, tick);
            log("pure_natural_freeze", new { trigger = "schedule", owned = owned.Count, suspended, frozen = FrozenCount, explicitPids = policy.FreezePids });
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            log("pure_natural_freeze_failed", new { type = error.GetType().Name, message = error.Message });
        }
        finally { Interlocked.Exchange(ref freezing, 0); }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        foreach (var timer in timers) timer.Dispose();
        timers.Clear();
        int resumed = 0;
        lock (frozen)
        {
            foreach (int pid in frozen)
                if (resume(pid)) resumed++;
            frozen.Clear();
        }
        log("pure_natural_schedule_stopped", new { resumed, frozen = FrozenCount });
    }

    private static string Hex(int value) => "0x" + ((uint)value).ToString("X8");
}
