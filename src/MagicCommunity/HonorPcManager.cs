using System.Diagnostics;
using System.Globalization;
using System.IO;
using Microsoft.Win32;

namespace MagicCommunity;

// On-demand entry points only. Never trim other processes or automate deletion.
public static class HonorPcManager
{
    public static bool? SignedIn
    {
        get
        {
            try
            {
                using var key=Registry.CurrentUser.OpenSubKey(@"Software\HONOR\HonorID");
                return Convert.ToString(key?.GetValue("UserLoginFlag"),CultureInfo.InvariantCulture) switch {"1"=>true,"0"=>false,_=>null};
            }
            catch(Exception error) when(error is IOException or UnauthorizedAccessException or System.Security.SecurityException){return null;}
        }
    }
    public static bool ServicesVisible => SignedIn == true;
    public static void RequireSignedIn() { if (!ServicesVisible) throw new InvalidOperationException("荣耀服务当前不可用。"); }
    public static string? Find()
    {
        foreach(var hive in new[]{Registry.CurrentUser,Registry.LocalMachine})
        {
            try
            {
                using var key=hive.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\App Paths\PCManager.exe");
                if(key?.GetValue(null) is string registered && Valid(registered.Trim('"')) is {} path)return path;
            }
            catch(Exception error) when(error is IOException or UnauthorizedAccessException or System.Security.SecurityException){}
        }
        return Valid(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"HONOR","PCManager","PCManager.exe"));
    }
    private static string? Valid(string path)=>Path.IsPathFullyQualified(path)&&Path.GetFileName(path).Equals("PCManager.exe",StringComparison.OrdinalIgnoreCase)&&File.Exists(path)?path:null;
    public static void OpenAccount()=>Open("/account");
    public static void OpenCleanup()=>Open("/doctor/computerClean");
    private static void Open(string page)
    {
        RequireSignedIn();
        string program=Find()??throw new InvalidOperationException("未找到荣耀电脑管家，请先安装或修复电脑管家。");
        var start=new ProcessStartInfo(program){UseShellExecute=false,WorkingDirectory=Path.GetDirectoryName(program)!};
        // The separator is the literal OEM string, not whitespace or an HTML entity to decode.
        start.ArgumentList.Add("--openmodule=para&nbsp;"+page);
        using var process=Process.Start(start)??throw new InvalidOperationException("电脑管家未能启动，请手动打开。");
    }
}
