using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace MagicCommunity;

internal static class PureNaturalOperationsTests
{
    public static object Run(string root)
    {
        string folder = Path.Combine(root, "evidence", "preview", "pure-natural-operations-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
        Directory.CreateDirectory(folder);
        var checks = new List<string>();
        void Check(string name, Action test) { test(); checks.Add(name); }
        void Expect(bool value, string text) { if (!value) throw new InvalidOperationException(text); }

        Check("dry-run-does-not-touch-actions", () =>
        {
            var fake = new FakeActions();
            var policy = new PureNaturalPolicy { ServiceControls = true, DiskMigration = true, MigrationTargetRoot = Path.Combine(folder, "target") };
            var result = PureNaturalOperations.Apply(folder, policy, fake);
            Expect(fake.Calls.Count == 0, "dry-run invoked the adapter");
            Expect(result.GetType().GetProperty("applied")!.GetValue(result) is false, "dry-run reported applied");
        });
        Check("apply-stops-and-records-service", () =>
        {
            var fake = new FakeActions();
            var policy = new PureNaturalPolicy { Mode = "apply", ServiceControls = true, ServiceNames = ["SysMain"] };
            var result = PureNaturalOperations.Apply(folder, policy, fake);
            Expect(fake.Calls.SequenceEqual(["query:SysMain", "stop:SysMain", "query:SysMain"]), "unexpected service sequence: " + string.Join(",", fake.Calls));
            Expect(result.GetType().GetProperty("applied")!.GetValue(result) is true, "apply did not report applied");
        });
        Check("apply-service-error-does-not-abort", () =>
        {
            var fake = new FakeActions() { FailStop = "whesvc" };
            var policy = new PureNaturalPolicy { Mode = "apply", ServiceControls = true, ServiceNames = ["SysMain", "whesvc"] };
            PureNaturalOperations.Apply(folder, policy, fake);
            Expect(fake.Calls.Contains("stop:whesvc"), "second service skipped after failure");
        });
        Check("migration-moves-then-links", () =>
        {
            var fake = new FakeActions();
            Directory.CreateDirectory(Path.Combine(folder, "home", "Desktop"));
            Directory.CreateDirectory(Path.Combine(folder, "target"));
            var policy = new PureNaturalPolicy
            {
                Mode = "apply",
                DiskMigration = true,
                MigrationCandidates = ["Desktop"],
                MigrationTargetRoot = Path.Combine(folder, "target")
            };
            var result = PureNaturalOperations.ApplyWithHome(folder, policy, fake, Path.Combine(folder, "home"));
            Expect(fake.Calls.Contains("move:" + Path.Combine(folder, "home", "Desktop")), "no move recorded");
            Expect(fake.Calls.Any(call => call.StartsWith("junction:", StringComparison.Ordinal)), "no junction recorded");
            var receiptPath = result.GetType().GetProperty("receipt")!.GetValue(result) as string;
            Expect(receiptPath is not null && File.Exists(receiptPath), "receipt file missing");
            string receipt = File.ReadAllText(receiptPath!);
            Expect(receipt.Contains("\"type\":\"migration\"", StringComparison.Ordinal) ||
                   receipt.Contains("migration", StringComparison.Ordinal), "receipt has no migration entry");
        });
        Check("migration-junction-failure-rolls-back", () =>
        {
            var fake = new FakeActions() { FailJunction = true };
            Directory.CreateDirectory(Path.Combine(folder, "home2", "Desktop"));
            var policy = new PureNaturalPolicy
            {
                Mode = "apply",
                DiskMigration = true,
                MigrationCandidates = ["Desktop"],
                MigrationTargetRoot = Path.Combine(folder, "target2")
            };
            Directory.CreateDirectory(Path.Combine(folder, "target2"));
            PureNaturalOperations.ApplyWithHome(folder, policy, fake, Path.Combine(folder, "home2"));
            Expect(fake.Calls.Count(call => call.StartsWith("move:", StringComparison.Ordinal)) == 2, "rollback move missing");
        });
        Check("migration-rejects-existing-destination", () =>
        {
            var fake = new FakeActions();
            Directory.CreateDirectory(Path.Combine(folder, "home3", "Desktop"));
            Directory.CreateDirectory(Path.Combine(folder, "target3", "Desktop"));
            var policy = new PureNaturalPolicy
            {
                Mode = "apply",
                DiskMigration = true,
                MigrationCandidates = ["Desktop"],
                MigrationTargetRoot = Path.Combine(folder, "target3")
            };
            PureNaturalOperations.ApplyWithHome(folder, policy, fake, Path.Combine(folder, "home3"));
            Expect(!fake.Calls.Any(call => call.StartsWith("move:", StringComparison.Ordinal)), "existing destination overwritten");
        });
        Check("migration-rejects-reparse-source", () =>
        {
            var fake = new FakeActions() { ReparseSource = true };
            Directory.CreateDirectory(Path.Combine(folder, "home4", "Desktop"));
            Directory.CreateDirectory(Path.Combine(folder, "target4"));
            var policy = new PureNaturalPolicy
            {
                Mode = "apply",
                DiskMigration = true,
                MigrationCandidates = ["Desktop"],
                MigrationTargetRoot = Path.Combine(folder, "target4")
            };
            PureNaturalOperations.ApplyWithHome(folder, policy, fake, Path.Combine(folder, "home4"));
            Expect(!fake.Calls.Any(call => call.StartsWith("move:", StringComparison.Ordinal)), "reparse source migrated");
        });
        Check("rollback-restores-service-and-directory", () =>
        {
            var fake = new FakeActions();
            string receipt = Path.Combine(folder, "receipt.json");
            File.WriteAllText(receipt, """{"rollback":[{"type":"service","name":"SysMain","previous":"RUNNING","next":"STOPPED"},{"type":"migration","name":"Desktop","junction":"C:\\j\\Desktop","target":"D:\\t\\Desktop"}]}""");
            PureNaturalOperations.Rollback(folder, receipt, fake);
            Expect(fake.Calls.Contains("start:SysMain"), "service not restarted");
            Expect(fake.Calls.Any(call => call.StartsWith("remove-junction:", StringComparison.Ordinal)), "junction not removed");
        });
        Check("policy-bounds-reject-invalid-intervals", () =>
        {
            bool threw = false;
            try { PureNaturalConfig.Validate(new PureNaturalPolicy { SystemCleanIntervalMs = 1000 }); }
            catch (InvalidDataException) { threw = true; }
            Expect(threw, "invalid interval accepted");
        });
        Check("policy-bounds-reject-relative-target", () =>
        {
            bool threw = false;
            try { PureNaturalConfig.Validate(new PureNaturalPolicy { MigrationTargetRoot = "relative\\path" }); }
            catch (InvalidDataException) { threw = true; }
            Expect(threw, "relative target accepted");
        });

        return new { version = AppController.Version, at = DateTimeOffset.Now, allPass = true, checks, testDirectory = folder, scope = "In-memory adapter only; no service, junction or user directory action." };
    }

    private sealed class FakeActions : IPureNaturalActions
    {
        internal List<string> Calls { get; } = [];
        internal string? FailStop { get; init; }
        internal bool FailJunction { get; init; }
        internal bool ReparseSource { get; init; }

        public bool IsReparsePoint(string path) => ReparseSource;

        private bool stopped;
        public string QueryServiceState(string name)
        {
            Calls.Add("query:" + name);
            return stopped ? "STOPPED" : "RUNNING";
        }

        // Mirrors the Windows adapter: state before, stop, state after.
        public ServiceReceipt StopService(string name)
        {
            string before = QueryServiceState(name);
            Calls.Add("stop:" + name);
            if (name == FailStop) throw new InvalidOperationException("stop failed");
            stopped = true;
            string after = QueryServiceState(name);
            return new ServiceReceipt(name, before != after, before, after, 0);
        }

        public bool StartService(string name) { Calls.Add("start:" + name); return true; }

        public void MoveDirectory(string source, string destination) { Calls.Add("move:" + source); }

        public void CreateJunction(string junction, string target)
        {
            Calls.Add("junction:" + junction);
            if (FailJunction) throw new IOException("junction failed");
        }

        public void RemoveJunction(string junction) { Calls.Add("remove-junction:" + junction); }

        public void RemoveEmptyDirectory(string path) { Calls.Add("remove-directory:" + path); }
    }
}
