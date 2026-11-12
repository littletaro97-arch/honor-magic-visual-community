using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
namespace MagicCommunity;
// Fixed vendor routes; never runs submitted bat files or starts services.
internal sealed class NativeHonorServices : IDisposable
{
    internal sealed record Entry(string Id,string Name,string Group,string[] Candidates,string Service="",string Argument="");
    internal static readonly Entry[] Catalog=[
        new("pc-manager","电脑管家","管家",[@"PCManager\PCManager.exe"]),
        new("yoyo-claw","YOYO Claw","YOYO",[@"MagicClaw\Launcher.exe",@"MagicClaw\HnMagicClawUI.exe"]),
        new("yoyo-meeting","YOYO 会议","YOYO",[@"NotifControlUI\MeetingAssistantUI.exe"]),
        new("screenshot","全屏截图","管家",[@"PCManager\Screenshot.exe"]),
        new("touchpad","触控板设置","管家",[@"PCManager\MagicTouchPadSettingUI.exe"]),
        new("hunter-camp","猎人营地","其他",[@"HunterCamp\HUNTERCAMP.exe"]),
        new("eye-care","显示器护眼","其他",[@"HnLcdEnhancement\MonitorManage.exe"]),
        new("magic-visual","Magic 视界","其他",[@"MagicAnimation\MagicVisuals.exe"],"MagicAnimationService"),
        new("magic-text","Magic文本","AI",[@"HNMagicAI\MagicText.exe"]),
        new("spatial-audio","空间音频","AI",[@"HNMagicAI\HnSmartAudioUI.exe"]),
        new("voice-input","语音输入","AI",[@"HNMagicAI\HnVoiceInputUI.exe"]),
        new("smart-search","智慧搜索","AI",[@"HNMagicAI\AISearchUI.exe"]),
        new("screen-share","屏幕共享","管家",[@"PCManager\PCManager.exe"],"PCManager Service Store","para /collaboration"),
        new("pad-extension","平板扩展","管家",[@"PCManager\PCManager.exe"],"PCManager Service Store","para /padExtension"),
        new("screen-cast","投屏","管家",[@"PCManager\PCManager.exe"],"PCManager Service Store","para /screenCast"),
        new("honor-share","荣耀分享","管家",[@"PCManager\PCManager.exe"],"PCManager Service Store","para /honorshare")
    ];
    internal sealed record Status(int Index,string Path,string Reason,bool Available);
    private readonly Action changed;private readonly object gate=new();private Status[] rows=[];private Task? scan;private long lastRead;private bool visible,disposed;
    internal NativeHonorServices(Action changed)=>this.changed=changed;
    internal static string Resolve(Entry entry,IEnumerable<string> roots){
        foreach(string root in roots.Where(Directory.Exists))foreach(string relative in entry.Candidates){string path=Path.Combine(root,"HONOR",relative);if(File.Exists(path))return Path.GetFullPath(path);}return "";
    }
    private static string[] Roots()=>new[]{Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86)}.Where(p=>p.Length>0).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    private static Status Inspect(int index){
        var entry=Catalog[index];string path=Resolve(entry,Roots());bool running=entry.Service.Length==0||Running(entry.Service);string reason=path.Length==0?"未安装":!running?"服务未运行":entry.Argument.Length>0?"路由待验证":"可用";
        return new(index,path,reason,path.Length>0&&running);
    }
    internal void Demand(bool requested){lock(gate){bool entering=requested&&!visible;visible=requested;if(!requested){rows=[];return;}if((entering||rows.Length==0)&&Environment.TickCount64-lastRead>1000)Refresh();}}
    internal void Refresh(){lock(gate){if(disposed||!visible||scan is {IsCompleted:false})return;lastRead=Environment.TickCount64;scan=Task.Run(()=>{
        Status[] next;try{next=Enumerable.Range(0,Catalog.Length).Select(Inspect).ToArray();}catch(Exception e)when(e is not OutOfMemoryException){next=[];}
        lock(gate){if(disposed||!visible)return;rows=next;}changed();
    });}}
    internal object Projection(Func<DesktopItem,string?>? icon){Status[] current;lock(gate)current=rows;return new{loading=current.Length==0,items=current.Select(row=>{
        var e=Catalog[row.Index];string? uri=row.Path.Length>0?icon?.Invoke(new DesktopItem{Name=e.Name,Target=row.Path}):null;
        string? png=uri is not null&&uri.StartsWith("data:image/png;base64,",StringComparison.Ordinal)&&uri.Length<22000?uri[22..]:null;
        return new{index=row.Index,id=e.Id,name=e.Name,group=e.Group,available=row.Available,status=row.Reason,png};
    }).OrderByDescending(e=>e.available).ThenBy(e=>e.index==0?0:1).ThenBy(e=>e.group,StringComparer.Ordinal).ThenBy(e=>e.index).ToArray()};}
    internal static string Launch(int index){
        HonorPcManager.RequireSignedIn();
        if(index<0||index>=Catalog.Length)throw new InvalidDataException();var row=Inspect(index);var entry=Catalog[index];
        if(!row.Available)throw new InvalidOperationException("缺少可调用路径："+row.Reason+(row.Reason=="服务未运行"?"。请先检查荣耀电脑管家及其服务。":"。"));
        var start=new ProcessStartInfo(row.Path){UseShellExecute=true,WorkingDirectory=Path.GetDirectoryName(row.Path)!};
        if(entry.Argument.Length>0)start.ArgumentList.Add(entry.Argument);using var process=Process.Start(start);
        return entry.Argument.Length>0?"已发送管家路由；是否打开对应页面仍需实机确认。":"已请求打开「"+entry.Name+"」。";
    }
    internal static void Hide(AppController controller,string board,string card,int index,bool restore){
        if(index<0||index>=Catalog.Length&&!restore)throw new InvalidDataException();
        controller.Commit(state=>{var target=DesktopCards.Require(state.Boards.Single(b=>b.Id==board),card);if(target.Type!="system.honor-service")throw new LayoutEditException("卡片已变化。");
            var settings=JsonNode.Parse(target.Settings.GetRawText())!.AsObject();var hidden=settings["removed"] is JsonArray a?a.Select(v=>v?.GetValue<string>()??"").Where(v=>Catalog.Any(e=>e.Id==v)).ToHashSet(StringComparer.Ordinal):new HashSet<string>();
            if(restore)hidden.Clear();else hidden.Add(Catalog[index].Id);settings["removed"]=new JsonArray(hidden.Order(StringComparer.Ordinal).Select(v=>(JsonNode?)JsonValue.Create(v)).ToArray());target.Settings=JsonSerializer.SerializeToElement(settings);DesktopCards.ValidateSettings(target.Settings);
        },restore?"已恢复全部荣耀服务入口。":"已从此卡片移除服务入口。");
    }
    private static bool Running(string name){
        IntPtr manager=OpenSCManager(null,null,1);if(manager==IntPtr.Zero)return false;try{IntPtr service=OpenService(manager,name,4);if(service==IntPtr.Zero)return false;try{return QueryServiceStatus(service,out var status)&&status.CurrentState==4;}finally{CloseServiceHandle(service);}}finally{CloseServiceHandle(manager);}
    }
    public void Dispose(){lock(gate){disposed=true;visible=false;rows=[];}}
    [StructLayout(LayoutKind.Sequential)]private struct ServiceStatus{public uint Type,CurrentState,Controls,Win32Exit,ServiceExit,CheckPoint,WaitHint;}
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode)]private static extern IntPtr OpenSCManager(string? machine,string? database,uint access);
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode)]private static extern IntPtr OpenService(IntPtr manager,string name,uint access);
    [DllImport("advapi32.dll")]private static extern bool QueryServiceStatus(IntPtr service,out ServiceStatus status);
    [DllImport("advapi32.dll")]private static extern bool CloseServiceHandle(IntPtr value);
}
