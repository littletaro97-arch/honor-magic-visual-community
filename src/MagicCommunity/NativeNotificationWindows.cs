using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

// Bounded event history; notification clicks do not start an app when an
// existing identified window merely failed to activate.
internal sealed class NativeNotificationWindows
{
    internal static bool HasSource(JsonElement provider,string id,string appId) =>
        id.Length is >0 and <=128 && appId.Length is >0 and <=512 &&
        Array(provider,"notifications").Any(n=>Text(n,"id")==id&&Text(n,"appId")==appId);
    private readonly Dictionary<string,long> recent = new(StringComparer.Ordinal);
    private long sequence;
    private readonly object gate = new();
    internal void Observe(JsonElement snapshot)
    {
        var windows = Array(snapshot,"windows").Take(512).ToArray();
        lock(gate)
        {
            var alive = windows.Select(w=>Text(w,"id")).ToHashSet(StringComparer.Ordinal);
            foreach(string id in recent.Keys.Where(id=>!alive.Contains(id)).ToArray())recent.Remove(id);
            foreach(var window in windows.Where(w=>Flag(w,"focused")))recent[Text(window,"id")]=++sequence;
        }
    }
    private static string PathKey(string path)=>path.Replace('/','\\').TrimEnd('\\').ToUpperInvariant();
    internal static bool IsQq(string appId,IEnumerable<string> targets) =>
        appId.Equals("QQ",StringComparison.OrdinalIgnoreCase)||appId.Equals("QQ.exe",StringComparison.OrdinalIgnoreCase)||
        targets.Any(path=>System.IO.Path.IsPathFullyQualified(path)&&System.IO.Path.GetFileName(path).Equals("QQ.exe",StringComparison.OrdinalIgnoreCase));
    internal static string[] ResidentPaths(IEnumerable<string> targets)=>targets.Where(p=>System.IO.Path.IsPathFullyQualified(p)&&System.IO.Path.GetExtension(p).Equals(".exe",StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    // Explorer and Windows brokers can own tray icons for unrelated features.
    // Their executable identity alone cannot identify a resident application.
    // Keep normal window activation and explicit clicks on those tray icons.
    private static bool SharedResidentHost(string executable)
    {
        if(!System.IO.Path.IsPathFullyQualified(executable))return false;
        string windows=Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if(windows.Length==0)return false;
        try {
            string path=PathKey(System.IO.Path.GetFullPath(executable));
            if(path==PathKey(System.IO.Path.Combine(windows,"explorer.exe")))return true;
            foreach(string folder in new[]{"System32","SysWOW64"})
            foreach(string name in new[]{"rundll32.exe","dllhost.exe","svchost.exe","ApplicationFrameHost.exe","RuntimeBroker.exe"})
                if(path==PathKey(System.IO.Path.Combine(windows,folder,name)))return true;
        }
        catch(ArgumentException){}
        catch(NotSupportedException){}
        return false;
    }
    internal static bool CanRestoreResident(string appId,IEnumerable<string> targets){
        var paths=ResidentPaths(targets);
        if(paths.Length>0)return paths.Any(path=>!SharedResidentHost(path));
        return appId.Length is >0 and <=128&&appId.IndexOfAny(['!','\\','/',':','"','\0'])<0;
    }
    internal static bool ResidentDockLaunch(string target,JsonElement arguments)=>
        !SharedResidentHost(target)&&(target.Length==0||System.IO.Path.GetExtension(target).Equals(".exe",StringComparison.OrdinalIgnoreCase)||System.IO.Path.GetExtension(target).Equals(".lnk",StringComparison.OrdinalIgnoreCase))&&
        (arguments.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null||arguments.ValueKind==JsonValueKind.String&&string.IsNullOrWhiteSpace(arguments.GetString())||arguments.ValueKind==JsonValueKind.Array&&arguments.GetArrayLength()==0);
    internal static bool MatchesResident(string appId,IEnumerable<string> targets,string executable){
        if(!System.IO.Path.IsPathFullyQualified(executable)||SharedResidentHost(executable))return false;
        var paths=ResidentPaths(targets);
        if(paths.Length>0)return paths.Any(p=>PathKey(p)==PathKey(executable));
        return CanRestoreResident(appId,[])&&(appId.Equals(System.IO.Path.GetFileName(executable),StringComparison.OrdinalIgnoreCase)||appId.Equals(System.IO.Path.GetFileNameWithoutExtension(executable),StringComparison.OrdinalIgnoreCase));
    }
    internal JsonElement Find(string appId,JsonElement snapshot,IEnumerable<string> executableTargets,Func<JsonElement,string>? windowAppId=null)
    {
        var targets=executableTargets.Where(p=>!string.IsNullOrWhiteSpace(p)).Select(PathKey).ToHashSet(StringComparer.Ordinal);
        bool Matches(JsonElement window)
        {
            string path=Text(window,"executable"),name=System.IO.Path.GetFileName(path);
            return appId.Length>0&&appId.Equals(Text(window,"aumid"),StringComparison.OrdinalIgnoreCase)||
                path.Length>0&&(targets.Contains(PathKey(path))||targets.Count==0&&appId.Length>0&&(appId.Equals(name,StringComparison.OrdinalIgnoreCase)||appId.Equals(System.IO.Path.GetFileNameWithoutExtension(name),StringComparison.OrdinalIgnoreCase)))||
                appId.Length>0&&windowAppId is not null&&appId.Equals(windowAppId(window),StringComparison.OrdinalIgnoreCase);
        }
        Dictionary<string,long> order;lock(gate)order=new(recent,StringComparer.Ordinal);
        return Array(snapshot,"windows").Take(512).Where(Matches)
            .OrderByDescending(w=>Flag(w,"focused")).ThenByDescending(w=>order.GetValueOrDefault(Text(w,"id")))
            .ThenBy(w=>Flag(w,"minimized")).ThenBy(w=>Text(w,"id"),StringComparer.Ordinal).FirstOrDefault();
    }
    internal static string? QqTrayIcon(JsonElement tray,IEnumerable<string> executableTargets)
        => ResidentTrayIcon("QQ",tray,executableTargets);
    internal static string? ResidentTrayIcon(string appId,JsonElement tray,IEnumerable<string> executableTargets)
    {
        var ids=ResidentTrayIds(appId,tray,executableTargets);
        return ids.Length==1?ids[0]:null;
    }
    internal static string[] ResidentTrayIds(string appId,JsonElement tray,IEnumerable<string> executableTargets)
    {
        var paths=executableTargets.ToArray();
        return Array(tray,"icons").Where(icon=>MatchesResident(appId,paths,Text(icon,"executable")))
            .Select(icon=>Text(icon,"id")).Where(id=>id.Length>0).Distinct(StringComparer.Ordinal).Take(2).ToArray();
    }
    internal async Task Open(string appId,JsonElement snapshot,IEnumerable<string> executableTargets,Func<JsonElement,Task> activate,Action<string> launch,Func<JsonElement,string>? windowAppId=null,Func<Task<bool>>? activateResidentQq=null)
    {
        if(appId.Length>512||appId.Length==0&&!executableTargets.Any(System.IO.Path.IsPathFullyQualified)||appId.Contains('"')||appId.Contains('\0'))throw new System.IO.InvalidDataException("Application identity differs.");
        var target=Find(appId,snapshot,executableTargets,windowAppId);
        if(target.ValueKind==JsonValueKind.Object){await activate(target);return;}
        if(CanRestoreResident(appId,executableTargets)&&activateResidentQq is not null&&await activateResidentQq())return;
        launch(appId);
    }
    // Bounded observation after a user action only. No background polling, no
    // second launch, and never manufacture success from a posted tray callback.
    internal async Task<JsonElement> WaitForWindow(string appId,IEnumerable<string> targets,
        Func<Task<JsonElement>> snapshot,Func<int,Task> delay,Func<JsonElement,string>? windowAppId=null)
    {
        var paths=targets.ToArray();var timer=System.Diagnostics.Stopwatch.StartNew();
        for(int attempt=0;attempt<8;attempt++){
            if(attempt>0)await delay(150);
            var remaining=TimeSpan.FromMilliseconds(1600)-timer.Elapsed;
            if(remaining<=TimeSpan.Zero)break;
            JsonElement current;
            try {current=await snapshot().WaitAsync(remaining);}catch(TimeoutException){break;}
            var target=Find(appId,current,paths,windowAppId);
            if(target.ValueKind==JsonValueKind.Object)return target;
        }
        return default;
    }
    internal static async Task<JsonElement> RestoreTrayWindow(Func<string,Task<bool>> gesture,Func<Task<JsonElement>> observe)
    {
        // A single click and a double-click are distinct tray contracts. Give
        // the ordinary handler time to expose its window before trying the
        // double-click handler once. Never send it after a window was observed.
        foreach(string action in new[]{"activate","double-click"}){
            if(!await gesture(action))break;
            var window=await observe();
            if(window.ValueKind==JsonValueKind.Object)return window;
        }
        return default;
    }
    // Unpackaged apps can assign an AUMID to a window rather than to the process.
    // Query only on a notification click, never on the move/idle event path.
    internal static string WindowAppId(JsonElement window)
    {
        if(!long.TryParse(Text(window,"hwnd"),NumberStyles.None,CultureInfo.InvariantCulture,out long value))return "";
        var hwnd=new IntPtr(value);GetWindowThreadProcessId(hwnd,out uint pid);
        if(pid==0||pid!=(uint)Number(window,"pid"))return "";
        IPropertyStore? store=null;PropertyValue property=default;
        try
        {
            var iid=typeof(IPropertyStore).GUID;
            if(SHGetPropertyStoreForWindow(hwnd,ref iid,out store)<0||store is null)return "";
            var key=new PropertyKey(new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"),5);
            if(store.GetValue(ref key,out property)<0||property.Type!=31)return "";
            string id=Marshal.PtrToStringUni(property.Pointer,Math.Min(513,Length(property.Pointer)))??"";
            return id.Length<=512?id:"";
        }
        catch(COMException){return "";}
        finally{PropVariantClear(ref property);if(store is not null&&Marshal.IsComObject(store))Marshal.ReleaseComObject(store);}
    }
    private static int Length(IntPtr value){if(value==IntPtr.Zero)return 0;int count=0;while(count<513&&Marshal.ReadInt16(value,count*2)!=0)count++;return count;}
    [StructLayout(LayoutKind.Sequential)]private struct PropertyKey(Guid format,uint id){public Guid Format=format;public uint Id=id;}
    [StructLayout(LayoutKind.Explicit,Size=24)]private struct PropertyValue{[FieldOffset(0)]public ushort Type;[FieldOffset(8)]public IntPtr Pointer;}
    [ComImport,Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore
    {
        [PreserveSig]int GetCount(out uint count);
        [PreserveSig]int GetAt(uint index,out PropertyKey key);
        [PreserveSig]int GetValue(ref PropertyKey key,out PropertyValue value);
        [PreserveSig]int SetValue(ref PropertyKey key,ref PropertyValue value);
        [PreserveSig]int Commit();
    }
    [DllImport("shell32.dll")]private static extern int SHGetPropertyStoreForWindow(IntPtr hwnd,ref Guid iid,out IPropertyStore store);
    [DllImport("ole32.dll")]private static extern int PropVariantClear(ref PropertyValue value);
    [DllImport("user32.dll")]private static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
}
