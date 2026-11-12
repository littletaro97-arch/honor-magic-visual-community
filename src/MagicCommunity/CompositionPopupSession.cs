using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using System.Windows.Threading;

namespace MagicCommunity;
// One shared native flyout. Receipts are bounded and complete on actual close,
// selection, replacement, render timeout or adapter shutdown; no idle polling.
internal sealed class CompositionPopupSession : IDisposable
{
    internal static readonly string[] Supported = NativePopupIds.Supported;
    internal static string[] Selected(string root)
    {
        if(InstalledDesktop.IsCurrent(root))return Supported;
        string path=Path.Combine(root,"app.current.json");if(!File.Exists(path))return [];
        using var file=JsonDocument.Parse(File.ReadAllBytes(path));return NativePopupIds.ReadSelection(file.RootElement);
    }
    private sealed class Receipt(string token, string kind)
    {
        public string Token = token, Kind = kind;
        public bool Waited, MusicView;
        public readonly Channel<JsonElement> Commands = Channel.CreateBounded<JsonElement>(new BoundedChannelOptions(8) { SingleReader = true, FullMode = BoundedChannelFullMode.DropOldest });
        public readonly TaskCompletionSource<bool> Ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly TaskCompletionSource<JsonElement> Done =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
    private readonly Dispatcher ui;
    private readonly Func<object, Task> send;
    private readonly Func<bool> healthy;
    private readonly string root;
    private readonly HashSet<string> selected;
    private readonly SemaphoreSlim opening = new(1, 1);
    private readonly Dictionary<string, Receipt> receipts = new(StringComparer.Ordinal);
    internal Func<DesktopItem,string?>? Icon;
    internal Func<ApplicationIconSource,string?>? ApplicationIcon;
    internal Func<ApplicationIconSource,string>? ApplicationIdentity;
    internal Func<string,string?>? InlineImage;
    private readonly Dictionary<string,ApplicationIconSource> images=new(StringComparer.Ordinal);
    private void RegisterImages(JsonElement data)
    {
        images.Clear();
        if(!data.TryGetProperty("rows",out var rows)||rows.ValueKind!=JsonValueKind.Array)return;
        foreach(var row in rows.EnumerateArray().Take(192))
            if(row.TryGetProperty("id",out var id)&&row.TryGetProperty("image",out var image)&&image.ValueKind==JsonValueKind.String)
            {
                string key=id.GetString()??"",path=image.GetString()??"";
                string umid=row.TryGetProperty("iconUmid",out var app)&&app.ValueKind==JsonValueKind.String?app.GetString()??"":"";
                images[key]=new(key,path,umid,path,0,key.StartsWith("app:",StringComparison.Ordinal)||key.StartsWith("notification:",StringComparison.Ordinal)&&umid.Length>0);
            }
    }
    // Reuse the existing bounded icon cache before sending the first apps
    // frame. A miss only queues the existing STA worker; no Shell work or wait
    // is added to the UI thread. At most 18 tiles / 64 KiB of base64 is copied.
    private JsonElement WarmApplications(JsonElement data)
    {
        var node=System.Text.Json.Nodes.JsonNode.Parse(data.GetRawText())!.AsObject();int budget=Math.Clamp(196608-Encoding.UTF8.GetByteCount(data.GetRawText())-1024,0,65536);
        if(node["rows"] is System.Text.Json.Nodes.JsonArray rows)
            foreach(var row in rows.OfType<System.Text.Json.Nodes.JsonObject>().Where(r=>r["id"]?.GetValue<string>().StartsWith("app:",StringComparison.Ordinal)==true).Take(18)){
                row.Remove("iconPng");string id=row["id"]!.GetValue<string>();
                if(!images.TryGetValue(id,out var source)||!source.Application)continue;
                string? uri=ApplicationIcon?.Invoke(source);row["iconExecutable"]=ApplicationIdentity?.Invoke(source);
                if(uri is null||!uri.StartsWith("data:image/png;base64,",StringComparison.Ordinal)||uri.Length-22>budget)continue;
                string png=uri[22..];row["iconPng"]=png;budget-=png.Length;
            }
        return JsonSerializer.SerializeToElement(node);
    }
    internal object Icons(JsonElement request)
    {
        if(current?.Token!=request.GetProperty("token").GetString()||disposed)throw new InvalidDataException("Popup icons expired.");
        var keys=request.GetProperty("keys");if(keys.GetArrayLength()>16)throw new InvalidDataException("Popup icon batch bound.");
        return new{icons=keys.EnumerateArray().Select(key=>{
            string id=key.GetString()??"";string? uri=InlineImage?.Invoke(id);
            if(uri is null&&images.TryGetValue(id,out var source)){
                if(source.Application)uri=ApplicationIcon?.Invoke(source);
                else if(Path.IsPathFullyQualified(source.Image)&&!source.Image.StartsWith(@"\\",StringComparison.Ordinal)&&File.Exists(source.Image))
                    uri=Icon?.Invoke(new DesktopItem{Name=Path.GetFileName(source.Image),Target=source.Image});
            }
            return new{key=id,iconExecutable=images.TryGetValue(id,out var known)&&known.Application?ApplicationIdentity?.Invoke(known):null,png=uri is not null&&uri.StartsWith("data:image/png;base64,",StringComparison.Ordinal)&&uri.Length<90000?uri[22..]:null};
        }).ToArray()};
    }
    private Receipt? current;
    private long window;
    private bool disposed;
    internal CompositionPopupSession(string root, IEnumerable<string> selected, Dispatcher ui, Func<object, Task> send,
                                     Func<bool> healthy)
    {
        this.root = root;
        this.selected = selected.Select(NativePopupIds.Canonical).ToHashSet(StringComparer.Ordinal);
        this.ui = ui;
        this.send = send;
        this.healthy = healthy;
    }
    internal async Task<object> Open(JsonElement body, CancellationToken cancellation)
    {
        await opening.WaitAsync(cancellation);
        try
        {
            return await OpenCore(body, cancellation);
        }
        finally
        {
            opening.Release();
        }
    }
    private async Task<object> OpenCore(JsonElement body, CancellationToken cancellation)
    {
        string kind = NativePopupIds.Canonical(body.GetProperty("widgetId").GetString() ?? "");
        bool musicView=body.TryGetProperty("data",out var dataView)&&dataView.TryGetProperty("nativeMusicView",out var musicFlag)&&musicFlag.ValueKind==JsonValueKind.True;
        string? toggle=null;
        var receipt = await ui.InvokeAsync(() =>
                                           {
                                               if (disposed || !healthy() || !selected.Contains(kind))
                                                   return null;
                                               if(current?.Kind==kind&&current.MusicView==musicView&&kind!="@magic/task-switcher")toggle=current.Token;
                                               FinishCurrent("cancel");
                                               while (receipts.Count >= 8)
                                               {
                                                   var old = receipts.First();
                                                   old.Value.Ready.TrySetResult(false);
                                                   old.Value.Done.TrySetResult(
                                                       JsonSerializer.SerializeToElement(new { action = "cancel" }));
                                                   receipts.Remove(old.Key);
                                               }
                                               var next = new Receipt(Guid.NewGuid().ToString("N"), kind){MusicView=musicView};
                                               receipts.Add(next.Token, next);
                                               if(toggle is null)current = next;
                                               else{
                                                   // A close click owns its own completed receipt. Reusing
                                                   // the old token races its already active Wait reader.
                                                   next.Ready.TrySetResult(true);
                                                   next.Done.TrySetResult(JsonSerializer.SerializeToElement(new{action="cancel"}));
                                               }
                                               return next;
                                           });
        if (receipt is null)
            return new { accepted = false };
        if(toggle is not null){await send(new{type="popup-hide",token=toggle});return new{accepted=true,token=receipt.Token};}
        try
        {
            var payload = body.GetProperty("payload").Clone();
            var data = body.GetProperty("data").Clone();
            if (Encoding.UTF8.GetByteCount(data.GetRawText()) > 196608)
                throw new InvalidDataException("弹窗数据超过上限。");
            // Enrich account state locally. Executable paths and HTTP credentials
            // are never accepted from the trigger payload or sent to the renderer.
            object account = kind=="@magic/apps-menu" ? (object)new{version=AppController.Version,name=Environment.UserName} : kind is "@magic/user-menu" or "@magic/power-menu"
                                 ? (object) new { version = AppController.Version, name = Environment.UserName,
                                                  honorSignedIn = HonorPcManager.SignedIn,
                                                  accountAvailable = HonorPcManager.Find() is not null }
                                 : new {};
            await ui.InvokeAsync(()=>{RegisterImages(data);if(kind=="@magic/apps-menu")data=WarmApplications(data);});
            if(Encoding.UTF8.GetByteCount(data.GetRawText())>196608)throw new InvalidDataException("原生弹窗数据超过上限。");
            await send(new { type = "popup", token = receipt.Token, kind, payload, data, account });
            if (!await receipt.Ready.Task.WaitAsync(TimeSpan.FromMilliseconds(800), cancellation))
                throw new InvalidOperationException(kind=="@magic/apps-menu"?"原生开始菜单未完成显示；未创建旧版兼容菜单。":"弹窗未完成显示，转交兼容入口。");
            return new { accepted = true, token = receipt.Token, window };
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            await ui.InvokeAsync(() =>
                                 {
                                     if (current == receipt)
                                         FinishCurrent("cancel");
                                     receipts.Remove(receipt.Token);
                                 });
            await send(new { type = "popup-hide", token = receipt.Token });
            return new { accepted = false };
        }
    }
    internal async Task<object> Wait(JsonElement body, CancellationToken cancellation)
    {
        string token = body.GetProperty("token").GetString() ?? "";
        var receipt = await ui.InvokeAsync(() =>
                                           {
                                               if (!receipts.TryGetValue(token, out var value) || value.Waited)
                                                   throw new InvalidDataException("弹窗回执已失效。");
                                               value.Waited = true;
                                               return value;
                                           });
        bool finished = false;
        using var readStop = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        try
        {
            var command = receipt.Commands.Reader.ReadAsync(readStop.Token).AsTask();
            var ready = await Task.WhenAny(receipt.Done.Task, command).WaitAsync(TimeSpan.FromMinutes(5), cancellation);
            if (!command.IsCompletedSuccessfully && ready == receipt.Done.Task) { finished = true; return await receipt.Done.Task; }
            return new { keepOpen = true, result = await command };
        }
        catch { finished = true; throw; }
        finally
        {
            readStop.Cancel();
            await ui.InvokeAsync(() => {
                receipt.Waited = false;
                if (finished) { receipts.Remove(token); if (current == receipt) FinishCurrent("cancel"); }
            });
            if (finished) await send(new { type = "popup-hide", token });
        }
    }
    internal void Command(JsonElement request)
    {
        string token = request.GetProperty("token").GetString() ?? "";
        if (current?.Token != token || disposed) return;
        var value = request.GetProperty("result");
        if (value.GetProperty("action").GetString() != "native-command" || Encoding.UTF8.GetByteCount(value.GetRawText()) > 8192)
            throw new InvalidDataException("原生弹窗操作超过允许范围。");
        current.Commands.Writer.TryWrite(value.Clone());
    }
    internal async Task<object> Update(JsonElement body, CancellationToken cancellation)
    {
        string token = body.GetProperty("token").GetString() ?? "";
        bool active = await ui.InvokeAsync(() => current?.Token == token && !disposed && healthy());
        if (!active) return new { accepted = false };
        if(body.TryGetProperty("close",out var close)&&close.ValueKind==JsonValueKind.True){await ui.InvokeAsync(()=>FinishCurrent("cancel"));await send(new{type="popup-hide",token});return new{accepted=true};}
        var data = body.GetProperty("data").Clone();
        if (Encoding.UTF8.GetByteCount(data.GetRawText()) > 196608) throw new InvalidDataException("原生弹窗数据超过上限。");
        await ui.InvokeAsync(()=>{RegisterImages(data);if(current?.Kind=="@magic/apps-menu")data=WarmApplications(data);});
        if(Encoding.UTF8.GetByteCount(data.GetRawText())>196608)throw new InvalidDataException("原生弹窗数据超过上限。");
        await send(new { type = "popup-update", token, data });
        return new { accepted = true };
    }
    internal async Task Close(CancellationToken cancellation){
        string? token=await ui.InvokeAsync(()=>{string? token=current?.Token;FinishCurrent("cancel");return token;});
        if(token is not null)await send(new{type="popup-hide",token});
    }
    internal void Shown(JsonElement request)
    {
        string token = request.GetProperty("token").GetString() ?? "";
        long handle = request.GetProperty("window").GetInt64();
        if (current?.Token != token || disposed)
            throw new InvalidDataException("弹窗显示请求已过期。");
        if (request.TryGetProperty("shown", out var shown) && !shown.GetBoolean())
        {
            FinishCurrent("cancel");
            return;
        }
        if (!CompositionRendererIdentity.Verify(handle, root))
            throw new InvalidDataException("弹窗窗口身份不匹配。");
        window = handle;
        current.Ready.TrySetResult(true);
    }
    internal void Result(JsonElement request)
    {
        string token = request.GetProperty("token").GetString() ?? "";
        if (current?.Token != token)
            return;
        var result = request.GetProperty("result");
        string action = result.GetProperty("action").GetString() ?? "";
        bool valid = action == "cancel" || (current.Kind switch { "@magic/keyboard-selector" => action is
                                                                  "keyboard-select" or "keyboard-settings",
                                                                  "@magic/calendar-popup" => action == "date-settings",
                                                                  "@magic/user-menu" => action is
                                                                  "settings" or "update" or "about" or "account" or "feedback" or "exit" or
                                                                  "sleep" or "lock" or "restart" or "shutdown",
                                                                  "@magic/power-menu" => action is
                                                                  "settings" or "exit" or "sleep" or "lock" or "restart" or "shutdown",
                                                                  _ => false });
        if (!valid || Encoding.UTF8.GetByteCount(result.GetRawText()) > 4096)
            throw new InvalidDataException("弹窗操作无效。");
        if (action is "restart" or "shutdown" &&
            (!result.TryGetProperty("confirmed", out var confirmed) || !confirmed.GetBoolean()))
            throw new InvalidDataException("该操作需要确认。");
        current.Ready.TrySetResult(true);
        current.Done.TrySetResult(result.Clone());
        current = null;images.Clear();
    }
    private void FinishCurrent(string action)
    {
        if (current is null)
            return;
        current.Ready.TrySetResult(false);
        current.Done.TrySetResult(JsonSerializer.SerializeToElement(new { action }));
        current = null;images.Clear();
    }
    public void Dispose()
    {
        if (disposed)
            return;
        disposed = true;
        FinishCurrent("cancel");
        foreach (var value in receipts.Values)
        {
            value.Ready.TrySetResult(false);
            value.Done.TrySetResult(JsonSerializer.SerializeToElement(new { action = "cancel" }));
        }
        receipts.Clear();
        try
        {
            if (window != 0 && CompositionRendererIdentity.Verify(window, root))
                ShowWindowAsync(new(window), 0);
        }
        catch (ArgumentException)
        {
        }
        catch (InvalidOperationException)
        {
        }
    }
    [DllImport("user32.dll")]
    private static extern bool ShowWindowAsync(IntPtr hwnd, int command);
}
