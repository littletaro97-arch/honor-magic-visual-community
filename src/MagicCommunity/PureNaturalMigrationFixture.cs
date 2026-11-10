using System.Diagnostics;
using System.IO;

namespace MagicCommunity;

// Real-filesystem P4 fixture. It runs the production Windows adapter against an
// isolated artifacts directory so the move/junction/rollback path is exercised
// without touching the signed-in profile.
internal static class PureNaturalMigrationFixture
{
    internal static object Run(string root)
    {
        string folder = Path.Combine(root, "artifacts", "p4-fixture-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
        string home = Path.Combine(folder, "home");
        string target = Path.Combine(folder, "target");
        Directory.CreateDirectory(Path.Combine(home, "Desktop"));
        Directory.CreateDirectory(target);
        File.WriteAllText(Path.Combine(home, "Desktop", "keep.txt"), "fixture");

        var adapter = new WindowsPureNaturalActions();
        var policy = new PureNaturalPolicy
        {
            Mode = "apply",
            DiskMigration = true,
            MigrationCandidates = ["Desktop"],
            MigrationTargetRoot = target
        };
        var applied = PureNaturalOperations.ApplyWithHome(folder, policy, adapter, home);
        string junction = Path.Combine(home, "Desktop");
        string moved = Path.Combine(target, "Desktop");
        bool movedExists = Directory.Exists(moved) && File.Exists(Path.Combine(moved, "keep.txt"));
        bool junctionIsLink = Directory.Exists(junction) &&
            (File.GetAttributes(junction) & FileAttributes.ReparsePoint) != 0;
        bool readableThroughJunction = File.Exists(Path.Combine(junction, "keep.txt"));

        var receiptProperty = applied.GetType().GetProperty("receipt");
        string receipt = receiptProperty?.GetValue(applied) as string ?? string.Empty;
        var rolledBack = PureNaturalOperations.Rollback(folder, receipt, adapter);
        bool restored = Directory.Exists(junction) && File.Exists(Path.Combine(junction, "keep.txt")) &&
            (File.GetAttributes(junction) & FileAttributes.ReparsePoint) == 0;
        bool targetCleared = !Directory.Exists(moved);

        bool allPass = movedExists && junctionIsLink && readableThroughJunction && restored && targetCleared;
        return new
        {
            version = AppController.Version,
            at = DateTimeOffset.Now,
            allPass,
            folder,
            movedExists,
            junctionIsLink,
            readableThroughJunction,
            restored,
            targetCleared,
            receipt,
            applied,
            rolledBack,
            scope = "Isolated artifacts fixture; the signed-in user profile, services and registry were not modified."
        };
    }
}
