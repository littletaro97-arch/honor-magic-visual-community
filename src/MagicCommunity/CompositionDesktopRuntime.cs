using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;

namespace MagicCommunity;
// Rendering adapter only. Installed file operations pass through the existing
// authenticated DesktopBridge, its menu epoch, durable transactions and guard.
public sealed partial class CompositionDesktopRuntime : IDisposable
{
    private static readonly JsonSerializerOptions Wire=new(){PropertyNamingPolicy=JsonNamingPolicy.CamelCase};
    private readonly DesktopLayoutUndo layoutUndo=new();
    private readonly DesktopGridEditSession gridEditor;
    private readonly SemaphoreSlim sorting=new(1,1);
    private readonly AppController controller;
    private readonly DesktopBridge? bridge;
    private readonly HttpClient? client;
    private readonly bool preview;
    private readonly string diagnostics,session=Guid.NewGuid().ToString("N");
    private readonly Dispatcher ui=Dispatcher.CurrentDispatcher;
    private readonly CancellationTokenSource stop=new();
    private readonly SemaphoreSlim sender=new(1,1);
    private readonly NamedPipeServerStream pipe;
    private readonly Dictionary<string,(long Window,double Scale)> windows=new(StringComparer.Ordinal);
    private readonly DesktopIcons? previewIcons;
    private readonly DesktopCardCatalog catalog;
    private readonly Dictionary<string,TaskCompletionSource<JsonElement>> probes=new(StringComparer.Ordinal);
    private readonly HashSet<string> visible=new(StringComparer.Ordinal);
    private readonly CompositionCardData data;
    private readonly EncoDeckCardSource encodeck;
    private readonly NativeTodoCards todos=new();
    private readonly NativeHonorServices honorServices;
    private readonly NativeHandoffCards handoff;
    private readonly EncoDeckIntegration encodeckApp;
    private readonly NativeWeather weather;
    private readonly NativeClipboardImage clipboardImage;
    private readonly CompositionPopupSession? popups;
    private readonly CompositionShellSession? shell;
    private NativeSystemUiSession? independentSystem;
    private InstalledDesktopTransition? entrance;
    internal Exception? StartupFailure { get; private set; }
    private CompositionDragLease? dragLease;
    private Process? child;
    private CompositionRendererIdentity? identity;
    private bool disposed,connected,queued,previewBenchmarkComplete;
    private readonly string pipeName="MagicCompositionTrial-"+Guid.NewGuid().ToString("N");
    private const int MaxPacket=1024*1024;
    public bool HasLostTakeover=>bridge?.HasLostTakeover??false;
    private CompositionDesktopRuntime(AppController controller,DesktopBridge? bridge,string diagnostics,bool preview)
    {
        this.controller=controller;gridEditor=new(controller,layoutUndo);this.bridge=bridge;this.preview=preview;this.diagnostics=diagnostics;honorServices=new(QueueSnapshot);handoff=new(controller,QueueSnapshot,bridge is null?null:bridge.NativeIcon);
        Directory.CreateDirectory(diagnostics);
        pipe=new(pipeName,PipeDirection.InOut,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous|PipeOptions.CurrentUserOnly);
        catalog=new(bridge?.NativeCardDefinitions??DesktopCardCatalog.Load(Path.Combine(AppContext.BaseDirectory,"desktop-widget/card-catalog.json")).Types);
        client=bridge?.NativeClient();
        if(bridge is not null){bridge.NativeIconsReady+=IconsReady;bridge.NativeIconInvalidated+=IconInvalidated;bridge.NativeImageInvalidated+=ImageInvalidated;bridge.NativeWorkspaceChanged+=QueueSnapshot;}
        else previewIcons=new(IconsReady);
        weather=new(packet=>{if(connected&&!disposed)_=Send(new{type="weather-data",data=packet});},notice:text=>{if(connected&&!disposed)_=Send(new{type="card-notice",title="天气",text});},log:controller.Log);
        clipboardImage=new(packet=>{if(connected&&!disposed)_=Send(new{type="clipboard-image",data=packet});});
        data=new(packet=>{if(connected&&!disposed)_=Send(new{type="card-data",data=packet});});
        encodeckApp=new(controller.Root);
        encodeck=new(encodeckApp,packet=>connected&&!disposed?Send(new{type="encodeck-data",data=packet}):Task.CompletedTask);
        if(bridge is not null){popups=new(controller.Root,CompositionPopupSession.Selected(controller.Root),ui,Send,()=>connected&&!disposed&&bridge.NativePopupReady);popups.Icon=bridge.NativePanelIcon;popups.ApplicationIcon=bridge.NativeApplicationIcon;popups.ApplicationIdentity=bridge.NativeApplicationIdentity;bridge.NativePopups=popups;shell=new(controller.Root,ui,Send){Icon=bridge.NativePanelIcon,ApplicationIcon=bridge.NativeApplicationIcon,ApplicationIdentity=bridge.NativeApplicationIdentity,InvalidateApplicationIcon=bridge.NativeInvalidateApplicationIcon,OpenSettings=bridge.NativeOpenSettings,StaticWorkspace=bridge.NativeShellWorkspace,PowerMode=mode=>{if(controller.PowerMode!=mode){controller.PowerMode=mode;controller.SetStatus("电源状态已更新。");}}};bridge.NativeShell=shell;bridge.NativeProbe=Probe;}
        controller.Changed+=QueueSnapshot;
    }
    private async Task<object> Probe(JsonElement body,CancellationToken cancellation){
        string action=body.TryGetProperty("action",out var a)?a.GetString()??"snapshot":"snapshot";
        if(action=="close-popup"){if(popups is not null)await popups.Close(cancellation);action="snapshot";}
        if(action=="open-popup"){
            string widget=NativePopupIds.Canonical(body.GetProperty("widget").GetString()??"");
            if(!CompositionPopupSession.Selected(controller.Root).Contains(widget))throw new InvalidDataException("该原生弹窗未启用。");
            shell?.Enqueue(JsonSerializer.SerializeToElement(new{id="__measure-popup",widget}));action="snapshot";
        }
        if(action is not ("snapshot" or "open-folder" or "close-folder" or "show-dock-preview" or "hide-dock-preview" or "show-flyout"))throw new InvalidDataException("不支持的测量动作。");
        string? board=Optional(body,"board"),folder=Optional(body,"folder");
        if(action is "open-folder" or "close-folder")await ui.InvokeAsync(()=>{var b=Board(board??throw new InvalidDataException());if(action=="open-folder"&&!b.Boxes.Any(f=>f.Id==folder))throw new InvalidDataException("文件夹已变化。");});
        string nonce=Guid.NewGuid().ToString("N");var result=new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        await ui.InvokeAsync(()=>{if(probes.Count>=4)throw new InvalidDataException("测量请求过多。");probes.Add(nonce,result);});
        try{long? epoch=body.TryGetProperty("expectedInputEpoch",out var epochValue)&&epochValue.ValueKind==JsonValueKind.Number?epochValue.GetInt64():null;await Send(new{type="resource-probe",nonce,action,board,folder,expectedInputEpoch=epoch});var received=await result.Task.WaitAsync(TimeSpan.FromSeconds(5),cancellation);var report=System.Text.Json.Nodes.JsonNode.Parse(received.GetRawText())!.AsObject();if(independentSystem is not null)report["independentSystem"]=JsonSerializer.SerializeToNode(independentSystem.Diagnostics);return JsonSerializer.SerializeToElement(report);}
        finally{await ui.InvokeAsync(()=>probes.Remove(nonce));}
    }
    private object Snapshot()
    {
        var definitions=bridge?.NativeCardDefinitions??catalog.Types;
        bool honorSignedIn=HonorPcManager.ServicesVisible;
        var boards=controller.State.Boards.Where(b=>controller.Screens.Any(s=>s.Id==b.Id)).ToArray();
        if(boards.Length==0)throw new InvalidDataException("没有可显示的桌面。");
        var visibleCards=boards.Where(b=>visible.Contains(b.Id)).SelectMany(b=>DesktopCards.Items(b).Where(c=>b.Grid?.Placements.ContainsKey(DesktopCards.Key(c))==true))
            .Concat(recoveryVisible?DesktopDisplayRecovery.Offline(controller.State,controller.Screens).Where(b=>b.Id==recoveryBoard).SelectMany(DesktopCards.Items).Where(c=>recoveryCardIds.Contains(DesktopCards.Key(c))):[]).ToArray();
        var visibleTypes=visibleCards.Select(c=>c.Type).ToArray();
        if(!preview){weather.Demand(visibleCards);clipboardImage.Demand(visibleCards.Any(c=>c.Type=="system.clipboard-image"));encodeck.Demand(visibleTypes.Contains("system.encodeck",StringComparer.Ordinal));}
        independentSystem?.MediaCardDemand(!preview&&visibleTypes.Contains("system.media",StringComparer.Ordinal));data.Demand(visibleTypes);handoff.Demand(visibleCards);honorServices.Demand(honorSignedIn&&!preview&&visibleTypes.Contains("system.honor-service",StringComparer.Ordinal));
        return new{session,iconIdentityVersion=bridge?.NativeIconIdentityVersion??previewIcons?.IdentityVersion??0,revision=controller.State.Revision,recoveryCount=DesktopDisplayRecovery.Offline(controller.State,controller.Screens).Sum(b=>DesktopGridActions.Expected(b).Count),components=true,desktopRuntime=!preview,syntheticIcons=false,workspaceVersion=bridge?.NativeWorkspaceVersion??0,
            handoffData=handoff.Projection(visibleCards),honorData=honorServices.Projection(bridge is null?null:bridge.NativeIcon),todoData=visibleTypes.Any(NativeTodoCards.IsTodo)?todos.Projection(controller.State):new{},preferences=CommunityPreferences.Load(controller.Root),recycleBinFull=RecycleBinWatch.CurrentFull,frontendVersion=AppController.Version,fixtureRoot=diagnostics,honorSignedIn,cardDefinitions=definitions.Where(d=>honorSignedIn||d.Type!="system.honor-service").ToArray(),cardCatalogVersion=bridge?.NativeCardCatalogVersion??0,boards=boards.Select(board=>{
                var screen=controller.Screens.Single(s=>s.Id==board.Id);double scale=windows.GetValueOrDefault(board.Id).Scale;if(scale==0)scale=1;
                var projection=bridge?.Projection(board,screen,scale)??DesktopGridLayout.Project(board,screen,scale);var placements=board.Grid?.Placements??new Dictionary<string,GridRect>();
                var allTiles=NativeDesktopTrial.DesktopTiles(board).ToArray();var names=allTiles.ToDictionary(t=>t.Key,t=>t.Name);
                foreach(var c in DesktopCards.Items(board))names[DesktopCards.Key(c)]=definitions.FirstOrDefault(d=>d.Type==c.Type)?.Title??"卡片";
                var tiles=allTiles.Where(t=>placements.ContainsKey(t.Key)).Select(t=>(object)new{t.Key,t.Name,target=t.Item?.Target,iconExecutable=t.Item is null?"":bridge?.NativeDesktopIdentity(t.Item)??previewIcons?.DesktopIdentity(t.Item)??"",isVirtual=t.Item?.IsVirtual??false,isFolder=t.Item?.IsFolder??false,folderId=t.Folder?.Id,showItemCount=t.Folder?.ShowItemCount??true,items=t.Folder?.Items.Select(i=>new{key=DesktopGridActions.ItemKey(i),i.Name,i.Target,iconExecutable=bridge?.NativeDesktopIdentity(i)??previewIcons?.DesktopIdentity(i)??"",isVirtual=i.IsVirtual,isFolder=i.IsFolder})}).ToList();
                foreach(var card in DesktopCards.Items(board).Where(c=>placements.ContainsKey(DesktopCards.Key(c)))){
                    var definition=definitions.FirstOrDefault(d=>d.Type==card.Type);
                    tiles.Add(new{key=DesktopCards.Key(card),name=definition?.Title??"卡片",card=new{card.Id,card.Type,card.Columns,card.Rows,Settings=card.Type=="system.handoff"?JsonSerializer.SerializeToElement(new{version=1}):card.Settings,custom=definition?.Custom}});
                }
                return new{board.Id,board.Name,screen,
                    projection=preview?new{originX=14d,originY=76d,stepX=96d,stepY=96d,scale=scale}:new{originX=projection.OriginX/scale,originY=projection.OriginY/scale,stepX=projection.StepX/scale,stepY=projection.StepY/scale,scale=scale},
                    sortMode=board.SortSettings?.Mode??"custom",sortDescending=board.SortSettings?.Descending??false,gridNotice=projection.Notice,pageId=board.ActivePageId,pages=board.Pages?.Select(p=>p.Id).ToArray(),paged=board.Paged,pageLimit=DesktopPages.MaximumPages,gridColumns=board.Grid?.Columns??0,gridRows=board.Grid?.Rows??0,
                    pageCounts=board.Pages?.ToDictionary(p=>p.Id,p=>p.Grid.Placements.Count),unplaced=board.Unplaced.Keys.Take(1024).Select(key=>new{key,name=names.GetValueOrDefault(key,"未命名项目")}),
                    placements=placements.Select(p=>new{key=p.Key,p.Value.X,p.Value.Y,p.Value.W,p.Value.H}),tiles};
            })};
    }
    private void QueueSnapshot()
    {
        if(disposed||!connected||queued)return;queued=true;
        ui.BeginInvoke(DispatcherPriority.Background,new Action(async()=>{
            queued=false;if(disposed)return;
            try{await Send(new{type="state",snapshot=Snapshot()});if(recoveryVisible)await Send(new{type="recovery-view",data=RecoveryView(recoveryBoard,recoveryFolder,recoveryOffset)});}catch(Exception e){controller.Log("composition_snapshot_error",new{error=e.Message});}
        }));
    }
    private void ImageInvalidated(string path){if(!disposed&&connected)_=Send(new{type="images-invalidated",path});}
    private void IconInvalidated(string key){if(!disposed&&connected){_=Send(new{type="icons-invalidated",keys=new[]{key}});if(key==DesktopGridActions.ItemKey(new DesktopItem{Target=ShellClipboardCommands.RecycleBinTarget,IsVirtual=true}))QueueSnapshot();}}
    private void IconsReady(){if(!disposed&&connected){_=Send(new{type="icons-ready"});ui.BeginInvoke(new Action(()=>{if(!disposed)handoff.RefreshImages();}));QueueSnapshot();}}
    private void Record(string phase,object detail)
    {
        File.AppendAllText(Path.Combine(diagnostics,"interaction.jsonl"),JsonSerializer.Serialize(new{at=DateTimeOffset.Now,phase,detail},Wire)+Environment.NewLine);
    }
    private async Task Send(object value)
    {
        byte[] bytes=JsonSerializer.SerializeToUtf8Bytes(value,Wire);if(bytes.Length>MaxPacket)throw new InvalidDataException("原生桌面数据超过单包上限。");
        await sender.WaitAsync(stop.Token);
        try{byte[] header=new byte[4];BinaryPrimitives.WriteInt32LittleEndian(header,bytes.Length);await pipe.WriteAsync(header,stop.Token);await pipe.WriteAsync(bytes,stop.Token);}
        catch(Exception e)when(e is IOException or OperationCanceledException or ObjectDisposedException){if(!disposed)stop.Cancel();}
        finally{sender.Release();}
    }
    private async Task<JsonElement> Call(string path,object body,string method="POST")
    {
        if(client is null)throw new InvalidOperationException("隔离预览不调用正式桌面接口。");
        using var request=new HttpRequestMessage(new HttpMethod(method),path);
        if(method!="GET")request.Content=new StringContent(JsonSerializer.Serialize(body,Wire),Encoding.UTF8,"application/json");
        using var response=await client.SendAsync(request,stop.Token).ConfigureAwait(false);
        using var json=JsonDocument.Parse(await response.Content.ReadAsByteArrayAsync(stop.Token));var value=json.RootElement.Clone();
        if(!response.IsSuccessStatusCode)throw new LayoutEditException(value.TryGetProperty("error",out var error)?error.GetString()??"桌面操作失败":"桌面操作失败");
        return value;
    }
    private static string Text(JsonElement value,string name)=>value.GetProperty(name).GetString()??throw new InvalidDataException(name);
    private static string? Optional(JsonElement value,string name)=>value.TryGetProperty(name,out var v)&&v.ValueKind==JsonValueKind.String?v.GetString():null;
    private Board Board(string id)=>controller.State.Boards.Single(b=>b.Id==id&&controller.Screens.Any(s=>s.Id==b.Id));
    private DesktopItem Item(string board,string? folder,string key)
    {
        if(!key.StartsWith("item:",StringComparison.Ordinal))throw new InvalidDataException("不是文件引用。");
        var b=Board(board);return (folder is null?b.RootItems:b.Boxes.Single(f=>f.Id==folder).Items).Single(i=>DesktopGridActions.ItemKey(i)==key);
    }
    private NativeDesktopTrial.DragPayload Drag(JsonElement request)
    {
        var drag=request.GetProperty("drag").Deserialize<NativeDesktopTrial.DragPayload>(new JsonSerializerOptions{PropertyNameCaseInsensitive=true})??throw new InvalidDataException();
        if(drag.Session!=session||!(drag.Revision==controller.State.Revision&&drag.Lease is null||dragLease?.Accept(drag,controller.State.Revision)==true)||drag.Keys.Length is <1 or >256||!drag.Keys.Contains(drag.Anchor)||drag.Keys.Distinct().Count()!=drag.Keys.Length)throw new LayoutEditException("拖放来源或布局版本已变化。");
        foreach(string key in drag.Keys){if(key.StartsWith("item:",StringComparison.Ordinal))_=Item(drag.Board,drag.Folder,key);else if(drag.Keys.Length!=1||drag.Folder is not null||!(dragLease?.Accept(drag,controller.State.Revision)==true?Board(drag.Board).Pages?.SingleOrDefault(p=>p.Id==dragLease.SourcePage)?.Grid:Board(drag.Board).Grid)!.Placements.ContainsKey(key))throw new InvalidDataException("拖放成员已变化。");}
        return drag with {Revision=controller.State.Revision};
    }
    private async Task<object> Execute(JsonElement request)
    {
        string op=Text(request,"op");long id=request.GetProperty("id").GetInt64(),beforeRevision=controller.State.Revision;object? result=null;
        try{
            if(Text(request,"session")!=session)throw new InvalidDataException("原生会话已变化。");
            bool undo=op=="clipboard"&&Optional(request,"action")=="undo";
            if(!undo&&op is not ("handoff-info" or "file-info" or "recovery-open" or "recovery-icons" or "snapshot" or "icons" or "telemetry" or "finishOutbound" or "register" or "visibility" or "popup-shown" or "popup-result" or "popup-command" or "popup-icons" or "card-manage" or "context-menu" or "shell-ready" or "shell-command" or "shell-icons" or "resource-report" or "grid-settings" or "flyout-adjust")&&request.GetProperty("revision").GetInt64()!=controller.State.Revision)throw new LayoutEditException("布局已变化，请重试。");
            switch(op){
                case "handoff-info":result=await HandoffInfo(request);break;
                case "file-info":result=await FileHoverInfo(request);break;
                case "recovery-open":result=await ui.InvokeAsync(()=>RecoveryOpen(request));break;
                case "recovery-action":result=await ui.InvokeAsync(()=>RecoveryAction(request));break;
                case "recovery-icons":result=await ui.InvokeAsync(()=>RecoveryIcons(request));break;
                case "snapshot":break;
                case "flyout-adjust":{
                    if(preview||independentSystem is null)throw new InvalidOperationException("系统调节服务尚未就绪。");
                    if(!CompositionRendererIdentity.Verify(request.GetProperty("window").GetInt64(),controller.Root))throw new InvalidDataException("通知窗口已变化。");
                    result=await independentSystem.AdjustFlyout(request);break;
                }
                case "grid-settings":{
                    result=await ui.InvokeAsync(()=>{string board=Text(request,"board");_ = Board(board);var display=controller.Screens.Single(s=>s.Id==board);double scale=windows.GetValueOrDefault(board).Scale;if(scale==0)scale=1;
                        return gridEditor.Handle(board,display,scale,bridge?.NativeDock(board)??DesktopDockFootprint.None,request,bridge?.Projection(controller.State.Boards.Single(row=>row.Id==board),display,scale));});break;
                }
                case "desktop-sort":{
                    if(!await sorting.WaitAsync(0,stop.Token))throw new LayoutEditException("正在读取排序信息，请稍候。");
                    try{
                        string board=Text(request,"board"),mode=Text(request,"mode");bool descending=request.GetProperty("descending").GetBoolean();
                        var before=await ui.InvokeAsync(()=>{if(controller.Store.ReadOnly||controller.Collections.IsBusy)throw new LayoutEditException("当前布局暂时不可编辑。");return DesktopGridLayout.Copy(Board(board));});
                        var properties=await DesktopSort.Metadata(controller.Root,before,mode,stop.Token);
                        var sorted=await Task.Run(()=>DesktopSort.Arrange(before,mode,descending,properties),stop.Token);
                        await ui.InvokeAsync(()=>{
                            layoutUndo.CheckSize(before,sorted);
                            controller.Commit(state=>{var index=state.Boards.FindIndex(b=>b.Id==board);if(index<0||DesktopGridLayout.Stamp(state.Boards[index])!=DesktopGridLayout.Stamp(before))throw new LayoutEditException("读取排序信息期间桌面已变化，原位置已保留，请重试。");state.Boards[index]=sorted;state.SchemaVersion=Math.Max(7,state.SchemaVersion);},"桌面排序已完成，卡片与大文件夹位置保留。");
                            layoutUndo.Push(before,Board(board));
                        });
                    }finally{sorting.Release();}break;
                }
                case "popup-shown":await ui.InvokeAsync(()=>popups?.Shown(request));break;
                case "popup-result":await ui.InvokeAsync(()=>popups?.Result(request));break;
                case "popup-command":await ui.InvokeAsync(()=>popups?.Command(request));break;
                case "resource-report":await ui.InvokeAsync(()=>{if(probes.TryGetValue(Text(request,"nonce"),out var probe))probe.TrySetResult(request.GetProperty("data").Clone());});break;
                case "shell-ready":await ui.InvokeAsync(()=>shell?.Ready(request));break;
                case "shell-command":await ui.InvokeAsync(()=>shell?.Command(request));break;
                case "popup-icons":result=await ui.InvokeAsync(()=>popups?.Icons(request));break;
                case "shell-icons":result=await ui.InvokeAsync(()=>shell?.Icons(request));break;
                case "register":{
                    string board=Text(request,"board");var screen=await ui.InvokeAsync(()=>controller.Screens.Single(s=>s.Id==board));
                    long hwnd=request.GetProperty("window").GetInt64();double scale=request.GetProperty("dpi").GetDouble()/96;
                    if(!double.IsFinite(scale)||scale is <.5 or >8)throw new InvalidDataException();
                    await ui.InvokeAsync(()=>{if(!preview)NativeRegion.Verify(hwnd,controller.Root);windows[board]=(hwnd,scale);});
                    if(!preview){
                        await Call("/register",new{monitorId=board,rect=new{left=screen.X,top=screen.Y,right=screen.X+screen.Width,bottom=screen.Y+screen.Height},scale});
                        await Call("/prepare-window",new{monitorId=board,widgetId=DesktopBridge.WidgetId,windowId=hwnd});
                        await Call("/ready",new{monitorId=board,widgetId=DesktopBridge.WidgetId,windowId=hwnd,frontendVersion=AppController.Version,origin="native-composition",sdkVersion="native-1",viewport=new{width=screen.Width/scale,height=screen.Height/scale},regions=new[]{new{left=0,top=0,right=screen.Width/scale,bottom=screen.Height/scale}}});
                    }
                    QueueSnapshot();break;
                }
                case "visibility":{
                    await ui.InvokeAsync(()=>{string board=Text(request,"board");if(request.GetProperty("visible").GetBoolean())visible.Add(board);else visible.Remove(board);});QueueSnapshot();break;
                }
                case "icons":{
                    result=await ui.InvokeAsync(()=>{
                        string board=Text(request,"board");string? folder=Optional(request,"folder");var keys=request.GetProperty("keys").EnumerateArray().Select(v=>v.GetString()!).ToArray();if(keys.Length>16)throw new InvalidDataException();
                        // Read current membership: hotplug, page changes or file moves can
                        // legitimately invalidate an outstanding, read-only icon batch.
                        var current=controller.State.Boards.SingleOrDefault(b=>b.Id==board&&controller.Screens.Any(s=>s.Id==b.Id));
                        var members=folder is null?current?.RootItems:current?.Boxes.SingleOrDefault(f=>f.Id==folder)?.Items;
                        return new{icons=keys.Select(key=>{if(!key.StartsWith("item:",StringComparison.Ordinal))throw new InvalidDataException("不是文件引用。");var item=members?.SingleOrDefault(i=>DesktopGridActions.ItemKey(i)==key);string? uri=item is null?null:bridge?.NativeIcon(item)??previewIcons?.Get(item);return new{key,png=uri is not null&&uri.StartsWith("data:image/png;base64,",StringComparison.Ordinal)&&uri.Length<90000?uri[22..]:null};}).ToArray()};
                    });break;
                }
                case "telemetry":{
                    string phase=Text(request,"phase");if(phase.Length>80)throw new InvalidDataException();
                    var detail=request.TryGetProperty("detail",out var value)?value:JsonSerializer.SerializeToElement(new{});
                    if(detail.GetRawText().Length>32768)throw new InvalidDataException();Record("composition-"+phase,detail);
                    if(preview&&phase=="benchmark-complete")previewBenchmarkComplete=true;
                    if(phase.StartsWith("benchmark-",StringComparison.Ordinal))StateStore.AtomicWrite(Path.Combine(diagnostics,"phase.json"),JsonSerializer.Serialize(new{phase,at=DateTimeOffset.Now},Wire),null);
                    break;
                }
                case "prepareOutbound":{
                    var drag=await ui.InvokeAsync(()=>Drag(request));
                    var lease=await ui.InvokeAsync(()=>dragLease=new CompositionDragLease(drag,Board(drag.Board).ActivePageId));
                    var intents=JsonSerializer.SerializeToElement(Array.Empty<string>());
                    if(!preview&&drag.Keys.All(k=>k.StartsWith("item:",StringComparison.Ordinal))&&await ui.InvokeAsync(()=>drag.Keys.All(k=>!Item(drag.Board,drag.Folder,k).IsVirtual))){
                        var response=await Call("/desktop-native-drag-source",new{monitorId=drag.Board,windowId=windows[drag.Board].Window,revision=drag.Revision,itemIds=drag.Keys.Select(k=>k[5..]).ToArray()});intents=response.GetProperty("dragIds").Clone();
                    }
                    result=new{intents,lease=lease.Token,page=lease.SourcePage};break;
                }
                case "finishOutbound":{
                    await ui.InvokeAsync(()=>{if(dragLease?.Token==Optional(request,"lease"))dragLease=null;});
                    if(!preview&&request.GetProperty("intents").GetArrayLength()>0){string board=Text(request,"board");await Call("/desktop-native-drag-finish",new{monitorId=board,windowId=windows[board].Window,dragIds=request.GetProperty("intents")});}break;
                }
                case "drag-page":{
                    var drag=await ui.InvokeAsync(()=>Drag(request));string board=Text(request,"board"),page=Text(request,"page");
                    await ui.InvokeAsync(()=>{var b=Board(board);int from=b.Pages?.FindIndex(p=>p.Id==b.ActivePageId)??-1,to=b.Pages?.FindIndex(p=>p.Id==page)??-1;if(drag.Lease is null||dragLease is null||!b.Paged||from<0||Math.Abs(to-from)!=1)throw new LayoutEditException("拖动只能切换相邻的现有页面。");});
                    long before=controller.State.Revision;
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>DesktopPages.Select(state.Boards.Single(b=>b.Id==board),page),"拖动已切换预览页。"));
                    else await Call("/desktop-page-action",new{monitorId=board,revision=before,kind="select",pageId=page});
                    await ui.InvokeAsync(()=>dragLease!.Advance(before,controller.State.Revision));break;
                }
                case "move":case "moveAuto":{
                    var drag=await ui.InvokeAsync(()=>Drag(request));string destination=Text(request,"destination");string? folder=Optional(request,"destinationFolder");
                    int x=request.TryGetProperty("x",out var px)?px.GetInt32():0,y=request.TryGetProperty("y",out var py)?py.GetInt32():0;
                    string? sourcePage=await ui.InvokeAsync(()=>drag.Lease is not null?dragLease?.SourcePage:null);
                    if(destination==drag.Board&&sourcePage is not null&&await ui.InvokeAsync(()=>Board(drag.Board).ActivePageId!=sourcePage)){
                        string destinationPage=await ui.InvokeAsync(()=>Board(destination).ActivePageId!);
                        if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>DesktopPages.Drop(state,state.Boards.Single(b=>b.Id==destination),sourcePage,destinationPage,drag.Keys,drag.Anchor,drag.Folder,folder,x,y),"跨页位置已保存。"));
                        else await Call("/grid-action",new{monitorId=destination,revision=drag.Revision,kind="page-drop",sourcePageId=sourcePage,destinationPageId=destinationPage,tileIds=drag.Keys,tileId=drag.Anchor,sourceFolder=drag.Folder,destinationFolder=folder,x,y});
                        break;
                    }
                    if(op=="moveAuto"){
                        var slot=await ui.InvokeAsync(()=>{var source=Board(drag.Board);var rect=source.Grid!.Placements.GetValueOrDefault(drag.Anchor)??new GridRect(0,0,1,1);return DesktopGridActions.ReserveSpace(Board(destination),rect.W,rect.H);});x=slot.X;y=slot.Y;
                    }
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>{
                        var source=state.Boards.Single(b=>b.Id==drag.Board);var target=state.Boards.Single(b=>b.Id==destination);
                        DesktopPages.OnSourcePage(source,sourcePage,()=>{
                        if(drag.Anchor.StartsWith("card:",StringComparison.Ordinal)){if(folder is not null)throw new LayoutEditException("卡片不能放入文件夹。");if(source.Id==target.Id)DesktopGridActions.Place(source,drag.Anchor,x,y);else DesktopCards.Transfer(source,target,drag.Anchor[5..],x,y);}
                        else if(drag.Anchor.StartsWith("folder:",StringComparison.Ordinal)){if(folder is not null)throw new LayoutEditException("文件夹不能嵌套。");if(source.Id==target.Id)DesktopGridActions.Place(source,drag.Anchor,x,y);else UnifiedDesktopGrid.Transfer(state,source,target,drag.Anchor,null,x,y);}
                        else DesktopGroupBridgeActions.Reference(state,source,target,drag.Keys,drag.Anchor,drag.Folder,folder,x,y);
                        });
                    },"原生预览布局已更新。"));
                    else if(drag.Anchor.StartsWith("item:",StringComparison.Ordinal))await Call("/composition-reference",new{monitorId=drag.Board,revision=drag.Revision,destination,keys=drag.Keys,anchor=drag.Anchor,sourceFolder=drag.Folder,destinationFolder=folder,sourcePageId=sourcePage,x,y});
                    else if(destination!=drag.Board)await Call("/grid-action",new{monitorId=drag.Board,revision=drag.Revision,kind="move-screen",tileId=drag.Anchor,boardId=destination,sourcePageId=sourcePage,x,y});
                    else await Call("/grid-action",new{monitorId=drag.Board,revision=drag.Revision,kind="place",tileId=drag.Anchor,x,y});
                    break;
                }
                case "reorder":{
                    int? insertionIndex=request.TryGetProperty("insertionIndex",out var insertion)&&insertion.ValueKind==JsonValueKind.Number?insertion.GetInt32():null;
                    var drag=await ui.InvokeAsync(()=>Drag(request));string target=Text(request,"target");
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>DesktopFolderOrder.Reorder(state.Boards.Single(b=>b.Id==drag.Board),drag.Folder??throw new InvalidDataException(),drag.Keys,drag.Anchor,target,insertionIndex),"预览文件夹顺序已更新。"));
                    else await Call("/composition-folder-order",new{monitorId=drag.Board,revision=drag.Revision,folder=drag.Folder,keys=drag.Keys,anchor=drag.Anchor,target,insertionIndex});break;
                }
                case "open":{
                    if(preview)throw new LayoutEditException("此性能预览不打开真实文件。");
                    string board=Text(request,"board"),key=Text(request,"key");await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind="open",itemId=key[5..]});break;
                }
                case "rename":{
                    if(preview)throw new LayoutEditException("性能预览不执行文件重命名。");
                    string board=Text(request,"board"),key=Text(request,"key");string name=Text(request,"name");
                    if(key.StartsWith("folder:",StringComparison.Ordinal))await Call("/composition-folder-rename",new{monitorId=board,revision=controller.State.Revision,folder=key[7..],name});
                    else{var renamed=await Call("/desktop-file-operation",new{monitorId=board,revision=controller.State.Revision,itemId=key[5..],action="rename",name});if(renamed.TryGetProperty("itemKey",out var returned)&&returned.ValueKind==JsonValueKind.String)key=returned.GetString()!;}
                    result=new{key};break;
                }
                case "card-manage":{
                    if(preview)throw new LayoutEditException("隔离预览不管理真实卡片包。");
                    string board=Text(request,"board"),kind=Text(request,"kind");
                    if(kind is "list" or "open-directory")result=await Call("/desktop-cards/manage",new{monitorId=board,kind});
                    else if(kind=="open-dependency")result=await Call("/desktop-cards/manage",new{monitorId=board,kind,type=Text(request,"type")});
                    else if(kind=="uninstall")result=await Call("/desktop-cards/manage",new{monitorId=board,kind,type=Text(request,"type"),fingerprint=Text(request,"fingerprint"),instances=request.GetProperty("instances").GetInt32(),revision=request.GetProperty("catalogRevision").GetInt64()});
                    else throw new InvalidDataException("未知卡片管理操作。");break;
                }
                case "page-add":{
                    string board=Text(request,"board");bool before=request.GetProperty("before").GetBoolean(),edge=request.TryGetProperty("edge",out var pageEdge)&&pageEdge.GetBoolean();
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>DesktopPages.Add(state.Boards.Single(b=>b.Id==board),before,edge),"已新建预览页。"));
                    else await Call("/desktop-page-action",new{monitorId=board,revision=controller.State.Revision,kind="add",before,edge});break;
                }
                case "page":{
                    string board=Text(request,"board"),page=Text(request,"page");
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>DesktopPages.Select(state.Boards.Single(b=>b.Id==board),page),"已切换预览页。"));
                    else await Call("/desktop-page-action",new{monitorId=board,revision=controller.State.Revision,kind="select",pageId=page});break;
                }
                case "context-menu":{
                    if(preview)throw new LayoutEditException("隔离预览不调用真实 Windows Shell 菜单。");
                    string board=Text(request,"board"),action=Text(request,"action"),requestId=Text(request,"requestId");
                    if(requestId.Length!=32||!requestId.All(Uri.IsHexDigit))throw new InvalidDataException("菜单会话无效。");
                    var ids=request.GetProperty("keys").EnumerateArray().Select(v=>v.GetString()??"").Where(k=>k.StartsWith("item:",StringComparison.Ordinal)).Select(k=>k[5..]).ToArray();
                    if(ids.Length>100)throw new InvalidDataException();
                    object OpenBody(bool full)=>ids.Length==0?(object)new{monitorId=board,requestId,fullFileMenu=full}:new{monitorId=board,requestId,fullFileMenu=full,itemIds=ids};
                    if(action=="open"){
                        var data=await Call("/desktop-context-open",OpenBody(false));
                        result=new{compact=data.GetProperty("compact"),more=data.GetProperty("more"),optimizedFile=data.TryGetProperty("optimizedFile",out var optimized)&&optimized.ValueKind==JsonValueKind.True,cancelled=data.TryGetProperty("cancelled",out var cancelled)&&cancelled.ValueKind==JsonValueKind.True,requestId};
                    }else if(action is "prefetch" or "full"){
                        string route=action=="prefetch"?"/desktop-context-prefetch":"/desktop-context-open";
                        var data=await Call(route,OpenBody(true));
                        result=new{compact=data.GetProperty("compact"),more=data.GetProperty("more"),requestId};
                    }else if(action=="items")result=await Call("/desktop-context-items",new{monitorId=board,requestId,path=request.GetProperty("path")});
                    else if(action=="dismiss")result=await Call("/menu-dismiss",new{monitorId=board,requestId});
                    else if(action=="invoke"){
                        try{result=await Call("/desktop-context-invoke",new{monitorId=board,requestId,path=request.GetProperty("path"),x=request.GetProperty("cellX").GetInt32(),y=request.GetProperty("cellY").GetInt32()});}
                        finally{await Call("/menu-dismiss",new{monitorId=board,requestId});}
                    }else if(action=="native"){
                        try{await Call("/desktop-context-open",OpenBody(true));
                            result=await Call("/desktop-context-native-track",new{monitorId=board,requestId,x=request.GetProperty("x").GetInt32(),y=request.GetProperty("y").GetInt32(),cellX=request.GetProperty("cellX").GetInt32(),cellY=request.GetProperty("cellY").GetInt32()});}
                        finally{await Call("/menu-dismiss",new{monitorId=board,requestId});}
                    }else throw new InvalidDataException("未知菜单操作。");break;
                }
                case "desktop-action":{
                    string kind=Text(request,"kind");
                    if(kind is "display" or "personalize"){
                        if(!preview){_ = await ui.InvokeAsync(()=>Board(Text(request,"board")).Id);Process.Start(new ProcessStartInfo(kind=="display"?"ms-settings:display":"ms-settings:personalization"){UseShellExecute=true})?.Dispose();}
                        break;
                    }
                    if(kind is not ("settings" or "feedback" or "refresh" or "exit"))throw new InvalidDataException();
                    if(!preview)await Call("/action",new{monitorId=Text(request,"board"),kind});break;
                }
                case "unplaced-action":{
                    string board=Text(request,"board"),key=Text(request,"key"),kind=Text(request,"kind");
                    if(kind is not ("restore" or "move-screen"))throw new InvalidDataException();
                    if(preview){if(kind!="restore")throw new LayoutEditException("隔离预览只恢复本页待放置项目。");
                        await ui.InvokeAsync(()=>controller.Commit(state=>{var b=state.Boards.Single(b=>b.Id==board);var old=b.Unplaced.GetValueOrDefault(key)??throw new LayoutEditException("待放置项目已变化。");var rect=DesktopGridActions.ReserveSpace(b,old.W,old.H);b.Unplaced.Remove(key);b.Grid!.Placements.Add(key,rect);},"待放置项目已恢复。"));
                    }else await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind=kind=="restore"?"restore-unplaced":"move-screen",tileId=key,boardId=kind=="restore"?board:Text(request,"destination")});break;
                }
                case "page-delete":{
                    string board=Text(request,"board"),page=Text(request,"page");
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>DesktopPages.Delete(state.Boards.Single(b=>b.Id==board),page),"预览空页已删除。"));
                    else await Call("/desktop-page-action",new{monitorId=board,revision=controller.State.Revision,kind="delete",pageId=page});break;
                }
                case "menu":case "clipboard":{
                    string board=Text(request,"board");
                    if(op=="clipboard"&&Text(request,"action")=="undo"&&await ui.InvokeAsync(()=>layoutUndo.Undo(controller,board)))break;
                    if(preview)break;
                    if(op=="clipboard")result=await Call("/desktop-clipboard-command",new{monitorId=board,action=Text(request,"action"),x=request.TryGetProperty("x",out var pasteX)?pasteX.GetInt32():(int?)null,y=request.TryGetProperty("y",out var pasteY)?pasteY.GetInt32():(int?)null,itemIds=request.GetProperty("keys").EnumerateArray().Where(v=>v.GetString()!.StartsWith("item:",StringComparison.Ordinal)).Select(v=>v.GetString()![5..]).ToArray()});
                    else{
                        string requestId=Guid.NewGuid().ToString("N");var selected=request.GetProperty("keys").EnumerateArray().Where(v=>v.GetString()!.StartsWith("item:",StringComparison.Ordinal)).Select(v=>v.GetString()![5..]).ToArray();
                        if(selected.Length==0)await Call("/desktop-context-open",new{monitorId=board,requestId,fullFileMenu=true});
                        else await Call("/desktop-context-open",new{monitorId=board,requestId,fullFileMenu=true,itemIds=selected});
                        await Call("/desktop-context-native-track",new{monitorId=board,requestId,x=request.GetProperty("x").GetInt32(),y=request.GetProperty("y").GetInt32()});
                    }break;
                }
                case "physical-drop":{
                    if(preview)throw new LayoutEditException("性能预览不移动真实文件。");
                    var drag=await ui.InvokeAsync(()=>Drag(request));
                    await Call("/desktop-group-drop",new{monitorId=Text(request,"board"),revision=drag.Revision,itemIds=drag.Keys.Select(k=>k.StartsWith("item:",StringComparison.Ordinal)?k[5..]:throw new InvalidDataException()).ToArray(),targetId=Text(request,"target")[5..],control=request.GetProperty("control").GetBoolean(),shift=request.GetProperty("shift").GetBoolean()});break;
                }
                case "import":{
                    if(preview)throw new LayoutEditException("性能预览不导入真实文件。");
                    string board=Text(request,"board");string? folder=Optional(request,"folder");var paths=request.GetProperty("paths").EnumerateArray().Select(v=>v.GetString()??"").ToArray();
                    if(paths.Length is <1 or >100)throw new InvalidDataException();
                    int allowed=request.GetProperty("allowedEffects").GetInt32();if(allowed is <1 or >3)throw new InvalidDataException();
                    bool control=request.GetProperty("control").GetBoolean(),shift=request.GetProperty("shift").GetBoolean();
                    if(allowed==1&&!shift)control=true;else if(allowed==2&&!control)shift=true;
                    uint effect=folder is not null?1:ShellClipboardCommands.SelectionEffect(paths,Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),control,shift);
                    if((allowed&effect)==0)throw new LayoutEditException("来源不允许此次复制或移动，原位置已保留。");
                    if(folder is not null)await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind="drop-files",folderId=folder,paths});
                    else await Call("/desktop-import-files",new{monitorId=board,paths,control,shift});result=new{effect};break;
                }
                case "folder-action":{
                    string board=Text(request,"board"),kind=Text(request,"kind"),folder=Text(request,"folderId");
                    if(preview)await ui.InvokeAsync(()=>controller.Commit(state=>{var b=state.Boards.Single(b=>b.Id==board);if(kind=="folder-count")DesktopGridActions.SetFolderCount(b,folder,request.GetProperty("show").GetBoolean());else if(kind=="dissolve")DesktopGridActions.Dissolve(b,folder);else if(kind=="resize")DesktopGridActions.Resize(b,folder,request.GetProperty("columns").GetInt32(),request.GetProperty("rows").GetInt32());else throw new InvalidDataException();},"预览文件夹已更新。"));
                    else if(kind=="folder-count")await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind,folderId=folder,show=request.GetProperty("show").GetBoolean()});
                    else if(kind=="dissolve")await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind,folderId=folder});
                    else await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind,folderId=folder,columns=request.GetProperty("columns").GetInt32(),rows=request.GetProperty("rows").GetInt32()});break;
                }
                case "desktop-create":{
                    string action=Text(request,"action");
                    if(action is not ("folder" or "text"))throw new InvalidDataException("未知的新建类型。");
                    if(!preview)await Call("/desktop-create-item",new{monitorId=Text(request,"board"),action,x=request.GetProperty("x").GetInt32(),y=request.GetProperty("y").GetInt32()});
                    break;
                }
                case "new-folder":{
                    if(!preview)await Call("/grid-action",new{monitorId=Text(request,"board"),revision=controller.State.Revision,kind="new-folder",x=request.GetProperty("x").GetInt32(),y=request.GetProperty("y").GetInt32()});break;
                }
                case "card-action":{
                    string board=Text(request,"board"),kind=Text(request,"kind");
                    string clockNotice="";
                    if(kind=="resize"&&request.GetProperty("columns").GetInt32()==2&&request.GetProperty("rows").GetInt32()==1){clockNotice=await ui.InvokeAsync(()=>{var card=DesktopCards.Require(Board(board),Text(request,"cardId"));return card.Type=="system.clock"&&(!card.Settings.TryGetProperty("fontSize",out var font)||!font.TryGetInt32(out int size)||size>56)?"「1 行 × 2 列」只放得下 56 号时间，已降到 56 号。":"";});}
                    if(IsEncoDeckAction(kind)){
                        if(preview)throw new LayoutEditException("隔离预览不连接外部耳机应用。");
                        await ui.InvokeAsync(()=>{var card=DesktopCards.Require(Board(board),Text(request,"cardId"));if(card.Type!="system.encodeck"||!visible.Contains(board)||Board(board).Grid?.Placements.ContainsKey(DesktopCards.Key(card))!=true)throw new InvalidDataException("耳机卡片当前不可见。");});
                        if(kind=="encodeck-location"){
                            string? executable=await ui.InvokeAsync(()=>{var picker=new Microsoft.Win32.OpenFileDialog{Title="指定 EncoDeck 安装位置",Filter="EncoDeck 程序|EncoDeck.exe",CheckFileExists=true,Multiselect=false};return picker.ShowDialog()==true?picker.FileName:null;});
                            if(executable is not null){encodeckApp.Configure(executable);encodeck.SelectionChanged();await Send(new{type="card-notice",title="EncoDeck",text="安装位置已保存，正在重新连接 EncoDeck。"});}break;
                        }
                        if(kind is "encodeck-restart" or "encodeck-open"){
                            string encoResult=await encodeckApp.Perform(kind=="encodeck-restart",stop.Token);
                            await Send(new{type="card-notice",title="EncoDeck",text=encoResult});
                            encodeck.Retry();break;
                        }
                        if(kind=="encodeck-connect"){if(!await encodeck.SendActionAsync("refresh",null,stop.Token))_ = Send(new{type="card-notice",title="EncoDeck",text="正在重新连接；后台实时电量和控制需要桌面卡片适配版 EncoDeck。"});encodeck.Retry();break;}
                        if(kind=="encodeck-refresh"&&!encodeck.IsProgramRunning){encodeck.Retry();break;}
                        int? choice=kind=="encodeck-mode"||kind=="encodeck-level"?request.GetProperty("index").GetInt32():null;
                        string action=kind switch{"encodeck-mode"=>"setNoiseMode","encodeck-level"=>"setNoiseOption",_=>"refresh"};
                        if(!await encodeck.SendActionAsync(action,choice,stop.Token))_ = Send(new{type="card-notice",title="耳机卡片",text="操作未完成；请确认 EncoDeck 已连接耳机后重试。"});
                        break;
                    }
                    if(kind is "image-copy" or "image-clear" or "image-save" or "weather-refresh" or "weather-city" or "weather-info"){
                        if(preview)throw new LayoutEditException("隔离预览不读取剪贴板或联网。");
                        await ui.InvokeAsync(()=>{var card=DesktopCards.Require(Board(board),Text(request,"cardId"));if(!visible.Contains(board)||Board(board).Grid?.Placements.ContainsKey(DesktopCards.Key(card))!=true)throw new LayoutEditException("卡片当前不可见。");
                            if(kind.StartsWith("image-",StringComparison.Ordinal)){if(card.Type!="system.clipboard-image")throw new InvalidDataException();if(kind=="image-save")ui.BeginInvoke(new Action(async()=>{try{await clipboardImage.Save();}catch(Exception ex){MessageBox.Show(ex.Message,"图片剪贴板");}}));else{bool copied=clipboardImage.Action(kind);if(kind=="image-copy"&&copied)_=Send(new{type="card-notice",title="图片剪贴板",text="已复制图片"});}}
                            else{if(card.Type is not ("system.weather" or "system.time-weather"))throw new InvalidDataException();if(kind=="weather-refresh")_=Send(new{type="card-notice",title="天气",text=weather.Refresh(card)});else if(kind=="weather-city")ui.BeginInvoke(new Action(()=>weather.Choose(controller,board,card.Id)));else ui.BeginInvoke(new Action(()=>weather.ShowInformation(card)));}});break;
                    }
                    if(kind.StartsWith("handoff-",StringComparison.Ordinal)){
                        if(preview)throw new LayoutEditException("隔离预览不编辑真实交接数据或剪贴板。");
                        string handoffId=Text(request,"cardId");await ui.InvokeAsync(()=>{var card=DesktopCards.Require(Board(board),handoffId);if(card.Type!="system.handoff"||!visible.Contains(board)||Board(board).Grid?.Placements.ContainsKey(DesktopCards.Key(card))!=true)throw new InvalidDataException("交接卡片当前不可见。");});
                        if(kind=="handoff-edit")await ui.InvokeAsync(()=>handoff.Show(board,handoffId)).Task.Unwrap();
                        else if(kind is "handoff-copy" or "handoff-text"){string notice=await ui.InvokeAsync(()=>handoff.Copy(board,handoffId,kind=="handoff-text")).Task.Unwrap();await Send(new{type="card-notice",title="文本交接台",text=notice});}
                        else throw new InvalidDataException();break;
                    }
                    if(kind.StartsWith("media-",StringComparison.Ordinal)){
                        if(preview||independentSystem is null)throw new LayoutEditException("媒体服务尚未就绪。");
                        await ui.InvokeAsync(()=>{var card=DesktopCards.Require(Board(board),Text(request,"cardId"));if(card.Type!="system.media"||!visible.Contains(board)||Board(board).Grid?.Placements.ContainsKey(DesktopCards.Key(card))!=true)throw new InvalidDataException("音乐卡片当前不可见。");});
                        await independentSystem.MediaCardAction(kind,Text(request,"sessionId"),Text(request,"trackId"),request.TryGetProperty("positionMs",out var mediaPosition)?mediaPosition.GetDouble():0);break;
                    }
                    if(kind.StartsWith("honor-",StringComparison.Ordinal)){
                        HonorPcManager.RequireSignedIn();
                        if(preview)throw new LayoutEditException("隔离预览不读取或启动荣耀服务。");
                        string honorCardId=Text(request,"cardId");await ui.InvokeAsync(()=>{var card=DesktopCards.Require(Board(board),honorCardId);if(card.Type!="system.honor-service"||!visible.Contains(board)||Board(board).Grid?.Placements.ContainsKey(DesktopCards.Key(card))!=true)throw new InvalidDataException("卡片当前不可见。");});
                        int index=request.TryGetProperty("index",out var choice)?choice.GetInt32():0;
                        if(kind=="honor-refresh")honorServices.Refresh();
                        else if(kind is "honor-hide" or "honor-restore")await ui.InvokeAsync(()=>NativeHonorServices.Hide(controller,board,honorCardId,index,kind=="honor-restore"));
                        else if(kind=="honor-open"){string notice=await Task.Run(()=>NativeHonorServices.Launch(index),stop.Token);await Send(new{type="card-notice",title="荣耀服务",text=notice});}
                        else throw new InvalidDataException();break;
                    }
                    if(kind.StartsWith("todo-",StringComparison.Ordinal)){
                        await ui.InvokeAsync(()=>{string id=Text(request,"cardId");
                            if(kind=="todo-toggle")todos.Toggle(controller,board,id,Text(request,"itemId"),request.GetProperty("done").GetBoolean());
                            else if(kind=="todo-clear")todos.Clear(controller,board,id);
                            else if(kind=="todo-undo")todos.Undo(controller,board,id);
                            else if(kind is "todo-add" or "todo-edit" or "todo-completed")todos.Show(controller,board,id,kind=="todo-completed",Optional(request,"itemId")??"",Optional(request,"date")??"");
                            else throw new InvalidDataException();});break;
                    }
                    if(kind=="transfer"){if(preview)throw new LayoutEditException("预览不转移真实卡片。");await Call("/grid-action",new{monitorId=board,revision=controller.State.Revision,kind="move-screen",tileId="card:"+Text(request,"cardId"),boardId=Text(request,"destination")});break;}
                    if(preview){
                        if(kind is "cleanup" or "shortcut")throw new LayoutEditException("隔离性能预览不启动外部程序。");
                        await ui.InvokeAsync(()=>controller.Commit(state=>{var b=state.Boards.Single(b=>b.Id==board);
                            if(kind=="create"){var card=DesktopCards.Add(state,b,catalog,Text(request,"type"),request.GetProperty("columns").GetInt32(),request.GetProperty("rows").GetInt32(),request.GetProperty("settings"));DesktopCreation.TryPlace(b,DesktopCards.Key(card),DesktopCreation.Position(request));}
                            else if(kind=="remove")DesktopCards.Remove(b,Text(request,"cardId"));
                            else if(kind=="configure")DesktopCards.Configure(b,Text(request,"cardId"),request.GetProperty("settings"));
                            else if(kind=="resize")DesktopCards.Resize(b,catalog,Text(request,"cardId"),request.GetProperty("columns").GetInt32(),request.GetProperty("rows").GetInt32());
                            else throw new InvalidDataException();},"预览卡片已更新。"));if(clockNotice.Length>0)await Send(new{type="card-notice",title="时钟",text=clockNotice});break;
                    }
                    string path=kind=="cleanup"?"/desktop-card-cleanup":kind=="shortcut"?"/desktop-card-shortcut":"/desktop-card-action";
                    var payload=request.Deserialize<Dictionary<string,JsonElement>>()!;payload["monitorId"]=JsonSerializer.SerializeToElement(board);if(request.TryGetProperty("cardId",out var cardId))payload["id"]=cardId;payload["revision"]=JsonSerializer.SerializeToElement(controller.State.Revision);
                    if(kind=="create"&&DesktopCreation.Position(request) is { } position){
                        bool fits=await ui.InvokeAsync(()=>Board(board).Grid is {} grid&&DesktopGridActions.CanFit(grid,new GridRect(position.X,position.Y,request.GetProperty("columns").GetInt32(),request.GetProperty("rows").GetInt32())));
                        if(!fits){payload.Remove("x");payload.Remove("y");}
                    }
                    string? removedHandoff=kind=="remove"?await ui.InvokeAsync(()=>{var old=DesktopCards.Require(Board(board),Text(request,"cardId"));return old.Type=="system.handoff"?old.Id:null;}):null;
                    await Call(path,payload);if(removedHandoff is not null)await ui.InvokeAsync(()=>handoff.Removed(removedHandoff));if(clockNotice.Length>0)await Send(new{type="card-notice",title="时钟",text=clockNotice});break;
                }
                default:throw new InvalidDataException("不支持的原生桌面操作。");
            }
            object? snapshot=op!="context-menu"&&(op is "snapshot" or "register"||controller.State.Revision!=beforeRevision)?await ui.InvokeAsync(Snapshot):null;
            return new{id,ok=true,result,snapshot};
        }catch(Exception error)when(error is not OutOfMemoryException){
            bool background=op is "handoff-info" or "file-info" or "flyout-adjust" or "register" or "icons" or "popup-icons" or "shell-icons"||op=="context-menu"&&Optional(request,"action") is not ("invoke" or "native");
            controller.Log("composition_operation_error",new{op,error=error.Message});
            // Background reads/cancelled menus do not invalidate layout or own
            // a modal error. Their callbacks decide whether the UI is current.
            return new{id,ok=false,error=error.Message,background,snapshot=background?null:await ui.InvokeAsync(Snapshot)};
        }
    }
    internal static bool IsEncoDeckAction(string? kind)=>kind is "encodeck-connect" or "encodeck-refresh" or "encodeck-mode" or "encodeck-level" or "encodeck-location" or "encodeck-restart" or "encodeck-open";
    internal static bool IsDeferredCardAction(string? operation,string? kind)=>operation=="card-action"&&(kind is "handoff-edit" or "handoff-copy" or "handoff-text"||IsEncoDeckAction(kind));
    private async Task Serve()
    {
        var menuWork=new List<Task>();var commandWork=new List<Task>();bool completedPreview=false;
        async Task ReplyMenu(JsonElement request){try{await Send(await Execute(request));}catch(Exception error)when(error is not OutOfMemoryException){if(!disposed)controller.Log("composition_menu_reply_error",new{error=error.Message});}}
        async Task ReplyAfterCommands(JsonElement request,Task[] pending){await Task.WhenAll(pending);await ReplyMenu(request);}
        try{
            using var startup=CancellationTokenSource.CreateLinkedTokenSource(stop.Token);startup.CancelAfter(TimeSpan.FromSeconds(30));
            await pipe.WaitForConnectionAsync(startup.Token);
            if(child is null||!GetNamedPipeClientProcessId(pipe.SafePipeHandle.DangerousGetHandle(),out uint peer)||peer!=child.Id)throw new InvalidDataException("原生通信进程不匹配。");
            connected=true;await Send(new{type="initial",snapshot=await ui.InvokeAsync(Snapshot)});
            if(NativeSystemDeployment.Selected(controller.Root) is {} deployment){
                // Keep the renderer transport serving while readiness callbacks
                // arrive. Waiting here would deadlock shell-ready behind Start.
                _=StartIndependent(deployment);
            }
            while(!stop.IsCancellationRequested){
                byte[] h=new byte[4];await pipe.ReadExactlyAsync(h,stop.Token);int length=BinaryPrimitives.ReadInt32LittleEndian(h);if(length is <2 or >MaxPacket)throw new InvalidDataException();byte[] bytes=new byte[length];await pipe.ReadExactlyAsync(bytes,stop.Token);using var document=JsonDocument.Parse(bytes);var request=document.RootElement.Clone();
                menuWork.RemoveAll(task=>task.IsCompleted);commandWork.RemoveAll(task=>task.IsCompleted);
                string? operation=Optional(request,"op"),action=Optional(request,"action");
                if(IsDeferredCardAction(operation,Optional(request,"kind"))||operation is "handoff-info" or "file-info" or "desktop-sort" or "flyout-adjust" or "physical-drop"||operation=="context-menu"&&action!="dismiss"||operation is "clipboard" or "finishOutbound"&&commandWork.Count>0){
                    // Keep reading the pipe while Shell queries or a wizard are
                    // awaited. Otherwise Dismiss sits behind the query it must
                    // cancel, and every subsequent right click pays that delay.
                    // Journal completion is cleanup, not a new optional job.
                    // Under backpressure it waits rather than losing the lease.
                    if(menuWork.Count>=16&&operation=="finishOutbound")await ReplyAfterCommands(request,commandWork.ToArray());
                    else if(menuWork.Count>=16)await Send(new{id=request.GetProperty("id").GetInt64(),ok=false,background=operation=="context-menu",error=operation=="physical-drop"?"正在完成之前的文件操作，请完成后重试。":"菜单请求过多，请关闭当前菜单后重试。"});
                    else{
                        // File drops and their drag-journal completion stay in
                        // command order while icons/panel replies keep flowing.
                        // Ctrl+Z must also follow a preceding file move/delete.
                        var reply=operation is "clipboard" or "physical-drop" or "finishOutbound"?ReplyAfterCommands(request,commandWork.ToArray()):ReplyMenu(request);
                        menuWork.Add(reply);if(operation is "clipboard" or "desktop-sort" or "physical-drop" or "finishOutbound"||action is "invoke" or "native")commandWork.Add(reply);
                    }
                }else await Send(await Execute(request));
            }
        }catch(EndOfStreamException)when(preview&&previewBenchmarkComplete&&child is not null){
            try{await child.WaitForExitAsync(stop.Token).WaitAsync(TimeSpan.FromSeconds(2));completedPreview=child.ExitCode==0;}catch(Exception e)when(e is not OutOfMemoryException){Record("runtime-end",new{error=e.Message});}
        }catch(Exception e)when(e is not OutOfMemoryException){if(!disposed)Record("runtime-end",new{error=e.Message});}
        finally{connected=false;if(!disposed)await ui.InvokeAsync(()=>{controller.Log("composition_transport_ended",new{backendPid=Environment.ProcessId,rendererPid=child?.Id,rendererExited=child?.HasExited,rendererExitCode=child is {HasExited:true}?child.ExitCode:(int?)null});bridge?.Dispose();Application.Current.Shutdown(completedPreview?0:1);});}
    }
    private async Task StartIndependent(NativeSystemDeployment deployment)
    {
        try{independentSystem=await NativeSystemUiSession.Start(controller,deployment,shell??throw new InvalidOperationException(),popups??throw new InvalidOperationException(),child!.Id,ui,async action=>{await Call("/community-menu",new{kind=action});},Send,controller.Log,stop.Token);if(disposed)independentSystem.Dispose();else{entrance?.Complete(true);independentSystem.MediaCardChanged=value=>{if(connected&&!disposed)_=Send(new{type="media-data",data=value});};QueueSnapshot();}}
        catch(Exception error)when(error is not OutOfMemoryException){if(!disposed){StartupFailure=error;entrance?.Complete(false);controller.Log("independent_native_start_failed",new{error=error.ToString()});ui.BeginInvoke(new Action(()=>Application.Current.Shutdown(1)));}}
    }
    public static bool Selected(string root)
    {
        if(InstalledDesktop.IsCurrent(root))return true;
        string path=Path.Combine(root,"app.current.json");if(!File.Exists(path))return false;
        using var value=JsonDocument.Parse(File.ReadAllBytes(path));
        return value.RootElement.TryGetProperty("desktopRenderer",out var renderer)&&renderer.GetString()=="composition";
    }
    public static CompositionDesktopRuntime Start(AppController controller,DesktopBridge bridge,bool logon=false)
    {
        string root=controller.Root;
        if(!InstalledDesktop.IsCurrent(root)&&(!File.Exists(Path.Combine(root,".git","HEAD"))||File.ReadAllText(Path.Combine(root,".git","HEAD")).Trim()!="ref: refs/heads/main"))throw new InvalidOperationException("正式原生桌面仅从主目录 main 加载。");
        NativeHostSelection.RequireNativePopupSupport(root);
        var runtime=new CompositionDesktopRuntime(controller,bridge,Path.Combine(root,"runtime/desktop/composition"),false);
        try
        {
            runtime.entrance=InstalledDesktopTransition.Start(controller,logon);
            runtime.Launch(0);
            return runtime;
        }
        catch { runtime.Dispose(); throw; }
    }
    private void Launch(int cycles,bool popupBenchmark=false)
    {
        string exe=CompositionRendererIdentity.Resolve();var start=new ProcessStartInfo(exe){UseShellExecute=false,WorkingDirectory=AppContext.BaseDirectory};
        foreach(string arg in new[]{"--pipe",pipeName,"--parent",Environment.ProcessId.ToString(),"--parent-created",Process.GetCurrentProcess().StartTime.ToFileTimeUtc().ToString(),"--cycles",(popupBenchmark?0:cycles).ToString(),"--popup-cycles",(popupBenchmark?cycles:0).ToString()})start.ArgumentList.Add(arg);
        child=Process.Start(start)??throw new IOException("原生桌面宿主未启动。");
        if(!preview){identity=new(controller.Root,child,exe);bridge?.NativeRendererStarted(child.Id);}
        child.EnableRaisingEvents=true;child.Exited+=(_,_)=>ui.BeginInvoke(new Action(()=>{if(!disposed){controller.Log("composition_renderer_exited",new{rendererPid=child.Id,exitCode=child.ExitCode});bridge?.Dispose();Application.Current.Shutdown(child.ExitCode);}}));
        Record("session",new{backendPid=Environment.ProcessId,rendererPid=child.Id,renderer=exe,preview,desktopTakeover=!preview});_=Serve();
    }
    public static int Preview(string root,string directory,int cycles,bool popupBenchmark=false)
    {
        directory=Path.GetFullPath(directory);root=Path.GetFullPath(root);
        if(!File.Exists(Path.Combine(root,".git"))||!directory.StartsWith(Path.GetFullPath(Path.Combine(root,"artifacts","native-desktop-trial"))+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase)||Directory.Exists(directory))throw new InvalidOperationException("请使用工作树内新的隔离预览目录。");
        if(cycles!=0&&cycles is <2 or >100)throw new ArgumentException("开合次数无效。");
        var app=new Application{ShutdownMode=ShutdownMode.OnExplicitShutdown};
        using(var seed=new NativeDesktopTrial(root,directory,"Composition components preview",40)){}
        var controller=new AppController(directory);var catalog=DesktopCardCatalog.Load(Path.Combine(AppContext.BaseDirectory,"desktop-widget/card-catalog.json"));
        controller.Commit(state=>{foreach(var board in state.Boards){DesktopPages.Initialize(state,board,new AccessoryProjection(14,76,96,96,1,12,7),true);foreach(var type in catalog.Types){var size=type.Sizes[0];DesktopCards.Add(state,board,catalog,type.Type,size.Columns,size.Rows,JsonSerializer.SerializeToElement(new{}));}}},"已加入隔离系统卡片。");
        using var runtime=new CompositionDesktopRuntime(controller,null,directory,true);runtime.Launch(cycles,popupBenchmark);return app.Run();
    }
    internal async Task StopForExit()
    {
        // Keep the actual owned process handle until it has ended. The regular
        // Dispose path stays synchronous for failures and OS shutdown.
        var renderer = child; child = null;
        var system = independentSystem;
        try {
            Dispose();
            Task rendererEnded = renderer is null ? Task.CompletedTask : renderer.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            Task systemEnded = system?.Released.WaitAsync(TimeSpan.FromSeconds(10)) ?? Task.CompletedTask;
            await Task.WhenAll(rendererEnded, systemEnded);
        } finally { renderer?.Dispose(); }
    }
    public void Dispose()
    {
        if(disposed)return;disposed=true;entrance?.Complete(false);gridEditor.Clear();independentSystem?.Dispose();popups?.Dispose();shell?.Dispose();if(bridge is not null){bridge.NativePopups=null;bridge.NativeShell=null;bridge.NativeProbe=null;}bridge?.Dispose();controller.Changed-=QueueSnapshot;
        if(bridge is not null){bridge.NativeIconsReady-=IconsReady;bridge.NativeIconInvalidated-=IconInvalidated;bridge.NativeImageInvalidated-=ImageInvalidated;bridge.NativeWorkspaceChanged-=QueueSnapshot;}
        controller.ApplyKeyboardPreferences=null;handoff.Dispose();honorServices.Dispose();todos.Dispose();encodeck.Dispose();weather.Dispose();clipboardImage.Dispose();data.Dispose();previewIcons?.Dispose();stop.Cancel();pipe.Dispose();client?.Dispose();identity?.Dispose();child?.Dispose();
    }
    [DllImport("kernel32.dll",SetLastError=true)]private static extern bool GetNamedPipeClientProcessId(IntPtr pipe,out uint pid);
}
