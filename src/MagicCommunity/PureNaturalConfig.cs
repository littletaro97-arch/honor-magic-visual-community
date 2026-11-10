using System.IO;
using System.Linq;
using System.Text.Json;

namespace MagicCommunity;

// P1/P2/P3/P4 policy. Automatic controls stay opt-in; P3/P4 execute only when
// the caller also selects mode=apply, so a loaded policy never mutates the host.
internal sealed class PureNaturalPolicy
{
    public bool SystemMemoryClean { get; set; } = false;
    public bool OwnedProcessFreeze { get; set; } = false;
    public bool ServiceControls { get; set; } = false;
    public bool DiskMigration { get; set; } = false;
    public string Mode { get; set; } = "dry-run";
    public int SystemCleanIntervalMs { get; set; } = 1_800_000;
    public int OwnedFreezeIntervalMs { get; set; } = 600_000;
    public int OwnedFreezeIdleMs { get; set; } = 120_000;
    public int[] FreezePids { get; set; } = [];
    public string[] ServiceNames { get; set; } = ["SysMain", "whesvc"];
    public string[] MigrationCandidates { get; set; } = ["Desktop", "Downloads", "Documents"];
    public string? MigrationTargetRoot { get; set; }
    public int FreezeIdleMs { get; set; } = 120_000;
    public long FreezeWorkingSetBytes { get; set; } = 96L * 1024 * 1024;
}

internal static class PureNaturalConfig
{
    internal const string FileName = "pure-natural.json";
    internal const int MaxBytes = 16 * 1024;
    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true
    };
    private static readonly string[] Allowed =
    [
        "systemMemoryClean", "ownedProcessFreeze", "serviceControls", "diskMigration", "mode",
        "systemCleanIntervalMs", "ownedFreezeIntervalMs", "ownedFreezeIdleMs", "freezePids",
        "serviceNames", "migrationCandidates", "migrationTargetRoot", "freezeIdleMs", "freezeWorkingSetBytes"
    ];

    internal static string PathOf(string root) => Path.Combine(root, "data", FileName);

    internal static PureNaturalPolicy Load(string root)
    {
        string path = PathOf(root);
        if (!File.Exists(path)) return new PureNaturalPolicy();
        var info = new FileInfo(path);
        if (info.Length > MaxBytes) throw new InvalidDataException("Pure natural policy exceeds bound.");
        string text = File.ReadAllText(path);
        using var document = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = 16 });
        foreach (var property in document.RootElement.EnumerateObject())
            if (!Allowed.Contains(property.Name)) throw new InvalidDataException("Unknown pure natural policy key: " + property.Name);
        var policy = JsonSerializer.Deserialize<PureNaturalPolicy>(text, Json) ?? throw new InvalidDataException("Empty pure natural policy.");
        Validate(policy);
        return policy;
    }

    internal static void Save(string root, PureNaturalPolicy policy)
    {
        Validate(policy);
        string path = PathOf(root);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        StateStore.AtomicWrite(path, JsonSerializer.Serialize(policy, Json), null);
    }

    internal static void Validate(PureNaturalPolicy policy)
    {
        if (policy.Mode is not ("dry-run" or "apply")) throw new InvalidDataException("Invalid pure natural mode.");
        if (policy.FreezeIdleMs is < 1000 or > 3_600_000) throw new InvalidDataException("Freeze idle bound exceeded.");
        if (policy.FreezeWorkingSetBytes is < 16L * 1024 * 1024 or > 1024L * 1024 * 1024)
            throw new InvalidDataException("Freeze working-set bound exceeded.");
        if (policy.SystemCleanIntervalMs is < 60_000 or > 24 * 60 * 60 * 1000)
            throw new InvalidDataException("System clean interval bound exceeded.");
        if (policy.OwnedFreezeIntervalMs is < 60_000 or > 24 * 60 * 60 * 1000)
            throw new InvalidDataException("Owned freeze interval bound exceeded.");
        if (policy.OwnedFreezeIdleMs is < 1_000 or > 24 * 60 * 60 * 1000)
            throw new InvalidDataException("Owned freeze idle bound exceeded.");
        if (policy.FreezePids is null || policy.FreezePids.Length > 16 || policy.FreezePids.Any(pid => pid <= 0))
            throw new InvalidDataException("Invalid freeze pid list.");
        if (policy.MigrationTargetRoot is { Length: > 0 } target &&
            (!Path.IsPathFullyQualified(target) || target.Length > 260 || target.Contains("..")))
            throw new InvalidDataException("Invalid migration target root.");
        ValidateNames(policy.ServiceNames, 16, 64, "service");
        ValidateNames(policy.MigrationCandidates, 16, 64, "migration candidate");
    }

    private static void ValidateNames(string[] names, int count, int length, string label)
    {
        if (names is null || names.Length == 0 || names.Length > count) throw new InvalidDataException("Invalid " + label + " count.");
        foreach (string name in names)
            if (string.IsNullOrWhiteSpace(name) || name.Length > length || name.Contains('\\') || name.Contains('/') || name.Contains(".."))
                throw new InvalidDataException("Invalid " + label + " name.");
    }

    // Planning is pure data: it reports what apply would do without doing it.
    internal static object Plan(string root, PureNaturalPolicy policy)
    {
        Validate(policy);
        bool execute = policy.Mode == "apply";
        return new
        {
            protocol = "Magic.PureNatural.Policy/1",
            root,
            mode = policy.Mode,
            execute,
            p1 = new
            {
                enabled = policy.SystemMemoryClean,
                automaticIntervalMs = policy.SystemCleanIntervalMs,
                operation = "memory-clean",
                commands = new[] { 4, 2 },
                informationClass = 80
            },
            p2 = new
            {
                enabled = policy.OwnedProcessFreeze,
                automaticIntervalMs = policy.OwnedFreezeIntervalMs,
                policy.OwnedFreezeIdleMs,
                explicitPids = policy.FreezePids,
                policy.FreezeWorkingSetBytes,
                operation = "process-freeze",
                identity = "owned-child-pid+image+created"
            },
            p3 = new
            {
                enabled = policy.ServiceControls,
                operation = "service-control",
                services = policy.ServiceNames.Select(name => new { name, desiredState = "stopped", action = execute ? "apply" : "plan-only" }).ToArray()
            },
            p4 = new
            {
                enabled = policy.DiskMigration,
                operation = "disk-migration",
                targetRoot = policy.MigrationTargetRoot,
                candidates = policy.MigrationCandidates.Select(name => new
                {
                    name,
                    steps = new[] { "scan", "select-target", "move", "junction", "icon", "rollback-receipt" },
                    action = execute ? "apply" : "plan-only"
                }).ToArray()
            },
            scope = "Configuration and plan only; no service, driver, third-party process or user file is changed by Plan()."
        };
    }
}
