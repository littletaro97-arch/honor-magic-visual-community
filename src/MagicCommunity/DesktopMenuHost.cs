using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace MagicCommunity;

// Shell extensions stay outside the permanent WPF adapter. A completed query
// worker may be reused briefly to avoid reloading every native extension on
// each click. Each worker has one expiry timer and no idle polling. The native
// menu may request one independent full query after presenting the fast page.
public sealed partial class DesktopMenuHost(string root,bool startSuspended=false,bool reuseQueries=true,Action<string,object>? trace=null) : IDisposable
{
    private const string FullQueryBudgetMessage="Windows 扩展菜单暂未响应，已停止加载。请点击“重试加载更多选项”，或在资源管理器中操作。";
    internal static readonly TimeSpan QueryBudget=TimeSpan.FromSeconds(10);
    private static readonly TimeSpan WarmRetention=TimeSpan.FromSeconds(10);
    private const int MaximumQueriesPerWorker=8;
    private static readonly JsonSerializerOptions Wire=new(){PropertyNamingPolicy=JsonNamingPolicy.CamelCase};
    private ShellMenuFailureBackoff providerFailures=new();
    private readonly SemaphoreSlim gate=new(1);
    private readonly object stateGate=new();
    private readonly CancellationTokenSource lifetime=new();
    private readonly HashSet<string> cancelled=[];
    private readonly Queue<string> order=new();
    private Process? worker;
    private Timer? warmExpiry;
    private long warmGeneration;
    private bool querying;
    private int queries;
    private bool suspended=startSuspended,disposed;
    private long epoch;
    private string? active;
    private int starts;
    private readonly ConcurrentDictionary<int,Process> invoking=new();
    private readonly ConcurrentDictionary<int,StreamReader> responses=new();
    internal bool IsInvokingProcess(uint pid)=>invoking.ContainsKey((int)pid)||prefetchCommands.Keys.Any(host=>host.IsInvokingProcess(pid));
    public long CaptureEpoch(){lock(stateGate)return epoch;}
    public void EnsureEpoch(long expected){lock(stateGate)VerifyEpoch(expected);}
    private void VerifyEpoch(long expected)
    {
        ObjectDisposedException.ThrowIf(disposed,this);
        if(epoch!=expected)throw new OperationCanceledException("Desktop menu lease has expired.");
        if(suspended)throw new OperationCanceledException("Desktop menu host is suspended.");
    }
    private bool IsCurrentEpoch(long expected){lock(stateGate)return !disposed&&!suspended&&epoch==expected;}
    private void CancelRequest(string id){if(cancelled.Add(id)){order.Enqueue(id);if(order.Count>256)cancelled.Remove(order.Dequeue());}}
    public async Task<object> Route(string route,JsonElement body,CancellationToken stopping,string? target=null,string? destination=null,bool folderBackground=false,long? expectedEpoch=null,string[]? targets=null)
    {
        string id=body.TryGetProperty("requestId",out var request)?request.GetString()??throw new InvalidDataException():throw new InvalidDataException();
        if(id.Length!=32||!id.All(Uri.IsHexDigit))throw new InvalidDataException();
        var timing=Stopwatch.StartNew();
        long requestEpoch;
        // Capture before waiting for the serial query gate. A pause/resume can
        // never turn already queued work into a new desktop lease.
        lock(stateGate)
        {
            requestEpoch=expectedEpoch??epoch;VerifyEpoch(requestEpoch);
            if(route=="/menu-dismiss")
            {
                CancelRequest(id);
                if(active==id)
                {
                    CancelPrefetch();
                    if(querying||!reuseQueries||queries>=MaximumQueriesPerWorker)StopWorker();
                    else RetainWorker();
                }
                return new{ok=true};
            }
        }
        string[]? selection=targets is null?null:ShellDesktop.ValidateSelection(targets);
        if(selection is {Length:>1}&&route is not ("/desktop-context-open" or "/desktop-context-prefetch" or "/desktop-properties" or "/desktop-clipboard-command" or "/desktop-clipboard-prepare" or "/desktop-group-drop"))throw new InvalidDataException("此命令需要单个项目。");
        bool fullFileMenu=route=="/desktop-context-open"&&body.TryGetProperty("fullFileMenu",out var full)&&full.GetBoolean();
        if(route=="/desktop-context-prefetch")return await Prefetch(id,target,selection,folderBackground,requestEpoch,stopping).ConfigureAwait(false);
        if(fullFileMenu){var ready=await PrefetchedFull(id,target,selection,folderBackground,requestEpoch,stopping).ConfigureAwait(false);if(ready is not null)return ready;}
        if(body.TryGetProperty("path",out var prefetchedPath)&&prefetchedPath.ValueKind==JsonValueKind.Array&&prefetchedPath.GetArrayLength()>0&&prefetchedPath[0].GetInt32()>=PrefetchIndex)
            return await RoutePrefetched(route,id,body,requestEpoch,stopping).ConfigureAwait(false);
        using var timeout=CancellationTokenSource.CreateLinkedTokenSource(stopping,lifetime.Token);
        // Package activation is not a file transaction: its owned worker must remain cancelable.
        bool command=route is "/desktop-context-native-track" or "/desktop-context-invoke" or "/desktop-file-operation" or "/desktop-create-item" or "/desktop-properties" or "/desktop-clipboard-command" or "/desktop-clipboard-prepare" or "/desktop-import-files" or "/desktop-group-drop";
        // Includes queueing, worker startup, COM bind/query and metadata read.
        // Cancellation cannot interrupt a synchronous third-party COM call;
        // the catch path terminates only this menu's owned worker instead.
        // A full file menu invokes installed handlers synchronously. The UI
        // reports loading immediately and can dismiss this lease at any time.
        // Use the same bounded lifetime as folder/background menus: the former
        // 1.8 s deadline also charged queueing/startup and rejected valid menus.
        if(!command)timeout.CancelAfter(QueryBudget);
        try{await gate.WaitAsync(timeout.Token).ConfigureAwait(false);}
        catch(OperationCanceledException)when(fullFileMenu&&timeout.IsCancellationRequested&&IsCurrentEpoch(requestEpoch)&&!stopping.IsCancellationRequested&&!lifetime.IsCancellationRequested){throw new LayoutEditException(FullQueryBudgetMessage);}
        bool held=true;Process? executing=null,running=null;
        try
        {
            lock(stateGate)
            {
                VerifyEpoch(requestEpoch);
                if(route is "/desktop-jump-list" or "/desktop-jump-open" or "/desktop-sort-properties" or "/desktop-context-open" or "/desktop-file-operation" or "/desktop-create-item" or "/desktop-properties" or "/desktop-clipboard-command" or "/desktop-clipboard-prepare" or "/desktop-import-files" or "/desktop-group-drop")
                {
                    if(cancelled.Contains(id))return new{requestId=id,cancelled=true,compact=Array.Empty<object>(),more=Array.Empty<object>()};
                    // More is a fresh query of the same authenticated lease.
                    // Reuse only initialized code; Open releases the old HMENU
                    // and rebuilds IDs. A different active request still cancels.
                    bool reuse=route=="/desktop-context-open"&&reuseQueries&&(active is null||active==id&&fullFileMenu)&&!querying&&queries<MaximumQueriesPerWorker&&worker is {HasExited:false};
                    CancelPrefetch();
                    CancelRetention();if(!reuse){StopWorker();StartWorker();}active=id;
                    if(route=="/desktop-context-open"){menuTarget=target;menuTargets=selection??[];menuBackground=folderBackground;menuCanPrefetch=false;}
                    if(route=="/desktop-context-open")queries++;
                }
                else if(active!=id||worker is null)throw new InvalidDataException("Menu expired.");
                running=worker??throw new InvalidOperationException();
                querying=!command;
            }
            string op=route switch{"/desktop-jump-list"=>"jump-list","/desktop-jump-open"=>"jump-open","/desktop-sort-properties"=>"sort-properties","/desktop-context-open"=>"open","/desktop-context-items"=>"children","/desktop-context-invoke"=>"invoke","/desktop-context-native-track"=>"track","/desktop-file-operation"=>"file","/desktop-create-item"=>"create","/desktop-properties"=>"properties","/desktop-clipboard-command"=>"clipboard","/desktop-clipboard-prepare"=>"prepare","/desktop-import-files"=>"import","/desktop-group-drop"=>"group",_=>throw new InvalidDataException()};
            int[] path=body.TryGetProperty("path",out var value)?value.EnumerateArray().Select(x=>x.GetInt32()).ToArray():[];
            if(op is "children" or "invoke"&&path.Length is <1 or >16)throw new InvalidDataException();
            string? action=body.TryGetProperty("action",out var actionValue)?actionValue.GetString():null;
            JsonElement? transfer=op is "jump-list" or "jump-open" or "import" or "group" or "track" or "sort-properties"?body.Clone():null;
            if(command)
            {
                // Claim before sending a command to the worker. Once committed,
                // a temporary desktop pause must not kill an actual file move,
                // create operation or user-owned property dialog mid-command.
                lock(stateGate){VerifyEpoch(requestEpoch);if(worker!=running||active!=id)throw new OperationCanceledException();CancelPrefetch();executing=running;invoking[running.Id]=running;worker=null;active=null;}
                gate.Release();held=false;
            }
            double queuedAndStartMs=timing.Elapsed.TotalMilliseconds;
            await running.StandardInput.WriteLineAsync(JsonSerializer.Serialize(new{op,requestId=id,path,target,targets=selection,destination,action,folderBackground,fullFileMenu,transfer,providerBackoff=op=="open"?providerFailures.Export():null},Wire).AsMemory(),timeout.Token).ConfigureAwait(false);
            await running.StandardInput.FlushAsync(timeout.Token).ConfigureAwait(false);
            string line=await responses[running.Id].ReadLineAsync(timeout.Token).ConfigureAwait(false)??throw new IOException("Shell menu worker ended.");
            if(line.Length>2_097_152)throw new InvalidDataException();
            using var response=JsonDocument.Parse(line);
            if(!response.RootElement.GetProperty("ok").GetBoolean()){
                if(response.RootElement.TryGetProperty("cancelled",out var cancelledResult)&&cancelledResult.ValueKind==JsonValueKind.True)throw new OperationCanceledException("Windows 文件操作已取消。");
                int hresult=response.RootElement.TryGetProperty("hresult",out var hr)&&hr.TryGetInt32(out int code)?code:unchecked((int)0x80131509);
                trace?.Invoke("desktop_menu_query_failed",new{operation=op,hresult=$"0x{hresult:X8}",
                    phase=response.RootElement.TryGetProperty("phase",out var phase)?phase.GetString():null,
                    backgroundSource=response.RootElement.TryGetProperty("backgroundSource",out var source)?source.GetInt32():-1,
                    errorType=response.RootElement.TryGetProperty("errorType",out var type)?type.GetString():null,
                    backgroundFailures=response.RootElement.TryGetProperty("backgroundFailures",out var failures)?failures:(JsonElement?)null,
                    pathDepth=path.Length,selectionCount=selection?.Length??(target is null?0:1)});
                throw new QueryFailure(response.RootElement.GetProperty("error").GetString(),hresult);
            }
            if(!command)lock(stateGate){VerifyEpoch(requestEpoch);if(worker!=running||active!=id)throw new OperationCanceledException("Menu expired.");querying=false;}
            if(op=="open"&&response.RootElement.TryGetProperty("providerBackoff",out var failed)&&failed.ValueKind==JsonValueKind.Array)
                providerFailures.Import(failed.Deserialize<ShellMenuFailureBackoff.Failure[]>(Wire)??[]);
            var data=response.RootElement.GetProperty("data").Clone();
            if(op=="open")lock(stateGate){VerifyEpoch(requestEpoch);if(worker!=running||active!=id)throw new OperationCanceledException("Menu expired.");menuCanPrefetch=data.TryGetProperty("optimizedFile",out var eligible)&&eligible.GetBoolean();}
            if(!command)trace?.Invoke("desktop_menu_query",new{operation=op,queuedAndStartMs,workerRoundTripMs=timing.Elapsed.TotalMilliseconds-queuedAndStartMs,totalMs=timing.Elapsed.TotalMilliseconds,pathDepth=path.Length,selectionCount=selection?.Length??(target is null?0:1),stages=data.TryGetProperty("timings",out var stages)?stages:(System.Text.Json.JsonElement?)null});
            else if(op=="group")trace?.Invoke("desktop_group_drop_worker",new{queuedAndStartMs,workerRoundTripMs=timing.Elapsed.TotalMilliseconds-queuedAndStartMs,totalMs=timing.Elapsed.TotalMilliseconds,selectionCount=selection?.Length??0,contentsLogged=false});
            return data;
        }
        catch(OperationCanceledException)when(fullFileMenu&&timeout.IsCancellationRequested&&IsCurrentEpoch(requestEpoch)&&!stopping.IsCancellationRequested&&!lifetime.IsCancellationRequested)
        {
            lock(stateGate){if(epoch==requestEpoch&&worker==running&&active==id)StopWorker();}
            throw new LayoutEditException(FullQueryBudgetMessage);
        }
        catch(Exception error)
        {
            lock(stateGate)
            {
                bool expired=!command&&(epoch!=requestEpoch||worker!=running||active!=id);
                if(epoch==requestEpoch&&worker==running&&active==id)StopWorker();
                if(expired&&error is IOException or InvalidOperationException)throw new OperationCanceledException("Menu expired.",error);
            }
            throw;
        }
        finally
        {
            if(executing is not null){try{executing.StandardInput.Close();await executing.WaitForExitAsync(lifetime.Token).ConfigureAwait(false);}finally{invoking.TryRemove(executing.Id,out _);if(responses.TryRemove(executing.Id,out var reader))reader.Dispose();executing.Dispose();}}
            if(held)gate.Release();
        }
    }
    private void StartWorker()
    {
        if(disposed||suspended||lifetime.IsCancellationRequested)throw new OperationCanceledException();
        using var parent=Process.GetCurrentProcess();
        var pipe=new AnonymousPipeServerStream(PipeDirection.In,HandleInheritability.Inheritable);
        var info=new ProcessStartInfo(Environment.ProcessPath!){UseShellExecute=false,CreateNoWindow=true,RedirectStandardInput=true,RedirectStandardOutput=true,RedirectStandardError=true};
        if(Path.GetFileNameWithoutExtension(info.FileName).Equals("dotnet",StringComparison.OrdinalIgnoreCase))info.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
        foreach(string value in new[]{"--shell-menu-worker","--root",root,"--parent",parent.Id.ToString(),"--parent-created",parent.StartTime.ToFileTimeUtc().ToString(),"--menu-pipe",pipe.GetClientHandleAsString()})info.ArgumentList.Add(value);
        try{worker=Process.Start(info)??throw new InvalidOperationException("Shell menu worker did not start.");_ = DrainOutput(worker.StandardOutput.BaseStream);_ = DrainOutput(worker.StandardError.BaseStream);pipe.DisposeLocalCopyOfClientHandle();responses[worker.Id]=new StreamReader(pipe,Encoding.UTF8);starts++;}
        catch{try{StopWorker();}finally{pipe.Dispose();}throw;}
        if(suspended||lifetime.IsCancellationRequested){StopWorker();throw new OperationCanceledException();}
    }
    private static async Task DrainOutput(Stream stream)
    {
        // Extensions also write unterminated/binary diagnostics. Consume bytes,
        // never lines: inheriting an unread launcher pipe can stall QueryContextMenu.
        // The protocol remains on its separate pipe. No output is retained in RAM.
        try{await stream.CopyToAsync(Stream.Null).ConfigureAwait(false);}
        catch(Exception error)when(error is IOException or ObjectDisposedException){Debug.WriteLine("Shell diagnostic stream ended: "+error.GetType().Name);}
    }
    private void StopWorker()
    {
        CancelRetention();bool interrupt=querying;var old=Interlocked.Exchange(ref worker,null);active=null;querying=false;queries=0;if(old is null)return;
        try
        {
            // A cancelled query may still be writing its request. Closing that
            // StreamWriter would throw before the process could be reclaimed.
            // Interrupt owned in-flight workers directly; completed ones can
            // exit gracefully when their input is closed.
            if(!interrupt)try{old.StandardInput.Close();}catch(IOException){}catch(InvalidOperationException){interrupt=true;}
            if(interrupt?!old.HasExited:!old.WaitForExit(200))
            {
                // Process.Start retained the owned process handle. Never look
                // up a PID/name again, and never terminate a process tree.
                try{old.Kill(entireProcessTree:false);}
                catch(InvalidOperationException)when(old.HasExited){}
                catch(System.ComponentModel.Win32Exception)when(old.HasExited){}
                if(!old.WaitForExit(1000))throw new TimeoutException("The owned Shell menu worker did not exit after cancellation.");
            }
        }
        finally{if(responses.TryRemove(old.Id,out var reader))reader.Dispose();old.Dispose();}
    }
    private void CancelRetention(){warmGeneration++;warmExpiry?.Dispose();warmExpiry=null;}
    private void RetainWorker()
    {
        CancelRetention();
        // This one-way reset has no response frame. The previous query has
        // completed, so the pipe is empty; the next Open is ordered after it.
        // Release selection/menu handles while retaining initialized code.
        try{worker!.StandardInput.WriteLine(JsonSerializer.Serialize(new{op="dismiss",requestId=active},Wire));worker.StandardInput.Flush();}
        catch(IOException){StopWorker();return;}
        active=null;long generation=warmGeneration;
        warmExpiry=new Timer(_=>
        {
            lock(stateGate)
            {
                if(generation!=warmGeneration||active is not null||querying)return;
                try{StopWorker();}
                catch(Exception error)when(error is not OutOfMemoryException){Debug.WriteLine("Shell menu expiry: "+error);}
            }
        },null,WarmRetention,Timeout.InfiniteTimeSpan);
    }
    public void Suspend()
    {
        lock(stateGate){if(disposed)return;suspended=true;epoch++;if(active is not null)CancelRequest(active);CancelPrefetch();StopWorker();}
    }
    public void Resume()
    {
        lock(stateGate){ObjectDisposedException.ThrowIf(disposed,this);if(suspended){epoch++;suspended=false;}}
    }
    public void Dispose()
    {
        Process[] committed;
        lock(stateGate){if(disposed)return;disposed=true;suspended=true;epoch++;if(active is not null)CancelRequest(active);CancelPrefetch();StopWorker();committed=invoking.Values.ToArray();}
        foreach(var host in prefetchCommands.Keys)host.Dispose();
        try{foreach(var process in committed)try{if(!process.HasExited)process.Kill(entireProcessTree:false);}catch(InvalidOperationException){}}
        finally{lifetime.Cancel();}
    }
    public static int RunWorker(int parentId,long created,string responseHandle)
    {
        using var ole=new OleScope();
        if(Environment.GetEnvironmentVariable("MAGIC_MENU_TRACE")=="1")DesktopContextMenu.Trace=phase=>File.AppendAllText(Path.Combine(Path.GetTempPath(),"magic-menu-trace-"+Environment.ProcessId+".log"),DateTime.UtcNow.ToString("O")+" "+phase+Environment.NewLine);
        using var parent=Process.GetProcessById(parentId);
        if(parent.StartTime.ToFileTimeUtc()!=created||parent.HasExited)return 2;
        parent.Exited+=(_,_)=>Environment.Exit(0);parent.EnableRaisingEvents=true;
        using var menu=new DesktopContextMenu();bool opened=false;
        using var reader=new StreamReader(Console.OpenStandardInput(),Encoding.UTF8);
        // Native extensions write arbitrary, sometimes unterminated text to
        // stdout. Keep protocol bytes on a private inherited pipe instead.
        using var reply=new AnonymousPipeClientStream(PipeDirection.Out,responseHandle);
        using var writer=new StreamWriter(reply,new UTF8Encoding(false)){AutoFlush=true};
        while(reader.ReadLine() is { } line)
        {
            try
            {
                if(line.Length>2_097_152)throw new InvalidDataException();using var doc=JsonDocument.Parse(line);var body=doc.RootElement;
                string op=body.GetProperty("op").GetString()!,id=body.GetProperty("requestId").GetString()!;
                object result;
                string? target=body.TryGetProperty("target",out var selected)?selected.GetString():null;
                string[]? targets=body.TryGetProperty("targets",out var selectedMany)&&selectedMany.ValueKind!=JsonValueKind.Null?ShellDesktop.ValidateSelection(selectedMany.EnumerateArray().Select(value=>value.GetString()??throw new InvalidDataException())):null;
                if(op=="dismiss"&&opened){menu.Dismiss(id);opened=false;continue;}
                // Open always releases the previous HMENU/COM selection and
                // builds fresh command IDs. Only loaded extension code is reused.
                if(op=="jump-list"&&!opened)result=NativeJumpLists.Read(body.GetProperty("transfer"));
                else if(op=="jump-open"&&!opened)result=NativeJumpLists.OpenPackage(body.GetProperty("transfer"),body.GetProperty("destination").GetString()!);
                else if(op=="sort-properties"&&!opened)result=DesktopSort.ReadProperties(body.GetProperty("transfer"));
                else if(op=="open"){
                    if(body.TryGetProperty("providerBackoff",out var failed)&&failed.ValueKind==JsonValueKind.Array)ShellExplorerCommands.Failures.Import(failed.Deserialize<ShellMenuFailureBackoff.Failure[]>(Wire)??[]);
                    opened=true;result=menu.Open(id,target,body.TryGetProperty("folderBackground",out var background)&&background.GetBoolean(),body.TryGetProperty("fullFileMenu",out var full)&&full.GetBoolean(),targets);}
                else if(op=="children"&&opened){var clock=Stopwatch.StartNew();var items=menu.Children(id,body.GetProperty("path").EnumerateArray().Select(x=>x.GetInt32()).ToArray());result=new{items,timings=new{childrenMs=clock.Elapsed.TotalMilliseconds}};}
                else if(op=="track"&&opened){using var commandLifetime=new ShellCommandLifetime();var point=body.GetProperty("transfer");result=menu.TrackNative(id,point.GetProperty("x").GetInt32(),point.GetProperty("y").GetInt32());}
                else if(op=="invoke"&&opened){using(var commandLifetime=new ShellCommandLifetime())menu.Invoke(id,body.GetProperty("path").EnumerateArray().Select(x=>x.GetInt32()).ToArray());result=new{ok=true,createdPath=menu.CreatedPath};}
                else if(op=="file"&&!opened)result=DesktopFileChanges.Execute(body.GetProperty("action").GetString()!,target!,body.GetProperty("destination").GetString());
                else if(op=="create"&&!opened){string action=body.GetProperty("action").GetString()!;if(action is not ("folder" or "text"))throw new InvalidDataException();string? createdPath=ShellFileMove.Create(target!,action=="folder");result=new{ok=true,createdPath};}
                else if(op=="properties"&&!opened){using var commandLifetime=new ShellCommandLifetime();ShellProperties.Show(targets??[target??throw new InvalidDataException()]);result=new{ok=true};}
                else if(op is "clipboard" or "prepare"&&!opened){using var commandLifetime=new ShellCommandLifetime();result=op=="clipboard"&&body.GetProperty("action").GetString()=="undo"?menu.Undo(id):ShellClipboardCommands.Execute(body.GetProperty("action").GetString()!,target,op=="prepare",targets);}
                else if(op=="group"&&!opened){var transfer=body.GetProperty("transfer");using var commandLifetime=new ShellCommandLifetime();result=ShellClipboardCommands.TransferSelection(targets??throw new InvalidDataException(),body.GetProperty("destination").GetString()!,transfer.GetProperty("control").GetBoolean(),transfer.GetProperty("shift").GetBoolean());}
                else if(op=="import"&&!opened){var transfer=body.GetProperty("transfer");result=ShellClipboardCommands.TransferFiles(transfer.GetProperty("paths").EnumerateArray().Select(p=>p.GetString()!).ToArray(),target!,transfer.GetProperty("control").GetBoolean(),transfer.GetProperty("shift").GetBoolean());}
                else throw new InvalidDataException();
                writer.WriteLine(JsonSerializer.Serialize(new{ok=true,data=result,providerBackoff=op=="open"?ShellExplorerCommands.Failures.Export():null},Wire));
                if(op is "jump-open" or "track" or "invoke" or "file" or "create" or "properties" or "clipboard" or "prepare" or "import" or "group")return 0;
            }
            catch(Exception error){DesktopContextMenu.Trace?.Invoke(error.ToString());writer.WriteLine(JsonSerializer.Serialize(new{ok=false,error=error.Message,hresult=error.HResult,errorType=error.GetType().Name,phase=menu.QueryStage,backgroundSource=menu.BackgroundSource,backgroundFailures=menu.BackgroundFailures,cancelled=error is OperationCanceledException||error.HResult==unchecked((int)0x80270000)||error.HResult==unchecked((int)0x800704C7)},Wire));return 1;}
        }
        return 0;
    }
    private sealed class QueryFailure:InvalidOperationException
    {
        internal QueryFailure(string? message,int hresult):base(message){HResult=hresult;}
    }
    public static async Task<object> TestContract(string root)
    {
        using var host=new DesktopMenuHost(root,reuseQueries:false);var checks=new List<object>();
        void Check(string name,bool pass){if(!pass)throw new InvalidDataException(name);checks.Add(new{name,pass});}
        async Task<JsonElement> Call(string route,string id,int[]? path=null){using var doc=JsonDocument.Parse(JsonSerializer.Serialize(new{requestId=id,path=path??[]}));return JsonSerializer.SerializeToElement(await host.Route(route,doc.RootElement,CancellationToken.None),Wire);}
        string id=Guid.NewGuid().ToString("N");var first=await Call("/desktop-context-open",id);
        Check("six real primary commands and Paste from Windows",first.GetProperty("compact").GetArrayLength()==7&&first.GetProperty("compact").EnumerateArray().Any(row=>row.GetProperty("key").GetString()=="paste"));
        var refresh=first.GetProperty("compact").EnumerateArray().Single(row=>row.GetProperty("key").GetString()=="refresh").GetProperty("index").GetInt32();
        var view=await Call("/desktop-context-items",id,[0]);Check("lazy Shell View submenu",view.GetProperty("items").GetArrayLength()>0);
        var old=host.worker!;int oldPid=old.Id;await Call("/desktop-context-invoke",id,[refresh]);
        Check("native Refresh executes and transient worker exits",host.worker is null&&!ProcessExists(oldPid));
        string second=Guid.NewGuid().ToString("N");await Call("/desktop-context-open",second);await Call("/menu-dismiss",id);
        Check("old cancellation cannot close newer worker",host.active==second&&host.worker is {HasExited:false});
        await Call("/menu-dismiss",second);int starts=host.starts;var late=await Call("/desktop-context-open",second);
        Check("cancelled late open starts no worker",late.GetProperty("cancelled").GetBoolean()&&host.starts==starts);
        uint gdi=GetGuiResources(new IntPtr(-1),0),user=GetGuiResources(new IntPtr(-1),1);
        for(int i=0;i<20;i++){string next=Guid.NewGuid().ToString("N");await Call("/desktop-context-open",next);int pid=host.worker!.Id;await Call("/menu-dismiss",next);Check("worker reclaimed cycle "+i,!ProcessExists(pid));}
        uint gdiAfter=GetGuiResources(new IntPtr(-1),0),userAfter=GetGuiResources(new IntPtr(-1),1);
        Check("parent GDI and USER handles stable",gdiAfter<=gdi+2&&userAfter<=user+2);
        Check("no worker remains while menu is closed",host.worker is null);
        return new{allPass=true,checks,workerStarts=host.starts,gdiBefore=gdi,gdiAfter,userBefore=user,userAfter,scope="Live transient Shell workers and Refresh only; no file moves or desktop visual acceptance"};
    }
    public static async Task<object> TestFileQueries(string root)
    {
        string fixture=Path.Combine(root,"artifacts","menu-query-fixtures",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(fixture);
        string text=Path.Combine(fixture,"owned.txt"),shortcut=Path.Combine(fixture,"owned.lnk");File.WriteAllText(text,"Owned query fixture; no commands are invoked.");
        // Create a shortcut only inside the fixture; never launch its target.
        dynamic shell=Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell")!)!;object? link=null;
        try{dynamic created=shell.CreateShortcut(shortcut);link=created;created.TargetPath=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"notepad.exe");created.Save();}
        finally{if(link is not null)Marshal.ReleaseComObject(link);Marshal.ReleaseComObject(shell);}
        using var host=new DesktopMenuHost(root,reuseQueries:false);var checks=new List<object>();var timings=new List<object>();
        void Check(string name,bool pass){if(!pass)throw new InvalidDataException(name);checks.Add(new{name,pass});}
        JsonElement Body(string id,bool full=true)=>JsonSerializer.SerializeToElement(new{requestId=id,fullFileMenu=full});
        async Task<JsonElement> Open(string id,string target,CancellationToken token=default)
        {
            var clock=Stopwatch.StartNew();var result=JsonSerializer.SerializeToElement(await host.Route("/desktop-context-open",Body(id),token,target:target),Wire);
            if(result.GetProperty("cancelled").GetBoolean())return result;
            timings.Add(new{extension=Path.GetExtension(target),milliseconds=clock.ElapsedMilliseconds,full=result.GetProperty("fullFileMenu").GetBoolean(),rows=result.GetProperty("more").GetArrayLength()});return result;
        }
        Task<object> Dismiss(string id)=>host.Route("/menu-dismiss",Body(id),CancellationToken.None);
        bool HasProperties(JsonElement result)=>result.GetProperty("more").EnumerateArray().Any(row=>row.GetProperty("key").GetString()=="properties");
        string textId=Guid.NewGuid().ToString("N");var first=await Open(textId,text);
        Check("full text menu retains real Shell commands",HasProperties(first)&&first.GetProperty("fullFileMenu").GetBoolean());
        await Dismiss(textId);Check("dismiss reclaims the owned query worker",host.worker is null);
        string linkId=Guid.NewGuid().ToString("N");var linked=await Open(linkId,shortcut);
        Check("full application shortcut menu completes",HasProperties(linked)&&linked.GetProperty("fullFileMenu").GetBoolean());
        await Dismiss(linkId);
        string queuedId=Guid.NewGuid().ToString("N");Task<JsonElement> queued;
        await host.gate.WaitAsync();
        try{queued=Open(queuedId,shortcut);await Task.Delay(2100);Check("valid full query survives queueing beyond the old 1.8 second cutoff",!queued.IsCompleted);}
        finally{host.gate.Release();}
        Check("queued full query returns usable commands",HasProperties(await queued));
        int queuedPid=host.worker!.Id;await Dismiss(queuedId);
        Check("completed full query leaves no worker after dismissal",host.worker is null&&!ProcessExists(queuedPid));
        using(var cancel=new CancellationTokenSource())
        {
            var interrupted=Open(Guid.NewGuid().ToString("N"),shortcut,cancel.Token);int pid=host.worker!.Id;cancel.Cancel();
            try{await interrupted;throw new InvalidDataException("cancelled query succeeded");}catch(OperationCanceledException){}
            Check("caller cancellation reclaims its synchronous Shell worker",host.worker is null&&!ProcessExists(pid));
        }
        await host.gate.WaitAsync();int startsBeforeTimeout=host.starts;
        try
        {
            using var safety=new CancellationTokenSource(QueryBudget+TimeSpan.FromSeconds(3));
            try{await Open(Guid.NewGuid().ToString("N"),shortcut,safety.Token);throw new InvalidDataException("query deadline was not enforced");}
            catch(LayoutEditException error){Check("full query still has a bounded retryable deadline",error.Message==FullQueryBudgetMessage&&!safety.IsCancellationRequested);}
        }
        finally{host.gate.Release();}
        Check("expired queued request starts no extra worker",host.worker is null&&host.starts==startsBeforeTimeout);
        string retryId=Guid.NewGuid().ToString("N");Check("new lease succeeds after cancellation",HasProperties(await Open(retryId,text)));
        await Dismiss(queuedId);Check("old dismissal cannot close the replacement lease",host.active==retryId&&host.worker is {HasExited:false});
        await Dismiss(retryId);int before=host.starts;var late=await Open(retryId,text);
        Check("cancelled late open cannot restart a worker",late.GetProperty("cancelled").GetBoolean()&&host.starts==before);
        return new{allPass=true,checks,timings,queryBudgetMilliseconds=QueryBudget.TotalMilliseconds,fixture,scope="Owned text/shortcut files and transient queries only; no Shell commands, clipboard, desktop state or Explorer Refresh invoked."};
    }
    public static async Task<object> TestWarmQueries(string root,string? source=null)
    {
        string fixture=Path.Combine(root,"artifacts","menu-warm-fixtures",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(fixture);
        string target=Path.Combine(fixture,source is null?"owned.txt":"owned.lnk");
        if(source is null)File.WriteAllText(target,"Owned warm menu fixture.");
        else{if(!Path.GetExtension(source).Equals(".lnk",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Expected a shortcut fixture.");File.Copy(source,target);}
        using var host=new DesktopMenuHost(root);var checks=new List<object>();var timings=new List<object>();
        void Check(string name,bool pass){if(!pass)throw new InvalidDataException(name);checks.Add(new{name,pass});}
        JsonElement Body(string id)=>JsonSerializer.SerializeToElement(new{requestId=id,fullFileMenu=true});
        async Task<JsonElement> Open(string id)
        {
            var clock=Stopwatch.StartNew();var response=JsonSerializer.SerializeToElement(await host.Route("/desktop-context-open",Body(id),CancellationToken.None,target:target),Wire);
            timings.Add(new{milliseconds=clock.Elapsed.TotalMilliseconds,rows=response.GetProperty("more").GetArrayLength(),pid=host.worker!.Id});return response;
        }
        Task<object> Dismiss(string id)=>host.Route("/menu-dismiss",Body(id),CancellationToken.None);
        string firstId=Guid.NewGuid().ToString("N");var first=await Open(firstId);int firstPid=host.worker!.Id;
        await Dismiss(firstId);Check("completed query retains exactly one temporary worker",host.worker is {HasExited:false}&&host.active is null&&!host.querying&&host.responses.Count==1);
        try{await host.Route("/desktop-context-items",JsonSerializer.SerializeToElement(new{requestId=firstId,path=new[]{0}}),CancellationToken.None);throw new InvalidDataException("closed lease accepted command path");}catch(InvalidDataException error)when(error.Message=="Menu expired."){}
        Check("closed lease cannot use retained command IDs",host.worker?.Id==firstPid);
        for(int n=0;n<3;n++)
        {
            string id=Guid.NewGuid().ToString("N");var response=await Open(id);
            // Installed extensions may add/remove rows after their first query.
            // Verify a fresh usable lease rather than a frozen extension count.
            Check("warm query rebuilds full native menu with a new lease "+n,host.worker?.Id==firstPid&&host.starts==1&&response.GetProperty("requestId").GetString()==id&&response.GetProperty("more").EnumerateArray().Any(row=>row.GetProperty("key").GetString()=="properties")&&response.GetProperty("fullFileMenu").GetBoolean());
            await Dismiss(id);await Dismiss(firstId);Check("old dismissal cannot evict a newer retained query "+n,host.worker?.Id==firstPid);
        }
        var retained=host.worker!;retained.Refresh();long workingSet=retained.WorkingSet64,privateBytes=retained.PrivateMemorySize64;var cpu=retained.TotalProcessorTime;var idle=Stopwatch.StartNew();
        await Task.Delay(400);retained.Refresh();double idleCpuMs=(retained.TotalProcessorTime-cpu).TotalMilliseconds,idleSampleMs=idle.Elapsed.TotalMilliseconds;
        cpu=retained.TotalProcessorTime;idle.Restart();await Task.Delay(2000);retained.Refresh();double settledIdleCpuMs=(retained.TotalProcessorTime-cpu).TotalMilliseconds,settledIdleSampleMs=idle.Elapsed.TotalMilliseconds;
        uint gdi=GetGuiResources(retained.Handle,0),user=GetGuiResources(retained.Handle,1);
        int remaining=MaximumQueriesPerWorker-host.queries;
        for(int n=0;n<remaining;n++){string id=Guid.NewGuid().ToString("N");await Open(id);await Dismiss(id);}
        Check("native allocations are bounded by the per-worker query limit",host.worker is null&&!ProcessExists(firstPid)&&host.starts==1);
        string expiringId=Guid.NewGuid().ToString("N");await Open(expiringId);int expiringPid=host.worker!.Id;await Dismiss(expiringId);
        await Task.Delay(WarmRetention+TimeSpan.FromMilliseconds(1400));
        Check("quiet worker expires without further input or polling",host.worker is null&&!ProcessExists(expiringPid)&&host.warmExpiry is null);
        string pendingId=Guid.NewGuid().ToString("N");var pending=Open(pendingId);int pendingPid=host.worker!.Id;await Dismiss(pendingId);
        try{await pending;throw new InvalidDataException("dismissed pending query succeeded");}catch(Exception error)when(error is IOException or OperationCanceledException or ObjectDisposedException){}
        Check("in-flight cancellation kills rather than retains its worker",host.worker is null&&!ProcessExists(pendingPid));
        string suspendedId=Guid.NewGuid().ToString("N");await Open(suspendedId);int suspendedPid=host.worker!.Id;await Dismiss(suspendedId);host.Suspend();
        Check("desktop suspension immediately reclaims retained workers",host.worker is null&&!ProcessExists(suspendedPid)&&host.warmExpiry is null);
        host.Resume();string resumedId=Guid.NewGuid().ToString("N");await Open(resumedId);int resumedPid=host.worker!.Id;await Dismiss(resumedId);host.Dispose();
        Check("host disposal reclaims worker and expiry timer",host.worker is null&&!ProcessExists(resumedPid)&&host.warmExpiry is null);
        return new{allPass=true,checks,timings,warmRetentionMilliseconds=WarmRetention.TotalMilliseconds,maximumQueriesPerWorker=MaximumQueriesPerWorker,workingSetBytes=workingSet,privateBytes,idleCpuMs,idleSampleMs,settledIdleCpuMs,settledIdleSampleMs,gdi,user,fixture,scope="Owned text or copied shortcut fixture. Full menu queries only; no native commands, clipboard writes or desktop state changes."};
    }
    private static bool ProcessExists(int pid){try{using var p=Process.GetProcessById(pid);return !p.HasExited;}catch(ArgumentException){return false;}}
    private sealed class OleScope:IDisposable{public OleScope()=>Marshal.ThrowExceptionForHR(OleInitialize(IntPtr.Zero));public void Dispose()=>OleUninitialize();}
    [DllImport("ole32.dll")]private static extern int OleInitialize(IntPtr reserved);
    [DllImport("ole32.dll")]private static extern void OleUninitialize();
    [DllImport("user32.dll")]private static extern uint GetGuiResources(IntPtr process,uint flags);
}
