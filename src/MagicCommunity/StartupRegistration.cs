using System.Diagnostics;
using System.IO;
namespace MagicCommunity;
// Windows owns the MSIX StartupTask state, so it is never mirrored into
// ui-preferences.json: a cached copy would lie as soon as the user switches the
// task off in Task Manager. MagicNativeEntry.exe is the only component carrying
// the StartupTask WinRT projection (the installer drives the same entry point).
// Every call leaves a line in the package temp log so a failed bridge can be
// diagnosed without guessing.
internal static class StartupRegistration
{
    internal enum Registration{Unknown,Enabled,EnabledByPolicy,Disabled,DisabledByUser,DisabledByPolicy}
    private static string Stub=>Path.Combine(AppContext.BaseDirectory,"MagicNativeEntry.exe");
    internal static bool IsOn(Registration state)=>state is Registration.Enabled or Registration.EnabledByPolicy;
    // DisabledByPolicy and Unknown have no writable path, so the switch stays inert.
    internal static bool Toggleable(Registration state)=>state is Registration.Enabled or Registration.EnabledByPolicy or Registration.Disabled or Registration.DisabledByUser;
    internal static string Describe(Registration state)=>state switch
    {
        Registration.Enabled=>"已开启",
        Registration.EnabledByPolicy=>"已由组织策略开启",
        Registration.Disabled=>"已关闭",
        Registration.DisabledByUser=>"已被 Windows 关闭",
        Registration.DisabledByPolicy=>"已由组织策略禁用",
        _=>"状态未知"
    };
    internal static (Registration State,string Detail) Query()=>Run("state");
    internal static (Registration State,string Detail) Apply(bool enabled)=>Run(enabled?"enable":"disable");
    private static (Registration State,string Detail) Run(string command)
    {
        string stub=Stub;
        if(!File.Exists(stub))
        {
            const string missing="未找到 MagicNativeEntry.exe，当前可能是不带包身份的开发版。";
            Trace(command,-1,missing,"");
            return(Registration.Unknown,missing);
        }
        try
        {
            var start=new ProcessStartInfo(stub){UseShellExecute=false,RedirectStandardOutput=true,CreateNoWindow=true};
            start.ArgumentList.Add("--configure-startup");start.ArgumentList.Add(command);
            using var process=Process.Start(start);
            if(process is null){const string failed="无法启动开机启动配置程序。";Trace(command,-1,failed,"");return(Registration.Unknown,failed);}
            string output=process.StandardOutput.ReadToEnd();
            if(!process.WaitForExit(15000))
            {
                try{process.Kill(true);}catch(Exception e) when(e is InvalidOperationException or NotSupportedException or System.ComponentModel.Win32Exception){}
                const string timeout="开机启动配置超时。";Trace(command,-2,timeout,output);return(Registration.Unknown,timeout);
            }
            // Strip NUL padding defensively: a wide-character writer would otherwise
            // look like an unknown state instead of a parse failure.
            string trimmed=output.Replace("\0",string.Empty).Trim();var state=Parse(trimmed);
            // The native stub returns 0 whenever it could report a state, so a
            // non-zero code is a real refusal, not a readable "disabled".
            int exitCode=process.ExitCode;
            string detail=state switch
            {
                Registration.Unknown when exitCode==5=>"Windows 拒绝了该操作，请在「任务管理器 › 启动应用」中手动开启",
                Registration.Unknown when exitCode==3=>"安装版入口拒绝了请求（退出码 3）",
                Registration.Unknown when exitCode!=0=>$"退出码 {exitCode}",
                Registration.Unknown=>"未识别的返回内容",
                _=>string.Empty
            };
            Trace(command,exitCode,detail,trimmed);
            return(state,detail);
        }
        catch(Exception e) when(e is IOException or InvalidOperationException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            Trace(command,-3,e.Message,string.Empty);
            return(Registration.Unknown,e.Message);
        }
    }
    private static void Trace(string command,int exitCode,string detail,string output)
    {
        try
        {
            string path=Path.Combine(Path.GetTempPath(),"magic-startup-bridge.log");
            string line=$"{DateTime.Now:O}\t{command}\texit={exitCode}\tdetail={detail}\tstdout={output}";
            // The settings page can be opened many times; keep the file bounded.
            if(File.Exists(path)&&new FileInfo(path).Length>65536)File.Delete(path);
            File.AppendAllText(path,line+Environment.NewLine);
        }
        catch(Exception e) when(e is IOException or UnauthorizedAccessException or NotSupportedException){}
    }
    private static Registration Parse(string output)=>output switch
    {
        "enabled"=>Registration.Enabled,
        "enabled-by-policy"=>Registration.EnabledByPolicy,
        "disabled"=>Registration.Disabled,
        "disabled-by-user"=>Registration.DisabledByUser,
        "disabled-by-policy"=>Registration.DisabledByPolicy,
        _=>Registration.Unknown
    };
}
