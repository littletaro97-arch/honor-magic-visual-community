using System.IO;
using System.Runtime.InteropServices;
using System.Xml;
using System.Xml.Linq;

namespace MagicCommunity;

// Resolve only on an explicit launch. A legacy desktop AUMID may still point
// into an old WindowsApps version after an update. Never guess by display name
// or executable basename: a PWA with the same name is a different application.
internal static class RegisteredApplication
{
    internal sealed record Identity(string Aumid, string Executable);
    private static readonly SemaphoreSlim gate = new(1, 1);
    internal static async Task<Identity> ResolveAsync(string aumid, CancellationToken cancellation)
    {
        if (!await gate.WaitAsync(TimeSpan.FromSeconds(5), cancellation))
            throw new IOException("正在核对应用入口，请稍后重试。");
        var done = new TaskCompletionSource<Identity>(TaskCreationOptions.RunContinuationsAsynchronously);
        var worker = new Thread(() =>
        {
            try { done.TrySetResult(Resolve(aumid)); }
            catch (Exception error) when (error is not OutOfMemoryException) { done.TrySetException(error); }
            finally { gate.Release(); }
        }) { Name = "Magic application identity", IsBackground = true };
        worker.SetApartmentState(ApartmentState.STA);
        try { worker.Start(); } catch { gate.Release(); throw; }
        // A slow Shell lookup cannot block a popup or spawn repeated workers.
        return await done.Task.WaitAsync(TimeSpan.FromSeconds(5), cancellation).ConfigureAwait(false);
    }

    internal static string Family(string fullName)
    {
        string[] fields = fullName.Split('_');
        return fields.Length == 5 && Version.TryParse(fields[1], out _) && fields[0].Length > 0 && fields[4].Length > 0
            ? fields[0] + "_" + fields[4] : "";
    }
    internal static bool PackagedTarget(string target, out string family, out string relative)
    {
        family = relative = "";
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "WindowsApps") + "\\";
        if (!Path.IsPathFullyQualified(target) || !target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return false;
        string rest = target[root.Length..]; int separator = rest.IndexOf('\\');
        if (separator < 0) return false;
        family = Family(rest[..separator]); relative = rest[(separator + 1)..];
        return family.Length > 0 && relative.Length > 0 && !Path.IsPathRooted(relative) &&
            !relative.Split('\\', '/').Any(part => part is ".." or ".");
    }
    private static string Property(object item, string key)
    {
        try { return ((dynamic)item).ExtendedProperty(key) as string ?? ""; }
        catch (COMException) { return ""; }
    }
    private static void Release(object? value) { if (value is not null && Marshal.IsComObject(value)) Marshal.ReleaseComObject(value); }
    internal static Identity Resolve(string aumid)
    {
        if (!NativeJumpLists.ValidId(aumid)) throw new InvalidDataException("Application identity differs.");
        object? shell = null, folder = null, original = null, items = null;
        try
        {
            shell = Activator.CreateInstance(Type.GetTypeFromProgID("Shell.Application")!);
            folder = ((dynamic)shell!).NameSpace("shell:AppsFolder");
            original = ((dynamic)folder!).ParseName(aumid);
            string target = original is null ? "" : Property(original, "System.Link.TargetParsingPath");
            if (!PackagedTarget(target, out string family, out string relative)) return new(aumid, target);
            items = ((dynamic)folder!).Items();
            var candidates = new HashSet<Identity>();
            for (int i = 0, count = Math.Min(8192, (int)((dynamic)items).Count); i < count; ++i)
            {
                object? item = null;
                try
                {
                    item = ((dynamic)items).Item(i); if (item is null) continue;
                    if (!family.Equals(Family(Property(item, "System.AppUserModel.PackageFullName")), StringComparison.OrdinalIgnoreCase)) continue;
                    string id = Property(item, "System.AppUserModel.ID"), root = Property(item, "System.AppUserModel.PackageInstallPath");
                    if (!id.StartsWith(family + "!", StringComparison.OrdinalIgnoreCase) || !Path.IsPathFullyQualified(root)) continue;
                    string manifest = Path.Combine(root, "AppxManifest.xml");
                    if (!File.Exists(manifest) || new FileInfo(manifest).Length > 2 * 1024 * 1024) continue;
                    using var reader = XmlReader.Create(manifest, new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = 2 * 1024 * 1024 });
                    var application = XDocument.Load(reader).Descendants().FirstOrDefault(node => node.Name.LocalName == "Application" &&
                        (string?)node.Attribute("Id") == id[(family.Length + 1)..] &&
                        relative.Equals(((string?)node.Attribute("Executable") ?? "").Replace('/', '\\'), StringComparison.OrdinalIgnoreCase));
                    string executable = Path.Combine(root, relative);
                    if (application is not null && File.Exists(executable)) candidates.Add(new(id, executable));
                }
                finally { Release(item); }
            }
            if (candidates.Count == 1) return candidates.Single();
            // A known stale WindowsApps alias must not fall back to launching
            // the old EXE or another app that happens to share its name.
            throw new IOException("此通知的应用入口已失效，未找到唯一的当前安装入口。请从 Windows 开始菜单打开应用。");
        }
        finally { Release(items); Release(original); Release(folder); Release(shell); }
    }
}
