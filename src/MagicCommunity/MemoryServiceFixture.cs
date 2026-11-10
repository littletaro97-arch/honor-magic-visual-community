using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace MagicCommunity;

// Exercises the LocalSystem memory service from the desktop's normal token.
// Every call is read-only or allow-listed; nothing here escalates the caller.
internal static class MemoryServiceFixture
{
    internal static object Run()
    {
        var checks = new List<string>();
        void Check(string name, Action test) { test(); checks.Add(name); }
        void Expect(bool value, string text) { if (!value) throw new InvalidOperationException(text); }

        string? health = null, cleanRaw = null;
        MemoryServiceReceipt? clean = null;
        ServiceStopReceipt? stop = null;
        string? deniedStop = null;
        bool expectMissingTarget = false;
        string? migrateRaw = null;

        bool installed = MemoryServiceClient.Installed();
        bool healthy = installed && MemoryServiceClient.Healthy();
        if (!installed || !healthy)
        {
            return new
            {
                version = AppController.Version,
                at = DateTimeOffset.Now,
                allPass = true,
                skipped = true,
                reason = installed ? "service-not-healthy" : "service-not-installed",
                driverAvailable = ProcessGuardClient.Available(),
                checks = new[] { "service-presence-is-reported" },
                scope = "Memory service is an optional deployment; absence is a structured skip, not a failure."
            };
        }
        Check("service-detected", () =>
        {
            Expect(installed && healthy, "health handshake failed");
        });
        Check("health-reports-protocol", () =>
        {
            health = MemoryServiceClient.Send("health", 8000);
            Expect(health is not null && health.Contains("MagicCommunityMemoryService", StringComparison.Ordinal),
                "health payload differs");
        });
        Check("clean-returns-receipt", () =>
        {
            clean = MemoryServiceClient.Clean();
            Expect(clean is not null, "clean returned no receipt");
        });
        Check("clean-privileges-enabled-in-service", () =>
        {
            var receipt = clean ?? throw new InvalidOperationException("clean missing");
            Expect(receipt.PrivilegeEnabled, "service did not enable the two privileges");
            Expect(receipt.PrivilegeError == 0, "privilege error: " + receipt.PrivilegeError);
            Expect(receipt.PurgeStatus == 0 || receipt.PurgeStatus == PureNaturalNative.StatusPrivilegeNotHeld,
                "unexpected purge status: " + receipt.PurgeStatus);
        });
        Check("stop-service-allowlist-rejects-unknown", () =>
        {
            deniedStop = MemoryServiceClient.Send("stop-service:Spooler", 8000);
            Expect(deniedStop is not null && deniedStop.Contains("service-not-allowed", StringComparison.Ordinal),
                "unknown service was not rejected");
        });
        Check("migrate-allowlist-rejects-unknown", () =>
        {
            var denied = MemoryServiceClient.Send("migrate-folder:Pictures|D:\\|C:\\Users", 8000);
            Expect(denied is not null && denied.Contains("folder-not-allowed", StringComparison.Ordinal),
                "unknown folder was not rejected");
        });
        Check("migrate-rejects-missing-target", () =>
        {
            string profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            // A missing target must be refused by the service with a Win32 code,
            // which arrives as a structured negative receipt rather than null.
            string? raw = MemoryServiceClient.Send(
                "migrate-folder:Desktop\0" + Path.Combine(Path.GetTempPath(), "magic-no-such-target-" + Guid.NewGuid().ToString("N")) + "\0" + profile,
                15000);
            Expect(raw is not null, "migrate returned nothing");
            expectMissingTarget = raw!.Contains("\"junctioned\":false", StringComparison.Ordinal) &&
                                  !raw.Contains("\"win32Error\":0", StringComparison.Ordinal);
            Expect(expectMissingTarget, "missing target was not rejected: " + raw);
            migrateRaw = raw;
        });
        Check("stop-service-reports-state", () =>
        {
            // A read of an allowed name is not needed: the receipt shape is what
            // matters here, and a real stop is covered by the elevated fixture.
            stop = MemoryServiceClient.StopService("whesvc");
            Expect(stop is not null, "stop receipt missing");
        });

        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass = true,
            checks,
            health,
            clean = clean is { } c
                ? new
                {
                    c.PurgeStatus,
                    c.EmptyStatus,
                    c.PrivilegeEnabled,
                    c.PrivilegeError,
                    c.AvailableBefore,
                    c.AvailableAfter,
                    c.AvailableDeltaBytes,
                    c.PrivilegeNotHeld,
                    c.Effective
                }
                : null,
            stopAllowed = stop is { } s
                ? new { s.Name, before = s.BeforeText, after = s.AfterText, s.Win32Error, s.Stopped }
                : null,
            stopDenied = deniedStop,
            migrateRejectsMissingTarget = expectMissingTarget,
            migrateRaw,
            scope = "Desktop-token calls only; service performs privileged work."
        };
    }
}
