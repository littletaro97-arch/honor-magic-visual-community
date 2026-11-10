using System;
using System.Collections.Generic;
using System.IO;

namespace MagicCommunity;

internal static class PureNaturalTests
{
    public static object Run(string root)
    {
        string folder = Path.Combine(root, "evidence", "preview", "pure-natural-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        var logs = new List<object>();
        long now = 1_000_000L;
        long workingSet = 200L * 1024 * 1024;
        bool lowPower = false;
        bool trimOk = true;
        int collect = 0;
        int trim = 0;
        void Check(string name, Action test) { test(); checks.Add(name); }
        void Expect(bool value, string text) { if (!value) throw new InvalidOperationException(text); }
        using var engine = PureNaturalEngine.ForTest(true, () => workingSet, () => now, () => lowPower,
            () => collect++, () => { trim++; return trimOk; }, (name, value) => logs.Add(new { name, value }));

        Check("threshold-trims-and-calls-strategy", () =>
        {
            var result = engine.TrimIfNeeded(PureNaturalEngine.StartupThresholdBytes, "startup");
            Expect(result.Trimmed && result.Succeeded && result.Reason == "trimmed", "threshold trim failed");
            Expect(collect == 1 && trim == 1 && engine.TrimCount == 1, "trim strategy not invoked once");
        });
        Check("minimum-interval-is-enforced", () =>
        {
            now += 1_000;
            var result = engine.TrimIfNeeded(PureNaturalEngine.StartupThresholdBytes, "startup");
            Expect(!result.Trimmed && result.Reason == "interval", "minimum interval ignored");
            Expect(collect == 1 && trim == 1, "interval trim unexpectedly invoked strategy");
        });
        Check("below-threshold-is-preserved", () =>
        {
            now += PureNaturalEngine.MinimumIntervalMs;
            workingSet = 10L * 1024 * 1024;
            var result = engine.TrimIfNeeded(PureNaturalEngine.StartupThresholdBytes, "startup");
            Expect(!result.Trimmed && result.Reason == "below-threshold", "below-threshold trim accepted");
        });
        Check("battery-saver-defers-trim", () =>
        {
            now += PureNaturalEngine.MinimumIntervalMs;
            workingSet = 200L * 1024 * 1024;
            lowPower = true;
            var result = engine.TrimIfNeeded(PureNaturalEngine.StartupThresholdBytes, "startup");
            Expect(!result.Trimmed && result.Reason == "battery-saver", "battery saver trim accepted");
            lowPower = false;
        });
        Check("api-failure-does-not-advance-schedule", () =>
        {
            now += PureNaturalEngine.MinimumIntervalMs;
            trimOk = false;
            var result = engine.TrimIfNeeded(PureNaturalEngine.PeriodicThresholdBytes, "periodic");
            Expect(result.Trimmed && !result.Succeeded && result.Reason == "api-failed", "API failure misreported");
            Expect(engine.TrimCount == 1, "failed trim incremented success count");
        });
        Check("retry-after-api-failure-succeeds", () =>
        {
            now += PureNaturalEngine.MinimumIntervalMs;
            trimOk = true;
            var result = engine.TrimIfNeeded(PureNaturalEngine.InteractionThresholdBytes, "interaction");
            Expect(result.Trimmed && result.Succeeded && engine.TrimCount == 2, "retry did not recover");
            Expect(collect == 3 && trim == 3, "strategy call count mismatch");
        });
        Check("disabled-engine-never-calls-strategy", () =>
        {
            using var disabled = PureNaturalEngine.ForTest(false, () => workingSet, () => now, () => false,
                () => collect++, () => { trim++; return true; }, (name, value) => logs.Add(new { name, value }));
            var result = disabled.TrimIfNeeded(PureNaturalEngine.StartupThresholdBytes, "startup");
            Expect(!result.Trimmed && result.Reason == "disabled", "disabled engine accepted trim");
            Expect(collect == 3 && trim == 3, "disabled engine invoked strategy");
        });
        Check("win32-working-set-api-is-available", () =>
        {
            Expect(PureNaturalEngine.TrimCurrentProcess(), "EmptyWorkingSet and SetProcessWorkingSetSize both failed");
        });
        Check("preference-default-is-enabled", () =>
        {
            Expect(new CommunityPreferences().WorkingSetTrim, "working-set trim preference default changed");
        });
        Check("evaluation-receipts-are-bounded", () =>
        {
            Expect(logs.Count == 7, "unexpected diagnostic count");
            Expect(PureNaturalEngine.StartupThresholdBytes < PureNaturalEngine.InteractionThresholdBytes &&
                PureNaturalEngine.InteractionThresholdBytes < PureNaturalEngine.PeriodicThresholdBytes,
                "threshold ordering changed");
        });
        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass = true,
            checks,
            testDirectory = folder,
            scope = "Owned policy and Win32 working-set API checks; no system memory-list command or third-party process action"
        };
    }
}
