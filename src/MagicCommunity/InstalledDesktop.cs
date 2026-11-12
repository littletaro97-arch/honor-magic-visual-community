using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MagicCommunity;

// Authority comes from an installed MSIX or the protected Win32 installation.
// A user-writable marker cannot opt a development checkout into desktop loading.
internal static class InstalledDesktop
{
    internal const string Protocol = "Magic.Install/1";
    private static string? currentRoot, family, installId;
    internal static string? Root => currentRoot;
    internal static bool IsCurrent(string root) => currentRoot is not null &&
        string.Equals(Path.GetFullPath(root).TrimEnd('\\'), currentRoot, StringComparison.OrdinalIgnoreCase);
    internal static string RegistrationPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MagicCommunity", "Deployment", "installation.json");
    internal static string? Resolve()
    {
        bool unpackaged=Win32Installation.IsUnpackaged;
        if(unpackaged)Win32Installation.ValidateCode();
        string code = unpackaged?Win32Installation.CodeRoot:PackageText(GetCurrentPackagePath);
        if (!string.Equals(Path.GetFullPath(code).TrimEnd('\\'), AppContext.BaseDirectory.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("安装程序的包身份与入口不一致。");
        family = unpackaged?Win32Installation.Family:PackageText(GetCurrentPackageFamilyName);
        if (!unpackaged&&!family.StartsWith("MagicCommunity.Native_", StringComparison.Ordinal)) throw new InvalidDataException("安装身份不匹配。");
        if (!File.Exists(RegistrationPath)) return null;
        var record = Read(RegistrationPath);
        // The first Win32 launch explicitly offers migration of the old profile.
        if(unpackaged&&record.GetProperty("packageFamily").GetString()!=family&&record.GetProperty("ownerSid").GetString()==WindowsIdentity.GetCurrent().User!.Value)return null;
        if (record.GetProperty("protocol").GetString() != Protocol || record.GetProperty("packageFamily").GetString() != family ||
            record.GetProperty("ownerSid").GetString() != WindowsIdentity.GetCurrent().User!.Value)
            throw new InvalidDataException("安装记录不属于当前用户或应用。");
        if (record.GetProperty("state").GetString() != "installed") return null;
        string root = ValidatePath(record.GetProperty("dataRoot").GetString()!);
        installId = record.GetProperty("installId").GetString();
        if (!Guid.TryParseExact(installId, "N", out _)) throw new InvalidDataException("安装记录标识无效。");
        if(!HasProfile(record,WindowsIdentity.GetCurrent().User!.Value))return null;
        InstalledProfileDirectories.PrepareBackendDirectory(root);
        currentRoot = root;
        return root;
    }
    internal static bool HasProfile(JsonElement record,string sid)
    {
        string root=ValidatePath(record.GetProperty("dataRoot").GetString()!);
        if(record.GetProperty("protocol").GetString()!=Protocol||record.GetProperty("ownerSid").GetString()!=sid||!Guid.TryParseExact(record.GetProperty("installId").GetString(),"N",out _))
            throw new InvalidDataException("安装记录不属于当前账户或标识无效。");
        // A removed data marker is recoverable, but an offline disk is not an
        // empty profile. Never silently replace it with a new default profile.
        if(!Directory.Exists(Path.GetPathRoot(root)))throw new InvalidDataException("数据磁盘当前不可用："+Path.GetPathRoot(root)+"。请连接原磁盘后再启动。");
        JsonElement marker;
        try{marker=Read(Path.Combine(root,".magic-data.json"));}
        catch(Exception error)when(error is FileNotFoundException or DirectoryNotFoundException){return false;}
        if(marker.GetProperty("protocol").GetString()!=Protocol||marker.GetProperty("installId").GetString()!=record.GetProperty("installId").GetString()||marker.GetProperty("ownerSid").GetString()!=sid||
            !string.Equals(Path.GetFullPath(marker.GetProperty("dataRoot").GetString()!).TrimEnd('\\'),root,StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("数据目录归属不一致；请保留文件并检查安装记录。");
        return true;
    }
    internal static string ValidatePath(string value)
    {
        if (!Path.IsPathFullyQualified(value) || value.StartsWith(@"\\") || value.Contains('"')) throw new InvalidDataException("请选择本机磁盘上的完整路径。");
        string path = Path.GetFullPath(value).TrimEnd('\\');
        if (path.Length < 4 || path.Length > 200 || path[1] != ':' || path[2] != '\\' ||
            path[3..].Contains(':') || value.Split('\\').Any(part => part.EndsWith('.') || part.EndsWith(' ')))
            throw new InvalidDataException("数据路径无效，不能使用磁盘根目录、末尾空格或句点。");
        foreach (string part in value[3..].TrimEnd('\\').Split('\\'))
            if (string.IsNullOrEmpty(part) || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                System.Text.RegularExpressions.Regex.IsMatch(part, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$", System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                throw new InvalidDataException("路径包含 Windows 不支持的文件名。");
        string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string defaultRoot = Path.Combine(local, "MagicCommunity", "UserData");
        if (path.StartsWith(local.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase) && !path.Equals(defaultRoot, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("AppData 内仅支持默认独立数据目录。");
        foreach (var special in new[] { Environment.SpecialFolder.UserProfile, Environment.SpecialFolder.DesktopDirectory,
            Environment.SpecialFolder.MyDocuments, Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolder.ApplicationData })
            if (path.Equals(Environment.GetFolderPath(special), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("数据须位于专用子目录。");
        foreach (var protectedPath in new[] { Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Packages"),Win32Installation.CodeRoot })
            if (!string.IsNullOrEmpty(protectedPath) && (path.Equals(protectedPath, StringComparison.OrdinalIgnoreCase) || path.StartsWith(protectedPath.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException("不能使用系统或应用包目录存放用户数据。");
        for (string? ancestor = path; ancestor is not null; ancestor = Path.GetDirectoryName(ancestor))
            if (Directory.Exists(ancestor) && ((File.GetAttributes(ancestor) & FileAttributes.ReparsePoint) != 0 ||
                File.Exists(Path.Combine(ancestor, ".git")) || Directory.Exists(Path.Combine(ancestor, ".git"))))
                throw new InvalidDataException("数据目录不能位于链接或源码工作树内。");
        return path;
    }
    internal static NativeSystemDeployment Deployment(string root)
    {
        if (!IsCurrent(root)) throw new InvalidDataException("安装入口未确认。");
        var manifest = Read(Path.Combine(AppContext.BaseDirectory, "installed-desktop.json"), 65536);
        if (manifest.GetProperty("protocol").GetString() != Protocol) throw new InvalidDataException("安装包缺少桌面清单。");
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in manifest.GetProperty("files").EnumerateObject())
        {
            if (entry.Name != Path.GetFileName(entry.Name) || entry.Name.Contains(':') || files.Count >= 128) throw new InvalidDataException("安装清单文件名无效。");
            string path = Path.Combine(AppContext.BaseDirectory, entry.Name);
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("安装文件不能是链接。");
            using var stream = File.OpenRead(path);
            if (!string.Equals(Convert.ToHexString(SHA256.HashData(stream)), entry.Value.GetString(), StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("安装文件校验失败：" + entry.Name);
            files.Add(entry.Name, entry.Value.GetString()!);
        }
        foreach (string name in new[] { "MagicWindowAction.exe", "MagicSystemNative.exe", "MagicTrayCapture.dll", "MagicTaskbarGuard.exe", "MagicDesktopComposition.exe", "MagicCommunity.dll" })
            if (!files.ContainsKey(name)) throw new InvalidDataException("安装文件不完整：" + name);
        return new NativeSystemDeployment { Root=root, CodeDirectory=AppContext.BaseDirectory, DataDirectory=Path.Combine(root,"data","native-system"),
            PackageFamily=family!, Nonce=installId!, Files=files, Continuous=true, Installed=true };
    }
    internal static JsonObject Authorization(string root) => IsCurrent(root)
        ? new JsonObject { ["protocol"]=NativeSystemClient.Protocol, ["nonce"]=installId, ["userSid"]=WindowsIdentity.GetCurrent().User!.Value, ["acceptanceMode"]="continuous", ["durationSeconds"]=null, ["expiresUtc"]=null }
        : throw new InvalidDataException("数据目录未经安装入口验证。");
    internal static JsonElement Read(string path, int max=16384)
    {
        var file=new FileInfo(path);if(file.Length>max||(file.Attributes&FileAttributes.ReparsePoint)!=0)throw new InvalidDataException("安装记录过大或含链接。");
        using var json=JsonDocument.Parse(File.ReadAllBytes(path),new JsonDocumentOptions{MaxDepth=16});return json.RootElement.Clone();
    }
    private delegate int Query(ref uint length, StringBuilder? text);
    private static string PackageText(Query query)
    {
        uint length=0;if(query(ref length,null)!=122||length>32768||length==0)throw new InvalidDataException("此入口需要已安装的 MSIX 身份。");
        var text=new StringBuilder((int)length);int result=query(ref length,text);if(result!=0)throw new System.ComponentModel.Win32Exception(result);return text.ToString();
    }
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern int GetCurrentPackagePath(ref uint length,StringBuilder? value);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern int GetCurrentPackageFamilyName(ref uint length,StringBuilder? value);
}
