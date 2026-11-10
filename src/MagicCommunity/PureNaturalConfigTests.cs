using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace MagicCommunity;

internal static class PureNaturalConfigTests
{
    public static object Run(string root)
    {
        string folder = Path.Combine(root, "evidence", "preview", "pure-natural-config-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
        string fixture = Path.Combine(folder, "data");
        Directory.CreateDirectory(fixture);
        var checks = new List<string>();
        void Check(string name, Action test) { test(); checks.Add(name); }
        void Expect(bool value, string text) { if (!value) throw new InvalidOperationException(text); }
        void ExpectInvalid(Action action, string text)
        {
            try { action(); }
            catch (InvalidDataException) { return; }
            catch (JsonException) { return; }
            throw new InvalidOperationException(text);
        }
        string path = PureNaturalConfig.PathOf(folder);

        Check("defaults-are-opt-in-and-dry-run", () =>
        {
            var policy = new PureNaturalPolicy();
            Expect(!policy.SystemMemoryClean && !policy.OwnedProcessFreeze && !policy.ServiceControls && !policy.DiskMigration, "defaults changed");
            Expect(policy.Mode == "dry-run", "default mode changed");
        });
        Check("save-load-roundtrip", () =>
        {
            var policy = new PureNaturalPolicy { SystemMemoryClean = true, OwnedProcessFreeze = true, FreezeIdleMs = 5000 };
            PureNaturalConfig.Save(folder, policy);
            var loaded = PureNaturalConfig.Load(folder);
            Expect(loaded.SystemMemoryClean && loaded.OwnedProcessFreeze && loaded.FreezeIdleMs == 5000, "roundtrip lost fields");
        });
        Check("unknown-key-is-rejected", () =>
        {
            File.WriteAllText(path, """{"systemMemoryClean":true,"unexpected":1}""");
            ExpectInvalid(() => PureNaturalConfig.Load(folder), "unknown key accepted");
        });
        Check("oversize-is-rejected", () =>
        {
            File.WriteAllText(path, new string('x', PureNaturalConfig.MaxBytes + 1));
            ExpectInvalid(() => PureNaturalConfig.Load(folder), "oversize accepted");
        });
        Check("bounds-are-enforced", () =>
        {
            ExpectInvalid(() => PureNaturalConfig.Validate(new PureNaturalPolicy { Mode = "destroy" }), "invalid mode accepted");
            ExpectInvalid(() => PureNaturalConfig.Validate(new PureNaturalPolicy { FreezeIdleMs = 0 }), "invalid idle accepted");
            ExpectInvalid(() => PureNaturalConfig.Validate(new PureNaturalPolicy { FreezeWorkingSetBytes = 1 }), "invalid working set accepted");
            ExpectInvalid(() => PureNaturalConfig.Validate(new PureNaturalPolicy { ServiceNames = ["..\evil"] }), "path traversal accepted");
        });
        Check("plan-is-read-only-by-default", () =>
        {
            var policy = new PureNaturalPolicy();
            var plan = JsonSerializer.SerializeToElement(PureNaturalConfig.Plan(folder, policy));
            Expect(plan.GetProperty("execute").GetBoolean() == false, "dry-run unexpectedly executes");
            Expect(plan.GetProperty("mode").GetString() == "dry-run", "mode mismatch");
            Expect(plan.GetProperty("p3").GetProperty("services").GetArrayLength() == 2, "service plan missing");
            Expect(plan.GetProperty("p4").GetProperty("candidates").GetArrayLength() == 3, "migration plan missing");
            Expect(!Directory.Exists(Path.Combine(folder, "sysmain-stopped")), "plan created side effects");
        });
        Check("apply-mode-is-explicit", () =>
        {
            var policy = new PureNaturalPolicy { Mode = "apply", ServiceControls = true };
            var plan = JsonSerializer.SerializeToElement(PureNaturalConfig.Plan(folder, policy));
            Expect(plan.GetProperty("execute").GetBoolean(), "apply mode not reflected");
        });
        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass = true,
            checks,
            testDirectory = folder,
            scope = "Owned configuration validation and dry-run plan; no service, driver, third-party process or user file action"
        };
    }
}
