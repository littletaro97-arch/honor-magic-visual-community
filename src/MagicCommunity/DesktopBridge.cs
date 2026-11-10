using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Media.Imaging;

namespace MagicCommunity;

// Loopback-only, per-session authenticated adapter. No arbitrary file reading or shell command endpoint.
public sealed class DesktopBridge : IDisposable
{
    public const string WidgetId = "@magic-community/desktop";
    private static readonly JsonSerializerOptions Wire = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    private static readonly HashSet<string> Origins = ["http://tauri.localhost", "https://tauri.localhost", "tauri://localhost", "http://asset.localhost", "https://asset.localhost"];
    private readonly AppController controller;
    private readonly DesktopCardCatalog systemCardCatalog=DesktopCardCatalog.Load(Path.Combine(AppContext.BaseDirectory,"desktop-widget","card-catalog.json"));
    private DesktopCardCatalog cardCatalog=new([]);
    private readonly CustomCardLibrary customCards;
    private long cardCatalogVersion;
    private void RefreshCardCatalog()
    {
        var next=new DesktopCardCatalog(systemCardCatalog.Types.Concat(customCards.Scan()).Take(DesktopCardCatalog.MaximumTypes));
        if(JsonSerializer.Serialize(cardCatalog.Types,Wire)==JsonSerializer.Serialize(next.Types,Wire))return;
        cardCatalog=next;cardCatalogVersion++;changes.Notify();NativeWorkspaceChanged?.Invoke();controller.Log("custom_card_catalog_changed",new{version=cardCatalogVersion,types=next.Types.Count});
    }
    private readonly Action showSettings, showAbout, showFeedback, quit;
    private readonly TcpListener listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource stopping = new();
    private readonly SemaphoreSlim clients = new(32);
    private readonly string token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
    private readonly Dictionary<string, string> monitorBoards = new(StringComparer.Ordinal);
    private readonly Dictionary<string, object> ready = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string?> iconCache = new(StringComparer.Ordinal);
    private readonly string sessionId = Guid.NewGuid().ToString("N");
    private readonly DesktopChanges changes=new();
    private readonly DesktopIcons icons;
    private readonly NativeDesktopTracker nativeDesktop;
    private readonly RecycleBinWatch recycleBin;
    private readonly DesktopDirectoryWatch? directoryWatch;
    private readonly Dictionary<string,double> boardScales=new(StringComparer.Ordinal);
    private readonly Dictionary<string,long> widgetWindows=new(StringComparer.Ordinal);
    private readonly Dictionary<string,(double X,double Y)> boardScroll=new(StringComparer.Ordinal);
    private NativeDesktopLease? nativeLease;
    private string? currentTopology;
    private long leaseGeneration;
    private long workspaceRevision;
    private string currentWorkspace;
    private bool deferredWorkspaceSignal;
    private readonly DesktopWorkAreaWatch? workAreaWatch;
    private readonly DesktopMenuHost contextMenu;
    private readonly SemaphoreSlim fileOperations=new(1);
    private readonly SemaphoreSlim clipboardCommands=new(1);
    public bool HasLostTakeover { get; private set; }
    private int eventStreamClients;
    private long eventStreamNotifications,eventStreamHeartbeats,gridStateReads;
    private readonly Dictionary<string,long> dropEvents=new(StringComparer.Ordinal);
    public int Port => ((IPEndPoint)listener.LocalEndpoint).Port;
    public string SessionPath => Path.Combine(controller.Root, "runtime", "desktop", "session.json");
    internal CompositionPopupSession? NativePopups {get;set;}
    internal CompositionShellSession? NativeShell {get;set;}
    internal Func<JsonElement,CancellationToken,Task<object>>? NativeProbe {get;set;}
    private int nativeRendererPid;
    internal void NativeRendererStarted(int pid)
    {
        if(!compositionRenderer)return;
        nativeRendererPid=pid;
        using var document=JsonDocument.Parse(File.ReadAllBytes(SessionPath));var value=document.RootElement.Deserialize<Dictionary<string,JsonElement>>()!;value["nativeRendererPid"]=JsonSerializer.SerializeToElement(pid);
        StateStore.AtomicWrite(SessionPath,JsonSerializer.Serialize(value,Wire),null);
    }
    internal bool NativePopupReady=>compositionRenderer&&nativeLease?.IsAlive==true&&!HasLostTakeover;
    internal event Action? NativeIconsReady;
    internal event Action<string>? NativeIconInvalidated;
    internal event Action<string>? NativeImageInvalidated;
    private readonly Dictionary<string,DesktopDockFootprint> nativeDocks=new(StringComparer.Ordinal);
    internal DesktopDockFootprint NativeDock(string board)=>nativeDocks.GetValueOrDefault(board,DesktopDockFootprint.None);
    internal AccessoryProjection Projection(Board board,Display display,double scale,LayoutState? layout=null)
    {
        double cell=UnifiedDesktopGrid.Cell;
        if(!display.Primary&&controller.Screens.FirstOrDefault(screen=>screen.Primary) is {} primary&&(layout??controller.State).Boards.FirstOrDefault(row=>row.Id==primary.Id) is {} primaryBoard){
            double primaryScale=boardScales.GetValueOrDefault(primary.Id,primary.Dpi>0?primary.Dpi/96.0:1);
            cell=DesktopGridLayout.Project(primaryBoard,primary,primaryScale,NativeDock(primary.Id)).StepX/primaryScale;
        }
        return DesktopGridLayout.Project(board,display,scale,NativeDock(board.Id),preferredCell:cell);
    }
    internal void NativeShellWorkspace(JsonElement data){
        double Number(JsonElement v,string key,double fallback)=>v.ValueKind==JsonValueKind.Object&&v.TryGetProperty(key,out var n)&&n.TryGetDouble(out double d)&&double.IsFinite(d)?d:fallback;
        string Text(JsonElement v,string key,string fallback)=>v.ValueKind==JsonValueKind.Object&&v.TryGetProperty(key,out var n)&&n.ValueKind==JsonValueKind.String?n.GetString()??fallback:fallback;
        bool Flag(JsonElement v,string key,bool fallback)=>v.ValueKind==JsonValueKind.Object&&v.TryGetProperty(key,out var n)&&n.ValueKind is JsonValueKind.True or JsonValueKind.False?n.GetBoolean():fallback;
        if(!data.TryGetProperty("monitors",out var monitors)||monitors.ValueKind!=JsonValueKind.Array)return;
        var dock=data.TryGetProperty("dockSettings",out var d)?d:default;
        var toolbar=data.TryGetProperty("toolbarSettings",out var t)?t:default;
        string edge=Text(dock,"position","Bottom"),barEdge=Text(toolbar,"position","Top");
        double thickness=Number(dock,"size",40)+2*Number(dock,"padding",8)+Number(dock,"margin",8);
        var next=new Dictionary<string,DesktopDockFootprint>(StringComparer.Ordinal);
        foreach(var monitor in monitors.EnumerateArray()){
            string id=Text(monitor,"id","");if(id.Length==0)continue;
            double barOffset=Flag(monitor,"toolbar",false)&&edge==barEdge?Number(toolbar,"itemSize",16)+2*Number(toolbar,"padding",8)+2*Number(toolbar,"margin",0):0;
            next[id]=new(Flag(monitor,"dock",false)&&Flag(dock,"enabled",true),edge,Math.Clamp(thickness+barOffset,0,2048));
        }
        if(next.Count==nativeDocks.Count&&next.All(v=>nativeDocks.TryGetValue(v.Key,out var old)&&old==v.Value))return;
        nativeDocks.Clear();foreach(var row in next)nativeDocks.Add(row.Key,row.Value);
        workspaceRevision++;NativeWorkspaceChanged?.Invoke();changes.Notify();
    }
    internal event Action? NativeWorkspaceChanged;
    internal long NativeWorkspaceVersion=>workspaceRevision;
    internal long NativeCardCatalogVersion=>cardCatalogVersion;
    internal IReadOnlyList<DesktopCardDefinition> NativeCardDefinitions=>cardCatalog.Types;
    internal string? NativeIcon(DesktopItem item)=>icons.Get(item);
    internal string? NativePanelIcon(DesktopItem item)=>icons.Get(item,true);
    internal string? NativeApplicationIcon(ApplicationIconSource source)=>icons.GetApplication(source);
    internal void NativeInvalidateApplicationIcon(string identity)=>icons.InvalidateApplication(identity);
    internal void NativeOpenSettings()=>showSettings();
    internal System.Net.Http.HttpClient NativeClient()
    {
        var client=new System.Net.Http.HttpClient{BaseAddress=new Uri($"http://127.0.0.1:{Port}"),Timeout=Timeout.InfiniteTimeSpan};
        client.DefaultRequestHeaders.Authorization=new("Bearer",token);return client;
    }
    private readonly bool compositionRenderer;
    public DesktopBridge(AppController controller, Action showSettings, Action showFeedback, Action quit,bool compositionRenderer=false,Action? showAbout=null)
    {
        this.compositionRenderer=compositionRenderer;
        this.controller = controller; contextMenu=new(controller.Root,startSuspended:true,trace:controller.Log); this.showSettings = showSettings; this.showAbout=showAbout??showSettings; this.showFeedback = showFeedback; this.quit = quit;
        customCards=new(CustomCardLibrary.DefaultRoot,controller.Log);customCards.Recover();RefreshCardCatalog();
        icons=new(()=>{changes.Notify();NativeIconsReady?.Invoke();});icons.ApplicationIconFailed+=detail=>controller.Log("native_application_icon_unavailable",new{detail});icons.ImageChanged+=path=>NativeImageInvalidated?.Invoke(path);nativeDesktop=new(NativeDesktopChanged);recycleBin=new(()=>{icons.InvalidateRecycleBin();changes.Notify();NativeIconInvalidated?.Invoke(DesktopGridActions.ItemKey(new DesktopItem{Target=ShellClipboardCommands.RecycleBinTarget,IsVirtual=true}));});controller.Changed+=ControllerChanged;controller.Collections.BecameIdle+=ControllerChanged;
        currentWorkspace=WorkspaceSignature();
        controller.PrepareDesktopChange=state=>
        {
            foreach(var board in state.Boards)if(board.Grid?.NativeInitialized==true&&boardScales.TryGetValue(board.Id,out var scale))
            {
                var screen=controller.Screens.FirstOrDefault(row=>row.Id==board.Id);
                if(screen is {Primary:false}&&board.IconGrid is null){
                    var adaptive=Projection(board,screen,scale,state);
                    if(board.Grid.Columns!=adaptive.Columns||board.Grid.Rows!=adaptive.Rows)UnifiedDesktopGrid.Ensure(board,adaptive);
                }
                var before=controller.State.Boards.FirstOrDefault(row=>row.Id==board.Id)?.Boxes.Select(box=>box.Id).ToHashSet(StringComparer.Ordinal)??[];
                if(board.Boxes.Any(box=>!before.Contains(box.Id))){var display=controller.Screens.Single(screen=>screen.Id==board.Id);var native=nativeDesktop.Snapshot;UnifiedDesktopGrid.Ensure(board,Projection(board,display,scale));}
            }
        };
        try { listener.Start();ProvisionWidget();workAreaWatch=new(WorkspaceChanged,WallpaperBroadcast);EnsureWallpaperGuard();directoryWatch=new(()=>{if(nativeLease is not null&&!HasLostTakeover&&!stopping.IsCancellationRequested&&fileOperations.CurrentCount>0&&clipboardCommands.CurrentCount>0&&!controller.Collections.IsBusy)controller.RefreshDesktop();},reason=>controller.Log("desktop_watch_unavailable",new{reason})); }
        catch
        {
            listener.Stop();controller.Changed-=ControllerChanged;controller.Collections.BecameIdle-=ControllerChanged;controller.PrepareDesktopChange=null;
            controller.WallpaperGuard?.Dispose();controller.WallpaperGuard=null;directoryWatch?.Dispose();workAreaWatch?.Dispose();contextMenu.Dispose();recycleBin.Dispose();icons.Dispose();nativeDesktop.Dispose();stopping.Cancel();stopping.Dispose();throw;
        }
        _ = Accept();
        controller.Log("desktop_bridge_started", new { sessionId, port = Port, widgetId = WidgetId,sessionNotifications=workAreaWatch.SessionNotificationsRegistered,sessionNotificationError=workAreaWatch.SessionNotificationError });
    }
    private void FinishCreatedItem(string? boardId,string? createdPath,(int X,int Y)? point)
    {
        nativeDesktop.Refresh();controller.RefreshDesktop();
        // A callback identifies this command's exact result; unrelated files
        // that appeared concurrently keep their own routing and placement.
        string desktop=Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        var item=createdPath is not null&&StringComparer.OrdinalIgnoreCase.Equals(Path.GetDirectoryName(createdPath),desktop)?controller.Desktop.FirstOrDefault(row=>StringComparer.OrdinalIgnoreCase.Equals(row.Target,createdPath)):null;
        var screen=controller.Screens.FirstOrDefault(row=>row.Id==boardId);
        if(item is not null&&screen is not null&&boardScales.TryGetValue(screen.Id,out var scale)&&!controller.Store.ReadOnly)
            controller.Commit(state=>DesktopCreation.PlaceItem(state,state.Boards.Single(row=>row.Id==screen.Id),item,Projection(state.Boards.Single(b=>b.Id==screen.Id),screen,scale),point),"已在当前屏幕新建桌面项目。");
    }
    private void NativeDesktopChanged()
    {
        Application.Current.Dispatcher.BeginInvoke(()=>
        {
            if(stopping.IsCancellationRequested||HasLostTakeover||fileOperations.CurrentCount==0||clipboardCommands.CurrentCount==0)return;
            var shown=nativeDesktop.Snapshot.VisibleTargets;
            if(shown is not null&&!shown.ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(controller.Desktop.Select(item=>item.Target)))controller.RefreshDesktop();
            changes.Notify();
        });
    }
    private void SuspendTakeover(string reason)
    {
        leaseGeneration++;contextMenu.Suspend();nativeLease?.Dispose();nativeLease=null;
        HasLostTakeover=true;ready.Clear();
        foreach(long window in widgetWindows.Values)try{NativeRegion.Verify(window,controller.Root);HideDesktopWindow(new(window),0);}catch{}
        WriteReadyState();controller.Log("desktop_native_restored",new{reason,menuEpoch=contextMenu.CaptureEpoch()});changes.Notify();
    }
    private void ReconcileTopology()
    {
        // Work-area reservations are geometry changes, not display removal.
        // They must not tear down the guard or leave the Shell menu paused.
        string next=JsonSerializer.Serialize(controller.Screens.Select(screen=>screen.Id).OrderBy(id=>id,StringComparer.Ordinal));
        if(currentTopology is not null&&currentTopology!=next)SuspendTakeover("display_topology_changed");
        currentTopology=next;
        foreach(string monitor in monitorBoards.Keys.ToArray())
            if(!controller.Screens.Any(screen=>screen.Id==monitorBoards[monitor])){ready.Remove(monitor);widgetWindows.Remove(monitor);monitorBoards.Remove(monitor);}
        foreach(string monitor in ready.Keys.ToArray())
            try{if(!widgetWindows.TryGetValue(monitor,out long window))throw new InvalidDataException();NativeRegion.Verify(window,controller.Root);}
            catch{ready.Remove(monitor);}
    }
    private string WorkspaceSignature()=>JsonSerializer.Serialize(controller.Screens.OrderBy(screen=>screen.Id,StringComparer.Ordinal).Select(screen=>new{screen.Id,screen.X,screen.Y,screen.Width,screen.Height}));
    private void ControllerChanged()
    {
        changes.Notify();
        if(!deferredWorkspaceSignal||controller.Collections.IsBusy||stopping.IsCancellationRequested)return;
        deferredWorkspaceSignal=false;
        Application.Current.Dispatcher.BeginInvoke(WorkspaceChanged);
    }
    private void EnsureWallpaperGuard()
    {
        if(controller.Screens.Count<2){controller.WallpaperGuard?.Dispose();controller.WallpaperGuard=null;return;}
        try{controller.WallpaperGuard??=new(controller);}
        catch(Exception error){controller.Log("secondary_wallpaper_initialization_failed",new{error=error.GetType().Name});}
    }
    private void WallpaperBroadcast(int message,IntPtr parameter,IntPtr data)
    {
        var guard=controller.WallpaperGuard;if(guard is null)return;
        if(message==0x007E||message==0x0218||message==0x02B1){guard.Signal(SecondaryWallpaperPolicy.Trigger.TopologyChanged);return;}
        if(message!=0x001A)return;
        // Only a specific wallpaper settings notification can adopt a changed
        // image as the new baseline. Generic work-area/theme broadcasts cannot.
        long action=parameter.ToInt64();string section=data==IntPtr.Zero?"":System.Runtime.InteropServices.Marshal.PtrToStringUni(data)??"";
        if(action is 0x14 or 0x15||section.Equals(@"Control Panel\Desktop",StringComparison.OrdinalIgnoreCase))
            guard.Signal(SecondaryWallpaperPolicy.Trigger.WindowsSettingsObserved);
        else if(section.Equals("WindowsThemeElement",StringComparison.OrdinalIgnoreCase))
            guard.Signal(SecondaryWallpaperPolicy.Trigger.ReadOnlyCheck);
    }
    private void WorkspaceChanged()
    {
        if(stopping.IsCancellationRequested)return;
        if(controller.Collections.IsBusy){deferredWorkspaceSignal=true;return;}
        try
        {
            controller.RefreshDisplays();ReconcileTopology();EnsureWallpaperGuard();
            string next=WorkspaceSignature();
            bool displaced=widgetWindows.Any(row=>
            {
                if(!monitorBoards.TryGetValue(row.Key,out var board))return false;
                var display=controller.Screens.FirstOrDefault(screen=>screen.Id==board);if(display is null)return false;
                try{var rect=NativeDesktopHost.ReadBounds(row.Value,controller.Root);return rect!=new NativeIconRect(display.DesktopLeft,display.DesktopTop,display.DesktopLeft+display.DesktopWidth,display.DesktopTop+display.DesktopHeight);}catch{return false;}
            });
            // A real restore notification also renews a lost lease even when
            // geometry is unchanged. Guard failure itself does not schedule a
            // retry: recovery remains event-driven and fails closed on /ready.
            bool recoveryNeeded=HasLostTakeover&&nativeLease is null&&widgetWindows.Count>0;
            if(next==currentWorkspace&&!displaced&&!recoveryNeeded)return;
            currentWorkspace=next;workspaceRevision++;NativeWorkspaceChanged?.Invoke();
            controller.Log("desktop_workspace_changed",new{workspaceRevision,displaced,recoveryNeeded,screens=controller.Screens.Select(screen=>new{screen.Id,screen.X,screen.Y,screen.Width,screen.Height})});
            changes.Notify();
        }
        catch(Exception error){controller.Log("desktop_workspace_change_failed",new{error=error.Message});}
    }
    private void ProvisionWidget()
    {
        if(compositionRenderer){
            StateStore.AtomicWrite(SessionPath,JsonSerializer.Serialize(new{sessionId,version=AppController.Version,pid=Environment.ProcessId,endpoint=$"http://127.0.0.1:{Port}",token,renderer="composition",nativePopups=CompositionPopupSession.Selected(controller.Root),startedAt=DateTimeOffset.Now},Wire),null);
            return;
        }
        string directory = Path.Combine(controller.Root, "runtime", "desktop", "widget");
        Directory.CreateDirectory(directory);
        string source = Path.Combine(AppContext.BaseDirectory, "desktop-widget");
        foreach (var name in new[] { "metadata.yml", "index.html", "index.css" })
            StateStore.AtomicWrite(Path.Combine(directory, name), File.ReadAllText(Path.Combine(source, name)), null);
        var session = new { sessionId, version = AppController.Version, pid = Environment.ProcessId, endpoint = $"http://127.0.0.1:{Port}", token,
            metadata = Path.Combine(directory, "metadata.yml"), startedAt = DateTimeOffset.Now };
        StateStore.AtomicWrite(Path.Combine(directory, "index.js"), "globalThis.__MAGIC_BRIDGE = " + JsonSerializer.Serialize(session, Wire) + ";\n" + File.ReadAllText(Path.Combine(source, "index.js")), null);
        StateStore.AtomicWrite(SessionPath, JsonSerializer.Serialize(session, Wire), null);
    }
    private async Task Accept()
    {
        try
        {
            while (!stopping.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(stopping.Token).ConfigureAwait(false);
                if (!clients.Wait(0)) { client.Dispose(); continue; }
                _ = Serve(client);
            }
        }
        catch (OperationCanceledException) { }
        catch (SocketException) when (stopping.IsCancellationRequested) { }
    }
    private async Task Serve(TcpClient client)
    {
        using (client)
        using (var deadline = CancellationTokenSource.CreateLinkedTokenSource(stopping.Token))
        {
            // Capture before any header/body or Dispatcher await. A connection
            // accepted under an old takeover may not submit work after resume.
            long menuEpoch=contextMenu.CaptureEpoch();
            deadline.CancelAfter(TimeSpan.FromSeconds(8));
            using var stream = client.GetStream();
            string? origin = null;bool eventStream=false;string? requestAction=null;
            try
            {
                var header = new List<byte>(); var single = new byte[1];
                while (header.Count < 16384)
                {
                    int n = await stream.ReadAsync(single, deadline.Token).ConfigureAwait(false);
                    if (n == 0) return;
                    header.Add(single[0]);
                    if (header.Count >= 4 && header[^4] == 13 && header[^3] == 10 && header[^2] == 13 && header[^1] == 10) break;
                }
                var lines = Encoding.ASCII.GetString(header.ToArray()).Split("\r\n");
                var first = lines[0].Split(' ');
                if (header.Count >= 16384 || first.Length != 3 || first[2] != "HTTP/1.1" || !first[1].StartsWith('/') || first[1].StartsWith("//")) { await Reply(stream, 400, new { error = "Invalid request" }, null, deadline.Token); return; }
                var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (var line in lines.Skip(1).Where(x => x.Length != 0))
                {
                    int colon = line.IndexOf(':');
                    if (colon <= 0 || !headers.TryAdd(line[..colon].Trim(), line[(colon+1)..].Trim())) throw new InvalidDataException("Duplicate or invalid header");
                }
                headers.TryGetValue("Origin", out origin);
                if (headers.GetValueOrDefault("Host") != $"127.0.0.1:{Port}") { await Reply(stream, 400, new { error = "Invalid host" }, null, deadline.Token); return; }
                if (origin is not null && !Origins.Contains(origin)) { await Reply(stream, 403, new { error = "Origin rejected" }, null, deadline.Token); return; }
                if (first[0] == "OPTIONS") { await Reply(stream, 204, new { }, origin, deadline.Token); return; }
                if (first[0] is not ("GET" or "POST")) { await Reply(stream, 405, new { error = "Method rejected" }, origin, deadline.Token); return; }
                byte[] supplied = Encoding.ASCII.GetBytes(headers.GetValueOrDefault("Authorization", ""));
                byte[] expected = Encoding.ASCII.GetBytes("Bearer " + token);
                if (!CryptographicOperations.FixedTimeEquals(supplied, expected)) { await Reply(stream, 401, new { error = "Authentication required" }, origin, deadline.Token); return; }
                if (headers.ContainsKey("Transfer-Encoding") || !int.TryParse(headers.GetValueOrDefault("Content-Length", "0"), out int length) || length is < 0 or > 262144 || (first[0] == "GET" && length != 0)) { await Reply(stream, 400, new { error = "Invalid body" }, origin, deadline.Token); return; }
                var body = new byte[length]; int offset = 0;
                while (offset < length) { int n = await stream.ReadAsync(body.AsMemory(offset), deadline.Token); if (n == 0) throw new EndOfStreamException(); offset += n; }
                var uri = new Uri($"http://127.0.0.1:{Port}" + first[1]);
                using var document = JsonDocument.Parse(length == 0 ? "{}" : Encoding.UTF8.GetString(body));
                if(document.RootElement.TryGetProperty("kind",out var diagnosticKind))requestAction=diagnosticKind.GetString();
                if(first[0]=="POST"&&uri.AbsolutePath=="/register")await nativeDesktop.Ready(deadline.Token).ConfigureAwait(false);
                if(first[0]=="POST"&&uri.AbsolutePath=="/grid-action"&&document.RootElement.TryGetProperty("kind",out var operationKind)&&operationKind.GetString() is "drop-files" or "drop-desktop" or "reference" or "dissolve")deadline.CancelAfter(TimeSpan.FromSeconds(60));
                if(first[0]=="POST"&&uri.AbsolutePath=="/composition-resource-probe"){
                    if(origin is not null||NativeProbe is null||headers.GetValueOrDefault("X-Magic-Native-Popup-Protocol")!="Magic.NativePopups/1")throw new InvalidDataException("原生测量入口不可用。");
                    deadline.CancelAfter(TimeSpan.FromSeconds(8));await Reply(stream,200,await NativeProbe(document.RootElement.Clone(),deadline.Token),null,deadline.Token);return;
                }
                if(first[0]=="POST"&&uri.AbsolutePath is "/composition-shell-update" or "/composition-shell-next"){
                    if(origin is not null||!compositionRenderer||NativeShell is null||headers.GetValueOrDefault("X-Magic-Native-Popup-Protocol")!="Magic.NativePopups/1"){await Reply(stream,200,new{accepted=false},null,deadline.Token);return;}
                    deadline.CancelAfter(uri.AbsolutePath=="/composition-shell-next"?Timeout.InfiniteTimeSpan:TimeSpan.FromSeconds(3));
                    object shellOutcome=uri.AbsolutePath=="/composition-shell-next"?await NativeShell.Next(deadline.Token):await NativeShell.Update(document.RootElement.Clone(),deadline.Token);await Reply(stream,200,shellOutcome,null,deadline.Token);return;
                }
                if(first[0]=="POST"&&uri.AbsolutePath is "/composition-popup-open" or "/composition-popup-wait" or "/composition-popup-update")
                {
                    if(origin is not null||!compositionRenderer||NativePopups is null||headers.GetValueOrDefault("X-Magic-Native-Popup-Protocol")!="Magic.NativePopups/1"){await Reply(stream,200,new{accepted=false},null,deadline.Token);return;}
                    deadline.CancelAfter(uri.AbsolutePath=="/composition-popup-wait"?TimeSpan.FromMinutes(6):TimeSpan.FromSeconds(2));
                    object popupOutcome=uri.AbsolutePath=="/composition-popup-open"?await NativePopups.Open(document.RootElement.Clone(),deadline.Token):uri.AbsolutePath=="/composition-popup-update"?await NativePopups.Update(document.RootElement.Clone(),deadline.Token):await NativePopups.Wait(document.RootElement.Clone(),deadline.Token);
                    await Reply(stream,200,popupOutcome,null,deadline.Token);return;
                }
                if(first[0]=="GET"&&uri.AbsolutePath=="/events")
                {
                    if(!long.TryParse(Query(uri,"after"),out var after)||after<0)throw new InvalidDataException();
                    eventStream=true;Interlocked.Increment(ref eventStreamClients);deadline.CancelAfter(Timeout.InfiniteTimeSpan);
                    await stream.WriteAsync(Encoding.ASCII.GetBytes($"HTTP/1.1 200 Response\r\nContent-Type: application/x-ndjson; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n{Cors(origin)}\r\n"),deadline.Token);
                    after=changes.Version;
                    await stream.WriteAsync(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new{changeVersion=after},Wire)+"\n"),deadline.Token);
                    while(!deadline.IsCancellationRequested)
                    {
                        try{await changes.Wait(after,deadline.Token).ConfigureAwait(false);}catch(TimeoutException){}
                        long current=changes.Version;
                        string line=current==after?"\n":JsonSerializer.Serialize(new{changeVersion=current},Wire)+"\n";after=current;
                        if(line=="\n")Interlocked.Increment(ref eventStreamHeartbeats);else Interlocked.Increment(ref eventStreamNotifications);
                        await stream.WriteAsync(Encoding.UTF8.GetBytes(line),deadline.Token);
                    }
                    return;
                }
                if(first[0]=="GET"&&uri.AbsolutePath=="/changes")
                {
                    deadline.CancelAfter(TimeSpan.FromSeconds(30));
                    if(!long.TryParse(Query(uri,"after"),out var after)||after<0)throw new InvalidDataException();
                    try{await changes.Wait(after,deadline.Token).ConfigureAwait(false);}catch(TimeoutException){}
                    await Reply(stream,200,new{changeVersion=changes.Version},origin,deadline.Token);return;
                }
                if(first[0]=="POST"&&uri.AbsolutePath=="/desktop-import-files")
                {
                    deadline.CancelAfter(Timeout.InfiniteTimeSpan);await fileOperations.WaitAsync(deadline.Token).ConfigureAwait(false);
                    var intents=new List<DesktopFileChanges.Intent>();string? boardId=null;
                    string desktop=Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                    try
                    {
                        var modifiers=NativeDesktopDrag.Modifiers();
                        if(compositionRenderer&&document.RootElement.TryGetProperty("control",out var control)&&document.RootElement.TryGetProperty("shift",out var shift))modifiers=(control.GetBoolean(),shift.GetBoolean());
                        var paths=await Application.Current.Dispatcher.InvokeAsync(()=>
                        {
                            string monitor=String(document.RootElement,"monitorId");
                            contextMenu.EnsureEpoch(menuEpoch);
                            if(nativeLease is null||HasLostTakeover||controller.Store.ReadOnly||controller.Collections.IsBusy||!monitorBoards.TryGetValue(monitor,out boardId))throw new InvalidDataException();
                            if(!widgetWindows.TryGetValue(monitor,out long own))throw new InvalidDataException();NativeRegion.Verify(own,controller.Root);
                            var selected=ShellClipboardCommands.ValidateIncoming(document.RootElement.GetProperty("paths").EnumerateArray().Select(p=>p.GetString()??"").ToArray(),desktop);
                            foreach(string path in selected)intents.Add(DesktopFileChanges.PrepareExternalDrag(controller.Root,path));return selected;
                        });
                        var before=Directory.EnumerateFileSystemEntries(desktop).ToHashSet(StringComparer.OrdinalIgnoreCase);
                        using var transfer=JsonDocument.Parse(JsonSerializer.Serialize(new{requestId=Guid.NewGuid().ToString("N"),paths,control=modifiers.Control,shift=modifiers.Shift}));
                        var outcome=await contextMenu.Route(uri.AbsolutePath,transfer.RootElement,deadline.Token,desktop,expectedEpoch:menuEpoch).ConfigureAwait(false);
                        await Application.Current.Dispatcher.InvokeAsync(()=>
                        {
                            controller.RefreshDesktop();var incoming=controller.Desktop.Where(item=>!item.IsVirtual&&Path.GetDirectoryName(item.Target)?.Equals(desktop,StringComparison.OrdinalIgnoreCase)==true&&!before.Contains(item.Target)).ToArray();
                            if(incoming.Length>0)controller.Commit(state=>
                            {
                                var board=state.Boards.Single(b=>b.Id==boardId);
                                foreach(var item in incoming){foreach(var other in state.Boards){other.RootItems.RemoveAll(p=>p.Target.Equals(item.Target,StringComparison.OrdinalIgnoreCase));other.Grid?.Placements.Remove(DesktopGridActions.ItemKey(item));}board.RootItems.Add(item);}
                            },"已接收文件，沿用现有网格安排空位。");
                            controller.Log("desktop_import_finished",new{count=paths.Length,contentsLogged=false});
                        });
                        await Reply(stream,200,outcome,origin,deadline.Token);return;
                    }
                    finally
                    {
                        try{await Application.Current.Dispatcher.InvokeAsync(()=>{controller.FinishNativeDrags(intents.Select(intent=>intent.Id));nativeDesktop.Refresh();});}
                        finally{fileOperations.Release();}
                    }
                }
                if(first[0]=="POST"&&uri.AbsolutePath=="/desktop-create-item")
                {
                    deadline.CancelAfter(Timeout.InfiniteTimeSpan);await fileOperations.WaitAsync(deadline.Token).ConfigureAwait(false);
                    try
                    {
                        var point=DesktopCreation.Position(document.RootElement);
                        string boardId=await Application.Current.Dispatcher.InvokeAsync(()=>{contextMenu.EnsureEpoch(menuEpoch);if(nativeLease is null||HasLostTakeover||controller.Store.ReadOnly||!monitorBoards.TryGetValue(String(document.RootElement,"monitorId"),out var id))throw new InvalidDataException();return id;});
                        string action=String(document.RootElement,"action");if(action is not ("folder" or "text"))throw new InvalidDataException();
                        string desktop=Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                        using var create=JsonDocument.Parse(JsonSerializer.Serialize(new{requestId=Guid.NewGuid().ToString("N"),action}));
                        var creationResult=JsonSerializer.SerializeToElement(await contextMenu.Route(uri.AbsolutePath,create.RootElement,deadline.Token,desktop,expectedEpoch:menuEpoch).ConfigureAwait(false));
                        string? createdPath=creationResult.TryGetProperty("createdPath",out var path)?path.GetString():null;
                        await Application.Current.Dispatcher.InvokeAsync(()=>FinishCreatedItem(boardId,createdPath,point));
                        await Reply(stream,200,new{ok=true},origin,deadline.Token);return;
                    }
                    finally{fileOperations.Release();}
                }
                JsonElement? crossFile=first[0]=="POST"&&uri.AbsolutePath=="/grid-action"&&document.RootElement.TryGetProperty("kind",out var crossKind)&&crossKind.GetString()=="cross-drop"?await Application.Current.Dispatcher.InvokeAsync(()=>CrossFileDrop(document.RootElement)):null;
                JsonElement? groupFile=first[0]=="POST"&&uri.AbsolutePath=="/desktop-group-drop"?document.RootElement:crossFile is { } crossGroup&&crossGroup.TryGetProperty("itemIds",out _)?crossGroup:null;
                if(groupFile is { } groupBody)
                {
                    var groupClock=System.Diagnostics.Stopwatch.StartNew();double queuedMs=0,verifiedMs=0,shellMs=0,refreshMs=0;int memberCount=0;bool completed=false;
                    deadline.CancelAfter(Timeout.InfiniteTimeSpan);await fileOperations.WaitAsync(deadline.Token).ConfigureAwait(false);
                    queuedMs=groupClock.Elapsed.TotalMilliseconds;
                    var intents=new List<DesktopFileChanges.Intent>();object groupOutcome;
                    try
                    {
                        var transfer=await Application.Current.Dispatcher.InvokeAsync(()=>{
                            contextMenu.EnsureEpoch(menuEpoch);if(nativeLease is null||HasLostTakeover||controller.Store.ReadOnly||!monitorBoards.ContainsKey(String(groupBody,"monitorId")))throw new InvalidDataException();
                            if(groupBody.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("桌面已更新，请重试。");
                            var paths=KnownSelection(groupBody);var target=Known(String(groupBody,"targetId"));
                            if(!DesktopFileDrop.IsRecycleBin(target)&&(target.IsVirtual||!target.IsFolder))throw new InvalidDataException();
                            if(paths.Any(path=>path.StartsWith("::",StringComparison.Ordinal)))throw new LayoutEditException("系统入口不能作为真实文件移动或回收。");
                            foreach(string path in paths)if(!System.IO.File.Exists(path)&&!System.IO.Directory.Exists(path))throw new LayoutEditException("部分选中项目已失效，原位置已保留。");
                            foreach(string path in paths)intents.Add(DesktopFileChanges.PrepareExternalDrag(controller.Root,path));
                            return (paths,destination:target.Target);
                        });
                        memberCount=transfer.paths.Length;verifiedMs=groupClock.Elapsed.TotalMilliseconds-queuedMs;
                        using var command=JsonDocument.Parse(JsonSerializer.Serialize(new{requestId=Guid.NewGuid().ToString("N"),control=groupBody.TryGetProperty("control",out var ctrl)&&ctrl.GetBoolean(),shift=groupBody.TryGetProperty("shift",out var shift)&&shift.GetBoolean()}));
                        groupOutcome=await contextMenu.Route("/desktop-group-drop",command.RootElement,deadline.Token,destination:transfer.destination,targets:transfer.paths,expectedEpoch:menuEpoch).ConfigureAwait(false);
                        shellMs=groupClock.Elapsed.TotalMilliseconds-queuedMs-verifiedMs;completed=true;
                    }
                    finally{var refreshClock=System.Diagnostics.Stopwatch.StartNew();try{await Application.Current.Dispatcher.InvokeAsync(()=>{controller.FinishNativeDrags(intents.Select(intent=>intent.Id));nativeDesktop.Refresh();});}finally{refreshMs=refreshClock.Elapsed.TotalMilliseconds;fileOperations.Release();controller.Log("desktop_group_drop_completed",new{queuedMs,verifiedMs,shellMs,refreshMs,totalMs=groupClock.Elapsed.TotalMilliseconds,memberCount,completed,contentsLogged=false});}}
                    await Reply(stream,200,groupOutcome,origin,deadline.Token);return;
                }
                if(first[0]=="POST"&&(uri.AbsolutePath=="/desktop-file-operation"||crossFile is not null))
                {
                    var fileBody=crossFile??document.RootElement;
                    deadline.CancelAfter(Timeout.InfiniteTimeSpan);await fileOperations.WaitAsync(deadline.Token).ConfigureAwait(false);
                    try
                    {
                        var intent=await Application.Current.Dispatcher.InvokeAsync(()=>
                        {
                            contextMenu.EnsureEpoch(menuEpoch);
                            if(nativeLease is null||HasLostTakeover||controller.Store.ReadOnly||!monitorBoards.ContainsKey(String(fileBody,"monitorId")))throw new InvalidDataException();
                            if(fileBody.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("桌面已更新，请重试。");
                            var item=Known(String(fileBody,"itemId"));if(item.IsVirtual)throw new LayoutEditException("系统虚拟图标不能作为普通文件移动。");
                            string action=String(fileBody,"action");string? folder=null;
                            if(action=="move"){var destination=Known(String(fileBody,"targetId"));if(destination.IsVirtual||!destination.IsFolder)throw new InvalidDataException();folder=destination.Target;}
                            if(action=="recycle"&&fileBody.TryGetProperty("targetId",out var bin)&&!DesktopFileDrop.IsRecycleBin(Known(bin.GetString()!)))throw new InvalidDataException();
                            string? name=fileBody.TryGetProperty("name",out var suppliedName)?suppliedName.GetString():null;
                            return DesktopFileChanges.Prepare(controller.Root,action,item.Target,folder,name);
                        });
                        using var operation=JsonDocument.Parse(JsonSerializer.Serialize(new{requestId=intent.Id,action=intent.Action}));
                        object fileResult;
                        try{fileResult=await contextMenu.Route("/desktop-file-operation",operation.RootElement,deadline.Token,intent.Source,intent.Destination,expectedEpoch:menuEpoch).ConfigureAwait(false);}
                        catch(OperationCanceledException)when(!deadline.IsCancellationRequested){await Reply(stream,200,new{ok=true,cancelled=true},origin,deadline.Token);return;}
                        finally{await Application.Current.Dispatcher.InvokeAsync(()=>{controller.RecoverShellChanges();nativeDesktop.Refresh();});}
                        await Reply(stream,200,new{ok=true,physicalOperation=intent.Action,itemKey=intent.Action=="rename"&&intent.Destination is not null&&(File.Exists(intent.Destination)||Directory.Exists(intent.Destination))?DesktopGridActions.ItemKey(new DesktopItem{Target=intent.Destination}):null},origin,deadline.Token);return;
                    }
                    finally{fileOperations.Release();}
                }
                if(first[0]=="POST"&&uri.AbsolutePath=="/desktop-clipboard-command")
                {
                    deadline.CancelAfter(Timeout.InfiniteTimeSpan);await clipboardCommands.WaitAsync(deadline.Token).ConfigureAwait(false);
                    try
                    {
                        string action=String(document.RootElement,"action");if(action is not ("copy" or "cut" or "paste" or "undo"))throw new InvalidDataException();
                        string[]? sources=await Application.Current.Dispatcher.InvokeAsync(()=>{contextMenu.EnsureEpoch(menuEpoch);if(nativeLease is null||HasLostTakeover||!monitorBoards.ContainsKey(String(document.RootElement,"monitorId")))throw new InvalidDataException();if(action is "paste" or "undo"){if(controller.Store.ReadOnly)throw new InvalidDataException();return null;}return KnownSelection(document.RootElement);});
                        string? pastePage=action=="paste"?await Application.Current.Dispatcher.InvokeAsync(()=>controller.State.Boards.Single(b=>b.Id==monitorBoards[String(document.RootElement,"monitorId")]).ActivePageId):null;
                        using var command=JsonDocument.Parse(JsonSerializer.Serialize(new{requestId=Guid.NewGuid().ToString("N"),action}));var watch=System.Diagnostics.Stopwatch.StartNew();
                        if(action=="undo")await Application.Current.Dispatcher.InvokeAsync(()=>controller.UndoPositions.Begin(controller.State));
                        object outcome;
                        try{outcome=await contextMenu.Route(uri.AbsolutePath,command.RootElement,deadline.Token,expectedEpoch:menuEpoch,targets:sources).ConfigureAwait(false);}
                        catch{if(action=="undo")await Application.Current.Dispatcher.InvokeAsync(()=>controller.UndoPositions.Cancel());throw;}
                        if(action=="undo")await Application.Current.Dispatcher.InvokeAsync(()=>{nativeDesktop.Refresh();controller.RefreshDesktop();});
                        controller.Log("desktop_clipboard_command",new{action,workerRoundTripMs=watch.Elapsed.TotalMilliseconds,stages=outcome,contentsLogged=false});
                        object clipboardReply=outcome;
                        // Place only paths identified by this Windows operation. Normal Shell events still apply.
                        if(action=="paste"){
                            var point=DesktopCreation.Position(document.RootElement);string monitorId=String(document.RootElement,"monitorId");
                            string[] pastedKeys=[];var pasteResult=JsonSerializer.SerializeToElement(outcome);var paths=pasteResult.TryGetProperty("createdPaths",out var created)?created.EnumerateArray().Select(v=>v.GetString()).OfType<string>().ToArray():[];
                            await Application.Current.Dispatcher.InvokeAsync(()=>{
                                nativeDesktop.Refresh();controller.RefreshDesktop();
                                if(point is null||!monitorBoards.TryGetValue(monitorId,out var destination)||!boardScales.TryGetValue(monitorId,out var scale))return;
                                var screen=controller.Screens.FirstOrDefault(s=>s.Id==monitorId);if(screen is null)return;
                                var incoming=paths.Select(path=>controller.Desktop.FirstOrDefault(row=>StringComparer.OrdinalIgnoreCase.Equals(row.Target,path))).OfType<DesktopItem>().ToArray();
                                pastedKeys=incoming.Select(DesktopGridActions.ItemKey).ToArray();
                                if(incoming.Length>0)controller.Commit(state=>{var board=state.Boards.Single(b=>b.Id==destination);var projection=Projection(state.Boards.Single(b=>b.Id==screen.Id),screen,scale);DesktopPages.OnSourcePage(board,pastePage,()=>DesktopCreation.PlaceItems(state,board,incoming,projection,point));
                                },"已在右键位置粘贴桌面项目。");
                            });
                            clipboardReply=new{ok=true,stages=outcome,keys=pastedKeys,page=pastePage};
                        }
                        await Reply(stream,200,clipboardReply,origin,deadline.Token);return;
                    }
                    finally{clipboardCommands.Release();}
                }
                if(first[0]=="POST"&&uri.AbsolutePath=="/desktop-clipboard-state")
                {
                    await Application.Current.Dispatcher.InvokeAsync(()=>{if(nativeLease is null||HasLostTakeover||!monitorBoards.ContainsKey(String(document.RootElement,"monitorId")))throw new InvalidDataException();});
                    await Reply(stream,200,DesktopClipboard.Inspect(),origin,deadline.Token);return;
                }
                if(first[0]=="POST"&&uri.AbsolutePath is "/desktop-context-native-track" or "/desktop-context-open" or "/desktop-context-prefetch" or "/desktop-context-items" or "/desktop-context-invoke" or "/menu-dismiss" or "/desktop-properties")
                {
                    deadline.CancelAfter(TimeSpan.FromSeconds(12));
                    if(uri.AbsolutePath is "/desktop-context-native-track" or "/desktop-context-invoke" or "/desktop-properties")deadline.CancelAfter(Timeout.InfiniteTimeSpan);
                    if(uri.AbsolutePath=="/desktop-context-native-track"&&!compositionRenderer)throw new InvalidDataException();
                    var target=await Application.Current.Dispatcher.InvokeAsync(()=>{contextMenu.EnsureEpoch(menuEpoch);if(uri.AbsolutePath is "/desktop-context-invoke" or "/desktop-context-native-track"&&controller.Store.ReadOnly)throw new LayoutEditException("桌面布局当前只读，已阻止修改操作。");if(nativeLease is null||HasLostTakeover||!monitorBoards.ContainsKey(String(document.RootElement,"monitorId")))throw new InvalidDataException();return uri.AbsolutePath=="/desktop-properties"||uri.AbsolutePath is "/desktop-context-open" or "/desktop-context-prefetch"&&(document.RootElement.TryGetProperty("itemId",out _)||document.RootElement.TryGetProperty("itemIds",out _))?KnownSelection(document.RootElement):null;});
                    var point=uri.AbsolutePath=="/desktop-context-invoke"?DesktopCreation.Position(document.RootElement):uri.AbsolutePath=="/desktop-context-native-track"&&document.RootElement.TryGetProperty("cellX",out var cellX)?DesktopCreation.Position(JsonSerializer.SerializeToElement(new{x=cellX.GetInt32(),y=document.RootElement.GetProperty("cellY").GetInt32()})):null;
                    string? creationBoard=uri.AbsolutePath is "/desktop-context-invoke" or "/desktop-context-native-track"?await Application.Current.Dispatcher.InvokeAsync(()=>monitorBoards.GetValueOrDefault(String(document.RootElement,"monitorId"))):null;
                    var menuResult=await contextMenu.Route(uri.AbsolutePath,document.RootElement,deadline.Token,expectedEpoch:menuEpoch,targets:target).ConfigureAwait(false);
                    if(uri.AbsolutePath is "/desktop-context-invoke" or "/desktop-context-native-track")
                    {
                        var data=JsonSerializer.SerializeToElement(menuResult);string? createdPath=data.TryGetProperty("createdPath",out var path)?path.GetString():null;
                        await Application.Current.Dispatcher.InvokeAsync(()=>FinishCreatedItem(creationBoard,createdPath,point));
                    }
                    await Reply(stream,200,menuResult,origin,deadline.Token);return;
                }
                var result = await Application.Current.Dispatcher.InvokeAsync(() => Route(first[0], uri, document.RootElement));
                await Reply(stream, result.Code, result.Body, origin, deadline.Token);
            }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                if(eventStream&&(ex is IOException or OperationCanceledException))return;
                controller.Log("desktop_request_error", new { type = ex.GetType().Name,action=requestAction,message=ex.Message,detail=ex.ToString() });
                string error=ex is LayoutEditException?ex.Message:"操作未完成，请检查输入或打开调试日志。";
                try { await Reply(stream, 400, new { error }, origin, deadline.Token); } catch { }
            }
            finally { if(eventStream)Interlocked.Decrement(ref eventStreamClients);clients.Release(); }
        }
    }
    private (int Code, object Body) Route(string method, Uri uri, JsonElement body)
    {
        if(method=="POST"&&uri.AbsolutePath=="/community-menu")
        {
            string action=String(body,"kind");
            if(action=="state")return(200,new{version=AppController.Version,honorSignedIn=HonorPcManager.SignedIn,accountAvailable=HonorPcManager.Find() is not null});
            if(action=="settings")Application.Current.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background,showSettings);
            else if(action=="update")Application.Current.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background,new Action(()=>CommunityUpdateWindow.Open(controller.Root)));
            else if(action=="about")Application.Current.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background,showAbout);
            else if(action=="account")HonorPcManager.OpenAccount();
            else if(action=="feedback")Application.Current.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background,showFeedback);
            else if(action=="exit")Application.Current.Dispatcher.BeginInvoke(quit);
            else return(400,new{error="Unknown community menu action"});
            controller.Log("community_menu_action",new{kind=action});return(200,new{ok=true});
        }
        if(method=="POST"&&uri.AbsolutePath=="/desktop-card-cleanup")
        {
            if(!monitorBoards.TryGetValue(String(body,"monitorId"),out var boardId))throw new InvalidDataException();
            var card=DesktopCards.Require(controller.State.Boards.Single(board=>board.Id==boardId),String(body,"id"));
            if(card.Type!="system.storage")throw new InvalidDataException("仅存储卡片支持此入口。");
            HonorPcManager.OpenCleanup();controller.Log("desktop_card_cleanup_opened",new{deletionAutomated=false});return(200,new{ok=true});
        }
        if(method=="POST"&&uri.AbsolutePath is "/desktop-native-drag-source" or "/desktop-native-drag-finish")
        {
            string monitor=String(body,"monitorId");long hwnd=body.GetProperty("windowId").GetInt64();
            if(uri.AbsolutePath=="/desktop-native-drag-finish")
            {
                var ids=body.TryGetProperty("dragIds",out var many)?many.EnumerateArray().Select(value=>value.GetString()??throw new InvalidDataException()).ToArray():[String(body,"dragId")];
                if(ids.Length is <1 or >256||ids.Any(id=>!issuedDrags.TryGetValue(id,out var owner)||owner.Monitor!=monitor||owner.Window!=hwnd))throw new InvalidDataException("拖动清理归属不一致。");
                if(controller.Store.ReadOnly){controller.Log("desktop_native_drag_cleanup_deferred",new{count=ids.Length});return(200,new{ok=true,recoveryDeferred=true});}
                controller.FinishNativeDrags(ids);foreach(string id in ids)issuedDrags.Remove(id);nativeDesktop.Refresh();controller.Log("desktop_native_drag_finished",new{monitorId=monitor,mode="host_ui_thread",count=ids.Length,contentsLogged=false});return(200,new{ok=true});
            }
            if(nativeLease is null||HasLostTakeover||controller.Store.ReadOnly||!monitorBoards.ContainsKey(monitor)||!widgetWindows.TryGetValue(monitor,out var own)||own!=hwnd)throw new InvalidDataException();
            NativeRegion.Verify(own,controller.Root);
            if(fileOperations.CurrentCount==0||controller.Collections.IsBusy||!NativeDesktopDrag.LeftPressed)throw new LayoutEditException("拖动已结束或上一项文件操作尚未完成，请重新拖动。");
            var paths=KnownSelection(body);if(paths.Any(path=>path.StartsWith("::",StringComparison.Ordinal)||!System.IO.File.Exists(path)&&!System.IO.Directory.Exists(path)))throw new LayoutEditException("所选项目包含不可拖出的系统入口或已失效文件。");
            foreach(string parent in paths.Where(System.IO.Directory.Exists))if(paths.Any(path=>!StringComparer.OrdinalIgnoreCase.Equals(parent,path)&&path.StartsWith(parent.TrimEnd('\\')+'\\',StringComparison.OrdinalIgnoreCase)))throw new LayoutEditException("不能同时拖动文件夹及其内部项目。");
            var intents=new List<DesktopFileChanges.Intent>();
            try{foreach(string path in paths)intents.Add(DesktopFileChanges.PrepareExternalDrag(controller.Root,path));}
            catch{controller.FinishNativeDrags(intents.Select(intent=>intent.Id));throw;}
            foreach(var intent in intents)issuedDrags[intent.Id]=(monitor,hwnd);
            controller.Log("desktop_native_drag_source",new{monitorId=monitor,mode="host_ui_thread",count=intents.Count,contentsLogged=false});return(200,new{source=intents[0].Source,dragId=intents[0].Id,sources=intents.Select(intent=>intent.Source).ToArray(),dragIds=intents.Select(intent=>intent.Id).ToArray()});
        }
        if(method=="POST"&&uri.AbsolutePath=="/desktop-drag-probe")
        {
            string monitor=String(body,"monitorId");
            if(nativeLease is null||HasLostTakeover||!monitorBoards.ContainsKey(monitor)||!widgetWindows.ContainsKey(monitor))throw new InvalidDataException();
            var target=NativeDesktopDrag.Inspect(widgetWindows.Values);
            return(200,new{eligible=target.Eligible,external=target.External});
        }
        if(method=="POST"&&uri.AbsolutePath=="/desktop-keyboard-state")
        {
            string monitor=String(body,"monitorId");
            if(nativeLease is null||HasLostTakeover||!monitorBoards.ContainsKey(monitor)||!widgetWindows.TryGetValue(monitor,out var own))throw new InvalidDataException();
            bool focused=NativeDesktopHost.ConfirmKeyboardAtDesktopDepth(own,controller.Root);
            controller.Log("desktop_keyboard_focus",new{monitorId=monitor,focused});return(200,new{focused});
        }
        if (method == "GET" && uri.AbsolutePath == "/health") return (200, new { ok = true, sessionId, version = AppController.Version,nativeDesktopReady=NativePopupReady,streamClients=Volatile.Read(ref eventStreamClients),streamNotifications=Interlocked.Read(ref eventStreamNotifications),streamHeartbeats=Interlocked.Read(ref eventStreamHeartbeats),gridStateReads=Interlocked.Read(ref gridStateReads) });
        if (method == "POST" && uri.AbsolutePath == "/register")
        {
            string monitor = String(body, "monitorId");
            if (monitor.Length is < 1 or > 180) throw new InvalidDataException();
            var rect = body.GetProperty("rect"); int left = rect.GetProperty("left").GetInt32(), top = rect.GetProperty("top").GetInt32(), right = rect.GetProperty("right").GetInt32(), bottom = rect.GetProperty("bottom").GetInt32();
            double scale = body.GetProperty("scale").GetDouble();
            if (right <= left || bottom <= top || right-left > 32768 || bottom-top > 32768 || !double.IsFinite(scale) || scale is < 0.5 or > 8) throw new InvalidDataException();
            controller.RefreshDisplays();ReconcileTopology();
            int Area(Display s) => Math.Max(0, Math.Min(right,s.X+s.Width)-Math.Max(left,s.X)) * Math.Max(0, Math.Min(bottom,s.Y+s.Height)-Math.Max(top,s.Y));
            var screens = controller.Screens.OrderByDescending(Area).ToArray();
            if (!compositionRenderer&&(screens.Length == 0 || Area(screens[0]) == 0 || (screens.Length > 1 && Area(screens[0]) == Area(screens[1])))) throw new InvalidDataException("Monitor cannot be uniquely matched");
            // Native boards already carry physical display identities. A stale
            // rectangle during hotplug must not register that board on a neighbor.
            var display = compositionRenderer
                ? controller.Screens.SingleOrDefault(screen=>screen.Id==monitor)??throw new InvalidDataException("原生桌面的屏幕已变化，请重新注册。")
                : screens[0];
            monitorBoards[monitor] = display.Id;
            boardScales[display.Id]=scale;
            var native=nativeDesktop.Snapshot;var board=controller.State.Boards.Single(b=>b.Id==display.Id);var projection=Projection(board,display,scale);
            if((controller.State.Boards.Single(b=>b.Id==display.Id).Grid is not { } existing||existing.FoldersOnly||!existing.NativeInitialized||existing.Columns!=projection.Columns||existing.Rows!=projection.Rows||controller.State.Boards.Single(b=>b.Id==display.Id).Pages is null)&&!controller.Store.ReadOnly)
                controller.Commit(state=>{var target=state.Boards.Single(b=>b.Id==display.Id);if(target.Pages is null)DesktopPages.Initialize(state,target,projection,display.Primary);if(projection.AdaptiveSecondary)DesktopGridLayout.ResizeAdaptiveSecondary(target,projection);else DesktopGridLayout.ResizeWithoutMoving(target,projection);},"统一桌面网格已就绪，文件路径保持不变。");
            // P0: the renderer positions the toolbar/desktop from this rect.
            // It must be the monitor rect (rcMonitor), otherwise a taskbar docked
            // top/left/right leaves a gap on that edge.
            return (200, new { boardId = display.Id, label = display.Label,workspaceRevision, rect = new { left=display.DesktopLeft, top=display.DesktopTop, right=display.DesktopLeft+display.DesktopWidth, bottom=display.DesktopTop+display.DesktopHeight } });
        }
        if(method=="GET"&&uri.AbsolutePath=="/grid-state")
        {
            Interlocked.Increment(ref gridStateReads);
            string monitor=Query(uri,"monitor");if(!monitorBoards.TryGetValue(monitor,out var boardId))return(404,new{code="monitor-registration-required",error="屏幕尚未注册。"});
            var board=controller.State.Boards.Single(b=>b.Id==boardId);
            if(board.Grid is null)return(400,new{error="网格尚未就绪，请检查只读布局状态。"});
            var all=controller.Desktop.Concat(controller.State.Boards.SelectMany(b=>b.RootItems)).Concat(controller.State.Boards.SelectMany(b=>b.Boxes).SelectMany(b=>b.Items)).DistinctBy(item=>item.Target,StringComparer.OrdinalIgnoreCase).ToArray();
            var display=controller.Screens.Single(screen=>screen.Id==boardId);double scale=boardScales[boardId];var native=nativeDesktop.Snapshot;var projection=Projection(board,display,scale);
            var visibleItems=board.RootItems.Concat(board.Boxes.SelectMany(box=>box.Items)).DistinctBy(item=>item.Target,StringComparer.OrdinalIgnoreCase);
            string[] hidden=[];
            string UnplacedName(string key)=>key.StartsWith("folder:",StringComparison.Ordinal)?board.Boxes.Single(box=>DesktopGridActions.FolderKey(box)==key).Name:
                key.StartsWith("card:",StringComparison.Ordinal)?cardCatalog.Types.FirstOrDefault(type=>type.Type==DesktopCards.Require(board,key[5..]).Type)?.Title??"卡片":
                board.RootItems.Single(item=>DesktopGridActions.ItemKey(item)==key).Name;
            return(200,new{version=AppController.Version,sessionId,revision=controller.State.Revision,changeVersion=changes.Version,workspaceRevision,nativeVersion=nativeDesktop.Revision,readOnly=controller.Store.ReadOnly,boardName=board.Name,mode="unified-desktop",takeoverActive=nativeLease is not null,hiddenFolders=hidden,
                projection=new{originX=projection.OriginX/scale,originY=projection.OriginY/scale,stepX=projection.StepX/scale,stepY=projection.StepY/scale,inset=UnifiedDesktopGrid.Inset},nativeIcons=native.Icons.Select(icon=>new{left=(icon.Left-display.DesktopLeft)/scale,top=(icon.Top-display.DesktopTop)/scale,right=(icon.Right-display.DesktopLeft)/scale,bottom=(icon.Bottom-display.DesktopTop)/scale}).ToArray(),
                collectionProblem=controller.Collections.Problem,snapshot=new{items=visibleItems.Select(item=>new{id=Id(item),name=item.Name,renameName=item.IsVirtual?null:Path.GetFileName(item.Target),icon=icons.Get(item),isFolder=item.IsFolder,isVirtual=item.IsVirtual,isRecycleBin=DesktopFileDrop.IsRecycleBin(item),managed=controller.Collections.IsManaged(item.Target)}).ToArray(),boards=controller.State.Boards.Select(b=>new{id=b.Id,name=b.Name,active=controller.Screens.Any(s=>s.Id==b.Id)}).ToArray()},
                cardApiVersion=DesktopCardCatalog.ApiVersion,cardCatalogVersion,cardDefinitions=cardCatalog.Types,
                board=new{boardId=board.Id,paged=board.Paged,pageId=board.ActivePageId,pages=board.Pages?.Select(page=>new{id=page.Id,count=page.Grid.Placements.Count}).ToArray(),unplaced=board.Unplaced.Keys.Select(key=>new{key,name=UnplacedName(key)}).ToArray(),columns=board.Grid.Columns,rows=board.Grid.Rows,knownIds=all.Select(Id).ToArray(),rootIds=board.RootItems.Where(item=>board.Grid.Placements.ContainsKey(DesktopGridActions.ItemKey(item))).Select(Id).ToArray(),folders=board.Boxes.Where(box=>board.Grid.Placements.ContainsKey(DesktopGridActions.FolderKey(box))).Select(box=>new{id=box.Id,name=box.Name,itemIds=box.Items.Select(Id).ToArray()}).ToArray(),cards=DesktopCards.Items(board).Where(card=>board.Grid.Placements.ContainsKey(DesktopCards.Key(card))).Select(card=>new{id=card.Id,type=card.Type,columns=card.Columns,rows=card.Rows,settings=card.Settings}).ToArray(),placements=board.Grid.Placements.ToDictionary(row=>row.Key,row=>new{x=row.Value.X,y=row.Value.Y,w=row.Value.W,h=row.Value.H})}});
        }
        if(method=="GET"&&uri.AbsolutePath=="/desktop-cards/catalog")return(200,new{apiVersion=DesktopCardCatalog.ApiVersion,types=cardCatalog.Types});
        if(method=="POST"&&uri.AbsolutePath=="/desktop-cards/manage")
        {
            string monitor=String(body,"monitorId"),kind=String(body,"kind");
            if(nativeLease is null||HasLostTakeover||!monitorBoards.ContainsKey(monitor))throw new LayoutEditException("桌面尚未就绪。");
            if(kind=="list"){RefreshCardCatalog();return(200,new{revision=controller.State.Revision,types=cardCatalog.Types.Select(value=>new{definition=value,instances=CustomCardLibrary.InstanceCount(controller.State,value.Type)}).ToArray()});}
            if(kind=="open-directory"){customCards.CheckRoot();Directory.CreateDirectory(customCards.Root);CustomCardShortcut.OpenPath(customCards.Root);return(200,new{ok=true});}
            if(kind=="open-dependency"){
                // Resolve the trusted catalog entry here, never a renderer-supplied URL.
                var link=systemCardCatalog.DependencyUri(String(body,"type"));
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(link.AbsoluteUri){UseShellExecute=true});return(200,new{ok=true});
            }
            if(kind!="uninstall"||controller.Store.ReadOnly||controller.Collections.IsBusy)throw new LayoutEditException("卡片管理操作当前不可用。");
            if(body.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("桌面已更新，请重新确认卸载。");
            var definition=cardCatalog.Require(String(body,"type"));
            if(definition.Custom is null||String(body,"fingerprint")!=definition.Custom.Fingerprint)throw new LayoutEditException("卡片包已变化，请重新打开面板。");
            if(body.GetProperty("instances").GetInt32()!=CustomCardLibrary.InstanceCount(controller.State,definition.Type))throw new LayoutEditException("卡片实例数量已变化，请重新确认。");
            try{customCards.Uninstall(definition,()=>controller.Commit(state=>CustomCardLibrary.RemoveInstances(state,definition.Type),"卡片类型已卸载。"));}
            finally{RefreshCardCatalog();}
            return(200,new{ok=true,revision=controller.State.Revision});
        }
        if(method=="POST"&&uri.AbsolutePath=="/desktop-card-shortcut")
        {
            string monitor=String(body,"monitorId");if(nativeLease is null||HasLostTakeover||!monitorBoards.TryGetValue(monitor,out var boardId))throw new LayoutEditException("桌面尚未就绪。");
            var card=DesktopCards.Require(controller.State.Boards.Single(board=>board.Id==boardId),String(body,"id"));var definition=cardCatalog.Require(card.Type);
            if(definition.Custom?.Kind!="shortcut")throw new LayoutEditException("不是快捷卡片。");
            int index=body.GetProperty("index").GetInt32();var entries=definition.Custom.Content.GetProperty("items");if(index<0||index>=entries.GetArrayLength())throw new InvalidDataException();
            var entry=entries[index];CustomCardShortcut.Open(entry.GetProperty("type").GetString()!,entry.GetProperty("target").GetString()!);return(200,new{ok=true});
        }
        if(method=="POST"&&uri.AbsolutePath=="/desktop-card-action")
        {
            string monitor=String(body,"monitorId"),kind=String(body,"kind");
            if(nativeLease is null||HasLostTakeover||!monitorBoards.TryGetValue(monitor,out var boardId))throw new LayoutEditException("桌面尚未就绪。");
            if(body.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("桌面已更新，请重试。");
            if(controller.Store.ReadOnly)throw new LayoutEditException("桌面布局当前只读。");
            string? addedId=null;int? Position(string key)=>body.TryGetProperty(key,out var value)?value.GetInt32():null;
            controller.Commit(state=>
            {
                var board=state.Boards.Single(row=>row.Id==boardId);
                if(kind=="create")addedId=DesktopCards.Add(state,board,cardCatalog,String(body,"type"),body.GetProperty("columns").GetInt32(),body.GetProperty("rows").GetInt32(),body.GetProperty("settings"),Position("x"),Position("y")).Id;
                else if(kind=="configure")DesktopCards.Configure(board,String(body,"id"),body.GetProperty("settings"));
                else if(kind=="resize")DesktopCards.Resize(board,cardCatalog,String(body,"id"),body.GetProperty("columns").GetInt32(),body.GetProperty("rows").GetInt32());
                else if(kind=="remove")DesktopCards.Remove(board,String(body,"id"));
                else if(kind=="place")DesktopGridActions.Place(board,"card:"+String(body,"id"),body.GetProperty("x").GetInt32(),body.GetProperty("y").GetInt32());
                else if(kind=="move-screen")
                {
                    string target=String(body,"boardId");if(!controller.Screens.Any(screen=>screen.Id==target)||!boardScales.ContainsKey(target))throw new LayoutEditException("目标屏幕尚未就绪。");
                    DesktopCards.Transfer(board,state.Boards.Single(row=>row.Id==target),String(body,"id"),Position("x"),Position("y"));
                }
                else throw new InvalidDataException("Unknown card action");
            },"卡片设置已保存。");
            return(200,new{ok=true,id=addedId,revision=controller.State.Revision});
        }
        if(method=="POST"&&uri.AbsolutePath=="/drop-diagnostic")
        {
            string monitor=String(body,"monitorId"),channel=String(body,"channel"),phase=String(body,"phase");
            if(!monitorBoards.ContainsKey(monitor)||channel is not ("native" or "html" or "cross-preview")||phase is not ("enter" or "drop" or "leave" or "unavailable"))throw new InvalidDataException();
            string key=channel+":"+phase;dropEvents[key]=dropEvents.GetValueOrDefault(key)+1;
            controller.Log("desktop_drop_event",new{monitorId=monitor,channel,phase,count=dropEvents[key]});return(200,new{ok=true});
        }
        if(method=="POST"&&uri.AbsolutePath=="/desktop-page-action")
        {
            string monitor=String(body,"monitorId"),kind=String(body,"kind");
            if(nativeLease is null||HasLostTakeover||!monitorBoards.TryGetValue(monitor,out var boardId))throw new LayoutEditException("桌面尚未就绪。");
            if(controller.Store.ReadOnly||body.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("布局只读或已更新，请重试。");
            controller.Commit(state=>{
                var board=state.Boards.Single(board=>board.Id==boardId);
                if(kind=="select")DesktopPages.Select(board,String(body,"pageId"));
                else if(kind=="add")DesktopPages.Add(board,body.TryGetProperty("before",out var before)&&before.GetBoolean(),body.TryGetProperty("edge",out var edge)&&edge.GetBoolean());
                else if(kind=="delete")DesktopPages.Delete(board,String(body,"pageId"));
                else throw new InvalidDataException("Unknown page action");
            },"桌面页面已更新。");
            return(200,new{ok=true,revision=controller.State.Revision});
        }
        if(method=="POST"&&uri.AbsolutePath=="/composition-reference")
        {
            string monitor=String(body,"monitorId"),destination=String(body,"destination");
            if(!compositionRenderer||nativeLease is null||HasLostTakeover||!monitorBoards.TryGetValue(monitor,out var sourceId)||!boardScales.ContainsKey(destination)||!controller.Screens.Any(s=>s.Id==destination))throw new LayoutEditException("目标桌面尚未就绪。");
            if(body.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("布局已变化。");
            string? Optional(string key)=>body.TryGetProperty(key,out var value)&&value.ValueKind==JsonValueKind.String?value.GetString():null;
            var keys=body.GetProperty("keys").EnumerateArray().Select(v=>v.GetString()!).ToArray();
            controller.Commit(state=>DesktopPages.OnSourcePage(state.Boards.Single(b=>b.Id==sourceId),Optional("sourcePageId"),()=>DesktopGroupBridgeActions.Reference(state,state.Boards.Single(b=>b.Id==sourceId),state.Boards.Single(b=>b.Id==destination),keys,String(body,"anchor"),Optional("sourceFolder"),Optional("destinationFolder"),body.GetProperty("x").GetInt32(),body.GetProperty("y").GetInt32())),"桌面引用位置已保存。");
            return(200,new{ok=true,revision=controller.State.Revision});
        }
        if(method=="POST"&&uri.AbsolutePath is "/composition-folder-order" or "/composition-folder-rename")
        {
            string monitor=String(body,"monitorId");
            if(!compositionRenderer||nativeLease is null||HasLostTakeover||!monitorBoards.TryGetValue(monitor,out var boardId))throw new LayoutEditException("原生桌面尚未就绪。");
            if(body.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("布局已变化。");
            if(uri.AbsolutePath=="/composition-folder-rename"){
                string name=String(body,"name").Trim();if(name.Length is <1 or >80)throw new LayoutEditException("文件夹名称须为 1–80 个字符。");
                controller.Commit(state=>state.Boards.Single(b=>b.Id==boardId).Boxes.Single(f=>f.Id==String(body,"folder")).Name=name,"文件夹名称已保存。");return(200,new{ok=true,revision=controller.State.Revision});
            }
            var keys=body.GetProperty("keys").EnumerateArray().Select(v=>v.GetString()!).ToArray();
            controller.Commit(state=>DesktopFolderOrder.Reorder(state.Boards.Single(b=>b.Id==boardId),String(body,"folder"),keys,String(body,"anchor"),String(body,"target"),body.TryGetProperty("insertionIndex",out var insertion)&&insertion.ValueKind==JsonValueKind.Number?insertion.GetInt32():null),"文件夹顺序已保存。");
            return(200,new{ok=true,revision=controller.State.Revision});
        }
        if(method=="POST"&&uri.AbsolutePath=="/grid-action")
        {
            string monitor=String(body,"monitorId"),kind=String(body,"kind");
            if(controller.Collections.IsBusy&&kind!="open")throw new LayoutEditException("文件移动或系统权限确认尚未完成，请稍候再操作。");
            if(!monitorBoards.TryGetValue(monitor,out var boardId))throw new InvalidDataException();
            if(body.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("布局已在另一处更新，请重试。");
            if(controller.Store.ReadOnly)throw new LayoutEditException("布局当前为只读，原文件已保护。");
            string? Optional(string name)=>body.TryGetProperty(name,out var value)&&value.ValueKind!=JsonValueKind.Null?value.GetString():null;
            if(kind=="open"){var item=Known(String(body,"itemId"));controller.Open(item.Target);return(200,new{ok=true});}
            if(kind is "drop-files" or "drop-desktop")
            {
                DesktopItem[] selected;
                if(kind=="drop-desktop")selected=DesktopDrop.Resolve(controller.Desktop,body.GetProperty("names").EnumerateArray().Select(row=>row.GetString()??"").ToArray());
                else
                {
                    var paths=body.GetProperty("paths").EnumerateArray().Select(row=>row.GetString()??"").Distinct(StringComparer.OrdinalIgnoreCase).ToArray();if(paths.Length is <1 or >100)throw new InvalidDataException();
                    selected=paths.Select(path=>controller.Desktop.FirstOrDefault(item=>StringComparer.OrdinalIgnoreCase.Equals(item.Target,path))??throw new LayoutEditException("移动收纳只接受 Windows 桌面的直接项目；请从桌面拖入。")).ToArray();
                }
                controller.Commit(state=>UnifiedDesktopGrid.Collect(state,state.Boards.Single(row=>row.Id==boardId),String(body,"folderId"),selected),"已虚拟收纳，原文件路径未变。");
                controller.Log("desktop_collected",new{kind,count=selected.Length,revision=controller.State.Revision});return(200,new{ok=true,revision=controller.State.Revision});
            }
            controller.Commit(state=>
            {
                var board=state.Boards.Single(b=>b.Id==boardId);
                var display=controller.Screens.Single(screen=>screen.Id==boardId);var native=nativeDesktop.Snapshot;var projection=Projection(board,display,boardScales[boardId]);
                if(kind=="page-drop")
                {
                    DesktopPages.Drop(state,board,String(body,"sourcePageId"),String(body,"destinationPageId"),body.GetProperty("tileIds").EnumerateArray().Select(value=>value.GetString()!).ToArray(),String(body,"tileId"),Optional("sourceFolder"),Optional("destinationFolder"),body.GetProperty("x").GetInt32(),body.GetProperty("y").GetInt32());
                }
                else if(kind=="restore-unplaced")
                {
                    string key=String(body,"tileId");var old=board.Unplaced.GetValueOrDefault(key)??throw new LayoutEditException("待放置项目已变化。");
                    var rect=DesktopGridActions.ReserveSpace(board,old.W,old.H);board.Unplaced.Remove(key);board.Grid!.Placements.Add(key,rect);
                }
                else if(kind=="cross-drop")
                {
                    double cx=body.GetProperty("clientX").GetDouble(),cy=body.GetProperty("clientY").GetDouble(),ox=body.GetProperty("offsetX").GetDouble(),oy=body.GetProperty("offsetY").GetDouble();
                    if(!new[]{cx,cy,ox,oy}.All(double.IsFinite)||Math.Abs(cx)>65536||Math.Abs(cy)>65536||ox is <0 or >4096||oy is <0 or >4096)throw new InvalidDataException();
                    double sourceScale=boardScales[boardId],px=display.DesktopLeft+cx*sourceScale,py=display.DesktopTop+cy*sourceScale;
                    var targetDisplay=controller.Screens.SingleOrDefault(screen=>screen.Id!=boardId&&px>=screen.X&&px<screen.X+screen.Width&&py>=screen.Y&&py<screen.Y+screen.Height)??throw new LayoutEditException("请放在另一屏的桌面区域，原位置已保留。");
                    if(!boardScales.TryGetValue(targetDisplay.Id,out var targetScale)||!widgetWindows.Any(row=>monitorBoards[row.Key]==targetDisplay.Id))throw new LayoutEditException("目标屏幕尚未就绪。");
                    var targetBoard=state.Boards.Single(row=>row.Id==targetDisplay.Id);var targetProjection=Projection(state.Boards.Single(b=>b.Id==targetDisplay.Id),targetDisplay,targetScale);UnifiedDesktopGrid.Ensure(targetBoard,targetProjection);
                    var scroll=boardScroll.GetValueOrDefault(targetDisplay.Id);
                    var point=UnifiedDesktopGrid.ProjectDrop(display,sourceScale,cx,cy,targetDisplay,targetScale,scroll.X,scroll.Y,ox,oy,targetProjection);
                    string key=String(body,"tileId");
                    double targetCell=targetProjection.StepX/targetScale;
                    string? destinationFolder=key.StartsWith("item:",StringComparison.Ordinal)?targetBoard.Boxes.FirstOrDefault(box=>{if(!targetBoard.Grid!.Placements.TryGetValue(DesktopGridActions.FolderKey(box),out var r))return false;return point.X>=r.X*targetCell&&point.X< (r.X+r.W)*targetCell-UnifiedDesktopGrid.Inset*targetCell/96&&point.Y>=r.Y*targetCell&&point.Y<(r.Y+r.H)*targetCell-UnifiedDesktopGrid.Inset*targetCell/96;})?.Id:null;
                    string? previousPage=board.ActivePageId;
                    if(Optional("sourcePageId") is {} sourcePage)DesktopPages.Select(board,sourcePage);
                    try
                    {
                        if(key.StartsWith("card:",StringComparison.Ordinal))DesktopCards.Transfer(board,targetBoard,key[5..],point.Column,point.Row);
                        else if(body.TryGetProperty("itemIds",out var groupIds)&&groupIds.GetArrayLength()>1)DesktopGroupBridgeActions.Reference(state,board,targetBoard,groupIds.EnumerateArray().Select(value=>"item:"+value.GetString()).ToArray(),key,Optional("sourceFolder"),destinationFolder,point.Column,point.Row);
                        else UnifiedDesktopGrid.Transfer(state,board,targetBoard,key,Optional("sourceFolder"),point.Column,point.Row,destinationFolder);
                    }finally{board.ActivePageId=previousPage;}
                }
                else if(kind=="group-reference")DesktopGroupBridgeActions.Reference(state,board,board,body.GetProperty("itemIds").EnumerateArray().Select(value=>"item:"+value.GetString()).ToArray(),"item:"+String(body,"itemId"),Optional("sourceFolder"),Optional("destinationFolder"),body.GetProperty("x").GetInt32(),body.GetProperty("y").GetInt32());
                else if(kind=="dissolve")DesktopGridActions.Dissolve(board,String(body,"folderId"));
                else if(kind=="place")DesktopGridActions.Place(board,String(body,"tileId"),body.GetProperty("x").GetInt32(),body.GetProperty("y").GetInt32());
                else if(kind=="resize")DesktopGridActions.Resize(board,String(body,"folderId"),body.GetProperty("columns").GetInt32(),body.GetProperty("rows").GetInt32());
                else if(kind=="reference")DesktopGridActions.MoveReference(state,board,Known(String(body,"itemId")),Optional("sourceFolder"),Optional("destinationFolder"),body.TryGetProperty("x",out var x)?x.GetInt32():null,body.TryGetProperty("y",out var y)?y.GetInt32():null);
                else if(kind=="new-folder")DesktopCreation.CreateFolder(board,projection,DesktopCreation.Position(body));
                else if(kind=="reposition-folders")UnifiedDesktopGrid.Ensure(board,projection);
                else if(kind=="move-screen")
                {
                    string target=String(body,"boardId");if(!controller.Screens.Any(display=>display.Id==target))throw new LayoutEditException("目标屏幕已断开，位置未改变。");
                    string key=String(body,"tileId");int? dropX=body.TryGetProperty("x",out var suppliedX)?suppliedX.GetInt32():null,dropY=body.TryGetProperty("y",out var suppliedY)?suppliedY.GetInt32():null;
                    DesktopPages.OnSourcePage(board,Optional("sourcePageId"),()=>{
                        var destination=state.Boards.Single(b=>b.Id==target);
                        if(key.StartsWith("card:",StringComparison.Ordinal))DesktopCards.Transfer(board,destination,key[5..],dropX,dropY);
                        else if(dropX.HasValue&&dropY.HasValue)UnifiedDesktopGrid.Transfer(state,board,destination,key,null,dropX.Value,dropY.Value);
                        else DesktopGridActions.MoveScreen(board,destination,key);
                    });
                    var targetDisplay=controller.Screens.Single(screen=>screen.Id==target);if(!boardScales.TryGetValue(target,out var targetScale))throw new LayoutEditException("目标屏幕尚未就绪。");
                    UnifiedDesktopGrid.Ensure(state.Boards.Single(b=>b.Id==target),Projection(state.Boards.Single(b=>b.Id==targetDisplay.Id),targetDisplay,targetScale));
                }
                else throw new InvalidDataException("Unknown grid action");
                if(kind is "place" or "resize")DesktopGridActions.Validate(board);
                if(kind is "place" or "page-drop" or "cross-drop" or "group-reference" or "reference" or "move-screen"){DesktopSort.Manual(board);if(body.TryGetProperty("boardId",out var moved))foreach(var b in state.Boards.Where(b=>b.Id==moved.GetString()))DesktopSort.Manual(b);}
            },"桌面布局已保存。");
            controller.Log("desktop_grid_action",new{kind,monitorId=monitor,revision=controller.State.Revision,source="authenticated_widget_not_human_acceptance"});
            return(200,new{ok=true,revision=controller.State.Revision});
        }
        if (method == "GET" && uri.AbsolutePath == "/state")
        {
            string monitor = Uri.UnescapeDataString(uri.Query.TrimStart('?').Split('&').FirstOrDefault(x=>x.StartsWith("monitor="))?[8..] ?? "");
            if (!monitorBoards.TryGetValue(monitor,out var boardId)) return (404,new { error = "Monitor not registered" });
            var board = controller.State.Boards.Single(x=>x.Id==boardId);
            var assigned = board.Boxes.SelectMany(x=>x.Items).Select(x=>x.Target).ToHashSet(StringComparer.OrdinalIgnoreCase);
            return (200,new { version=AppController.Version,sessionId,boardId,boardName=board.Name,readOnly=controller.Store.ReadOnly,
                boxes=board.Boxes.Select(x=>new { id=x.Id,name=x.Name,collapsed=x.Collapsed,items=x.Items.Select(Item).ToArray() }).ToArray(),
                library=controller.Desktop.Where(x=>!assigned.Contains(x.Target)).Select(Item).ToArray(),
                destinations=controller.State.Boards.SelectMany(b=>b.Boxes.Select(x=>new{id=x.Id,label=b.Name+" / "+x.Name})).ToArray() });
        }
        if(method=="POST"&&uri.AbsolutePath=="/prepare-window")
        {
            string monitor=String(body,"monitorId");
            if(!monitorBoards.TryGetValue(monitor,out var boardId)||String(body,"widgetId")!=WidgetId)throw new InvalidDataException();
            long hwnd=body.GetProperty("windowId").GetInt64();
            long parent=NativeDesktopHost.Attach(hwnd,controller.Screens.Single(screen=>screen.Id==boardId),controller.Root);
            controller.Log("desktop_window_attached",new{monitorId=monitor,windowId=hwnd,parent});return(200,new{ok=true,parent});
        }
        if (method == "POST" && uri.AbsolutePath == "/ready")
        {
            string monitor=String(body,"monitorId");
            controller.RefreshDisplays();ReconcileTopology();
            if (!monitorBoards.ContainsKey(monitor) || String(body,"widgetId")!=WidgetId) throw new InvalidDataException();
            long hwnd=body.GetProperty("windowId").GetInt64();NativeRegion.Verify(hwnd,controller.Root);
            if(nativeLease is not null&&(!nativeLease.IsAlive||!nativeLease.Owns(hwnd)))SuspendTakeover("desktop_window_recreated");
            widgetWindows[monitor]=hwnd;
            try
            {
            // Finalize OS visibility and Explorer ownership after the host show call.
            // The guard independently checks the resulting desktop owner and host.
            NativeDesktopHost.Attach(hwnd,controller.Screens.Single(screen=>screen.Id==monitorBoards[monitor]),controller.Root,show:true);
            int composition=NativeDesktopHost.RestoreComposition(hwnd,controller.Root);
            controller.Log("desktop_composition_ready",new{monitorId=monitor,windowId=hwnd,composition});
            System.Runtime.InteropServices.Marshal.ThrowExceptionForHR(composition);
            ApplyRegions(monitor,body);
            if(String(body,"frontendVersion")!=AppController.Version)throw new InvalidDataException("桌面前端与适配器版本不一致。");
            ready[monitor]=new { monitorId=monitor,at=DateTimeOffset.Now,widgetId=WidgetId,windowId=hwnd,origin=String(body,"origin"),sdkVersion=String(body,"sdkVersion"),frontendVersion=String(body,"frontendVersion"),mode="unified-desktop",inputRegionApplied=true };
            bool allReady=controller.Screens.All(screen=>monitorBoards.Any(row=>row.Value==screen.Id&&ready.ContainsKey(row.Key)));
            if(allReady&&nativeLease is null)
            {
                if(controller.Store.ReadOnly)throw new LayoutEditException("布局只读，保留 Windows 原图标。");
                var windows=monitorBoards.Where(row=>controller.Screens.Any(screen=>screen.Id==row.Value)).Select(row=>widgetWindows[row.Key]).Distinct().ToArray();
                long generation=++leaseGeneration;
                try{nativeLease=new NativeDesktopLease(controller.Root,windows,()=>{if(generation!=leaseGeneration)return;SuspendTakeover("guard_or_window_ended");},compositionRenderer?nativeRendererPid:0);if(!nativeLease.IsAlive)throw new InvalidOperationException("桌面恢复守护尚未就绪，菜单继续暂停。");contextMenu.Resume();HasLostTakeover=false;}
                catch{++leaseGeneration;ready.Remove(monitor);foreach(long window in widgetWindows.Values)try{NativeRegion.Verify(window,controller.Root);HideDesktopWindow(new(window),0);}catch{}throw;}
                controller.Log("desktop_native_takeover",new{windows=windows.Length,physicalFileMoves=false,menuEpoch=contextMenu.CaptureEpoch()});changes.Notify();
            }
            WriteReadyState();
            var finalRect=NativeDesktopHost.ReadBounds(hwnd,controller.Root);
            controller.Log("desktop_workspace_ready",new{monitorId=monitor,workspaceRevision,rect=finalRect});
            return (200,new { ok=true,workspaceRevision,rect=finalRect });
            }
            catch
            {
                contextMenu.Suspend();
                nativeLease?.Dispose();nativeLease=null;ready.Remove(monitor);HasLostTakeover=true;
                foreach(long window in widgetWindows.Values)try{NativeRegion.Verify(window,controller.Root);HideDesktopWindow(new(window),0);}catch{}
                try{WriteReadyState();}catch{}
                throw;
            }
        }
        if(method=="POST"&&uri.AbsolutePath=="/regions"){string monitor=String(body,"monitorId");return(200,new{ok=true,regions=ApplyRegions(monitor,body)});}
        if (method == "POST" && uri.AbsolutePath == "/action")
        {
            string monitor=String(body,"monitorId"), kind=String(body,"kind");
            if (!monitorBoards.TryGetValue(monitor,out var boardId)) throw new InvalidDataException();
            Box BoxIn(LayoutState state,string id) => state.Boards.SelectMany(x=>x.Boxes).Single(x=>x.Id==id);
            if(kind=="settings") showSettings();
            else if(kind=="feedback") Application.Current.Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background,showFeedback);
            else if(kind=="exit") Application.Current.Dispatcher.BeginInvoke(quit);
            else if(kind=="refresh") {nativeDesktop.Refresh();controller.RefreshDesktop();}
            else if(kind=="open" || kind=="reveal")
            {
                var item=Known(String(body,"itemId")); if(kind=="open")controller.Open(item.Target);else controller.Reveal(item);
            }
            else if(kind=="assign")
            {
                var item=Known(String(body,"itemId")); string box=String(body,"boxId");
                controller.Commit(s=>LayoutActions.Add(BoxIn(s,box),[item]),"桌面分组已保存。");
            }
            else if(kind=="drop-files")
            {
                var paths=body.GetProperty("paths").EnumerateArray().Select(x=>x.GetString()??"").ToArray();
                if(paths.Length is <1 or >100)throw new InvalidDataException();
                var items=paths.Select(ShellDesktop.FromPath).ToArray();string box=String(body,"boxId");
                controller.Commit(s=>LayoutActions.Add(BoxIn(s,box),items),"已加入拖入的文件引用。");
            }
            else if(kind=="move")
            {
                var item=Known(String(body,"itemId"));string from=String(body,"sourceBox"),to=String(body,"boxId");
                controller.Commit(s=>LayoutActions.Move(s,from,item.Target,to),"跨盒/跨屏分组已保存。");
            }
            else if(kind=="remove")
            {
                var item=Known(String(body,"itemId"));string box=String(body,"boxId");
                controller.Commit(s=>BoxIn(s,box).Items.RemoveAll(x=>StringComparer.OrdinalIgnoreCase.Equals(x.Target,item.Target)),"引用已移除，原文件保留。");
            }
            else if(kind=="toggle")
            {
                string box=String(body,"boxId");controller.Commit(s=>{var b=BoxIn(s,box);b.Collapsed=!b.Collapsed;},"收纳盒状态已保存。");
            }
            else return(400,new{error="Unknown action"});
            controller.Log("desktop_action",new{kind,monitorId=monitor,source="authenticated_widget_request_not_manual_acceptance"});
            return(200,new{ok=true});
        }
        return(404,new{error="Not found"});
    }
    private void WriteReadyState()=>StateStore.AtomicWrite(Path.Combine(controller.Root,"runtime/desktop/ready.json"),JsonSerializer.Serialize(new{sessionId,version=AppController.Version,nativeIconTakeover=nativeLease?.IsAlive==true&&!HasLostTakeover,instances=ready.Values.ToArray()},Wire),null);
    private static string String(JsonElement body,string name) => body.GetProperty(name).GetString() ?? throw new InvalidDataException();
    private int ApplyRegions(string monitor,JsonElement body)
    {
        if(!widgetWindows.TryGetValue(monitor,out long hwnd))throw new InvalidDataException("文件夹窗口尚未注册。");
        var viewport=body.GetProperty("viewport");var regions=body.GetProperty("regions").EnumerateArray().Select(rect=>(Left:rect.GetProperty("left").GetDouble(),Top:rect.GetProperty("top").GetDouble(),Right:rect.GetProperty("right").GetDouble(),Bottom:rect.GetProperty("bottom").GetDouble())).ToArray();
        double scrollX=body.TryGetProperty("scrollX",out var sx)?sx.GetDouble():0,scrollY=body.TryGetProperty("scrollY",out var sy)?sy.GetDouble():0;
        if(!double.IsFinite(scrollX)||!double.IsFinite(scrollY)||scrollX is <0 or >32768||scrollY is <0 or >32768)throw new InvalidDataException();
        boardScroll[monitorBoards[monitor]]=(scrollX,scrollY);
        return NativeRegion.Apply(hwnd,viewport.GetProperty("width").GetDouble(),viewport.GetProperty("height").GetDouble(),regions,controller.Root);
    }
    private static string Id(DesktopItem item) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(item.Target.ToUpperInvariant())));
    private JsonElement? CrossFileDrop(JsonElement body)
    {
        string monitor=String(body,"monitorId"),tile=String(body,"tileId");if(!tile.StartsWith("item:",StringComparison.Ordinal)||!monitorBoards.TryGetValue(monitor,out var boardId))return null;
        double x=body.GetProperty("clientX").GetDouble(),y=body.GetProperty("clientY").GetDouble();if(!double.IsFinite(x)||!double.IsFinite(y)||Math.Abs(x)>65536||Math.Abs(y)>65536)throw new InvalidDataException();
        var source=controller.Screens.Single(screen=>screen.Id==boardId);double scale=boardScales[boardId],px=source.X+x*scale,py=source.Y+y*scale;
        var display=controller.Screens.SingleOrDefault(screen=>screen.Id!=boardId&&px>=screen.X&&px<screen.X+screen.Width&&py>=screen.Y&&py<screen.Y+screen.Height);
        if(display is null||!boardScales.TryGetValue(display.Id,out var targetScale))return null;
        var scroll=boardScroll.GetValueOrDefault(display.Id);var point=UnifiedDesktopGrid.ProjectDrop(source,scale,x,y,display,targetScale,scroll.X,scroll.Y,0,0,Projection(controller.State.Boards.Single(b=>b.Id==display.Id),display,targetScale));
        var target=DesktopFileDrop.Hit(controller.State.Boards.Single(board=>board.Id==display.Id),point.X,point.Y,Projection(controller.State.Boards.Single(b=>b.Id==display.Id),display,targetScale).StepX/targetScale);if(target is null||Id(target)==tile[5..])return null;
        if(body.TryGetProperty("itemIds",out var groupIds)&&groupIds.GetArrayLength()>1){if(groupIds.EnumerateArray().Any(id=>id.GetString()==Id(target)))return null;return JsonSerializer.SerializeToElement(new{monitorId=monitor,revision=body.GetProperty("revision").GetInt64(),itemIds=groupIds.Clone(),targetId=Id(target),control=body.TryGetProperty("control",out var ctrl)&&ctrl.GetBoolean(),shift=body.TryGetProperty("shift",out var shift)&&shift.GetBoolean()});}
        return JsonSerializer.SerializeToElement(new{monitorId=monitor,revision=body.GetProperty("revision").GetInt64(),itemId=tile[5..],targetId=Id(target),action=DesktopFileDrop.IsRecycleBin(target)?"recycle":"move"});
    }
    private string[] KnownSelection(JsonElement body)
    {
        if(!body.TryGetProperty("itemIds",out var values))return [Known(String(body,"itemId")).Target];
        if(values.ValueKind!=JsonValueKind.Array||values.GetArrayLength() is <1 or >256)throw new LayoutEditException("一次请选择 1 至 256 个桌面项目。");
        var ids=values.EnumerateArray().Select(value=>value.ValueKind==JsonValueKind.String?value.GetString()!:throw new InvalidDataException()).Distinct(StringComparer.Ordinal).ToArray();
        return ids.Select(id=>Known(id).Target).ToArray();
    }
    private readonly Dictionary<string,(string Monitor,long Window)> issuedDrags=new(StringComparer.Ordinal);
    private DesktopItem Known(string id) => controller.Desktop.Concat(controller.State.Boards.SelectMany(x=>x.RootItems)).Concat(controller.State.Boards.SelectMany(x=>x.Boxes).SelectMany(x=>x.Items)).First(x=>Id(x)==id);
    private static string Query(Uri uri,string key)=>Uri.UnescapeDataString(uri.Query.TrimStart('?').Split('&').FirstOrDefault(x=>x.StartsWith(key+"=",StringComparison.Ordinal))?[(key.Length+1)..]??"");
    private object Item(DesktopItem item)
    {
        string key=item.IsVirtual?"virtual":item.IsFolder?"folder":Path.GetExtension(item.Target);
        if(!iconCache.TryGetValue(key,out var icon))
        {
            var source=ShellDesktop.Icon(item);
            if(source is not null){var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(source));using var stream=new MemoryStream();encoder.Save(stream);icon="data:image/png;base64,"+Convert.ToBase64String(stream.ToArray());}
            if(iconCache.Count<96)iconCache[key]=icon;
        }
        return new{id=Id(item),name=item.Name,icon,isFolder=item.IsFolder,isVirtual=item.IsVirtual};
    }
    private static async Task Reply(Stream stream,int code,object body,string? origin,CancellationToken cancellation)
    {
        byte[] payload=code==204?[]:JsonSerializer.SerializeToUtf8Bytes(body,Wire);
        string cors=Cors(origin);
        byte[] header=Encoding.ASCII.GetBytes($"HTTP/1.1 {code} Response\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {payload.Length}\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n{cors}\r\n");
        await stream.WriteAsync(header,cancellation);await stream.WriteAsync(payload,cancellation);
    }
    private static string Cors(string? origin)=>origin is not null&&Origins.Contains(origin)?$"Access-Control-Allow-Origin: {origin}\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Authorization, Content-Type\r\nAccess-Control-Allow-Private-Network: true\r\nVary: Origin\r\n":"";
    [System.Runtime.InteropServices.DllImport("user32.dll",EntryPoint="ShowWindow")]private static extern bool HideDesktopWindow(IntPtr hwnd,int command);
    private bool disposed;
    public void Dispose(){if(disposed)return;disposed=true;controller.WallpaperGuard?.Dispose();controller.WallpaperGuard=null;directoryWatch?.Dispose();workAreaWatch?.Dispose();contextMenu.Dispose();nativeLease?.Dispose();nativeLease=null;controller.PrepareDesktopChange=null;controller.Changed-=ControllerChanged;controller.Collections.BecameIdle-=ControllerChanged;recycleBin.Dispose();icons.Dispose();nativeDesktop.Dispose();stopping.Cancel();listener.Stop();stopping.Dispose();}
}
