using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace MagicCommunity;

internal static class Win32Installation
{
    internal const string Family="MagicCommunity.Unpackaged";
    internal const string UninstallKey=@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{80B5D4B1-E952-4B68-B38A-368DC145B09F}_is1";
    internal static string CodeRoot {
        get {
            using var machine=RegistryKey.OpenBaseKey(RegistryHive.LocalMachine,RegistryView.Registry64);
            using var key=machine.OpenSubKey(UninstallKey);
            if(key is not null){
                if(key.GetValue("InstallLocation",null,RegistryValueOptions.DoNotExpandEnvironmentNames) is not string root || string.IsNullOrWhiteSpace(root) || key.GetValueKind("InstallLocation")!=RegistryValueKind.String)
                    throw new InvalidDataException("安装位置记录无效，请重新运行安装器。");
                if(!Path.IsPathFullyQualified(root)||root.StartsWith(@"\\")||root.Contains('%'))throw new InvalidDataException("安装位置记录无效，请重新运行安装器。");
                return Path.GetFullPath(root).TrimEnd('\\');
            }
            // Compatibility with installations made before path selection.
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"MagicCommunityDesktop");
        }
    }
    internal static bool IsUnpackaged { get { uint length=0;return GetCurrentPackageFamilyName(ref length,IntPtr.Zero)==15700; } }
    internal static bool Privileged(string sid)=>sid is "S-1-5-18" or "S-1-5-32-544" or "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
    internal static bool IsFixedVolumeRoot(string path)
    {
        if(path.Length!=3||!char.IsLetter(path[0])||path[1]!=':'||path[2]!='\\'||GetDriveType(path)!=3)return false;
        var target=new System.Text.StringBuilder(32768);
        if(QueryDosDevice(path[..2],target,target.Capacity)==0)return false;
        const string prefix=@"\Device\HarddiskVolume";string value=target.ToString();
        return value.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)&&value.Length>prefix.Length&&value.AsSpan(prefix.Length).IndexOfAnyExceptInRange('0','9')<0;
    }
    internal static uint UnsafeWriteMask(bool ancestor,bool volumeRoot)
    {
        // A physical drive root cannot be replaced using DELETE on itself.
        // DELETE_CHILD, WRITE_DAC and WRITE_OWNER remain unsafe on that root.
        const uint authority=(uint)(FileSystemRights.DeleteSubdirectoriesAndFiles|FileSystemRights.ChangePermissions|FileSystemRights.TakeOwnership)|0x10000000u;
        if(ancestor&&volumeRoot)return authority;
        return authority|0x40000000u|(uint)(FileSystemRights.Delete|FileSystemRights.WriteAttributes|FileSystemRights.WriteExtendedAttributes)|(ancestor?0u:(uint)FileSystemRights.Write);
    }
    internal static void ValidateCode()
    {
        if(!string.Equals(AppContext.BaseDirectory.TrimEnd('\\'),CodeRoot,StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("请运行安装后的开始菜单入口，不能直接启动解压文件或开发工作树。");
        for(var ancestor=Directory.GetParent(CodeRoot);ancestor is not null;ancestor=ancestor.Parent)ValidateProtected(ancestor,true);
        var pending=new Queue<FileSystemInfo>();pending.Enqueue(new DirectoryInfo(CodeRoot));
        int count=0;
        while(pending.Count>0){
            var file=pending.Dequeue();
            if(++count>8192)throw new InvalidDataException("安装文件数量异常。");ValidateProtected(file);
            // Validate each directory before enumerating it; never recurse into
            // a junction merely to discover that its target is not trusted.
            if(file is DirectoryInfo directory)foreach(var child in directory.EnumerateFileSystemInfos())pending.Enqueue(child);
        }
    }
    internal static void ValidateProtected(FileSystemInfo file,bool ancestor=false)
    {
        if((file.Attributes&FileAttributes.ReparsePoint)!=0)throw new InvalidDataException("安装文件不能是链接："+file.Name);
        FileSystemSecurity acl=file is DirectoryInfo dir?dir.GetAccessControl():((FileInfo)file).GetAccessControl();
        if(acl.GetOwner(typeof(SecurityIdentifier)) is not SecurityIdentifier owner||!Privileged(owner.Value))throw new InvalidDataException("安装文件所有者不是管理员："+file.Name);
        // Keep this policy aligned with setup and the native entry/helper.
        uint writes=UnsafeWriteMask(ancestor,ancestor&&IsFixedVolumeRoot(file.FullName));
        foreach(FileSystemAccessRule rule in acl.GetAccessRules(true,true,typeof(SecurityIdentifier)))
            if(rule.AccessControlType==AccessControlType.Allow&&(rule.PropagationFlags&PropagationFlags.InheritOnly)==0&&(unchecked((uint)rule.FileSystemRights)&writes)!=0&&!Privileged(rule.IdentityReference.Value))
                throw new InvalidDataException("安装目录允许普通用户改写，请重新安装："+file.Name);
    }
    internal static bool Configure()
    {
        ValidateCode();
        string sid=WindowsIdentity.GetCurrent().User!.Value;
        string data=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"MagicCommunity","UserData");
        JsonElement? prior=null;
        if(File.Exists(InstalledDesktop.RegistrationPath)){
            prior=InstalledDesktop.Read(InstalledDesktop.RegistrationPath);
            if(prior.Value.GetProperty("ownerSid").GetString()!=sid)throw new InvalidDataException("安装记录不属于当前账户。");
            data=InstalledDesktop.ValidatePath(prior.Value.GetProperty("dataRoot").GetString()!);
        }
        bool reuse=prior is {} existing&&InstalledDesktop.HasProfile(existing,sid);
        var window=new Window{Title="Magic 视界社区版 · 首次运行与启动设置",Width=640,SizeToContent=SizeToContent.Height,ResizeMode=ResizeMode.NoResize,WindowStartupLocation=WindowStartupLocation.CenterScreen,FontFamily=CommunityFont.Family,FontSize=15};
        var panel=new StackPanel{Margin=new Thickness(28)};window.Content=panel;
        void Text(string text,int size=15)=>panel.Children.Add(new TextBlock{Text=text,FontSize=size,TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,0,14)});
        Text("配置当前账户",25);Text(reuse?"将沿用当前账户的数据目录、布局和卡片。更新不会更改存储位置。":prior is null?"程序已安装。请选择布局、卡片和反馈的专用数据目录。":"原数据目录或归属标记已缺失。请选择专用空目录重新配置；不会覆盖原目录中的文件。原安装记录会保留备份。");
        var path=new TextBox{Text=data,Padding=new Thickness(8),IsReadOnly=reuse,Margin=new Thickness(0,0,0,10)};panel.Children.Add(path);
        if(!reuse){var browse=new Button{Content="浏览数据文件夹…",HorizontalAlignment=HorizontalAlignment.Left,Padding=new Thickness(12,6,12,6),Margin=new Thickness(0,0,0,12)};browse.Click+=(_,_)=>{var picker=new OpenFolderDialog{Title="选择社区版专用空目录",InitialDirectory=Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)};if(picker.ShowDialog(window)==true)path.Text=picker.FolderName;};panel.Children.Add(browse);}
        var startup=new CheckBox{Content="登录 Windows 时自动进入社区版",IsChecked=prior?.TryGetProperty("startupRequested",out var enabled)==true&&enabled.ValueKind==JsonValueKind.True,Margin=new Thickness(0,6,0,12)};panel.Children.Add(startup);
        Text("卸载时可选择保留或删除数据。保留后重装会继续使用原位置。",13);
        var confirm=new Button{Content="保存并进入",HorizontalAlignment=HorizontalAlignment.Right,Padding=new Thickness(22,10,22,10)};panel.Children.Add(confirm);
        confirm.Click+=(_,_)=>{
            try{ConfigureProfile(path.Text,sid,startup.IsChecked==true,prior);window.DialogResult=true;}
            catch(Exception e)when(e is not OutOfMemoryException){MessageBox.Show(window,e.Message,"配置未完成",MessageBoxButton.OK,MessageBoxImage.Warning);}
        };
        return window.ShowDialog()==true;
    }
    internal static void ConfigureProfile(string value,string sid,bool startup,JsonElement? prior)
    {
        string root=InstalledDesktop.ValidatePath(value),markerPath=Path.Combine(root,".magic-data.json");
        string id=Guid.NewGuid().ToString("N");
        if(File.Exists(markerPath)){
            var marker=InstalledDesktop.Read(markerPath);
            if(marker.GetProperty("protocol").GetString()!=InstalledDesktop.Protocol||marker.GetProperty("ownerSid").GetString()!=sid||!string.Equals(Path.GetFullPath(marker.GetProperty("dataRoot").GetString()!).TrimEnd('\\'),root,StringComparison.OrdinalIgnoreCase)||!Guid.TryParseExact(marker.GetProperty("installId").GetString(),"N",out _))throw new InvalidDataException("已有数据目录归属不一致。");
            id=marker.GetProperty("installId").GetString()!;
            if(prior is { } record&&string.Equals(record.GetProperty("dataRoot").GetString()?.TrimEnd('\\'),root,StringComparison.OrdinalIgnoreCase)&&record.GetProperty("installId").GetString()!=id)throw new InvalidDataException("安装记录与数据目录标识不一致。");
        }else if(Directory.Exists(root)&&Directory.EnumerateFileSystemEntries(root).Any())throw new InvalidDataException("请选择新的空目录；不会覆盖已有文件。");
        Directory.CreateDirectory(root);
        var markerValue=new{protocol=InstalledDesktop.Protocol,ownerSid=sid,dataRoot=root,installId=id};
        StateStore.AtomicWrite(markerPath,JsonSerializer.Serialize(markerValue),null);
        string registration=InstalledDesktop.RegistrationPath;
        Directory.CreateDirectory(Path.GetDirectoryName(registration)!);
        if(File.Exists(registration))File.Copy(registration,registration+"."+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+".bak",false);
        StateStore.AtomicWrite(registration,JsonSerializer.Serialize(new{protocol=InstalledDesktop.Protocol,state="installed",ownerSid=sid,dataRoot=root,installId=id,packageFamily=Family,packageVersion=AppController.Version,installKind="win32",codeDirectory=CodeRoot,startupRequested=startup,startupConfigured=true}),null);
    }
    [DllImport("kernel32.dll")]private static extern int GetCurrentPackageFamilyName(ref uint length,IntPtr name);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern uint GetDriveType(string root);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern uint QueryDosDevice(string name,System.Text.StringBuilder target,int size);
}
