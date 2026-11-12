using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
namespace MagicCommunity;

// Read-only identity extraction on DesktopIcons' existing STA. No Resolve(),
// shortcut writes, recursive installation scans, window-title guesses or timers.
internal static class ApplicationIconIdentity
{
    internal static string Resolve(string path,string umid="")
    {
        if(umid.Equals("windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel",StringComparison.OrdinalIgnoreCase))return "aumid:"+umid.ToLowerInvariant();
        if(!Local(path)||!File.Exists(path))return string.IsNullOrWhiteSpace(umid)?"":"aumid:"+umid;
        string extension=Path.GetExtension(path).ToLowerInvariant();
        if(extension==".exe")return path;
        if(extension==".url"){
            if(new FileInfo(path).Length>32768)return "";
            foreach(string line in File.ReadLines(path))if(line.StartsWith("URL=",StringComparison.OrdinalIgnoreCase))return Steam(line[4..].Trim());
            return "";
        }
        if(extension!=".lnk"||new FileInfo(path).Length>1048576)return "";
        object? value=null;
        try{
            value=Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("00021401-0000-0000-C000-000000000046"),true)!);
            ((IPersistFile)value!).Load(path,0);var link=(IShellLink)value;
            var target=new StringBuilder(32768);Marshal.ThrowExceptionForHR(link.GetPath(target,target.Capacity,IntPtr.Zero,4));
            var args=new StringBuilder(32768);Marshal.ThrowExceptionForHR(link.GetArguments(args,args.Capacity));
            var icon=new StringBuilder(32768);Marshal.ThrowExceptionForHR(link.GetIconLocation(icon,icon.Capacity,out _));
            // An explicit custom shortcut image remains authoritative.
            if(Path.GetExtension(icon.ToString()).ToLowerInvariant() is ".ico" or ".png" or ".jpg" or ".jpeg" or ".bmp")return "";
            return Target(target.ToString(),args.ToString());
        }finally{if(value is not null&&Marshal.IsComObject(value))Marshal.FinalReleaseComObject(value);}
    }
    internal static string Target(string path,string arguments)
    {
        if(!Local(path)||!File.Exists(path)||!Path.GetExtension(path).Equals(".exe",StringComparison.OrdinalIgnoreCase))return "";
        string name=Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
        if(name=="steam"&&arguments.TrimStart().StartsWith("-applaunch ",StringComparison.OrdinalIgnoreCase))return Steam("steam://rungameid/"+arguments.Trim()[11..].Split(' ')[0]);
        if(arguments.Length>0&&(name is "explorer" or "steam" or "applicationframehost"||arguments.Contains("--app-",StringComparison.OrdinalIgnoreCase)))return "";
        if(name is "setup" or "install" or "uninstall" or "launcher" or "applicationframehost" or "powershell" or "pwsh" or "cmd" or "rundll32")return "";
        return path;
    }
    internal static string Steam(string uri)=>Uri.TryCreate(uri,UriKind.Absolute,out var value)&&value.Scheme=="steam"&&value.Host=="rungameid"&&ulong.TryParse(value.AbsolutePath.Trim('/'),out ulong id)&&id>0?"steam:"+id:"";
    private static bool Local(string path)=>Path.IsPathFullyQualified(path)&&!path.StartsWith(@"\\",StringComparison.Ordinal)&&!path.StartsWith(@"\\?",StringComparison.Ordinal)&&new DriveInfo(Path.GetPathRoot(path)!).DriveType==DriveType.Fixed;
    [ComImport,Guid("000214F9-0000-0000-C000-000000000046"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellLink
    {
        [PreserveSig]int GetPath([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder path,int count,IntPtr data,uint flags);
        void GetIDList(out IntPtr pidl);void SetIDList(IntPtr pidl);void GetDescription(IntPtr text,int count);void SetDescription(IntPtr text);
        void GetWorkingDirectory(IntPtr text,int count);void SetWorkingDirectory(IntPtr text);
        [PreserveSig]int GetArguments([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder text,int count);
        void SetArguments(IntPtr text);void GetHotkey(out short key);void SetHotkey(short key);void GetShowCmd(out int command);void SetShowCmd(int command);
        [PreserveSig]int GetIconLocation([Out,MarshalAs(UnmanagedType.LPWStr)]StringBuilder text,int count,out int index);
    }
}
