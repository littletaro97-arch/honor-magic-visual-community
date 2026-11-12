using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Runtime.InteropServices;
using System.Threading.Channels;
using System.Windows.Threading;
namespace MagicCommunity;
// One bounded, awaited command channel for both bars. No periodic state query.
internal sealed class CompositionShellSession : IDisposable
{
    private readonly Dispatcher ui;
    private readonly Func<object, Task> send;
    private readonly string root;
    private readonly NotificationSessionLease notifications;
    private readonly NativeSystemRuntime? systemRuntime;
    private readonly Channel<JsonElement> commands = Channel.CreateBounded<JsonElement>(
        new BoundedChannelOptions(32) { SingleReader = true, FullMode = BoundedChannelFullMode.Wait });
    private readonly HashSet<long> windows = [];
    private bool waiting, disposed;
    private readonly Dictionary<string,string> offeredHandles=new(StringComparer.Ordinal);
    private bool developerTools;
    private readonly Dictionary<string, ApplicationIconSource> assets = new(StringComparer.Ordinal);
    private readonly TaskCompletionSource<bool> ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal Func<DesktopItem, string?>? Icon;
    internal Func<ApplicationIconSource,string?>? ApplicationIcon;
    internal Func<ApplicationIconSource,string>? ApplicationIdentity;
    internal Func<string,string?>? InlineImage;
    internal int ReadyWindowCount=>windows.Count;
    internal event Action? BarReady;
    internal Action<string>? InvalidateApplicationIcon;
    internal Action<string>? PowerMode;
    internal Action? OpenSettings;
    internal Action<JsonElement>? ExtensionState;
    internal Action<JsonElement>? StaticWorkspace;
    internal CompositionShellSession(string root, Dispatcher ui, Func<object, Task> send)
    {
        this.root = root;
        notifications = new(root);
        systemRuntime = NativeSystemRuntime.Selected(root);
        this.ui = ui;
        this.send = send;
    }
    internal async Task<object> Update(JsonElement body, CancellationToken cancellation)
    {
        if (disposed)
            return new { accepted = false };
        var data = body.GetProperty("data").Clone();
        systemRuntime?.ValidateSnapshot(data);
        if (Encoding.UTF8.GetByteCount(data.GetRawText()) > 524288)
            throw new InvalidDataException("Shell snapshot exceeds bound.");
        await ui.InvokeAsync(() =>
                             {
                                 StaticWorkspace?.Invoke(data);
                                 if(data.TryGetProperty("powerMode",out var mode)&&mode.ValueKind==JsonValueKind.String)PowerMode?.Invoke(mode.GetString()??"暂不可用");
                                 windows.RemoveWhere(window=>!CompositionRendererIdentity.Verify(window,root));var priorAssets=new Dictionary<string,ApplicationIconSource>(assets);assets.Clear();offeredHandles.Clear();developerTools=data.TryGetProperty("devTools",out var developer)&&developer.ValueKind==JsonValueKind.True;
                                 foreach (string group in new[] { "dock", "toolbar" })
                                 if (data.TryGetProperty(group, out var dock))
                                     foreach (var section in new[] { "left", "center", "right" })
                                         if (dock.TryGetProperty(section, out var list) &&
                                             list.ValueKind == JsonValueKind.Array)
                                             foreach (var row in list.EnumerateArray())
                                                 if (row.TryGetProperty("key", out var key) &&
                                                     row.TryGetProperty("icon", out var path) &&
                                                     path.ValueKind == JsonValueKind.String && assets.Count < 256)
                                                     {
                                                         string Text(string field)=>row.TryGetProperty(field,out var v)&&v.ValueKind==JsonValueKind.String?v.GetString()??"":"";
                                                         long window=0;if(row.TryGetProperty("windows",out var rows)&&rows.ValueKind==JsonValueKind.Array&&rows.GetArrayLength()>0&&rows[0].TryGetProperty("hwnd",out var handle))handle.TryGetInt64(out window);
                                                         string identity=key.GetString()??"",image=path.GetString()??"";
                                                         var source=new ApplicationIconSource(identity,Text("path"),Text("umid"),image,window,Text("type")=="AppOrFile");
                                                         if(priorAssets.TryGetValue(identity,out var prior)&&prior!=source)InvalidateApplicationIcon?.Invoke(identity);
                                                         assets[identity]=source;
                                                         if(developerTools&&row.TryGetProperty("windows",out var offered)&&offered.ValueKind==JsonValueKind.Array)
                                                             offeredHandles[identity]=JsonSerializer.Serialize(offered.EnumerateArray().Where(w=>w.TryGetProperty("hwnd",out _)).Select(w=>w.GetProperty("hwnd").GetInt64().ToString("x")).ToArray());
                                                         if(row.TryGetProperty("badge",out var badge)&&badge.ValueKind==JsonValueKind.Object&&badge.TryGetProperty("kind",out var badgeKind)&&badgeKind.GetString()=="image"&&badge.TryGetProperty("value",out var badgePath)&&badgePath.ValueKind==JsonValueKind.String&&assets.Count<256){
                                                             string badgeId=identity+":badge";var badgeSource=new ApplicationIconSource(badgeId,"","",badgePath.GetString()??"",0,false);
                                                             if(priorAssets.TryGetValue(badgeId,out var priorBadge)&&priorBadge!=badgeSource)InvalidateApplicationIcon?.Invoke(badgeId);assets[badgeId]=badgeSource;
                                                         }
                                                     }
                             });
        string initialStatus = await ui.InvokeAsync(() => NotificationStatus(data));
        var augmented = JsonNode.Parse(data.GetRawText())!.AsObject();
        augmented["notificationSessionStatus"] = initialStatus;
        await send(new { type = "shell-data", data = augmented });
        if (data.TryGetProperty("monitors", out var monitors) && monitors.GetArrayLength() > 0)
        {
            try
            {
                await ready.Task.WaitAsync(TimeSpan.FromSeconds(2), cancellation);
            }
            catch (TimeoutException)
            {
                return new { accepted = false };
            }
        }
        string finalStatus = await ui.InvokeAsync(() => NotificationStatus(data));
        if (finalStatus != initialStatus)
        {
            augmented["notificationSessionStatus"] = finalStatus;
            await send(new { type = "shell-data", data = augmented });
        }
        return new { accepted = true };
    }
    private string NotificationStatus(JsonElement data)
    {
        int rendererPid = 0;
        long window = windows.FirstOrDefault();
        if (window != 0)
        {
            GetWindowThreadProcessId(new(window), out uint pid);
            rendererPid = (int)pid;
        }
        notifications.Update(data.TryGetProperty("notificationAccess", out var access) && access.ValueKind == JsonValueKind.True, rendererPid);
        return notifications.Status;
    }
    internal async Task<object> Next(CancellationToken cancellation)
    {
        bool acquired = await ui.InvokeAsync(() =>
                                             {
                                                 if (waiting || disposed)
                                                     return false;
                                                 waiting = true;
                                                 return true;
                                             });
        if (!acquired)
            throw new InvalidDataException("Shell command stream already active.");
        try
        {
            return await commands.Reader.ReadAsync(cancellation);
        }
        finally
        {
            await ui.InvokeAsync(() => waiting = false);
        }
    }
    internal void Enqueue(JsonElement command) => commands.Writer.TryWrite(command.Clone());
    internal void Ready(JsonElement body)
    {
        long window = body.GetProperty("window").GetInt64();
        if (!CompositionRendererIdentity.Verify(window, root))
            throw new InvalidDataException("Shell renderer identity differs.");
        if (windows.Count >= 128 && !windows.Contains(window))
            throw new InvalidDataException("Shell window bound.");
        windows.Add(window);
        ready.TrySetResult(true);
        BarReady?.Invoke();
    }
    internal void Command(JsonElement body)
    {
        if (disposed)
            return;
        long window = body.GetProperty("window").GetInt64();
        if (!windows.Contains(window) || !CompositionRendererIdentity.Verify(window, root))
            throw new InvalidDataException("Shell window expired.");
        var command = body.GetProperty("command");
        if (Encoding.UTF8.GetByteCount(command.GetRawText()) > 32768)
            throw new InvalidDataException("Shell command bound.");
        if (command.TryGetProperty("verb", out var verb) && verb.GetString() == "settings" &&
            command.TryGetProperty("id", out var id) && id.GetString() is "bar:toolbar" or "bar:dock")
        {
            (OpenSettings ?? throw new InvalidOperationException("社区版设置入口尚未就绪。"))();
            return;
        }
        // Fixed native action: no host command, PATH lookup, sampler or persistent service.
        if(command.TryGetProperty("verb",out var localVerb)&&localVerb.GetString()=="task-manager"&&
           command.TryGetProperty("id",out var surface)&&surface.GetString() is "bar:toolbar" or "bar:dock"){
            string executable=Path.Combine(Environment.SystemDirectory,"Taskmgr.exe");
            if(!File.Exists(executable))throw new FileNotFoundException("Windows 任务管理器不可用。",executable);
            using var process=System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(executable){UseShellExecute=true});
            return;
        }
        if(command.TryGetProperty("verb",out var copyVerb)&&copyVerb.GetString()=="copy-handles"){
            string identity=command.TryGetProperty("id",out var copyId)?copyId.GetString()??"":"";
            if(!developerTools||!offeredHandles.TryGetValue(identity,out var text))throw new InvalidOperationException("Window handles are no longer offered.");
            System.Windows.Clipboard.SetText(text);return;
        }
        if(command.TryGetProperty("verb",out var extensionVerb)&&extensionVerb.GetString() is "extension-render-state" or "extension-action"){
            var safe=JsonNode.Parse(command.GetRawText())!.AsObject();safe["surfaceWindow"]=window;var verified=JsonSerializer.SerializeToElement(safe);
            if(extensionVerb.GetString()=="extension-render-state"){ExtensionState?.Invoke(verified);return;}
            command=verified;
        }
        if(!commands.Writer.TryWrite(command.Clone()))throw new InvalidOperationException("操作暂未提交，请稍后重试。");
    }
    internal object Icons(JsonElement body)
    {
        var list = body.GetProperty("keys");
        if (list.GetArrayLength() > 16)
            throw new InvalidDataException("Shell icon batch bound.");
        return new { icons = list.EnumerateArray()
                                 .Select(value =>
                                         {
                                             string key = value.GetString() ?? "";
                                             string? uri=InlineImage?.Invoke(key) ?? (assets.TryGetValue(key,out var source)?ApplicationIcon?.Invoke(source):null);
                                             return new { key, iconExecutable=assets.TryGetValue(key,out var known)?ApplicationIdentity?.Invoke(known):null, png = uri is not null &&
                                                                             uri.StartsWith("data:image/png;base64,",
                                                                                            StringComparison.Ordinal) &&
                                                                             uri.Length < 90000
                                                                         ? uri[22..]
                                                                         : null };
                                         })
                                 .ToArray() };
    }
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    public void Dispose()
    {
        disposed = true;
        notifications.Dispose();
        commands.Writer.TryComplete();
        windows.Clear();
    }
}
