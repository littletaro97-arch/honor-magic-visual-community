using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace MagicCommunity;

internal static class PureNaturalSchedulerTests
{
    public static object Run(string root)
    {
        string folder = Path.Combine(root, "evidence", "preview", "pure-natural-schedule-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        var logs = new List<string>();
        void Check(string name, Action test) { test(); checks.Add(name); }
        void Expect(bool value, string text) { if (!value) throw new InvalidOperationException(text); }
        void Log(string name, object value) => logs.Add(name);

        Check("disabled-policy-does-not-start", () =>
        {
            var scheduler = PureNaturalScheduler.Start(new PureNaturalPolicy(), Log, () => 0L,
                () => throw new InvalidOperationException("clean must not run"),
                () => [], _ => throw new InvalidOperationException("suspend must not run"), _ => true);
            Expect(scheduler is null, "disabled policy started a scheduler");
        });
        Check("clean-interval-is-enforced", () =>
        {
            long now = 1_000_000L;
            int cleans = 0;
            var policy = new PureNaturalPolicy { SystemMemoryClean = true, SystemCleanIntervalMs = 60_000 };
            using var scheduler = PureNaturalScheduler.Start(policy, Log, () => now,
                () => { cleans++; return new CleanResult(true, new MemorySample(1, 100), new MemorySample(1, 200), [], 0, 0); },
                () => [], _ => true, _ => true, () => null)!;
            scheduler.CleanTick();
            Expect(cleans == 1, "first scheduled clean skipped");
            scheduler.CleanTick();
            Expect(cleans == 1, "interval did not suppress a second clean");
            now += 60_000;
            scheduler.CleanTick();
            Expect(cleans == 2, "elapsed interval did not allow a clean");
        });
        Check("freeze-targets-owned-children-only", () =>
        {
            long now = 5_000_000L;
            var suspended = new List<int>();
            var policy = new PureNaturalPolicy { OwnedProcessFreeze = true, OwnedFreezeIntervalMs = 60_000, FreezePids = [999_999] };
            using var scheduler = PureNaturalScheduler.Start(policy, Log, () => now,
                () => new CleanResult(true, new MemorySample(1, 1), new MemorySample(1, 1), [], 0, 0),
                () => [4242, 4243], pid => { suspended.Add(pid); return true; }, _ => true)!;
            scheduler.FreezeTick(1);
            Expect(suspended.SequenceEqual([4242, 4243]), "explicit foreign pid was not filtered: " + string.Join(",", suspended));
            Expect(scheduler.FrozenCount == 2, "frozen count mismatch");
        });
        Check("dispose-resumes-every-frozen-process", () =>
        {
            long now = 9_000_000L;
            var resumed = new List<int>();
            var policy = new PureNaturalPolicy { OwnedProcessFreeze = true, OwnedFreezeIntervalMs = 60_000 };
            var scheduler = PureNaturalScheduler.Start(policy, Log, () => now,
                () => new CleanResult(true, new MemorySample(1, 1), new MemorySample(1, 1), [], 0, 0),
                () => [5150, 5151], _ => true, pid => { resumed.Add(pid); return true; })!;
            scheduler.FreezeTick(1);
            scheduler.Dispose();
            Expect(resumed.SequenceEqual([5150, 5151]), "dispose did not resume all: " + string.Join(",", resumed));
            Expect(scheduler.FrozenCount == 0, "frozen table not cleared");
        });
        Check("clean-failure-is-logged-not-thrown", () =>
        {
            long now = 0L;
            var policy = new PureNaturalPolicy { SystemMemoryClean = true, SystemCleanIntervalMs = 60_000 };
            using var scheduler = PureNaturalScheduler.Start(policy, Log, () => now,
                () => throw new IOException("boom"), () => [], _ => true, _ => true, () => null)!;
            scheduler.CleanTick();
            Expect(logs.Contains("pure_natural_clean_failed"), "failure was not logged");
        });
        Check("default-policy-keeps-automation-off", () =>
        {
            var policy = new PureNaturalPolicy();
            Expect(!policy.SystemMemoryClean && !policy.OwnedProcessFreeze, "automation default changed to on");
            PureNaturalConfig.Validate(policy);
        });
        Check("apply-mode-required-for-service-and-migration", () =>
        {
            var plan = PureNaturalConfig.Plan(folder, new PureNaturalPolicy { ServiceControls = true, DiskMigration = true });
            var serialized = System.Text.Json.JsonSerializer.Serialize(plan);
            Expect(serialized.Contains("\"execute\":false"), "dry-run plan claimed execute");
        });

        return new { version = AppController.Version, at = DateTimeOffset.Now, allPass = true, checks, testDirectory = folder, scope = "Injected clock and fakes; no timer wait, service, process or file action." };
    }
}
