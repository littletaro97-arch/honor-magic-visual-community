using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace MagicCommunity;

// One native menu per windowless transient worker supplies real Shell commands
// to the existing desktop WebView. No tracking HWND or focus transfer.
public sealed partial class DesktopContextMenu : IDisposable
{
    public static Action<string>? Trace;
    public sealed record Entry(int Index,string Label,bool Submenu,bool Separator,bool Disabled,bool Checked,string? Key=null,string? Reason=null,string? Icon=null,string? Verb=null);
    private IContextMenu? context;
    internal string QueryStage {get;private set;}="idle";
    internal int BackgroundSource {get;private set;}=-1;
    internal sealed record BackgroundFailure(int Source,string Phase,int Hresult,string ErrorType);
    internal List<BackgroundFailure> BackgroundFailures {get;}=[];
    private ShellNewItemSite? newItemSite;
    public string? CreatedPath=>newItemSite?.CreatedPath;
    private const bool captureIcons=true;
    private readonly Dictionary<uint,string?> verbs=new();
    private readonly HashSet<IntPtr> initialized=new();
    private readonly Dictionary<string,Entry[]> preparedChildren=new();
    private IntPtr popup;
    private string? active;
    private string? target;
    private string[] targets=[];
    private bool optimizedFile;
    private readonly ShellNewIcons newIcons=new();
    private readonly ShellMenuIcons menuIcons=new();
    private readonly ShellExplorerCommands explorerCommands=new();
    private readonly ShellAssociationMenu associations=new();
    private readonly HashSet<string> cancelled=new(StringComparer.Ordinal);
    private readonly Queue<string> cancelledOrder=new();
    private static readonly Dictionary<string,string[]> PrimaryNames=new(){
        ["view"]=["查看","View"],["sort"]=["排序方式","Sort by"],["refresh"]=["刷新","Refresh"],
        ["new"]=["新建","New"],["display"]=["显示设置","Display settings"],["personalize"]=["个性化","Personalize"], ["paste"]=["粘贴","Paste"]};
    public object Open(string requestId,string? itemTarget=null,bool folderBackground=false,bool fullFileMenu=false,string[]? itemTargets=null,bool includeIcons=true,bool restrictHandlers=true)
    {
        // A Shell view can bind successfully yet reject QueryContextMenu
        // (including E_INVALIDARG). Retry the complete fresh lease, not just
        // binding. Never reuse partially populated HMENUs/positional IDs.
        bool desktop=itemTarget is null&&itemTargets is not {Length:>0};
        BackgroundFailures.Clear();
        try{
            object Attempt(int source){
                try{return OpenCore(requestId,itemTarget,folderBackground,fullFileMenu,itemTargets,includeIcons,restrictHandlers,source);}
                catch(Exception error){if(desktop)BackgroundFailures.Add(new(source,QueryStage,error.HResult,error.GetType().Name));throw;}
            }
            return desktop?TryBackgroundSources(Attempt):Attempt(0);
        }catch{Dismiss(requestId);throw;}
    }
    internal static object TryBackgroundSources(Func<int,object> attempt)
    {
        for(int source=0;;source++){
            try{return attempt(source);}
            catch(Exception error)when(source<2&&BackgroundUnavailable(error)){
                Trace?.Invoke($"desktop-source-{source}-unavailable hr=0x{error.HResult:X8} type={error.GetType().Name}");
            }
        }
    }
    internal static bool BackgroundUnavailable(Exception error)=>error is COMException or UnauthorizedAccessException or InvalidOperationException or InvalidCastException or ArgumentException;
    private object OpenCore(string requestId,string? itemTarget,bool folderBackground,bool fullFileMenu,string[]? itemTargets,bool includeIcons,bool restrictHandlers,int backgroundSource)
    {
        QueryStage="validate";BackgroundSource=itemTarget is null&&itemTargets is not {Length:>0}?backgroundSource:-1;
        ValidateId(requestId);
        if(cancelled.Contains(requestId))return new{requestId,cancelled=true,compact=Array.Empty<Entry>(),more=Array.Empty<Entry>()};
        if(folderBackground&&itemTargets is {Length:>0})throw new InvalidDataException();
        var selection=folderBackground||itemTargets is null&&itemTarget is null?Array.Empty<string>():ShellDesktop.ValidateSelection(itemTargets??[itemTarget!]);
        Dismiss();verbs.Clear();initialized.Clear();preparedChildren.Clear();newItemSite=null;active=requestId;targets=selection;target=selection.FirstOrDefault();
        try
        {
            var elapsed=System.Diagnostics.Stopwatch.StartNew();
            QueryStage="bind";
            Trace?.Invoke("bind-start");
            context=selection.Length>0?(IContextMenu)ShellDesktop.ContextObjects(selection,typeof(IContextMenu).GUID):itemTarget is null?DesktopBackground(backgroundSource):ItemContext(itemTarget,folderBackground);
            double bindMs=elapsed.Elapsed.TotalMilliseconds;
            Trace?.Invoke("bind-done");
            popup=CreatePopupMenu();if(popup==IntPtr.Zero)throw new Win32Exception();
            Trace?.Invoke("query-start");
            QueryStage="query";
            bool archiveFile=selection.Any(value=>!Directory.Exists(value)&&
                new[]{".zip",".7z",".rar",".gz",".bz2",".xz",".tar",".tgz",".iso",".cab"}.Contains(Path.GetExtension(value),StringComparer.OrdinalIgnoreCase));
            optimizedFile=!fullFileMenu&&(target is null||selection.All(value=>!value.StartsWith("::",StringComparison.Ordinal)));
            // The compact selection menu never displays legacy dynamic rows;
            // its modern commands are queried separately below. A hint flag
            // alone still activates legacy handlers that we immediately drop.
            // Restrict this fresh menu only; More binds an unrestricted object.
            bool compactRestricted=false;
            if(restrictHandlers&&optimizedFile&&target is not null&&context is IDefaultFolderMenuInitialize options)
                try{if(options.GetMenuRestrictions(0xffff,out uint current)>=0)compactRestricted=options.SetMenuRestrictions(current|0x10)>=0;}
                catch(COMException error){Trace?.Invoke("compact-restrictions-unavailable "+error.HResult);}
            // Keep virtual-object menus complete. Ordinary files, archives and
            // directories use stock verbs first; More builds a fresh full lease.
            // Background compact menus need only the stock primary commands.
            // Explicit More requests use the full query in the bounded
            // transient worker; never rebuild a live lease with different IDs.
            // Full means all root handlers, not synchronous population of every
            // unopened cascade (CMF_SYNCCASCADEMENU). Resolve already forwards
            // WM_INITMENUPOPUP when a submenu is actually requested.
            uint flags=target is null?0u:selection.Length==1?0x90u:0x10u;if(optimizedFile&&backgroundSource==0)flags|=0x800;
            using(var budget=new ShellExplorerCommands.QueryCallBudget(1500))
                Marshal.ThrowExceptionForHR(context.QueryContextMenu(popup,0,1,0x6fff,flags));
            Trace?.Invoke("query-done");
            double queryMs=elapsed.Elapsed.TotalMilliseconds-bindMs;
            QueryStage="read";
            var classic=Read(popup,captureIcons&&includeIcons,!optimizedFile);
            if(compactRestricted){
                try{classic=classic.Concat(CompactWindowsCommands(selection,classic,includeIcons)).ToArray();}
                catch(Exception error)when(ShellExplorerCommands.ProviderUnavailable(error)||error is InvalidOperationException){Trace?.Invoke($"compact-supplements-unavailable hr=0x{error.HResult:X8}");}
            }
            // Stock compression/pinning may appear on the first page. Installed
            // modern providers are queried by the separate post-display lease.
            QueryStage="modern";
            explorerCommands.Load(selection,classic,includePackaged:!optimizedFile);var more=classic.Concat(explorerCommands.Roots).ToArray();var compact=new List<Entry>();
            if(ShellAssociationMenu.Supports(selection)){
                // Replace only the root Open With entry. Its children are read
                // on demand and retain the same STA and request lease.
                more=more.Where(row=>row.Key!="openas"&&!ShellOpenWith.IsChooser(row)).Append(new Entry(ShellAssociationMenu.RootIndex,"打开方式",true,false,false,false,Key:"openas")).ToArray();
            }
            // Missing optional verbs do not invalidate the initialized first-page
            // lease. Only an explicit/separate full-menu query loads all handlers.
            if(target is null){foreach(var pair in PrimaryNames){var row=more.FirstOrDefault(item=>!item.Separator&&(string.Equals(item.Verb,pair.Key,StringComparison.OrdinalIgnoreCase)||pair.Value.Contains(Normalize(item.Label),StringComparer.OrdinalIgnoreCase)));if(row is not null)compact.Add(row with{Key=pair.Key});}}
            else foreach(string key in new[]{"open","runas","opennewwindow","openas","pintohome","pintostartscreen","compress","copyaspath","sharing","modernsharing","cut","copy","rename","delete","properties"}){var row=more.FirstOrDefault(item=>item.Key==key);if(row is not null)compact.Add(row);}
            if(!archiveFile)compact.AddRange(explorerCommands.Roots.Where(row=>row.Key=="modern"&&!compact.Any(old=>Normalize(old.Label)==Normalize(row.Label))));
            if(archiveFile&&!optimizedFile){compact.Clear();compact.AddRange(more);}
            QueryStage="ready";
            return new{requestId,cancelled=false,compact=compact.ToArray(),more,optimizedFile,compactRestricted,timings=new{bindMs,queryMs,readMs=elapsed.Elapsed.TotalMilliseconds-bindMs-queryMs,totalMs=elapsed.Elapsed.TotalMilliseconds,backgroundSource=BackgroundSource,backgroundFailures=BackgroundFailures.ToArray()},fullFileMenu=target is not null&&!optimizedFile,selectionCount=selection.Length,renameName=selection.Length!=1||target!.StartsWith("::",StringComparison.Ordinal)?null:Path.GetFileName(target)};
        }
        catch{Dismiss();throw;}
    }
    public object Undo(string requestId)
    {
        try{
            Open(requestId,includeIcons:false);
            var entry=Read(popup,false).FirstOrDefault(row=>row.Key=="undo"&&!row.Disabled&&!row.Separator);
            if(entry is null){Open(requestId,fullFileMenu:true,includeIcons:false);entry=Read(popup,false).FirstOrDefault(row=>row.Key=="undo"&&!row.Disabled&&!row.Separator);}
            if(entry is null)throw new InvalidOperationException("Windows 当前没有可撤销的文件操作。");
            Invoke(requestId,[entry.Index]);return new{ok=true};
        }finally{Dismiss(requestId);}
    }
    private static IContextMenu DesktopBackground(int source)
    {
        if(source==0){
            return NativeDesktop.WithView(view=>{
                var iid=typeof(IContextMenu).GUID;
                Marshal.ThrowExceptionForHR(((IShellView)view).GetItemObject(0,ref iid,out var menu));
                return (IContextMenu)menu;
            });
        }
        if(source==1)return (IContextMenu)ShellDesktop.BackgroundContext(typeof(IContextMenu).GUID);
        return ItemContext(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),true);
    }
    private static IContextMenu ItemContext(string target,bool background)
    {
        if(!background)return (IContextMenu)ShellDesktop.ContextObject(target,typeof(IContextMenu).GUID);
        var iid=typeof(IShellItem).GUID;Marshal.ThrowExceptionForHR(SHCreateItemFromParsingName(target,IntPtr.Zero,ref iid,out var item));
        try{var handler=new Guid(background?"3981E226-F559-11D3-8E3A-00C04F6837D5":"3981E225-F559-11D3-8E3A-00C04F6837D5");iid=typeof(IContextMenu).GUID;Marshal.ThrowExceptionForHR(item.BindToHandler(IntPtr.Zero,ref handler,ref iid,out var value));return (IContextMenu)value;}
        finally{Marshal.ReleaseComObject(item);}
    }
    private static void FlushClipboard()
    {
        int result;for(int retry=0;;retry++){result=OleFlushClipboard();if(result!=unchecked((int)0x800401D0)||retry==9)break;Thread.Sleep(20);}
        Marshal.ThrowExceptionForHR(result);
    }
    public Entry[] Children(string requestId,int[] path)
    {
        QueryStage="children";
        ValidateId(requestId);if(active!=requestId)throw new InvalidDataException("Menu expired.");
        if(path.Length>0&&path[0]==ShellAssociationMenu.RootIndex){if(path.Length!=1)throw new InvalidDataException();return associations.Children(targets);}
        if(path.Length>0&&supplemental.ContainsKey(path[0]))throw new InvalidDataException("This command has no submenu.");
        if(path.Length>0&&path[0]>=ShellExplorerCommands.FirstIndex)return explorerCommands.Children(path);
        string childKey=string.Join('/',path);
        if(path.Length>0&&preparedChildren.TryGetValue(childKey,out var prepared))return prepared;
        bool isNew=path.Length==1&&Read(popup,false).Any(row=>row.Index==path[0]&&row.Key=="new");
        // New type artwork is resolved below; background-menu icon providers
        // are unrelated and can load expensive DLLs before any row is shown.
        var items=Read(Resolve(requestId,path),captureIcons,!isNew);
        if(path.Length==0&&IsRecycleRoot)return items.Concat(explorerCommands.Roots).ToArray();
        var result=isNew?items.Select(row=>row.Separator?row:row with{Icon=row.Icon??newIcons.Get(row.Label),Key=row.Index==0?"new-folder":row.Index==1?"new-shortcut":Normalize(row.Label) is "文本文档" or "Text Document"?"new-text":row.Key??"document"}).ToArray():items;
        if(path.Length>0)preparedChildren[childKey]=result;
        return result;
    }
    // The same bounded worker query can now be tracked by User32. Shell
    // submenu messages stay on the worker's STA; no extra resident menu host.
    public object TrackNative(string requestId,int x,int y)
    {
        if(active!=requestId||context is null||popup==IntPtr.Zero||Math.Abs((long)x)>65536||Math.Abs((long)y)>65536)throw new InvalidDataException("Menu expired.");
        IntPtr owner=CreateWindowEx(0x80,"STATIC","Magic native menu",unchecked((int)0x80000000),x,y,1,1,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);
        if(owner==IntPtr.Zero)throw new Win32Exception();
        NativeWindowProc? callback=null;IntPtr previous=IntPtr.Zero;
        callback=(hwnd,message,w,l)=>{
            try{
                if(context is IContextMenu3 third&&message is 0x117 or 0x2b or 0x2c or 0x120&&third.HandleMenuMsg2(message,w,l,out var value)>=0)return value;
                if(context is IContextMenu2 second&&message is 0x117 or 0x2b or 0x2c&&second.HandleMenuMsg(message,w,l)>=0)return IntPtr.Zero;
            }catch(Exception error){Trace?.Invoke(error.Message);}
            return CallWindowProc(previous,hwnd,message,w,l);
        };
        previous=SetWindowLongPtr(owner,-4,Marshal.GetFunctionPointerForDelegate(callback));
        try{
            ShowWindow(owner,4);SetForegroundWindow(owner);
            uint chosen=TrackPopupMenuEx(popup,0x100|0x2,x,y,owner,IntPtr.Zero);
            PostMessage(owner,0,IntPtr.Zero,IntPtr.Zero);
            if(chosen==0)return new{ok=true,cancelled=true,createdPath=(string?)null};
            int[]? FindPath(IntPtr menu,List<int> prefix,int depth){
                if(depth>5)return null;
                for(int i=0;i<GetMenuItemCount(menu);i++){
                    var path=prefix.Append(i).ToArray();if(GetMenuItemID(menu,i)==chosen)return path;
                    var child=GetSubMenu(menu,i);if(child!=IntPtr.Zero){var found=FindPath(child,path.ToList(),depth+1);if(found is not null)return found;}
                }return null;
            }
            var command=FindPath(popup,[],0)??throw new InvalidDataException("Menu command changed.");
            Invoke(requestId,command);return new{ok=true,cancelled=false,createdPath=CreatedPath};
        }finally{SetWindowLongPtr(owner,-4,previous);DestroyWindow(owner);GC.KeepAlive(callback);}
    }
    private delegate IntPtr NativeWindowProc(IntPtr hwnd,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="CreateWindowExW",CharSet=CharSet.Unicode,SetLastError=true)]private static extern IntPtr CreateWindowEx(uint ex,string cls,string title,int style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr data);
    [DllImport("user32.dll",EntryPoint="SetWindowLongPtrW")]private static extern IntPtr SetWindowLongPtr(IntPtr hwnd,int index,IntPtr value);
    [DllImport("user32.dll",EntryPoint="CallWindowProcW")]private static extern IntPtr CallWindowProc(IntPtr previous,IntPtr hwnd,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll")]private static extern uint TrackPopupMenuEx(IntPtr menu,uint flags,int x,int y,IntPtr hwnd,IntPtr reserved);
    [DllImport("user32.dll")]private static extern bool ShowWindow(IntPtr hwnd,int command);
    [DllImport("user32.dll")]private static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")]private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll",EntryPoint="PostMessageW")]private static extern bool PostMessage(IntPtr hwnd,uint message,IntPtr w,IntPtr l);
    public void Invoke(string requestId,int[] path)
    {
        if(path.Length is <1 or >16)throw new InvalidDataException();
        ValidateId(requestId);if(active!=requestId)throw new InvalidDataException("Menu expired.");
        if(path[0]==ShellAssociationMenu.RootIndex){try{associations.Invoke(targets,path);}finally{Dismiss(requestId);}return;}
        if(supplemental.ContainsKey(path[0])){try{InvokeSupplement(path);}finally{Dismiss(requestId);}return;}
        if(path[0]>=ShellExplorerCommands.FirstIndex){
            bool compression=explorerCommands.IsCompression(path);
            try{explorerCommands.Invoke(path);}
            finally{Dismiss(requestId);if(compression)ShellCommandLifetime.DrainZipTasks();}
            return;
        }
        IntPtr parent=Resolve(requestId,path[..^1],true);int index=path[^1];
        if(index<0||index>=GetMenuItemCount(parent))throw new InvalidDataException();
        var row=Read(parent,false).Single(item=>item.Index==index);
        if(row.Key=="empty-bin"&&row.Disabled){Dismiss(requestId);return;}
        if(row.Disabled||row.Separator||row.Submenu)throw new InvalidDataException("Menu item cannot be invoked.");
        uint id=GetMenuItemID(parent,index);if(id is 0 or >0x6fff)throw new InvalidDataException();
        try
        {
            if(row.Key=="empty-bin"){RecycleBinWatch.EmptyWithConfirmation();return;}
            if(InvokeSelectedFileCommand(row))return;
            if(targets.Length==1&&ShellOpenWith.IsChooser(row)){ShellOpenWith.Show(targets);return;}
            if(targets.Length==1&&Path.GetExtension(targets[0]).Equals(".zip",StringComparison.OrdinalIgnoreCase)&&row.Verb=="extract"){
                // The Windows ZIP wizard is hosted by this transient STA and
                // does not reliably hold SHCreateThreadRef. Its hidden Explorer
                // ListView owner and an immediate worker exit can hide/kill it.
                ShellProperties.RunWindowCommand(()=>{
                    var extract=new InvokeInfo{Size=Marshal.SizeOf<InvokeInfo>(),Mask=0x100,Window=IntPtr.Zero,Verb=new IntPtr(id-1),Show=1};
                    Marshal.ThrowExceptionForHR(context!.InvokeCommand(ref extract));
                },true,"Windows 未显示解压窗口，请重试或在资源管理器中解压。");
                return;
            }
            if(targets.Length==0&&path.Length==2&&Read(popup,false).Any(entry=>entry.Index==path[0]&&entry.Key=="new"))newItemSite=ShellNewItemSite.Attach(context!);
            // Settings pages must not activate the hidden Explorer desktop owner.
            var owner=targets.Length==0&&row.Key is "personalize" or "display"?IntPtr.Zero:NativeDesktop.FindDesktopListView();
            var info=new InvokeInfo{Size=Marshal.SizeOf<InvokeInfo>(),Mask=0x100,Window=owner,Verb=new IntPtr(id-1),Show=1};
            Marshal.ThrowExceptionForHR(context!.InvokeCommand(ref info));
            if(row.Verb is "cut" or "copy" or "copyaspath")
            {
                FlushClipboard();
            }
        }
        finally{Dismiss(requestId);}
    }
    private bool InvokeSelectedFileCommand(Entry row)
    {
        if(targets.Length==0)return false;
        if(row.Verb=="properties"&&(targets.Length>1||Path.IsPathFullyQualified(targets[0]))){
            ShellProperties.Show(targets);
            return true;
        }
        if(row.Verb=="copyaspath"&&targets.All(Path.IsPathFullyQualified)){
            // The Shell offered and enabled this verb for this exact selection.
            // Publish its quoted paths as persistent Unicode text in one operation.
            string text=string.Join("\r\n",targets.Select(path=>"\""+path+"\""));
            // Eager native data remains owned by Windows after this worker
            // exits. WPF's additional OLE flush can fail after publishing text,
            // which reports failure even though another app can already paste.
            IntPtr owner=CreateWindowEx(0x80,"STATIC","Magic path clipboard",unchecked((int)0x80000000),0,0,1,1,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);
            if(owner==IntPtr.Zero)throw new Win32Exception();
            try{NativeHandoffClipboard.Put(owner,new(text,"",[]),onlyText:true);}
            finally{DestroyWindow(owner);}
            return true;
        }
        return false;
    }
    private IntPtr Resolve(string requestId,int[] path,bool invoking=false)
    {
        ValidateId(requestId);
        if(active!=requestId||context is null||popup==IntPtr.Zero||path.Length>16)throw new InvalidDataException("Menu expired.");
        IntPtr parent=popup;
        foreach(int index in path)
        {
            if(index<0||index>=GetMenuItemCount(parent))throw new InvalidDataException();
            if(invoking&&Read(parent,false).Single(row=>row.Index==index).Disabled)throw new InvalidDataException("Parent menu is disabled.");
            IntPtr child=GetSubMenu(parent,index);if(child==IntPtr.Zero)throw new InvalidDataException();
            // Reinitializing an already displayed ancestor can change its
            // positional IDs and repeat slow handlers on every click. Each
            // selection owns one HMENU; initialize each cascade only once.
            if(!initialized.Contains(child)){
                if(context is IContextMenu3 third)third.HandleMenuMsg2(0x117,child,new IntPtr(index),out _);
                else if(context is IContextMenu2 second)second.HandleMenuMsg(0x117,child,new IntPtr(index));
                initialized.Add(child);
            }
            parent=child;
        }
        return parent;
    }
    private static string Normalize(string label)=>Regex.Replace(label.Split('\t')[0],@"\(&.\)|&","").Trim();
    private static string? Key(string label)
    {
        string clean=Normalize(label);
        foreach(var row in PrimaryNames)if(row.Value.Contains(clean,StringComparer.OrdinalIgnoreCase))return row.Key;
        return clean switch{"粘贴" or "Paste"=>"paste","粘贴快捷方式" or "Paste shortcut"=>"paste-shortcut",_ when clean.StartsWith("撤销",StringComparison.Ordinal)||clean.StartsWith("撤消",StringComparison.Ordinal)||clean.StartsWith("Undo",StringComparison.OrdinalIgnoreCase)=>"undo",_=>null};
    }
    private string? Verb(uint id,IContextMenu? source=null)
    {
        if(context is null||id is 0 or >0x6fff)return null;
        if(source is null&&verbs.TryGetValue(id,out var cached))return cached;
        IntPtr buffer=Marshal.AllocHGlobal(2048);
        try{Marshal.WriteInt16(buffer,0);var value=(source??context).GetCommandString(new UIntPtr(id-1),4,IntPtr.Zero,buffer,1024)>=0?Marshal.PtrToStringUni(buffer):null;if(source is null)verbs[id]=value;return value;}
        catch(COMException error){Trace?.Invoke($"verb-unavailable id={id} hr=0x{error.HResult:X8}");if(source is null)verbs[id]=null;return null;}
        finally{Marshal.FreeHGlobal(buffer);}
    }
    private bool IsRecycleRoot=>targets.Length==1&&ShellClipboardCommands.IsRecycleDestination(targets[0]);
    private Entry[] Read(IntPtr menu,bool icons=true,bool fallbackIcons=true,IContextMenu? source=null)
    {
        int count=GetMenuItemCount(menu);if(count is <0 or >512)throw new InvalidDataException();var rows=new List<Entry>();
        for(int index=0;index<count;index++)
        {
            Trace?.Invoke("read-"+index+"-start");
            var info=new MenuItemInfo{Size=(uint)Marshal.SizeOf<MenuItemInfo>(),Mask=0x100|0x1|0x80};
            if(!GetMenuItemInfo(menu,(uint)index,true,ref info))throw new Win32Exception();
            uint state=info.State;var text=new StringBuilder(2048);GetMenuString(menu,(uint)index,text,text.Capacity,0x400);
            bool separator=(info.Type&0x800)!=0;string label=text.ToString();bool unknown=!separator&&string.IsNullOrWhiteSpace(label);
            bool unsupportedGrid=target is null&&(Key(label) is "view" or "sort");
            string? verb=separator?null:Verb(GetMenuItemID(menu,index),source);
            bool virtualRename=target?.StartsWith("::",StringComparison.Ordinal)==true&&verb=="rename";
            bool multipleRename=targets.Length>1&&verb=="rename";
            bool layerToggle=target is null&&(Normalize(label) is "显示桌面图标" or "Show desktop icons");
            string? reason=layerToggle?"统一桌面启用期间，由社区版管理图标层。":unknown?"此系统扩展未提供可读取的菜单文字。":null;
            if(unsupportedGrid)reason="统一网格的查看与排序尚未适配；暂不操作隐藏的 Windows 图标层。";
            if(virtualRename)reason="系统虚拟图标的重命名暂未适配。";
            if(multipleRename)reason="批量重命名暂未适配，请选择一个项目后原位重命名。";
            Trace?.Invoke("read-"+index+"-verb");
            if(optimizedFile&&!separator)label=ShellMenuLabels.Resolve(target!,label,verb);
            string? key=verb=="undo"?"undo":target is not null&&verb is "open" or "runas" or "opennewwindow" or "openas" or "pintohome" or "pintostartscreen" or "copyaspath" or "sharing" or "modernsharing" or "cut" or "copy" or "rename" or "delete" or "properties"?verb:Key(label);
            bool shellDisabled=(state&3)!=0;
            if(menu==popup&&source is null&&IsRecycleRoot&&string.Equals(verb,"empty",StringComparison.OrdinalIgnoreCase)){
                key="empty-bin";
                if(RecycleBinWatch.QueryFull() is bool full)shellDisabled=!full;
            }
            string? icon=null;
            if(icons&&!separator)try{icon=ShellMenuIcons.Stock(key)??NativeMenuBitmap.Encode(info.Bitmap)??(fallbackIcons?menuIcons.Get(targets.Length>1?null:target,verb,label):null);}
            catch(Exception error)when(error is COMException or ArgumentException or IOException or InvalidOperationException){Trace?.Invoke($"icon-unavailable index={index} hr=0x{error.HResult:X8}");}
            rows.Add(new(index,unknown?"未提供文字的系统扩展":label,GetSubMenu(menu,index)!=IntPtr.Zero,separator,shellDisabled||unknown||layerToggle||unsupportedGrid||virtualRename||multipleRename,(state&8)!=0,Key:key,Reason:reason,Icon:icon,Verb:verb));
            Trace?.Invoke("read-"+index+"-done");
        }
        return rows.ToArray();
    }
    public void Dismiss(string? requestId=null)
    {
        if(requestId is not null)
        {
            ValidateId(requestId);
            if(cancelled.Add(requestId)){cancelledOrder.Enqueue(requestId);if(cancelledOrder.Count>256)cancelled.Remove(cancelledOrder.Dequeue());}
            if(active!=requestId)return;
        }
        preparedChildren.Clear();
        associations.Dispose();ClearSupplements();explorerCommands.Dispose();active=null;target=null;targets=[];
        if(popup!=IntPtr.Zero){DestroyMenu(popup);popup=IntPtr.Zero;}
        if(context is not null&&Marshal.IsComObject(context))Marshal.ReleaseComObject(context);context=null;
    }
    private static void ValidateId(string value){if(value.Length!=32||!value.All(Uri.IsHexDigit))throw new InvalidDataException();}
    public static object Inspect(string? target=null)
    {
        using var menu=new DesktopContextMenu();string id=Guid.NewGuid().ToString("N");var root=menu.Open(id,target);var rows=menu.Read(menu.popup);
        return new{source=target is not null?"IShellItem BHID_SFUIObject":menu.BackgroundSource switch{0=>"Explorer IShellView SVGIO_BACKGROUND",1=>"Desktop namespace CreateViewObject",_=>"Desktop directory BHID_SFViewObject"},presentation="Shell query only; no menu window",root,
            submenus=rows.Where(row=>row.Submenu&&row.Key is "view" or "new").Select(row=>new{row.Key,items=menu.Children(id,[row.Index])}).ToArray(),commandsInvoked=false};
    }
    public void Dispose()=>Dismiss();
    [DllImport("ole32.dll")]private static extern int OleFlushClipboard();
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]private static extern int SHCreateItemFromParsingName(string path,IntPtr bind,ref Guid iid,out IShellItem item);
    [ComImport,Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]private interface IShellItem
    {
        [PreserveSig]int BindToHandler(IntPtr bind,ref Guid handler,ref Guid iid,[MarshalAs(UnmanagedType.Interface)]out object value);
    }
    internal static bool HasDefaultAction(object item)=>item is IContextMenu && DefaultAction(item,false);
    internal static void InvokeDefaultAction(object item)
    {
        if(!DefaultAction(item,true))throw new InvalidDataException("Windows 未提供此任务的默认操作。");
    }
    private static bool DefaultAction(object item,bool invokeIt)
    {
        var context=(IContextMenu)item;
        var menu=CreatePopupMenu();if(menu==IntPtr.Zero)throw new Win32Exception();
        try {
            if(context.QueryContextMenu(menu,0,1,0x6fff,1)<0)return false; // CMF_DEFAULTONLY
            uint command=GetMenuDefaultItem(menu,false,0);
            if(command is 0 or >0x6fff)return false;
            if(!invokeIt)return true;
            var invoke=new InvokeInfo{Size=Marshal.SizeOf<InvokeInfo>(),Mask=0x100,Verb=new IntPtr(command-1),Show=1};
            Marshal.ThrowExceptionForHR(context.InvokeCommand(ref invoke));
            return true;
        } finally {DestroyMenu(menu);}
    }
    [DllImport("user32.dll")]private static extern uint GetMenuDefaultItem(IntPtr menu,bool byPosition,uint flags);
    [StructLayout(LayoutKind.Sequential)]private struct InvokeInfo{public int Size;public uint Mask;public IntPtr Window,Verb,Parameters,Directory;public int Show;public uint HotKey;public IntPtr Icon;}
    [ComImport,Guid("000214E3-0000-0000-C000-000000000046"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]private interface IShellView
    {
        [PreserveSig]int GetWindow(out IntPtr hwnd);[PreserveSig]int ContextSensitiveHelp(bool enter);
        [PreserveSig]int TranslateAccelerator(IntPtr message);[PreserveSig]int EnableModeless(bool enable);
        [PreserveSig]int UIActivate(uint state);[PreserveSig]int Refresh();
        [PreserveSig]int CreateViewWindow(IntPtr previous,IntPtr settings,IntPtr browser,IntPtr rect,out IntPtr hwnd);
        [PreserveSig]int DestroyViewWindow();[PreserveSig]int GetCurrentInfo(IntPtr settings);
        [PreserveSig]int AddPropertySheetPages(uint reserved,IntPtr callback,IntPtr lparam);
        [PreserveSig]int SaveViewState();[PreserveSig]int SelectItem(IntPtr pidl,uint flags);
        [PreserveSig]int GetItemObject(uint item,ref Guid iid,[MarshalAs(UnmanagedType.Interface)]out object value);
    }
    [ComImport,Guid("000214E4-0000-0000-C000-000000000046"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]private interface IContextMenu
    {
        [PreserveSig]int QueryContextMenu(IntPtr menu,uint index,uint first,uint last,uint flags);
        [PreserveSig]int InvokeCommand(ref InvokeInfo info);
        [PreserveSig]int GetCommandString(UIntPtr id,uint flags,IntPtr reserved,IntPtr text,uint size);
    }
    [ComImport,Guid("000214F4-0000-0000-C000-000000000046"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]private interface IContextMenu2
    {
        [PreserveSig]int QueryContextMenu(IntPtr menu,uint index,uint first,uint last,uint flags);[PreserveSig]int InvokeCommand(ref InvokeInfo info);[PreserveSig]int GetCommandString(UIntPtr id,uint flags,IntPtr reserved,IntPtr text,uint size);
        [PreserveSig]int HandleMenuMsg(uint message,IntPtr w,IntPtr l);
    }
    [ComImport,Guid("BCFCE0A0-EC17-11D0-8D10-00A0C90F2719"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]private interface IContextMenu3
    {
        [PreserveSig]int QueryContextMenu(IntPtr menu,uint index,uint first,uint last,uint flags);[PreserveSig]int InvokeCommand(ref InvokeInfo info);[PreserveSig]int GetCommandString(UIntPtr id,uint flags,IntPtr reserved,IntPtr text,uint size);
        [PreserveSig]int HandleMenuMsg(uint message,IntPtr w,IntPtr l);[PreserveSig]int HandleMenuMsg2(uint message,IntPtr w,IntPtr l,out IntPtr result);
    }
    [ComImport,Guid("7690aa79-f8fc-4615-a327-36f7d18f5d91"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]private interface IDefaultFolderMenuInitialize
    {
        [PreserveSig]int Initialize(IntPtr window,IntPtr callback,IntPtr folder,IntPtr shellFolder,uint count,IntPtr items,IntPtr association,uint keys,IntPtr keyArray);
        [PreserveSig]int SetMenuRestrictions(uint flags);
        [PreserveSig]int GetMenuRestrictions(uint mask,out uint flags);
        [PreserveSig]int SetHandlerClsid(ref Guid clsid);
    }
    [DllImport("user32.dll")]private static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll")]private static extern bool DestroyMenu(IntPtr menu);
    [DllImport("user32.dll")]private static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")]private static extern uint GetMenuItemID(IntPtr menu,int index);
    [StructLayout(LayoutKind.Sequential)]private struct MenuItemInfo{public uint Size,Mask,Type,State,Id;public IntPtr Submenu,Checked,Unchecked;public UIntPtr Data;public IntPtr Text;public uint Count;public IntPtr Bitmap;}
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]private static extern bool GetMenuItemInfo(IntPtr menu,uint item,bool byPosition,ref MenuItemInfo info);
    [DllImport("user32.dll")]private static extern IntPtr GetSubMenu(IntPtr menu,int index);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]private static extern int GetMenuString(IntPtr menu,uint item,StringBuilder text,int size,uint flags);
}
