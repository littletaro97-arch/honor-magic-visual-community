using System.IO;
using System.Security.Principal;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Win32;

namespace MagicCommunity;

// The installed native entry already checks this per-account opt-in before
// starting the desktop. Never add a second Run entry/task or touch other users.
internal static class CommunityStartupSettings
{
    internal sealed record Status(bool Enabled,bool CanChange,string Detail);
    internal static Status ReadCurrent(string root)
    {
        if(!InstalledDesktop.IsCurrent(root))return new(false,false,"请在安装版中设置开机自启。");
        if(!Win32Installation.IsUnpackaged)return new(false,false,"此旧包版的启动项由 Windows 管理，请使用下方入口。");
        try {
            bool enabled=Preference(InstalledDesktop.Read(InstalledDesktop.RegistrationPath),root,WindowsIdentity.GetCurrent().User!.Value);
            if(!HasLogonEntry())return new(enabled,false,"安装版启动入口缺失或已变更，请重新运行安装程序修复。");
            return new(enabled,true,"登录当前 Windows 账户后自动进入社区版，不播放进场动画。Windows 的启动应用设置仍可单独禁用此入口。");
        }
        catch(Exception e)when(e is not OutOfMemoryException){return new(false,false,"暂时无法读取启动设置："+e.Message);}
    }
    internal static void SetCurrent(string root,bool enabled)
    {
        if(!InstalledDesktop.IsCurrent(root)||!Win32Installation.IsUnpackaged)throw new InvalidOperationException("请在当前安装版中更改开机自启。");
        if(!HasLogonEntry())throw new InvalidOperationException("安装版启动入口已变更，请重新运行安装程序修复。");
        SavePreference(InstalledDesktop.RegistrationPath,root,WindowsIdentity.GetCurrent().User!.Value,enabled);
    }
    private static bool HasLogonEntry()
    {
        using var machine=RegistryKey.OpenBaseKey(RegistryHive.LocalMachine,RegistryView.Registry64);
        using var run=machine.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
        string expected="\""+Path.Combine(Win32Installation.CodeRoot,"MagicNativeEntry.exe")+"\" --logon";
        return run?.GetValue("MagicCommunityDesktop",null,RegistryValueOptions.DoNotExpandEnvironmentNames) is string command&&
            string.Equals(command.Trim(),expected,StringComparison.OrdinalIgnoreCase);
    }
    internal static bool Preference(JsonElement record,string root,string sid)
    {
        static string Text(JsonElement record,string name)=>record.TryGetProperty(name,out var value)&&value.ValueKind==JsonValueKind.String?value.GetString()!:"";
        string dataRoot=Text(record,"dataRoot");
        if(Text(record,"protocol")!=InstalledDesktop.Protocol||Text(record,"state")!="installed"||
            Text(record,"ownerSid")!=sid||Text(record,"packageFamily")!=Win32Installation.Family||Text(record,"installKind")!="win32"||
            !Guid.TryParseExact(Text(record,"installId"),"N",out _)||!Path.IsPathFullyQualified(dataRoot)||
            !string.Equals(Path.GetFullPath(dataRoot).TrimEnd('\\'),Path.GetFullPath(root).TrimEnd('\\'),StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("启动设置不属于当前账户或当前数据目录。");
        if(!record.TryGetProperty("startupRequested",out var requested))return false;
        if(requested.ValueKind is not JsonValueKind.True and not JsonValueKind.False)throw new InvalidDataException("启动设置格式无效。");
        return requested.GetBoolean();
    }
    internal static JsonObject WithPreference(JsonElement record,string root,string sid,bool enabled)
    {
        _=Preference(record,root,sid);
        var result=JsonNode.Parse(record.GetRawText())!.AsObject();
        result["startupRequested"]=enabled;result["startupConfigured"]=true;
        return result;
    }
    // Also exercised with owned installation records by the isolated check.
    internal static void SavePreference(string path,string root,string sid,bool enabled)
    {
        var before=InstalledDesktop.Read(path);
        var after=WithPreference(before,root,sid,enabled);
        StateStore.AtomicWrite(path,after.ToJsonString(),path+".startup.bak");
        if(Preference(InstalledDesktop.Read(path),root,sid)!=enabled)throw new IOException("启动设置未成功保存，请重试。");
    }
}
