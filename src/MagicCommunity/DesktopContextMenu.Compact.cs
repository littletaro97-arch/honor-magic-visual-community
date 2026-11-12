using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace MagicCommunity;

public sealed partial class DesktopContextMenu
{
    // Windows keeps several ordinary commands in legacy handlers too. Query
    // these individually using the public default-folder menu restriction API,
    // retaining their initialized IContextMenu (state, IDs and invocation).
    // Never manufacture a verb or enable a command from a label alone.
    private sealed record Supplemental(IContextMenu Context,IntPtr Menu,int Position,Entry Row);
    private readonly Dictionary<int,Supplemental> supplemental=[];
    private readonly List<(IContextMenu Context,IntPtr Menu)> supplementalSources=[];
    private static readonly Guid ShortcutHandler=new("00021401-0000-0000-C000-000000000046");
    private const int SupplementalFirst=500000; // Below modern IDs, above all classic menu positions.

    private Entry[] CompactWindowsCommands(string[] selection,Entry[] primary,bool icons)
    {
        var requests=new List<(Guid Class,string[] Verbs)>();
        if(!primary.Any(r=>r.Key=="copyaspath")){
            using var copy=Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\Windows.copyaspath");
            if(Guid.TryParse(copy?.GetValue("VerbHandler") as string,out var copyClass))requests.Add((copyClass,["copyaspath"]));
            else Trace?.Invoke("compact-supplement-unavailable copyaspath registration");
        }
        // A shortcut/folder does not use Open With. Requiring that unrelated
        // registration used to discard its fast menu and query every handler.
        if(selection.Length==1&&!Directory.Exists(selection[0])&&!Path.GetExtension(selection[0]).Equals(".lnk",StringComparison.OrdinalIgnoreCase)&&!primary.Any(r=>r.Key=="openas")){
            using var open=Registry.ClassesRoot.OpenSubKey(@"*\shellex\ContextMenuHandlers\Open With");
            if(Guid.TryParse(open?.GetValue(null) as string,out var openClass))requests.Add((openClass,["openas"]));
            else Trace?.Invoke("compact-supplement-unavailable openas registration");
        }
        if(selection.Any(p=>Path.GetExtension(p).Equals(".lnk",StringComparison.OrdinalIgnoreCase)))requests.Add((ShortcutHandler,["open","runas","opennewwindow"]));
        var added=new List<Entry>();
        foreach(var request in requests)
        {
            IContextMenu? source=null;IntPtr handle=IntPtr.Zero;
            try
            {
                Trace?.Invoke($"compact-supplement-bind-start {request.Class}");
                source=(IContextMenu)ShellDesktop.ContextObjects(selection,typeof(IContextMenu).GUID);
                if(source is not IDefaultFolderMenuInitialize options||ReferenceEquals(source,context)||supplementalSources.Any(s=>ReferenceEquals(s.Context,source)))throw new InvalidOperationException("Windows isolated command menu unavailable.");
                Marshal.ThrowExceptionForHR(options.GetMenuRestrictions(0xffff,out uint current));
                var clsid=request.Class;Marshal.ThrowExceptionForHR(options.SetHandlerClsid(ref clsid));
                Marshal.ThrowExceptionForHR(options.SetMenuRestrictions(current|0x100));
                handle=CreatePopupMenu();if(handle==IntPtr.Zero)throw new System.ComponentModel.Win32Exception();
                // ShellLink delegates to the target's context menu. Restricting
                // this outer object to CLSID_ShellLink alone does NOT restrict
                // that nested query: it can still load every third-party handler.
                // Default-action discovery propagates CMF_DEFAULTONLY instead;
                // keep only the real enabled verbs/IDs returned by Windows.
                uint flags=selection.Length==1?0x890u:0x810u;
                if(request.Class==ShortcutHandler)flags|=0x1; // CMF_DEFAULTONLY
                Trace?.Invoke($"compact-supplement-query-start {request.Class} flags=0x{flags:X}");
                using(var budget=new ShellExplorerCommands.QueryCallBudget(600))Marshal.ThrowExceptionForHR(source.QueryContextMenu(handle,0,1,0x6fff,flags));
                Trace?.Invoke($"compact-supplement-query-done {request.Class}");
                var rows=Read(handle,icons,false,source).Where(r=>r.Verb is not null&&request.Verbs.Contains(r.Verb,StringComparer.OrdinalIgnoreCase)&&!primary.Any(p=>p.Key==r.Key)&&!added.Any(p=>p.Key==r.Key)).ToArray();
                if(rows.Any(r=>r.Submenu))throw new InvalidOperationException("Windows command uses a cascade; retain its full menu.");
                if(rows.Length==0)continue;
                foreach(var row in rows){int id=SupplementalFirst+supplemental.Count;var visible=row with{Index=id};supplemental.Add(id,new(source,handle,row.Index,visible));added.Add(visible);}
                supplementalSources.Add((source,handle));source=null;handle=IntPtr.Zero;
            }
            catch(Exception error)when(ShellExplorerCommands.ProviderUnavailable(error)||error is InvalidOperationException)
            {
                // An optional command must never discard working resource verbs
                // or silently escalate the first page to every installed handler.
                Trace?.Invoke($"compact-supplement-unavailable {request.Class} hr=0x{error.HResult:X8}");
            }
            finally{if(handle!=IntPtr.Zero)DestroyMenu(handle);if(source is not null&&Marshal.IsComObject(source))Marshal.ReleaseComObject(source);}
        }
        return added.ToArray();
    }
    private void InvokeSupplement(int[] path)
    {
        if(path.Length!=1||!supplemental.TryGetValue(path[0],out var offered))throw new InvalidDataException("Menu command expired.");
        var row=Read(offered.Menu,false,false,offered.Context).Single(r=>r.Index==offered.Position);
        if(row.Disabled||row.Separator||row.Submenu||row.Verb!=offered.Row.Verb)throw new InvalidDataException("Menu command changed.");
        if(InvokeSelectedFileCommand(row))return;
        if(targets.Length==1&&ShellOpenWith.IsChooser(row)){ShellOpenWith.Show(targets);return;}
        uint id=GetMenuItemID(offered.Menu,offered.Position);if(id is 0 or >0x6fff)throw new InvalidDataException();
        var info=new InvokeInfo{Size=Marshal.SizeOf<InvokeInfo>(),Mask=0x100,Window=NativeDesktop.FindDesktopListView(),Verb=new IntPtr(id-1),Show=1};
        Marshal.ThrowExceptionForHR(offered.Context.InvokeCommand(ref info));
        if(row.Verb=="copyaspath")FlushClipboard();
    }
    private void ClearSupplements()
    {
        supplemental.Clear();
        foreach(var source in supplementalSources){DestroyMenu(source.Menu);if(Marshal.IsComObject(source.Context))Marshal.ReleaseComObject(source.Context);}
        supplementalSources.Clear();
    }
}
