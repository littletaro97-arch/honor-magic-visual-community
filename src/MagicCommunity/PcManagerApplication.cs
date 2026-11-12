using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Microsoft.Win32;

namespace MagicCommunity;
// Discovery is performed once for a UI session; opening revalidates the file.
// No resident process, service, registry polling or account requirement is added.
internal sealed record PcManagerApplication(string Path, string Brand)
{
    internal static PcManagerApplication? Validate(string? candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate)) return null;
        try
        {
            var path=System.IO.Path.GetFullPath(candidate.Trim('"'));
            if (!System.IO.Path.IsPathFullyQualified(candidate.Trim('"')) || !File.Exists(path)) return null;
            string file=System.IO.Path.GetFileName(path);
            if (!file.Equals("PCManager.exe",StringComparison.OrdinalIgnoreCase) && !file.Equals("HwPCManager.exe",StringComparison.OrdinalIgnoreCase)) return null;
            var info=FileVersionInfo.GetVersionInfo(path);
            string company=info.CompanyName??"";
            string brand=company.Contains("HONOR",StringComparison.OrdinalIgnoreCase)?"荣耀":company.Contains("HUAWEI",StringComparison.OrdinalIgnoreCase)?"华为":"";
            return brand.Length>0 ? new(path,brand) : null;
        }
        catch(Exception e) when(e is IOException or UnauthorizedAccessException or ArgumentException or System.Security.SecurityException){return null;}
    }
    internal static PcManagerApplication? Find()
    {
        foreach(var folder in new[]{Environment.SpecialFolder.ProgramFiles,Environment.SpecialFolder.ProgramFilesX86})
            foreach(string brand in new[]{"HONOR","Huawei"})
                foreach(string sub in new[]{"","mainframe"})
                    foreach(string exe in new[]{"PCManager.exe","HwPCManager.exe"})
                        if(Validate(System.IO.Path.Combine(Environment.GetFolderPath(folder),brand,"PCManager",sub,exe)) is {} known)return known;
        foreach(var hive in new[]{RegistryHive.CurrentUser,RegistryHive.LocalMachine})
            foreach(var view in new[]{RegistryView.Registry64,RegistryView.Registry32})
                foreach(string exe in new[]{"PCManager.exe","HwPCManager.exe"})
                    try
                    {
                        using var baseKey=RegistryKey.OpenBaseKey(hive,view);
                        using var key=baseKey.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\App Paths\"+exe);
                        if(Validate(key?.GetValue(null) as string) is {} registered)return registered;
                    }
                    catch(Exception e) when(e is IOException or UnauthorizedAccessException or System.Security.SecurityException){}
        return null;
    }
    internal IEnumerable<string> AssistantCandidates()
    {
        string directory=System.IO.Path.GetDirectoryName(Path)!;
        yield return System.IO.Path.Combine(directory,"MessageCenterUI.exe");
        if(System.IO.Path.GetFileName(directory).Equals("mainframe",StringComparison.OrdinalIgnoreCase))
            yield return System.IO.Path.Combine(System.IO.Path.GetDirectoryName(directory)!,"MessageCenterUI.exe");
    }
    internal ProcessStartInfo AssistantStartInfo()
    {
        var current=Validate(Path);
        if(current is null || current.Brand!=Brand)throw new InvalidOperationException("电脑管家安装位置已变化，请重新进入社区版后再试。");
        foreach(string candidate in AssistantCandidates())
        {
            if(!File.Exists(candidate)||(File.GetAttributes(candidate)&FileAttributes.ReparsePoint)!=0)continue;
            try
            {
                // The companion has generic version metadata. Its embedded
                // publisher identifies the vendor; it runs at the current user
                // level, using the vendor's own single-instance /r entry point.
                using var publisher=X509Certificate.CreateFromSignedFile(candidate);
                string name=Brand=="荣耀"?"Honor":"Huawei";
                if(!publisher.Subject.Contains(name,StringComparison.OrdinalIgnoreCase))continue;
                return new ProcessStartInfo(candidate){Arguments="/r",UseShellExecute=true,WorkingDirectory=System.IO.Path.GetDirectoryName(candidate)!};
            }
            catch(Exception e)when(e is IOException or UnauthorizedAccessException or CryptographicException){}
        }
        throw new InvalidOperationException("未找到电脑管家的负一屏组件，请检查管家安装。可先使用 F11 打开负一屏。");
    }
    internal void Open()
    {
        using var process=Process.Start(AssistantStartInfo());
    }
}
