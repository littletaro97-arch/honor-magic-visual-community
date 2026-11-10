using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace MagicCommunity;

internal readonly record struct ServiceReceipt(string Name, bool Changed, string Before, string After, int ExitCode);

// The seam the P3/P4 executor crosses. Production uses the Windows adapter;
// tests use an in-memory adapter and assert the same call sequence.
internal interface IPureNaturalActions
{
    string QueryServiceState(string name);
    ServiceReceipt StopService(string name);
    bool StartService(string name);
    bool IsReparsePoint(string path);
    void MoveDirectory(string source, string destination);
    void CreateJunction(string junction, string target);
    void RemoveJunction(string junction);
    void RemoveEmptyDirectory(string path);
}

// Windows adapter. Only called from Apply(...) after the caller selected
// mode=apply, and every filesystem path is checked by the executor first.
internal sealed class WindowsPureNaturalActions : IPureNaturalActions
{
    private readonly string systemDirectory = Environment.SystemDirectory;

    public string QueryServiceState(string name)
    {
        var (exit, output) = Run(Path.Combine(systemDirectory, "sc.exe"), ["query", name], 5000);
        var match = System.Text.RegularExpressions.Regex.Match(output, @"(?im)^\s*STATE\s*:\s*\d+\s+([A-Z_]+)");
        return exit == 0 && match.Success ? match.Groups[1].Value : "unknown";
    }

    public ServiceReceipt StopService(string name)
    {
        string before = QueryServiceState(name);
        // Prefer the LocalSystem service so stopping SysMain/whesvc does not
        // require the desktop to hold an administrator token.
        var serviced = MemoryServiceClient.StopService(name);
        if (serviced is { } receipt)
            return new ServiceReceipt(receipt.Name, receipt.Before != receipt.After,
                receipt.BeforeText, receipt.AfterText, receipt.Stopped ? 0 : (int)receipt.Win32Error);
        var (exit, _) = Run(Path.Combine(systemDirectory, "sc.exe"), ["stop", name], 30000);
        string after = QueryServiceState(name);
        return new ServiceReceipt(name, before != after, before, after, exit);
    }

    public bool StartService(string name)
    {
        var (exit, _) = Run(Path.Combine(systemDirectory, "sc.exe"), ["start", name], 30000);
        return exit == 0;
    }

    public bool IsReparsePoint(string path) =>
        Directory.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;

    public void MoveDirectory(string source, string destination)
    {
        Directory.Move(source, destination);
    }

    // Junctions need cmd's mklink; no third-party tool is involved.
    public void CreateJunction(string junction, string target)
    {
        var (exit, output) = Run(Path.Combine(systemDirectory, "cmd.exe"), ["/c", "mklink", "/J", junction, target], 20000);
        if (exit != 0 || (Directory.Exists(junction) && (File.GetAttributes(junction) & FileAttributes.ReparsePoint) == 0))
            throw new IOException("Junction creation failed: " + output.Trim());
    }

    public void RemoveJunction(string junction)
    {
        if (!Directory.Exists(junction)) return;
        if ((File.GetAttributes(junction) & FileAttributes.ReparsePoint) == 0)
            throw new InvalidOperationException("Refusing to remove a directory that is not a junction.");
        Directory.Delete(junction, false);
    }

    public void RemoveEmptyDirectory(string path)
    {
        if (Directory.Exists(path) && !Directory.EnumerateFileSystemEntries(path).Any())
            Directory.Delete(path, false);
    }

    private static (int ExitCode, string Output) Run(string program, string[] arguments, int timeoutMs)
    {
        var start = new ProcessStartInfo(program)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        foreach (string argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException(program + " did not start.");
        string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
        if (!process.WaitForExit(timeoutMs))
        {
            try { process.Kill(entireProcessTree: true); } catch (Exception error) when (error is not OutOfMemoryException) { }
            return (-1, output);
        }
        return (process.ExitCode, output);
    }
}

// P3/P4 executor: one interface, dry-run by default, explicit apply, and a
// rollback receipt written before the first mutation is attempted.
internal static class PureNaturalOperations
{
    internal static object Apply(string root, PureNaturalPolicy policy, IPureNaturalActions? actions = null) =>
        ApplyWithHome(root, policy, actions, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));

    // Home is a parameter so tests migrate an isolated fixture instead of the
    // signed-in user's profile. Production always passes the real profile.
    internal static object ApplyWithHome(string root, PureNaturalPolicy policy, IPureNaturalActions? actions, string home)
    {
        PureNaturalConfig.Validate(policy);
        bool apply = policy.Mode == "apply";
        var services = new List<ServiceReceipt>();
        var migrations = new List<object>();

        if (!apply)
            return new
            {
                mode = policy.Mode,
                applied = false,
                services = policy.ServiceNames.Select(name => new { name, action = "plan-only", desiredState = "stopped" }).ToArray(),
                migrations = policy.MigrationCandidates.Select(name => new { name, action = "plan-only" }).ToArray(),
                reason = "dry-run; select mode=apply to execute",
                scope = "No service state or user file changed."
            };

        var adapter = actions ?? new WindowsPureNaturalActions();
        var rollback = new List<object>();

        if (policy.ServiceControls)
        {
            foreach (string name in policy.ServiceNames)
            {
                try
                {
                    var receipt = adapter.StopService(name);
                    services.Add(receipt);
                    if (receipt.Changed)
                        rollback.Add(new { type = "service", name, restore = "running", previous = receipt.Before, next = receipt.After });
                }
                catch (Exception error) when (error is not OutOfMemoryException)
                {
                    services.Add(new ServiceReceipt(name, false, "error", error.GetType().Name, -1));
                }
            }
        }

        if (policy.DiskMigration && !string.IsNullOrWhiteSpace(policy.MigrationTargetRoot))
        {
            string target = Path.GetFullPath(policy.MigrationTargetRoot!);
            if (!Directory.Exists(target))
                migrations.Add(new { ok = false, reason = "target-root-missing", target });
            else
                foreach (string name in policy.MigrationCandidates)
                {
                    string source = Path.Combine(home, name);
                    string destination = Path.Combine(target, name);
                    try
                    {
                        migrations.Add(Migrate(adapter, name, source, destination, rollback));
                    }
                    catch (Exception error) when (error is not OutOfMemoryException)
                    {
                        migrations.Add(new { ok = false, name, reason = error.GetType().Name, message = error.Message });
                    }
                }
        }

        string receiptPath = Path.Combine(root, "artifacts", "pure-natural-apply-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff") + ".json");
        Directory.CreateDirectory(Path.GetDirectoryName(receiptPath)!);
        StateStore.AtomicWrite(receiptPath,
            JsonSerializer.Serialize(new { at = DateTimeOffset.Now, policy, services, migrations, rollback }), null);

        return new
        {
            mode = policy.Mode,
            applied = true,
            services,
            migrations,
            rollback,
            receipt = receiptPath,
            scope = "Services stopped and user directories moved only for the selected policy; rollback entries recorded."
        };
    }

    private static object Migrate(IPureNaturalActions actions, string name, string source, string destination, List<object> rollback)
    {
        if (!Directory.Exists(source))
            return new { ok = false, name, reason = "source-missing", source };
        if (actions.IsReparsePoint(source))
            return new { ok = false, name, reason = "source-is-reparse-point", source };
        if (Directory.Exists(destination) || File.Exists(destination))
            return new { ok = false, name, reason = "destination-exists", destination };

        actions.MoveDirectory(source, destination);
        try
        {
            actions.CreateJunction(source, destination);
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            // A junction failure must not leave the user directory detached.
            actions.MoveDirectory(destination, source);
            return new { ok = false, name, reason = "junction-failed", message = error.Message, rolledBack = true };
        }
        rollback.Add(new { type = "migration", name, junction = source, target = destination });
        return new { ok = true, name, junction = source, target = destination };
    }

    // Rollback is explicit: remove junction, then move the directory back.
    internal static object Rollback(string root, string receiptPath, IPureNaturalActions? actions = null)
    {
        var adapter = actions ?? new WindowsPureNaturalActions();
        using var document = JsonDocument.Parse(File.ReadAllBytes(Path.GetFullPath(receiptPath)));
        var entries = new List<object>();
        foreach (var item in document.RootElement.GetProperty("rollback").EnumerateArray())
        {
            string type = item.GetProperty("type").GetString() ?? string.Empty;
            if (type == "service")
            {
                string name = item.GetProperty("name").GetString()!;
                entries.Add(new { type, name, restored = adapter.StartService(name) });
            }
            else if (type == "migration")
            {
                string junction = item.GetProperty("junction").GetString()!;
                string target = item.GetProperty("target").GetString()!;
                adapter.RemoveJunction(junction);
                adapter.MoveDirectory(target, junction);
                entries.Add(new { type, junction, target, restored = true });
            }
        }
        return new { receipt = Path.GetFullPath(receiptPath), root, entries, scope = "Junctions removed and directories moved back." };
    }
}
