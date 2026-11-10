using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Threading;

namespace MagicCommunity;

// Deep module: callers start it once and request an interaction trim; all
// thresholds, timers, managed collection, Win32 fallback and logging stay here.
public sealed class PureNaturalEngine : IDisposable
{
    internal const string DisableVariable = "MAGICCOMMUNITY_DISABLE_WORKING_SET_TRIM";
    internal const long StartupThresholdBytes = 64L * 1024 * 1024;
    internal const long PeriodicThresholdBytes = 120L * 1024 * 1024;
    internal const long InteractionThresholdBytes = 96L * 1024 * 1024;
    internal const long MinimumIntervalMs = 120_000L;
    internal const long PeriodicIntervalMs = 300_000L;
    internal const long Never = long.MinValue;
    private static readonly TimeSpan[] StartupDelays =
    [
        TimeSpan.FromSeconds(8), TimeSpan.FromSeconds(12), TimeSpan.FromSeconds(20),
        TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(45), TimeSpan.FromSeconds(120)
    ];

    private readonly bool enabled;
    private readonly Func<long> workingSet;
    private readonly Func<long> now;
    private readonly Func<bool> lowPower;
    private readonly Action collect;
    private readonly Func<bool> trim;
    private readonly Action<string, object> log;
    private readonly List<DispatcherTimer> startupTimers = [];
    private DispatcherTimer? periodic;
    private DispatcherTimer? interaction;
    private long lastTrim = Never;
    private int active;
    private int successful;
    private bool disposed;

    private PureNaturalEngine(bool enabled, Func<long> workingSet, Func<long> now, Func<bool> lowPower,
        Action collect, Func<bool> trim, Action<string, object> log, bool schedule)
    {
        this.enabled = enabled;
        this.workingSet = workingSet;
        this.now = now;
        this.lowPower = lowPower;
        this.collect = collect;
        this.trim = trim;
        this.log = log;
        if (schedule && enabled) Schedule();
    }

    public static PureNaturalEngine Start(AppController controller)
    {
        var preferences = CommunityPreferences.Load(controller.Root);
        bool environmentDisabled = Environment.GetEnvironmentVariable(DisableVariable) == "1";
        bool enabled = preferences.WorkingSetTrim && !environmentDisabled;
        controller.Log("pure_natural_started", new
        {
            version = AppController.Version,
            enabled,
            environmentDisabled,
            preference = preferences.WorkingSetTrim,
            startupDelaysSeconds = new[] { 8, 12, 20, 30, 45, 120 },
            startupThresholdBytes = StartupThresholdBytes,
            periodicThresholdBytes = PeriodicThresholdBytes,
            interactionThresholdBytes = InteractionThresholdBytes,
            minimumIntervalMs = MinimumIntervalMs,
            batterySaverAware = true
        });
        return new PureNaturalEngine(enabled, () => Environment.WorkingSet, () => Environment.TickCount64,
            () => CommunityPreferences.Power().Saver, CollectManaged, TrimCurrentProcess,
            (name, value) => controller.Log(name, value), true);
    }

    internal static PureNaturalEngine ForTest(bool enabled, Func<long> workingSet, Func<long> now,
        Func<bool> lowPower, Action collect, Func<bool> trim, Action<string, object> log) =>
        new(enabled, workingSet, now, lowPower, collect, trim, log, false);

    internal static MemoryTrimResult Evaluate(long workingSetBytes, long elapsedMs, long thresholdBytes,
        bool enabled, bool lowPower)
    {
        if (!enabled) return new(false, false, "disabled", workingSetBytes, thresholdBytes, elapsedMs);
        if (lowPower) return new(false, false, "battery-saver", workingSetBytes, thresholdBytes, elapsedMs);
        if (elapsedMs < MinimumIntervalMs) return new(false, false, "interval", workingSetBytes, thresholdBytes, elapsedMs);
        if (workingSetBytes < thresholdBytes) return new(false, false, "below-threshold", workingSetBytes, thresholdBytes, elapsedMs);
        return new(true, false, "threshold-reached", workingSetBytes, thresholdBytes, elapsedMs);
    }

    internal MemoryTrimResult TrimIfNeeded(long thresholdBytes, string trigger)
    {
        if (disposed) return new(false, false, "disposed", 0, thresholdBytes, 0);
        long current = workingSet();
        long tick = now();
        long previous = Interlocked.Read(ref lastTrim);
        long elapsed = previous == Never ? long.MaxValue : Math.Max(0L, tick - previous);
        var decision = Evaluate(current, elapsed, thresholdBytes, enabled, lowPower());
        if (!decision.Trimmed)
        {
            Write("pure_natural_skip", trigger, decision);
            return decision;
        }
        if (Interlocked.Exchange(ref active, 1) != 0)
        {
            var busy = decision with { Trimmed = false, Reason = "busy" };
            Write("pure_natural_skip", trigger, busy);
            return busy;
        }
        try
        {
            collect();
            bool succeeded = trim();
            if (succeeded)
            {
                Interlocked.Exchange(ref lastTrim, tick);
                Interlocked.Increment(ref successful);
            }
            var result = decision with { Succeeded = succeeded, Reason = succeeded ? "trimmed" : "api-failed" };
            Write("pure_natural_trim", trigger, result);
            return result;
        }
        finally
        {
            Interlocked.Exchange(ref active, 0);
        }
    }

    public void RequestInteractionTrim()
    {
        if (!enabled || disposed) return;
        interaction ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(12) };
        interaction.Stop();
        interaction.Tick -= InteractionTick;
        interaction.Tick += InteractionTick;
        interaction.Start();
    }

    public int TrimCount => Volatile.Read(ref successful);

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        foreach (var timer in startupTimers) timer.Stop();
        startupTimers.Clear();
        periodic?.Stop();
        if (interaction is not null)
        {
            interaction.Stop();
            interaction.Tick -= InteractionTick;
        }
    }

    private void Schedule()
    {
        foreach (var delay in StartupDelays)
        {
            var timer = new DispatcherTimer { Interval = delay };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                startupTimers.Remove(timer);
                TrimIfNeeded(StartupThresholdBytes, "startup");
            };
            startupTimers.Add(timer);
            timer.Start();
        }
        periodic = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(PeriodicIntervalMs) };
        periodic.Tick += (_, _) => TrimIfNeeded(PeriodicThresholdBytes, "periodic");
        periodic.Start();
    }

    private void InteractionTick(object? sender, EventArgs e)
    {
        interaction?.Stop();
        TrimIfNeeded(InteractionThresholdBytes, "interaction");
    }

    private void Write(string name, string trigger, MemoryTrimResult result) => log(name, new
    {
        trigger,
        result.Trimmed,
        result.Succeeded,
        result.Reason,
        result.WorkingSetBytes,
        result.ThresholdBytes,
        result.ElapsedMs,
        trimCount = TrimCount,
        scope = "Owned process working-set trim; no system memory-list command, service change or third-party process action"
    });

    private static void CollectManaged() => GC.Collect(GC.MaxGeneration, GCCollectionMode.Optimized, false, false);

    internal static bool TrimCurrentProcess()
    {
        IntPtr process = GetCurrentProcess();
        return EmptyWorkingSet(process) || SetProcessWorkingSetSize(process, (nint)(-1), (nint)(-1));
    }

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll", EntryPoint = "K32EmptyWorkingSet", SetLastError = true)]
    private static extern bool EmptyWorkingSet(IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetProcessWorkingSetSize(IntPtr process, nint minimum, nint maximum);
}

internal readonly record struct MemoryTrimResult(
    bool Trimmed,
    bool Succeeded,
    string Reason,
    long WorkingSetBytes,
    long ThresholdBytes,
    long ElapsedMs);
