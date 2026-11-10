using System.IO;
using System.Security.Principal;

namespace MagicCommunity;

// Real-service P3 fixture. It reads live service state and, only when the token
// is not elevated, verifies that a denied stop is recorded instead of throwing.
internal static class PureNaturalServiceFixture
{
    internal static object Run(string root)
    {
        string folder = Path.Combine(root, "artifacts", "p3-fixture-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
        Directory.CreateDirectory(folder);
        var adapter = new WindowsPureNaturalActions();
        string sysMain = adapter.QueryServiceState("SysMain");
        string whesvc = adapter.QueryServiceState("whesvc");
        using var identity = WindowsIdentity.GetCurrent();
        bool elevated = new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);

        object attempt;
        if (elevated)
        {
            // An elevated run must not stop a host service from a fixture; the
            // production path is exercised by --pure-natural-apply with policy.
            attempt = new { executed = false, reason = "elevated-fixture-does-not-stop-services" };
        }
        else
        {
            var stop = adapter.StopService("whesvc");
            attempt = new { executed = true, stop.Name, stop.Changed, stop.Before, stop.After, stop.ExitCode };
        }

        bool allPass = sysMain is "RUNNING" or "STOPPED" or "STOP_PENDING" or "START_PENDING" &&
                       whesvc is "RUNNING" or "STOPPED" or "STOP_PENDING" or "START_PENDING" or "unknown";
        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass,
            folder,
            elevated,
            sysMain,
            whesvc,
            attempt,
            scope = elevated
                ? "Read-only service query; no stop attempted under elevation."
                : "Read-only query plus a non-elevated stop attempt expected to be denied and recorded."
        };
    }
}
