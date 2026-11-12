using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace MagicCommunity;

internal sealed record StartApplication(string Id, string Name, string Target, string Aumid,
    string? TileArguments = null, bool Available = true, string Location = "");

internal sealed record WindowsStartCatalog(StartApplication[] All, StartApplication[] SearchOnly,
    StartApplication[]? Pins, string? PinReadError)
{
    internal static string Id(string identity) => "app:" +
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity.ToUpperInvariant())))[..32];

    // The opaque store's timestamp is only an invalidation hint. Unknown
    // storage layouts still refresh on the next open after the cache expires.
    internal static long LayoutStamp()
    {
        var state = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            @"Packages\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\LocalState");
        try { return Directory.EnumerateFiles(state, "start*.bin").Select(File.GetLastWriteTimeUtc)
            .Select(time => time.Ticks).DefaultIfEmpty().Max(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return 0; }
    }
    // Export runs only when the Start surface asks for a fresh catalog. Windows
    // PowerShell hosts the inbox StartLayout module even in a packaged process.
    private static async Task<JsonElement> ReadPinLayout(CancellationToken token)
    {
        string path = Path.Combine(Path.GetTempPath(), "MagicStart-" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"))
            { UseShellExecute = false, CreateNoWindow = true };
            foreach (string arg in new[] { "-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
                "$ErrorActionPreference='Stop'; StartLayout\\Export-StartLayout -Path $env:MAGIC_START_EXPORT" })
                info.ArgumentList.Add(arg);
            info.Environment["MAGIC_START_EXPORT"] = path;
            using var child = Process.Start(info) ?? throw new IOException("Windows 已固定列表导出未启动。");
            try { await child.WaitForExitAsync(token).ConfigureAwait(false); }
            catch (OperationCanceledException)
            {
                if (!child.HasExited) child.Kill();
                await child.WaitForExitAsync().ConfigureAwait(false);
                throw;
            }
            if (child.ExitCode != 0 || !File.Exists(path) || new FileInfo(path).Length > 2 * 1024 * 1024)
                throw new InvalidDataException("Windows 已固定列表导出失败。");
            // Some versions of the system module write a Unicode BOM.
            using var json = JsonDocument.Parse(File.ReadAllText(path));
            var list = json.RootElement.GetProperty("pinnedList");
            if (list.ValueKind != JsonValueKind.Array || list.GetArrayLength() > 4096)
                throw new InvalidDataException("Windows 已固定列表格式无效。");
            return list.Clone();
        }
        finally { if (File.Exists(path)) File.Delete(path); }
    }

    internal static async Task<WindowsStartCatalog> Read(CancellationToken cancellation)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromSeconds(12));
        JsonElement pins = default;
        string? error = null;
        try { pins = await Task.Run(() => ReadPinLayout(deadline.Token), deadline.Token).ConfigureAwait(false); }
        catch (Exception e) when (e is not OutOfMemoryException && !cancellation.IsCancellationRequested)
        { error = e.GetType().Name; }
        var done = new TaskCompletionSource<WindowsStartCatalog>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var shellLimit = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        shellLimit.CancelAfter(TimeSpan.FromSeconds(10));
        var token = shellLimit.Token;
        var worker = new Thread(() => {
            try { done.TrySetResult(ReadShell(pins, error, token)); }
            catch (Exception e) when (e is not OutOfMemoryException) { done.TrySetException(e); }
        }) { Name = "Magic Windows Start catalog", IsBackground = true };
        worker.SetApartmentState(ApartmentState.STA);
        worker.Start();
        return await done.Task.WaitAsync(token).ConfigureAwait(false);
    }

    private static void Release(object? value)
    {
        if (value is not null && Marshal.IsComObject(value)) Marshal.ReleaseComObject(value);
    }
    private static string Field(JsonElement obj, string name) =>
        obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var field) && field.ValueKind == JsonValueKind.String
            ? field.GetString() ?? "" : "";
    private static string Property(object item, string name)
    {
        try { return (string?)((dynamic)item).ExtendedProperty(name) ?? ""; }
        catch (COMException) { return ""; }
    }
    private static bool Local(string path) => path.Length > 0 && Path.IsPathFullyQualified(path) && !path.StartsWith(@"\\", StringComparison.Ordinal);
    private sealed record Link(string Id, string Target, string Arguments);
    private static Link ReadLink(object shell, string path)
    {
        if (!path.EndsWith(".lnk", StringComparison.OrdinalIgnoreCase)) return new("", path, "");
        object? folder = null, file = null;
        try
        {
            folder = ((dynamic)shell).NameSpace(Path.GetDirectoryName(path));
            if (folder is not null) file = ((dynamic)folder).ParseName(Path.GetFileName(path));
            return file is null ? new("", path, "") : new(Property(file, "System.AppUserModel.ID"),
                Property(file, "System.Link.TargetParsingPath"), Property(file, "System.Link.Arguments"));
        }
        catch (COMException) { return new("", path, ""); }
        finally { Release(file); Release(folder); }
    }

    private static WindowsStartCatalog ReadShell(JsonElement pinList, string? pinError, CancellationToken token)
    {
        var byId = new Dictionary<string, StartApplication>(StringComparer.OrdinalIgnoreCase);
        var paths = new Dictionary<string, StartApplication>(StringComparer.OrdinalIgnoreCase);
        // Only match an executable and its *exact* arguments if the match is unique.
        var launches = new Dictionary<(string,string), StartApplication?>();
        var search = new List<StartApplication>();
        var pins = new List<StartApplication>();
        object? shell = null, folder = null, items = null;
        try
        {
            shell = Activator.CreateInstance(Type.GetTypeFromProgID("Shell.Application")!)!;
            folder = ((dynamic)shell).NameSpace("shell:AppsFolder") ?? throw new IOException("Windows 应用目录暂不可用。");
            items = ((dynamic)folder).Items();
            int count = Math.Min((int)((dynamic)items).Count, 8192);
            for (int n = 0; n < count; n++)
            {
                token.ThrowIfCancellationRequested();
                object? item = null;
                try
                {
                    item = ((dynamic)items).Item(n);
                    if (item is null) continue;
                    string path = (string)((dynamic)item).Path, name = (string)((dynamic)item).Name;
                    string aumid = Property(item, "System.AppUserModel.ID");
                    bool local = Local(path);
                    if (aumid.Length == 0 && !local && NativeJumpLists.ValidId(path) && !path.StartsWith("::")) aumid = path;
                    string key = aumid.Length == 0 ? path : aumid;
                    if (key.Length == 0 || name.Length is 0 or > 512) continue;
                    string target = Property(item, "System.Link.TargetParsingPath");
                    // AppsFolder paths are frequently AUMIDs, including ordinary
                    // desktop apps. Keep their launch identity separate from the
                    // registered local file used by Open file location.
                    string location = local ? path : Local(target) ? target : "";
                    var app = new StartApplication(Id(key), name, local ? path : "", aumid, Location: location);
                    byId.TryAdd(key, app);
                    app = byId[key];
                    if (local) paths[path] = app;
                    if (target.Length == 0 && local && !path.EndsWith(".lnk", StringComparison.OrdinalIgnoreCase)) target = path;
                    if (Local(target))
                    {
                        var launch = (target.ToUpperInvariant(), Property(item, "System.Link.Arguments"));
                        if (!launches.TryAdd(launch, app) && launches[launch]?.Id != app.Id) launches[launch] = null;
                    }
                }
                catch (COMException) { /* An entry can disappear while Windows enumerates it. */ }
                finally { Release(item); }
            }
            var all = byId.Values.OrderBy(x => x.Name, StringComparer.CurrentCultureIgnoreCase).ThenBy(x => x.Id).ToArray();
            StartApplication ResolvePath(string path)
            {
                if (paths.TryGetValue(path, out var exact)) return exact;
                var link = ReadLink(shell, path);
                if (byId.TryGetValue(link.Id, out var known)) return paths[path] = known with { Location = path };
                if (Local(link.Target) && launches.TryGetValue((link.Target.ToUpperInvariant(), link.Arguments), out var match) && match is not null)
                    return paths[path] = match with { Location = path };
                var fallback = new StartApplication(Id(path), Path.GetFileNameWithoutExtension(path), path, link.Id, Available: File.Exists(path), Location: path);
                paths[path] = fallback;
                if (Local(link.Target)) launches.TryAdd((link.Target.ToUpperInvariant(), link.Arguments), fallback);
                return fallback;
            }
            if (pinList.ValueKind == JsonValueKind.Array) foreach (var pin in pinList.EnumerateArray())
            {
                token.ThrowIfCancellationRequested();
                string link = Environment.ExpandEnvironmentVariables(Field(pin, "desktopAppLink"));
                string id = Field(pin, "packagedAppId");
                if (id.Length == 0) id = Field(pin, "desktopAppId");
                if (pin.ValueKind == JsonValueKind.Object && pin.TryGetProperty("secondaryTile", out var tile) &&
                    NativeJumpLists.ValidId(Field(tile, "packagedAppId")) && Field(tile, "arguments").Length <= 8192)
                {
                    string appId = Field(tile, "packagedAppId"), args = Field(tile, "arguments"), name = Field(tile, "displayName");
                    pins.Add(new(Id(pin.GetRawText()), name.Length == 0 ? byId.GetValueOrDefault(appId)?.Name ?? appId : name,
                        "", appId, args));
                }
                else if (Local(link)) pins.Add(ResolvePath(link));
                else if (byId.TryGetValue(id, out var app)) pins.Add(app);
                else pins.Add(new(Id(pin.GetRawText()), id.Length > 0 ? id : "Windows 固定项", "", "", Available: false));
            }
            // Portable desktop utilities remain searchable, but never add another
            // tile to the unfiltered Windows All apps view.
            var seen = all.Select(a => a.Id).ToHashSet(StringComparer.Ordinal);
            foreach (string path in ApplicationSearchFiles.Read(new[] {
                Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory) }, token))
            {
                token.ThrowIfCancellationRequested();
                var app = ResolvePath(path);
                if (app.Available && seen.Add(app.Id)) search.Add(app);
            }
            return new(all, search.ToArray(), pinError is null ? pins.ToArray() : null, pinError);
        }
        finally { Release(items); Release(folder); Release(shell); }
    }
}
