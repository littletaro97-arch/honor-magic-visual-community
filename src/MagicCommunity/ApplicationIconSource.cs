using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Interop;
using System.Windows.Media.Imaging;

namespace MagicCommunity;

// Authenticated shell snapshot data only. Extraction runs on DesktopIcons' STA,
// never on the renderer/UI thread; WM_GETICON has a bounded response budget.
internal sealed record ApplicationIconSource(string Identity,string Path,string Umid,string Image,long Window,bool Application)
{
    internal string Stamp => string.Join('|',Identity,Umid,Path,Image,Window,FileStamp(Image),FileStamp(Path));
    private static string FileStamp(string path){try{var f=new FileInfo(path);return f.Exists?$"{f.Length}:{f.LastWriteTimeUtc.Ticks}":"missing";}catch{return "unavailable";}}
    internal string? Read(Action<string>? diagnostic=null)
    {
        var failures=new List<string>();
        string? Try(string stage,Func<string?> read){try{var value=read();if(value is not null)return value;failures.Add(stage+":empty");}catch(Exception e)when(e is not OutOfMemoryException){failures.Add(stage+":"+e.GetType().Name);}return null;}
        // Keep explicit icon-pack choices authoritative. Image is not a Shell icon.
        if(Image!=Path&&System.IO.Path.GetExtension(Image).ToLowerInvariant() is ".png" or ".ico" or ".jpg" or ".jpeg" or ".bmp")
            if(Try("pack-image",()=>DesktopIcons.Read(new(){Target=Image},true,resizeImage:true)) is { } packed)return packed;
        if(!Application)return Try("tray-image",()=>DesktopIcons.Read(new(){Target=Image},true,resizeImage:true));
        if(!string.IsNullOrWhiteSpace(Umid))
            if(Try("application-id-hires",()=>ShellImage(@"shell:AppsFolder\"+Umid)) is { } packageLogo)return packageLogo;
        if(System.IO.Path.IsPathFullyQualified(Path)&&!Path.StartsWith(@"\\",StringComparison.Ordinal)&&File.Exists(Path))
            if(Try("shell-hires",()=>ShellImage(Path)) is { } highResolution)return highResolution;
        if(Window!=0&&IsWindow(new(Window))){
            if(Try("window",()=>WindowIcon(new(Window))) is { } icon)return icon;
            if(Try("process",()=>{
                GetWindowThreadProcessId(new(Window),out uint pid);using var process=Process.GetProcessById((int)pid);
                string executable=process.MainModule?.FileName??"";return ExecutableIcon(executable);
            }) is { } module)return module;
        }
        if(Try("executable",()=>ExecutableIcon(Path)) is { } file)return file;
        if(System.IO.Path.GetExtension(Path).Equals(".lnk",StringComparison.OrdinalIgnoreCase)&&File.Exists(Path))
            if(Try("shortcut",()=>DesktopIcons.Read(new(){Target=Path},false)) is { } shortcut)return shortcut;
        // Shell virtual entries and ordinary files/directories still use their
        // registered source; do not replace them with a generic app glyph.
        if(Path.StartsWith("shell:",StringComparison.OrdinalIgnoreCase))
            if(Try("shell-item",()=>ShellImage(Path)) is { } shell)return shell;
        if(System.IO.Path.IsPathFullyQualified(Path)&&!Path.StartsWith(@"\\",StringComparison.Ordinal)&&(File.Exists(Path)||Directory.Exists(Path)))
            if(Try("file-item",()=>ShellImage(Path)??DesktopIcons.Read(new(){Target=Path},false)) is { } item)return item;
        diagnostic?.Invoke($"{Identity}; AUMID={Umid}; path={Path}; {string.Join(',',failures)}");
        return null;
    }
    private static string? PackageLogo(object item)
    {
        // Use only the package/icon metadata supplied by Windows. Several PWA
        // packages return a generic document from the image factory despite a
        // valid registered logo. Probe a small fixed qualifier set, no scan.
        string root=((dynamic)item).ExtendedProperty("System.AppUserModel.PackageInstallPath") as string??"";
        string relative=((dynamic)item).ExtendedProperty("System.Tile.SmallLogoPath") as string??"";
        if(!System.IO.Path.IsPathFullyQualified(root)||root.StartsWith(@"\\",StringComparison.Ordinal)||string.IsNullOrWhiteSpace(relative)||System.IO.Path.IsPathRooted(relative))return null;
        root=System.IO.Path.GetFullPath(root).TrimEnd('\\')+"\\";
        string path=System.IO.Path.GetFullPath(System.IO.Path.Combine(root,relative));
        if(!path.StartsWith(root,StringComparison.OrdinalIgnoreCase)||!System.IO.Path.GetExtension(path).Equals(".png",StringComparison.OrdinalIgnoreCase))return null;
        string stem=path[..^4];
        foreach(string qualifier in new[]{".targetsize-256_altform-unplated",".targetsize-256",".targetsize-128_altform-unplated",".targetsize-128",".targetsize-48_altform-unplated",".targetsize-48",".scale-200",".scale-100",""}){
            string candidate=stem+qualifier+".png";
            if(File.Exists(candidate)&&DesktopIcons.Read(new(){Target=candidate},true,resizeImage:true) is { } png)return png;
        }
        return null;
    }
    internal static string? ShellImage(string path)
    {
        // Called only by the existing background STA. ICONONLY avoids content
        // thumbnails; the owned HBITMAP and RCW are released on every path.
        const string appsFolder=@"shell:AppsFolder\";
        var iid=typeof(IShellItemImageFactory).GUID;IShellItemImageFactory factory;
        if(path.StartsWith(appsFolder,StringComparison.OrdinalIgnoreCase)){
            object? shell=null,folder=null,item=null;IntPtr pidl=IntPtr.Zero;
            try{
                shell=Activator.CreateInstance(Type.GetTypeFromProgID("Shell.Application",true)!);
                folder=((dynamic)shell!).NameSpace("shell:AppsFolder");
                item=((dynamic)folder!).ParseName(path[appsFolder.Length..]);
                if(item is null)return null;
                if(PackageLogo(item) is { } logo)return logo;
                Marshal.ThrowExceptionForHR(SHGetIDListFromObject(item,out pidl));
                Marshal.ThrowExceptionForHR(SHCreateItemFromIDList(pidl,ref iid,out factory));
            }finally{
                if(pidl!=IntPtr.Zero)Marshal.FreeCoTaskMem(pidl);
                foreach(var value in new[]{item,folder,shell})if(value is not null&&Marshal.IsComObject(value))Marshal.FinalReleaseComObject(value);
            }
        }else Marshal.ThrowExceptionForHR(SHCreateItemFromParsingName(path,IntPtr.Zero,ref iid,out factory));
        IntPtr bitmap=IntPtr.Zero;
        try{
            foreach(int edge in new[]{256,128}){
                int hr=factory.GetImage(new Size(edge,edge),4|1,out bitmap);
                try{
                    // Preserve valid opaque package logos as well as alpha.
                    if(hr>=0&&NativeMenuBitmap.Encode(bitmap,allowOpaque:true) is { } png)return png;
                }finally{if(bitmap!=IntPtr.Zero)DeleteObject(bitmap);bitmap=IntPtr.Zero;}
            }
            return null;
        }
        finally{if(bitmap!=IntPtr.Zero)DeleteObject(bitmap);Marshal.FinalReleaseComObject(factory);}
    }
    [StructLayout(LayoutKind.Sequential)]private readonly struct Size(int width,int height){public readonly int Width=width,Height=height;}
    [ComImport,Guid("BCC18B79-BA16-442F-80C4-8A59C30C463B"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemImageFactory{[PreserveSig]int GetImage(Size size,uint flags,out IntPtr bitmap);}
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]private static extern int SHCreateItemFromParsingName(string path,IntPtr bind,ref Guid iid,out IShellItemImageFactory item);
    [DllImport("shell32.dll")]private static extern int SHGetIDListFromObject([MarshalAs(UnmanagedType.IUnknown)]object item,out IntPtr pidl);
    [DllImport("shell32.dll")]private static extern int SHCreateItemFromIDList(IntPtr pidl,ref Guid iid,out IShellItemImageFactory image);
    [DllImport("gdi32.dll")]private static extern bool DeleteObject(IntPtr bitmap);
    private static string? ExecutableIcon(string path)
    {
        if(!System.IO.Path.IsPathFullyQualified(path)||path.StartsWith(@"\\",StringComparison.Ordinal)||!File.Exists(path))return null;
        if(System.IO.Path.GetExtension(path).ToLowerInvariant() is not (".exe" or ".dll" or ".ico"))return null;
        IntPtr[] large=new IntPtr[1];ExtractIconEx(path,0,large,null,1);
        if(large[0]==IntPtr.Zero)return null;try{return Encode(large[0]);}finally{DestroyIcon(large[0]);}
    }
    private static string? WindowIcon(IntPtr window)
    {
        IntPtr borrowed=IntPtr.Zero;
        if(SendMessageTimeout(window,0x007f,new(1),IntPtr.Zero,0x0002|0x0020,25,out var result)!=IntPtr.Zero)borrowed=(IntPtr)result;
        if(borrowed==IntPtr.Zero)borrowed=GetClassLongPtr(window,-14);
        if(borrowed==IntPtr.Zero&&SendMessageTimeout(window,0x007f,IntPtr.Zero,IntPtr.Zero,0x0002|0x0020,25,out result)!=IntPtr.Zero)borrowed=(IntPtr)result;
        if(borrowed==IntPtr.Zero)return null;IntPtr owned=CopyIcon(borrowed);if(owned==IntPtr.Zero)return null;
        try{return Encode(owned);}finally{DestroyIcon(owned);}
    }
    private static string Encode(IntPtr icon)
    {
        var image=Imaging.CreateBitmapSourceFromHIcon(icon,System.Windows.Int32Rect.Empty,BitmapSizeOptions.FromEmptyOptions());image.Freeze();
        var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(image));using var output=new MemoryStream();encoder.Save(output);
        if(output.Length>65536)throw new InvalidDataException("Application icon exceeds bound.");
        return "data:image/png;base64,"+Convert.ToBase64String(output.ToArray());
    }
    [DllImport("user32.dll")]private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")]private static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]private static extern IntPtr SendMessageTimeout(IntPtr window,uint message,IntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
    [DllImport("user32.dll",EntryPoint="GetClassLongPtrW")]private static extern IntPtr GetClassLongPtr(IntPtr window,int index);
    [DllImport("user32.dll")]private static extern IntPtr CopyIcon(IntPtr icon);
    [DllImport("user32.dll")]private static extern bool DestroyIcon(IntPtr icon);
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]private static extern uint ExtractIconEx(string file,int index,[Out]IntPtr[] large,[Out]IntPtr[]? small,uint count);
}
