using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Interop;
using System.Windows.Media.Imaging;

namespace MagicCommunity;

// Enumerate the Shell desktop namespace, including public desktop and virtual objects.
// This does not inspect or alter the Explorer ListView or desktop icon visibility settings.
public static class ShellDesktop
{
    [ComImport, Guid("000214E6-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellFolder
    {
        [PreserveSig] int ParseDisplayName(IntPtr hwnd, IntPtr pbc, [MarshalAs(UnmanagedType.LPWStr)] string name, ref uint eaten, out IntPtr pidl, ref uint attrs);
        [PreserveSig] int EnumObjects(IntPtr hwnd, uint flags, out IEnumIDList? list);
        [PreserveSig] int BindToObject(IntPtr pidl, IntPtr pbc, ref Guid riid, out IntPtr result);
        [PreserveSig] int BindToStorage(IntPtr pidl, IntPtr pbc, ref Guid riid, out IntPtr result);
        [PreserveSig] int CompareIDs(IntPtr param, IntPtr a, IntPtr b);
        [PreserveSig] int CreateViewObject(IntPtr hwnd, ref Guid riid, out IntPtr result);
        [PreserveSig] int GetAttributesOf(uint count, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex=0)] IntPtr[] pidls, ref uint attrs);
        [PreserveSig] int GetUIObjectOf(IntPtr hwnd, uint count, IntPtr apidl, ref Guid riid, IntPtr reserved, out IntPtr result);
        [PreserveSig] int GetDisplayNameOf(IntPtr pidl, uint flags, IntPtr strret);
        [PreserveSig] int SetNameOf(IntPtr hwnd, IntPtr pidl, [MarshalAs(UnmanagedType.LPWStr)] string name, uint flags, out IntPtr result);
    }
    [ComImport, Guid("000214F2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IEnumIDList
    {
        [PreserveSig] int Next(uint count, out IntPtr pidl, out uint fetched);
        [PreserveSig] int Skip(uint count);
        [PreserveSig] int Reset();
        [PreserveSig] int Clone(out IEnumIDList result);
    }
    [DllImport("shell32.dll")] private static extern int SHGetDesktopFolder(out IShellFolder folder);
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]private static extern int SHParseDisplayName(string name,IntPtr bind,out IntPtr pidl,uint mask,out uint attributes);
    [DllImport("shell32.dll")]private static extern int SHBindToParent(IntPtr pidl,ref Guid iid,out IShellFolder folder,out IntPtr child);
    // The desktop namespace can supply its background menu even when Explorer's
    // current IShellView is absent, restarting, or inaccessible in this session.
    public static object BackgroundContext(Guid iid)
    {
        Marshal.ThrowExceptionForHR(SHGetDesktopFolder(out var folder));IntPtr result=IntPtr.Zero;
        try{Marshal.ThrowExceptionForHR(folder.CreateViewObject(IntPtr.Zero,ref iid,out result));if(result==IntPtr.Zero)throw new InvalidOperationException("桌面 Shell 未提供背景菜单。");return Marshal.GetObjectForIUnknown(result);}
        finally{if(result!=IntPtr.Zero)Marshal.Release(result);Marshal.ReleaseComObject(folder);}
    }
    public static object ContextObject(string target,Guid iid,bool unowned=false)
    {
        Marshal.ThrowExceptionForHR(SHParseDisplayName(target,IntPtr.Zero,out var absolute,0,out _));IShellFolder? parent=null;IntPtr array=IntPtr.Zero,result=IntPtr.Zero;
        try
        {
            var folderId=typeof(IShellFolder).GUID;Marshal.ThrowExceptionForHR(SHBindToParent(absolute,ref folderId,out parent,out var child));array=Marshal.AllocHGlobal(IntPtr.Size);Marshal.WriteIntPtr(array,child);
            Marshal.ThrowExceptionForHR(parent.GetUIObjectOf(unowned?IntPtr.Zero:NativeDesktop.FindDesktopListView(),1,array,ref iid,IntPtr.Zero,out result));return Marshal.GetObjectForIUnknown(result);
        }
        finally{if(result!=IntPtr.Zero)Marshal.Release(result);if(array!=IntPtr.Zero)Marshal.FreeHGlobal(array);if(parent is not null)Marshal.ReleaseComObject(parent);Marshal.FreeCoTaskMem(absolute);}
    }
    public const int MaximumSelection=256;
    // The bridge resolves opaque item IDs first. This second boundary protects
    // the transient worker protocol: no partial selection and no duplicate work.
    public static string[] ValidateSelection(IEnumerable<string> values)
    {
        var result=new List<string>();var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);int count=0;
        foreach(string value in values)
        {
            if(++count>MaximumSelection)throw new InvalidDataException("一次最多操作 256 个桌面项目。");
            if(string.IsNullOrWhiteSpace(value)||value.Length>32767||value.IndexOf('\0')>=0||(!value.StartsWith("::",StringComparison.Ordinal)&&!Path.IsPathFullyQualified(value)))throw new InvalidDataException("无效的桌面项目。");
            string target=value.StartsWith("::",StringComparison.Ordinal)?value:Path.GetFullPath(value);
            if(seen.Add(target))result.Add(target);
        }
        if(result.Count==0)throw new InvalidDataException("未选择桌面项目。");
        return result.ToArray();
    }
    public static object ContextObjects(IEnumerable<string> targets,Guid iid,bool unowned=false)
    {
        string[] selection=ValidateSelection(targets);if(selection.Length==1)return ContextObject(selection[0],iid,unowned);
        var absolute=new List<IntPtr>();var parents=new List<IntPtr>();IShellFolder? parent=null;IntPtr array=IntPtr.Zero,result=IntPtr.Zero,desktopRoot=IntPtr.Zero;
        try
        {
            foreach(string target in selection)
            {
                Marshal.ThrowExceptionForHR(SHParseDisplayName(target,IntPtr.Zero,out var pidl,0,out _));absolute.Add(pidl);
                var ancestor=ILCloneFull(pidl);if(ancestor==IntPtr.Zero)throw new OutOfMemoryException();parents.Add(ancestor);
                if(!ILRemoveLastID(ancestor))throw new InvalidDataException("无法确定所选项目的 Shell 父目录。");
            }
            bool flat=parents.Skip(1).All(pidl=>ILIsEqual(parents[0],pidl));
            array=Marshal.AllocHGlobal(checked(IntPtr.Size*absolute.Count));
            if(flat)
            {
                var folderId=typeof(IShellFolder).GUID;Marshal.ThrowExceptionForHR(SHBindToParent(absolute[0],ref folderId,out parent,out _));
                for(int index=0;index<absolute.Count;index++)Marshal.WriteIntPtr(array,index*IntPtr.Size,ILFindLastID(absolute[index]));
                Marshal.ThrowExceptionForHR(parent.GetUIObjectOf(unowned?IntPtr.Zero:NativeDesktop.FindDesktopListView(),(uint)absolute.Count,array,ref iid,IntPtr.Zero,out result));
            }
            else
            {
                // User/Public desktop and virtual groups may have different
                // filesystem parents. Desktop is their common Shell root. A
                // single aggregate must receive every absolute PIDL; taking
                // just each last SHITEMID would refer to the wrong file.
                Marshal.ThrowExceptionForHR(SHGetDesktopFolder(out parent));desktopRoot=Marshal.AllocCoTaskMem(2);Marshal.WriteInt16(desktopRoot,0);
                for(int index=0;index<absolute.Count;index++)Marshal.WriteIntPtr(array,index*IntPtr.Size,absolute[index]);
                if(iid==typeof(System.Runtime.InteropServices.ComTypes.IDataObject).GUID)
                    Marshal.ThrowExceptionForHR(SHCreateDataObject(desktopRoot,(uint)absolute.Count,array,IntPtr.Zero,ref iid,out result));
                else if(iid==new Guid("000214E4-0000-0000-C000-000000000046"))
                {
                    var info=new DefaultContextMenu{Window=unowned?IntPtr.Zero:NativeDesktop.FindDesktopListView(),FolderPidl=desktopRoot,Folder=parent,Count=(uint)absolute.Count,Children=array};
                    Marshal.ThrowExceptionForHR(SHCreateDefaultContextMenu(ref info,ref iid,out result));
                }
                else throw new NotSupportedException("此 Shell 接口不支持跨目录多选。");
            }
            return Marshal.GetObjectForIUnknown(result);
        }
        finally
        {
            if(result!=IntPtr.Zero)Marshal.Release(result);if(array!=IntPtr.Zero)Marshal.FreeHGlobal(array);if(parent is not null)Marshal.ReleaseComObject(parent);
            if(desktopRoot!=IntPtr.Zero)Marshal.FreeCoTaskMem(desktopRoot);foreach(var pidl in parents)Marshal.FreeCoTaskMem(pidl);foreach(var pidl in absolute)Marshal.FreeCoTaskMem(pidl);
        }
    }
    [StructLayout(LayoutKind.Sequential)]private struct DefaultContextMenu
    {
        public IntPtr Window,Callback,FolderPidl;[MarshalAs(UnmanagedType.Interface)]public IShellFolder Folder;
        public uint Count;public IntPtr Children,Association;public uint KeyCount;public IntPtr Keys;
    }
    // ILCloneFull is a typed SDK inline alias, not a DLL export.
    [DllImport("shell32.dll",EntryPoint="ILClone")]private static extern IntPtr ILCloneFull(IntPtr pidl);
    [DllImport("shell32.dll")][return:MarshalAs(UnmanagedType.Bool)]private static extern bool ILRemoveLastID(IntPtr pidl);
    [DllImport("shell32.dll")][return:MarshalAs(UnmanagedType.Bool)]private static extern bool ILIsEqual(IntPtr first,IntPtr second);
    [DllImport("shell32.dll")]private static extern IntPtr ILFindLastID(IntPtr pidl);
    [DllImport("shell32.dll")]private static extern int SHCreateDataObject(IntPtr parent,uint count,IntPtr children,IntPtr inner,ref Guid iid,out IntPtr result);
    [DllImport("shell32.dll")]private static extern int SHCreateDefaultContextMenu(ref DefaultContextMenu menu,ref Guid iid,out IntPtr result);
    [DllImport("shell32.dll")] private static extern int SHGetNameFromIDList(IntPtr pidl, uint kind, out IntPtr name);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SHGetFileInfo(string path, uint attrs, ref SHFileInfo info, uint size, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(IntPtr icon);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct SHFileInfo
    {
        public IntPtr hIcon; public int iIcon; public uint attrs;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string displayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string typeName;
    }
    public static HashSet<string> HiddenTargets { get; private set; }=new(StringComparer.OrdinalIgnoreCase);
    public static List<DesktopItem> Enumerate(bool managedDesktop=false)
    {
        Marshal.ThrowExceptionForHR(SHGetDesktopFolder(out var folder));
        IEnumIDList? list = null;
        try
        {
            Marshal.ThrowExceptionForHR(folder.EnumObjects(IntPtr.Zero, 0x20 | 0x40, out list));
            var result = new List<DesktopItem>();
            var targets = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            int hr=1;
            while (list is not null && (hr = list.Next(1, out var pidl, out var fetched)) == 0)
            {
                if (fetched != 1 || pidl == IntPtr.Zero) throw new InvalidDataException("Shell 返回无效项目。");
                try
                {
                    string target = Name(pidl, 0x80028000), title = Name(pidl, 0);
                    uint attrs = 0x20000000; // SFGAO_FOLDER
                    Marshal.ThrowExceptionForHR(folder.GetAttributesOf(1, [pidl], ref attrs));
                    bool virtualItem = target.StartsWith("::", StringComparison.Ordinal);
                    if ((virtualItem || Path.IsPathFullyQualified(target)) && targets.Add(target))
                        result.Add(new() { Name = title, Target = target, IsVirtual = virtualItem, IsFolder = virtualItem?(attrs & 0x20000000) != 0:Directory.Exists(target), FromDesktop=true });
                }
                finally { Marshal.FreeCoTaskMem(pidl); }
            }
            Marshal.ThrowExceptionForHR(hr);
            var view=NativeDesktop.Read();
            if(!view.Available||view.VisibleTargets is null)throw new DesktopEnumerationUnavailableException("无法核对 Explorer 当前桌面清单："+view.Error);
            var shown=view.VisibleTargets.ToHashSet(StringComparer.OrdinalIgnoreCase);
            var physical=DesktopEnumerationGuard.ReadPhysicalTargets(ManagedDesktopFiles.DesktopRoots()).ToHashSet(StringComparer.OrdinalIgnoreCase);
            if(managedDesktop){
                // Retain the guard against an incomplete Shell namespace for
                // virtual objects; real desktop files have a separate authority.
                if(shown.Any(path=>path.StartsWith("::",StringComparison.Ordinal)&&!targets.Contains(path)))throw new DesktopEnumerationUnavailableException("Shell virtual desktop snapshot is incomplete.");
                foreach(string path in physical)if(targets.Add(path)){
                    try{var incoming=FromPath(path);incoming.FromDesktop=true;result.Add(incoming);}catch(FileNotFoundException error){throw new DesktopEnumerationUnavailableException("Desktop changed during enumeration.",error);}
                }
                var visible=result.Where(item=>item.IsVirtual?shown.Contains(item.Target):physical.Contains(item.Target)||shown.Contains(item.Target)&&(File.Exists(item.Target)||Directory.Exists(item.Target))).ToList();
                DesktopEnumerationGuard.Validate(result.Select(item=>item.Target),visible.Select(item=>item.Target),physical);
                var visibleTargets=visible.Select(item=>item.Target).ToHashSet(StringComparer.OrdinalIgnoreCase);
                HiddenTargets=result.Where(item=>!visibleTargets.Contains(item.Target)).Select(item=>item.Target).ToHashSet(StringComparer.OrdinalIgnoreCase);
                return visible.OrderByDescending(x=>x.IsFolder).ThenBy(x=>x.Name,StringComparer.CurrentCultureIgnoreCase).ToList();
            }
            DesktopEnumerationGuard.Validate(result.Select(item=>item.Target),shown,physical);
            HiddenTargets=result.Where(item=>!shown.Contains(item.Target)).Select(item=>item.Target).ToHashSet(StringComparer.OrdinalIgnoreCase);
            return result.Where(item=>shown.Contains(item.Target)).OrderByDescending(x => x.IsFolder).ThenBy(x => x.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }
        finally { if (list is not null) Marshal.ReleaseComObject(list); Marshal.ReleaseComObject(folder); }
    }
    private static string Name(IntPtr pidl, uint kind)
    {
        Marshal.ThrowExceptionForHR(SHGetNameFromIDList(pidl, kind, out var value));
        try { return Marshal.PtrToStringUni(value) ?? throw new InvalidDataException("Shell 名称为空。"); }
        finally { Marshal.FreeCoTaskMem(value); }
    }
    public static DesktopItem FromPath(string path)
    {
        path = Path.GetFullPath(path);
        if (!File.Exists(path) && !Directory.Exists(path)) throw new FileNotFoundException("拖入项目已不存在。", path);
        return new() { Name = Path.GetFileName(path.TrimEnd(Path.DirectorySeparatorChar)), Target = path, IsFolder = Directory.Exists(path) };
    }
    // Extension icons avoid asking arbitrary network paths for per-file icons on the UI thread.
    private static readonly Dictionary<string, BitmapSource?> Icons = new(StringComparer.OrdinalIgnoreCase);
    public static BitmapSource? Icon(DesktopItem item)
    {
        string key = item.IsVirtual ? "virtual" : item.IsFolder ? "folder" : Path.GetExtension(item.Target);
        if (Icons.TryGetValue(key, out var cached)) return cached;
        if (Icons.Count >= 96) return null;
        var info = new SHFileInfo();
        SHGetFileInfo(item.IsFolder || item.IsVirtual ? "folder" : "file" + key, item.IsFolder || item.IsVirtual ? 0x10u : 0x80u,
            ref info, (uint)Marshal.SizeOf<SHFileInfo>(), 0x100 | 0x1 | 0x10); // ICON | SMALLICON | USEFILEATTRIBUTES
        BitmapSource? icon = null;
        if (info.hIcon != IntPtr.Zero)
        {
            try { icon = Imaging.CreateBitmapSourceFromHIcon(info.hIcon, System.Windows.Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions()); icon.Freeze(); }
            finally { DestroyIcon(info.hIcon); }
        }
        Icons[key] = icon; return icon;
    }
}
