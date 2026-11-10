using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Threading;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

// The independent backend owns only this session. There is no legacy discovery,
// HTTP system-host forwarding or background service fallback in this adapter.
internal sealed partial class NativeSystemUiSession : IDisposable
{
    private readonly NativeSystemDeployment deployment;
    private readonly NativeSystemClient backend;
    private string notificationDigest = "";
    private readonly NativeNetworkPortal networkPortal;
    private readonly NativeNetworkDetails networkDetails, bluetoothDetails;
    private readonly ToolbarExtensions extensions;private int extensionProjectionQueued;
    private readonly NativeToolbarLyrics lyrics;
    private readonly ElevatedWindowActions elevatedWindows;
    private readonly NativeSystemPresentationSession presentation;
    private readonly CompositionShellSession shell;
    private readonly CompositionPopupSession popups;
    private readonly Dispatcher ui;
    private readonly int renderer;
    private readonly Func<string, Task> community;
    private readonly Action<string, object> log;
    private readonly Func<object, Task> send;
    private readonly CancellationTokenSource stop = new();
    private IReadOnlyDictionary<string, string> images = new Dictionary<string, string>(StringComparer.Ordinal);
    private readonly TaskCompletionSource barsReady = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private string? lastShellView;private long shellPublishes,shellDuplicates;
    internal object Diagnostics => new { shellPublishes=Interlocked.Read(ref shellPublishes),shellDuplicates=Interlocked.Read(ref shellDuplicates),presentation=presentation.Diagnostics };
    private readonly SemaphoreSlim popupGate = new(1, 1), configurationGate = new(1, 1);
    private readonly object currentGate = new();
    private readonly ConcurrentDictionary<string,byte> pendingWindowActions = new();
    private readonly RecycleBinCommand recycleBinCommand = new();
    private readonly ShowDesktopCommand showDesktopCommand = new();
    internal Task Released { get; private set; } = Task.CompletedTask;
    private readonly NativeNotificationWindows notificationWindows = new();
    private NativeSystemPanel? currentPanel;
    private string? currentToken, currentKind;
    private NativeTaskbarLease? taskbar;
    private sealed record View(JsonElement Data);
    private View latestView = new(default);
    private JsonElement CurrentView => Volatile.Read(ref latestView).Data;
    private int expectedBars, closing;private bool mediaPopupSubscribed;
    // Set by Start so Dispose can hand the settings window's hooks back.
    private AppController? host;
    private volatile bool notificationQuiet;
    private Task[] work = []; private long startMenuEpoch, quickSettingsEpoch, popupGeneration;
    private NativeSystemUiSession(NativeSystemDeployment deployment, NativeSystemClient backend, CompositionShellSession shell, CompositionPopupSession popups, int renderer, Dispatcher ui, Func<string, Task> community, Func<object, Task> send, Action<string, object> log)
    {
        this.deployment = deployment; this.backend = backend; this.shell = shell; this.popups = popups; this.renderer = renderer; this.ui = ui; this.community = community; this.send = send; this.log = log;
        elevatedWindows = new(deployment, log);
        presentation = new(backend, true, deployment.Installed ? NativeSystemBarDefaults.CreateInstalledConfiguration : null) { TrayProjection = ProjectTray };
        networkDetails = new(ReadNetworkDetails, PublishNetworkDetails, stop.Token);
        bluetoothDetails = new(ct => backend.Request("bluetooth-read", new { }, ct), PublishBluetoothDetails, stop.Token);
        networkPortal=new(PortalChanged,uri=>ui.BeginInvoke(new Action(()=>{if(stop.IsCancellationRequested||Volatile.Read(ref closing)!=0)return;try{OpenUri(uri);}catch(Exception e)when(e is not OutOfMemoryException){log("wifi_auth_open_failed",new{type=e.GetType().Name});}})));
        extensions=new(deployment.DataDirectory,QueueExtensionProjection,log,StateStore.AtomicWrite);shell.ExtensionState=ExtensionRenderState;
        lyrics=new(QueueExtensionProjection);
        LoadPreferences(); shell.InlineImage = Image; popups.InlineImage = Image; shell.BarReady += BarReady;
    }
    // While the card's tuning panel is open its in-memory copy is the truth: a slider
    // must reach the renderer without a disk write per step. Disk is written once, by
    // the panel, when it closes.
    private CommunityPreferences? cardDraft;
    // The standing tuning panel, if any. The card holds this process, not one handle:
    // WPF may replace the panel's HWND between EnsureHandle and Show.
    private AccountCardSettingsWindow? tuning;
    private CommunityPreferences Preferences() => Volatile.Read(ref cardDraft) ?? CommunityPreferences.Load(deployment.Root);
    internal void SetCardDraft(CommunityPreferences? draft) => Volatile.Write(ref cardDraft, draft);
    private static JsonObject PreferencesView(CommunityPreferences look) => new()
    {
        ["transparentToolbar"]=look.TransparentToolbar,
        ["coveredBackdrop"]=look.CoveredBackdrop,
        ["autoContrast"]=look.AutoContrast,
        ["iconScheme"]=look.IconScheme,
        ["windowAnimations"]=look.WindowAnimations,
        ["folderAnimations"]=look.FolderAnimations,
        ["brightnessHints"]=look.BrightnessHints,
        ["brightnessFollowSystem"]=look.BrightnessFollowSystem,
        ["showPages"]=look.ShowPages,
        ["fadePages"]=look.FadePages,
        ["dockDissolve"]=look.DockDissolve,
        ["reduceOnBatterySaver"]=look.ReduceOnBatterySaver,
        ["contextMenuLayout"]=look.ContextMenuColumns ? "columns" : "scroll",
        ["focusedSeparator"]=look.FocusedSeparator,
        ["toolbarDissolve"]=look.ToolbarDissolve,
        ["toolbarDragButtonOnly"]=look.ToolbarDragButtonOnly,
        ["accountCardWidth"]=look.AccountCardWidth,
        ["accountCardHeight"]=look.AccountCardHeight,
        ["accountCardX"]=look.AccountCardX,
        ["accountCardY"]=look.AccountCardY,
        ["accountCardRowHeight"]=look.AccountCardRowHeight,
        ["accountCardGap"]=look.AccountCardGap,
        ["accountCardFontSize"]=look.AccountCardFontSize,
        ["accountCardRadius"]=look.AccountCardRadius,
        ["accountCardOffsetX"]=look.AccountCardOffsetX,
        ["accountCardOffsetY"]=look.AccountCardOffsetY,
        ["accountCardMotion"]=look.AccountCardMotion
    };
    private string? Image(string id) => Volatile.Read(ref images).GetValueOrDefault(id) ?? CustomImage(id);
    // Custom bar/dock icons live on disk, not in the tray image cache, so serve them
    // here as the same PNG data URI the shell expects. Any format the WIC decoder
    // understands is re-encoded, which keeps the renderer to a single codec.
    private string? CustomImage(string id)
    {
        string? path; lock (customIcons) customIcons.TryGetValue(id, out path);
        if (path is null) return null;
        try
        {
            if (!File.Exists(path)) return null;
            using var stream = File.OpenRead(path);
            var decoder = System.Windows.Media.Imaging.BitmapDecoder.Create(stream, System.Windows.Media.Imaging.BitmapCreateOptions.IgnoreColorProfile, System.Windows.Media.Imaging.BitmapCacheOption.OnLoad);
            if (decoder.Frames.Count == 0) return null;
            System.Windows.Media.Imaging.BitmapSource source = decoder.Frames[0];
            // The bar cell is 16 DIP; 512 px stays crisp at every scale without paying
            // for the user's original resolution on every icon request.
            double edge = Math.Max(source.PixelWidth, source.PixelHeight);
            if (edge > 512) source = new System.Windows.Media.Imaging.TransformedBitmap(source, new System.Windows.Media.ScaleTransform(512d / edge, 512d / edge));
            var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
            encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(source));
            using var output = new MemoryStream();
            encoder.Save(output);
            return output.Length is > 0 and < 60000 ? "data:image/png;base64," + Convert.ToBase64String(output.ToArray()) : null;
        }
        catch (Exception e) when (e is not OutOfMemoryException) { return null; }
    }
    internal static async Task<NativeSystemUiSession> Start(AppController controller, NativeSystemDeployment deployment, CompositionShellSession shell, CompositionPopupSession popups, int renderer, Dispatcher ui, Func<string, Task> community, Func<object, Task> send, Action<string, object> log, CancellationToken cancellation)
    {
        var backend = await NativeSystemClient.Start(deployment.Backend, deployment.Files["MagicSystemNative.exe"], cancellation, deployment.DataDirectory);
        var owner = new NativeSystemUiSession(deployment, backend, shell, popups, renderer, ui, community, send, log);
        // Lending the live session to the settings window keeps one writer for the bar.
        owner.host = controller;
        popups.TuningVisible = () => Volatile.Read(ref owner.tuning) is not null;
        controller.ReadBarModules = owner.ReadBarModules; controller.SetBarModule = owner.SetBarModule;
        controller.RefreshPreferences = owner.RefreshPreferences;
        controller.RefreshCard = owner.RefreshCard; controller.CardWindow = () => owner.CardWindow; controller.HoldCard = owner.HoldCard; controller.CloseCard = owner.CloseCard; controller.CardDraft = owner.SetCardDraft;
        try
        {
            var initial = await owner.presentation.Initialize(cancellation); await owner.Publish(initial, cancellation);
            await owner.barsReady.Task.WaitAsync(TimeSpan.FromSeconds(10), cancellation);
            var expiry = deployment.StartAcceptance();
            owner.taskbar = await NativeTaskbarLease.Start(deployment.Guard, deployment.Files["MagicTaskbarGuard.exe"], renderer, backend.ProcessId, expiry, Path.Combine(deployment.Root, "runtime/system/native-taskbar"), () => ui.BeginInvoke(new Action(() => { if (Volatile.Read(ref owner.closing) == 0) Application.Current.Shutdown(); })), continuous:deployment.Continuous);
            owner.log("independent_native_ready", new { protocol = NativeSystemClient.Protocol, backendPid = backend.ProcessId, rendererPid = renderer, guardPid = owner.taskbar.ProcessId, expiresUtc = expiry, acceptanceMode=deployment.AcceptanceMode, originalGuiBootstrapped = false, backendDerivedFromSeelen = false, manualAcceptance = "pending" });
            var keyboard=ShortcutPreferences.Initialize(controller.Root,controller.Store.ReadOnly);
            try {
                await owner.SetKeyboardModes(keyboard,cancellation);
                var receipt=await backend.Request("shortcuts-subscribe",new{enabled=true,production=true},cancellation);
                if(Flag(receipt,"shortcutHookActive")!=(keyboard.Modes!=0))throw new IOException("快捷键钩子未就绪。");
                controller.KeyboardPreferences=keyboard;
                controller.ApplyKeyboardPreferences=value=>owner.ChangeKeyboard(controller,value);
            } catch(Exception e) when(e is not OutOfMemoryException && !cancellation.IsCancellationRequested) {
                await backend.Request("shortcuts-subscribe",new{enabled=false},cancellation);
                owner.log("keyboard_capture_unavailable",new{reason=e.GetType().Name});
            }
            using (var process = Process.GetCurrentProcess()) StateStore.AtomicWrite(Path.Combine(deployment.Root, "runtime/system/native-ready.json"), JsonSerializer.Serialize(new { schemaVersion = 1, protocol = NativeSystemClient.Protocol, nonce = deployment.Nonce, active = true, keyboardPreferencesVersion = 1, version = AppController.Version, adapterPid = Environment.ProcessId, adapterCreated = process.StartTime.ToFileTimeUtc().ToString(), backendPid = backend.ProcessId, rendererPid = renderer, guardPid = owner.taskbar.ProcessId, expiresUtc = expiry, acceptanceMode=deployment.AcceptanceMode, manualAcceptance = "pending" }), null);
            owner.work = [owner.Pump(), owner.Commands()]; return owner;
        }
        catch { owner.Dispose(); throw; }
    }
    private void BarReady() { if (expectedBars > 0 && shell.ReadyWindowCount >= expectedBars) barsReady.TrySetResult(); }
    // Writing ui-preferences.json only decides what the next view will say. The bar keeps
    // the view it was last handed, so the settings window calls this after every switch.
    internal async Task RefreshPreferences(CancellationToken token)
    {
        if (CurrentView.ValueKind != JsonValueKind.Object) return;
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(token, stop.Token);
        await Publish(JsonNode.Parse(CurrentView.GetRawText())!.AsObject(), linked.Token);
    }
    // The card's tuning panel edits shape while the card stands. A shell publish only
    // carries preferences to the renderer; the standing card is a popup and needs its
    // own repaint, which also re-runs the card's height contract. The panel throttles
    // its calls, so this is one repaint per settled change, never a loop.
    internal async Task RefreshCard(CancellationToken token)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(token, stop.Token);
        string? cardToken;
        lock (currentGate) cardToken = currentKind == "@magic/user-menu" ? currentToken : null;
        if (cardToken is null) return;
        // The card's rows carry the door switches as well as the shape, so this is the
        // same panel the card opened with, built from the unsaved draft. Only the card
        // is refreshed: the bars are not republished, so a drag costs one small packet.
        var panel = NativeSystemPanel.Build("@magic/user-menu", NetworkState(presentation.State), notificationQuiet: notificationQuiet, accountCard: Preferences());
        panel.Data["hostPid"] = Environment.ProcessId;
        bool stale;
        lock (currentGate) { stale = currentToken != cardToken; if (!stale) currentPanel = panel; }
        if (stale) return;
        // The hold repeats with every preview: a drag keeps the panel in front of the
        // card, and each refresh restates that the panel's process is not the reader
        // leaving.
        int hold = Volatile.Read(ref tuning) is null ? 0 : Environment.ProcessId;
        await popups.Update(JsonSerializer.SerializeToElement(new { token = cardToken, data = panel.Data, preferences = PreferencesView(Preferences()), holdPid = hold }), linked.Token);
    }
    // The panel is placed from the card's real screen rectangle, not from a guess.
    internal long CardWindow { get { lock (currentGate) { if (currentKind != "@magic/user-menu") return 0; } return popups.Window; } }
    internal Task CloseCard() => popups.Close(stop.Token);
    internal Task HoldCard(long window) => popups.Hold(window, stop.Token);
    private async Task Publish(JsonObject projected, CancellationToken cancellation, bool projectionOnly = false)
    {
        if(!projectionOnly){notificationWindows.Observe(presentation.State.Snapshot);
            if(presentation.State.Providers.TryGetValue("wifi",out var wifiStatus))networkPortal.Observe(wifiStatus);
            ProjectJumpLists(projected);EmitMediaCard();extensions.Configure(projected);}
        extensions.Project(projected);
        lyrics.Project(projected,presentation.State.Providers.GetValueOrDefault("media"));
        projected["hostPid"]=Environment.ProcessId;
        projected["magicNotificationQuiet"]=notificationQuiet;
        // The renderer reads these names straight out of the view (transparentToolbar,
        // coveredBackdrop, iconScheme, the animation switches). Restating them here is
        // what makes the settings window's switches reach the bar at all.
        var look = Preferences();
        projected["preferences"]=PreferencesView(look);
        // The mark is part of the row's own content, so the choice has to reach the rows
        // and not only the preferences block above them.
        NativeSystemPresentation.ApplyFocusedSeparator(projected, look.FocusedSeparator);
        if(!projectionOnly){var nextImages = new Dictionary<string, string>(StringComparer.Ordinal); foreach (var icon in Array(presentation.State.Providers.GetValueOrDefault("tray"), "icons")) { string png = Text(icon, "png"); if (png.Length is > 0 and < 90000) nextImages["tray:" + Text(icon, "id")] = "data:image/png;base64," + png; }
            Volatile.Write(ref images, nextImages);}
        // The notification centre draws straight from this array, so "the toast never
        // arrived" and "we arrived with no words and drew an empty card" have to be
        // told apart from the outside. One compact line per change is enough for that,
        // and it keeps the notification text itself out of the log.
        try
        {
            var rows = Array(presentation.State.Providers.GetValueOrDefault("notifications"), "notifications")
                .Select(row => new { app = Text(row, "appName"), appId = Text(row, "appId"), lines = Array(row, "text").Length, created = Text(row, "createdFileTime") }).ToArray();
            string digest = string.Join("|", rows.Select(row => row.appId + ":" + row.created + ":" + row.lines));
            if (digest != notificationDigest) { notificationDigest = digest; log("notifications_snapshot", new { count = rows.Length, rows }); }
        }
        catch { /* A diagnostic must never break the publish. */ }
        lock (orderReceipts) { projected["orderReceipts"] = new JsonArray(orderReceipts.Select(x => (JsonNode)x.DeepClone()).ToArray()); }
        lock (customIcons) foreach (string surface in new[] { "toolbar", "dock" }) foreach (string section in new[] { "left", "center", "right" }) foreach (var row in projected[surface]![section]!.AsArray()) if (customIcons.TryGetValue(row!["id"]!.GetValue<string>(), out var image)) {row["icon"] = image;row["customIcon"]=true;}
        // The left cell is composed on the renderer side from these five fields:
        // which bundled mark, whether it is hidden, where the wordmark comes from,
        // and which of the two mark styles to draw. They are restated from scratch
        // on every publish so republishing an already-projected view cannot leave a
        // mark behind after it was reset.
        lock (brandIcons) foreach (string surface in new[] { "toolbar", "dock" }) foreach (string section in new[] { "left", "center", "right" }) foreach (var row in projected[surface]![section]!.AsArray())
        {
            row!.AsObject().Remove("brandIcon"); row.AsObject().Remove("brandIconHide");
            row.AsObject().Remove("brandLabel"); row.AsObject().Remove("brandText"); row.AsObject().Remove("brandStyle");
            row.AsObject().Remove("brandDevice");
            if (!brandIcons.TryGetValue(row["id"]!.GetValue<string>(), out var brand)) continue;
            if (brand.Icon is { Length: > 0 } slug) row["brandIcon"] = slug;
            if (brand.IconHidden) row["brandIconHide"] = "1";
            if (brand.Device == "desktop") row["brandDevice"] = "desktop";
            row["brandLabel"] = brand.Label;
            if (brand.Text.Length > 0) row["brandText"] = brand.Text;
            if (brand.Style == "color") row["brandStyle"] = "color";
        }
        string serialized=projected.ToJsonString();using var document = JsonDocument.Parse(serialized); var view = document.RootElement.Clone(); Volatile.Write(ref latestView, new(view));
        expectedBars = Array(view, "monitors").Sum(m => (Flag(m, "toolbar") ? 1 : 0) + (Flag(m, "dock") ? 1 : 0));
        if (expectedBars == 0) throw new InvalidDataException("Independent validation requires an enabled toolbar or Dock.");
        if(lastShellView!=serialized){var result=await shell.Update(JsonSerializer.SerializeToElement(new { data = view }), cancellation);if(Flag(JsonSerializer.SerializeToElement(result),"accepted")){lastShellView=serialized;Interlocked.Increment(ref shellPublishes);}}else Interlocked.Increment(ref shellDuplicates);await ui.InvokeAsync(BarReady);
        if(projectionOnly)return;
        long menuEpoch = (long)Number(presentation.State.Snapshot, "startMenuEpoch"); if (menuEpoch > startMenuEpoch) { startMenuEpoch = menuEpoch; await Open("@magic/apps-menu", default); }
        long quickEpoch=(long)Number(presentation.State.Snapshot,"quickSettingsEpoch");
        if(quickEpoch>quickSettingsEpoch){quickSettingsEpoch=quickEpoch;await Open("@magic/quick-settings",default);}
        string? token, kind; bool powerView,displayView; lock (currentGate) { token = currentToken; kind = currentKind; powerView=currentPanel?.Data["nativePowerView"]?.GetValue<bool>()==true;displayView=currentPanel?.Data["nativeDisplayView"]?.GetValue<bool>()==true; }
        if (token is not null && kind is not null && kind is not ("@magic/apps-menu" or "@magic/keyboard-selector" or "@magic/calendar-popup" or "@magic/user-menu"))
        {
            var topic = kind switch { "@magic/bluetooth-popup" => "bluetooth", "@magic/network-popup" => "wifi", "@magic/media-popup" => "media", "@magic/notifications" => "notifications", "@magic/system-tray" => "tray", _ => "windows-system" };
            bool quickChanged = kind == "@magic/quick-settings" && presentation.ChangedTopics.Overlaps(new[] { "audio", "brightness", "wifi", "bluetooth" });
            if (!presentation.ChangedTopics.Contains(topic) && !quickChanged && !(kind == "@magic/media-popup" && presentation.ChangedTopics.Contains("audio")) && !presentation.ChangedTopics.Contains("configuration")) return;
            var state = presentation.State;
            if (topic == "bluetooth" && kind == "@magic/bluetooth-popup") { bluetoothDetails.Show(token); return; }
            if (topic == "wifi") { if (NetworkPopupAcceptsDetails(token)) networkDetails.Show(token); return; }
            if (kind == "@magic/media-popup") state = await MediaState(state, cancellation);
            var panel = NativeSystemPanel.Build(kind, NetworkState(state),powerView,notificationQuiet,displayView,kind=="@magic/user-menu"?CommunityPreferences.Load(deployment.Root):null); panel.Data["hostPid"] = Environment.ProcessId; lock (currentGate) { if (currentToken == token) currentPanel = panel; else return; }
            await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), cancellation);
        }
    }
    private async Task Pump()
    {
        try
        {
            while (!stop.IsCancellationRequested)
            {
                var next = await presentation.Next(5000, stop.Token);
                if (next is not null)
                {
                    // Only a new provider event invalidates the snapshot, not
                    // extension/media projections reusing the last topic set.
                    if (presentation.ChangedTopics.Contains("wifi")) networkDetails.Invalidate();
                    if (presentation.ChangedTopics.Contains("bluetooth")) bluetoothDetails.Invalidate();
                    await Publish(next, stop.Token);
                }
            }
        }
        catch (Exception error) when (error is not OutOfMemoryException) { Failed(error); }
    }
    private async Task Commands()
    {
        try { while (!stop.IsCancellationRequested) { var value = JsonSerializer.SerializeToElement(await shell.Next(stop.Token)); try { await ShellCommand(value); } catch (Exception error) when (error is not OutOfMemoryException) { await Notice(error); } } }
        catch (Exception error) when (error is not OutOfMemoryException) { Failed(error); }
    }
    private void Failed(Exception error) { if (stop.IsCancellationRequested) return; log("independent_native_session_failed", new { type = error.GetType().Name, error = error.Message }); taskbar?.RequestStop(); ui.BeginInvoke(new Action(() => Application.Current.Shutdown(1))); }
    private Task Notice(Exception error) { if (stop.IsCancellationRequested) return Task.CompletedTask; log("independent_native_action_failed", new { type = error.GetType().Name, error = error.Message, operation = (error as NativeSystemRequestException)?.Operation, errorCode = (error as NativeSystemRequestException)?.ErrorCode }); return send(new { type = "card-notice", title = "操作未完成", text = error.Message }); }
    private JsonElement? Offered(string id)
    {
        var latest = CurrentView; foreach (string surface in new[] { "toolbar", "dock" }) foreach (string section in new[] { "left", "center", "right" }) foreach (var row in Array(Object(latest, surface), section)) if (Text(row, "id") == id || Text(row, "key") == id) return row; return null;
    }
    private JsonElement ObjectFromProvider(string name)=>presentation.State.Providers.GetValueOrDefault(name);
    internal async Task<object> AdjustFlyout(JsonElement command){
        double value=Number(command,"value",double.NaN);if(!double.IsFinite(value)||value<0||value>1)throw new InvalidDataException("Adjustment outside 0..1.");
        string token = Text(command, "token");
        if (token.Length > 0)
        {
            Command? offered;
            lock (currentGate) offered = currentToken == token ? currentPanel?.Commands.GetValueOrDefault(Text(command, "row")) : null;
            if (offered is null || offered.Local.Length > 0 || offered.BooleanValue || offered.ValueField != "value" ||
                offered.Operation is not ("audio-action" or "brightness-action" or "microphone-action" or "mixer-action"))
                throw new InvalidDataException("调节入口已变化，请重新打开面板。");
            try { return await SetRange(offered, value,Flag(command,"feedback")); }
            catch (Exception error) when (error is not OutOfMemoryException) { await Notice(error); throw; }
        }
        string mode=Text(command,"mode");
        if(mode=="volume"){var audio=ObjectFromProvider("audio");if(!Flag(audio,"available"))throw new InvalidOperationException("音频设备暂不可用。");return await SetRange(Backend("audio-action",new{action="set-volume",deviceId=Text(audio,"defaultDeviceId")},"value"),value,Flag(command,"feedback"));}
        else if(mode=="brightness"){string target=Text(command,"displayId");if(!Array(ObjectFromProvider("brightness"),"displays").Any(d=>Text(d,"id")==target))throw new InvalidDataException("亮度目标已变化。");return await SetRange(Backend("brightness-action",new{displayId=target},"value"),value);}
        else throw new InvalidDataException("Unknown adjustment.");
    }
    private async Task<object> SetRange(Command action, double value,bool feedback=false)
    {
        var args = action.Arguments.DeepClone().AsObject(); args["value"] = value;
        var response = await backend.Request(action.Operation, args, stop.Token);
        double actual = Number(response, "value", double.NaN);
        if (!Flag(response, "accepted") || !double.IsFinite(actual) || actual < 0 || actual > 1) throw new InvalidOperationException("设备未确认调节结果，请重试。");
        if(NativeVolumeFeedback.ShouldPlay(feedback,action.Operation,actual,Flag(response,"muted"))&&!NativeVolumeFeedback.Play())
            log("volume_feedback_unavailable",new{reason="sound-sample"});
        return response;
    }
    internal static string SelectWindowAction(string verb,bool wasFocused,int count,bool minimized) =>
        verb=="close-one" ? "close" :
        verb is "close" or "minimize" or "restore" ? verb :
        verb=="activate"&&wasFocused&&count==1&&!minimized ? "minimize" : "activate";
    private async Task DispatchWindowAction(JsonElement window,string action,string key)
    {
        try { await PerformWindowAction(window,action); }
        catch(Exception error) when(error is not OutOfMemoryException) { await Notice(error); }
        finally { pendingWindowActions.TryRemove(key,out _); }
    }
    private async Task PerformWindowAction(JsonElement window,string action)
    {
        // The renderer grants this adapter its click permission. Pass it to the
        // actual foreground caller immediately before the interactive request.
        if(action is "activate" or "restore")NativeDesktopOcclusion.Covered(new IntPtr((long)Number(window,"hwnd")),renderer,lower:true);
        bool granted=(action is "activate" or "restore") && AllowSetForegroundWindow((uint)backend.ProcessId);
        var response=await backend.Request("window-action",new{windowId=Text(window,"nativeWindowId"),action},stop.Token);
        if(Flag(response,"needsElevation") && action is "activate" or "restore" or "minimize" or "close")
        {
            var observation = await ui.InvokeAsync(() => new WindowActionObservation(new IntPtr((long)Number(window,"hwnd"))));
            try
            {
                await elevatedWindows.Ensure(stop.Token);
                // Resolve the original opaque identity again after the UAC wait.
                // No fallback to another window from the same application.
                var identity=await backend.Request("window-action",new{windowId=Text(window,"nativeWindowId"),action="inspect"},stop.Token);
                await ui.InvokeAsync(() => { if(observation.Expired)throw new InvalidOperationException("原窗口已关闭，请重新选择。"); });
                response=await elevatedWindows.Perform(identity,action,stop.Token);
                await ui.InvokeAsync(() => { if(action!="close"&&observation.Expired)throw new InvalidOperationException("操作期间原窗口已关闭。"); });
            }
            finally { await ui.InvokeAsync(observation.Dispose); }
        }
        bool accepted=Flag(response,"accepted");
        log("independent_native_window_action",new{action,foregroundGranted=granted,accepted,response});
        if(!accepted)throw new InvalidOperationException(Flag(response,"restored") ? "窗口已恢复，但 Windows 未允许切到前台；请再次点击。" : "Windows 未完成窗口操作。请检查目标应用是否响应或受到权限限制。");
    }
    private async Task ShellCommand(JsonElement command)
    {
        string id = Text(command, "id"), verb = Text(command, "verb", "activate");
        if(verb=="extension-action"){_ = ExtensionInvoke(command);return;}
        // Tray icons are a list inside one module, so their order is not the module order
        // and cannot ride the same verb. This has to precede the bar:toolbar catch-all
        // below, which hands every other bar verb to ConfigureBar.
        if (verb == "tray-order") { await SetTrayOrder(Array(command, "trayOrder").Select(x => x.GetString() ?? "").Where(x => x.Length > 0).ToArray(), stop.Token); return; }
        if (id == "__measure-popup") { await Open(Text(command, "widget"), command); return; }
        if ((id is "bar:toolbar" or "bar:dock") && verb == "windows-authorization") { taskbar?.RevealWindowsEntry(); return; }
        if ((id is "bar:toolbar" or "bar:dock") && verb == "show-desktop") { showDesktopCommand.Request(Notice); return; }
        if (id is "bar:toolbar" or "bar:dock") { await ConfigureBar(id == "bar:dock" ? "dock" : "toolbar", command); return; }
        if (id == "" && Text(command, "widget").Length > 0) { await Open(Text(command, "widget"), command); return; }
        if(verb=="remove"&&Offered(id) is {} external&&Text(external,"type")=="Extension"){extensions.SetEnabled(Text(external,"nativeExtension"),false);return;}
        var offered = Offered(id) ?? throw new InvalidOperationException("此入口已变化，请重试。");
        if (Text(offered, "type") == "Uac") { taskbar?.RevealWindowsEntry(); return; }
        if (Text(offered, "type") is "Media" or "Music" && (verb is "prev" or "play" or "next")) {
            if(Text(offered,"type")=="Music"&&!Flag(offered,verb=="prev"?"canPrevious":verb=="next"?"canNext":Flag(offered,"playing")?"canPause":"canPlay"))return;
            await backend.Request("media-action", new { sessionId = Text(offered, "nativeSessionId"), trackId = Text(offered,"nativeTrackId"), action = verb == "prev" ? "previous" : verb == "play" ? "toggle" : "next" }, stop.Token); return;
        }
        if(Text(offered,"type")=="Music")return;
        if(Text(offered,"type")=="Media"&&verb=="open-source"){_ = OpenMediaSource(offered);return;}
        if (Text(offered, "type") == "Tray")
        {
            if (verb is "right" or "context") await popups.Close(stop.Token);
            await TrayAction(Text(offered, "nativeTrayId"), verb is "right" or "context" ? "context" : verb == "double" ? "double-click" : verb == "middle" ? "middle" : "activate", Number(command, "x"), Number(command, "y")); return;
        }
        if (Text(offered, "type") == "AppOrFile")
        {
            if(verb=="jump-prefetch"){_ = PrefetchJump(offered);return;}if(verb=="jump-open"){_ = OpenJump(offered,Text(command,"destination"));return;}
            if (verb is "pin" or "unpin" or "remove" or "open-location" or "run-as" or "runas" or "new-instance" or "launch" or "edit-icon") { await ApplicationCommand(offered, verb); return; }
            var windows = Array(offered, "windows");
            if (verb == "close") { foreach (var entry in windows) await PerformWindowAction(entry, "close"); return; }
            if (verb == "kill") { OpenUri(Path.Combine(Environment.SystemDirectory, "Taskmgr.exe")); return; }
            double target = Number(command, "target"); var window = windows.FirstOrDefault(w => Number(w, "hwnd") == target); if (window.ValueKind != JsonValueKind.Object) { if(target!=0)throw new InvalidOperationException("目标窗口已变化，请重新选择。"); window = windows.FirstOrDefault(); }
            if (window.ValueKind == JsonValueKind.Object && verb != "open")
            {
                string action = SelectWindowAction(verb,Flag(command,"wasFocused")&&!NativeDesktopOcclusion.Covered(new IntPtr((long)Number(window,"hwnd")),renderer),windows.Length,IsIconic(new IntPtr((long)Number(window,"hwnd"))));
                string key=Text(window,"nativeWindowId");
                if(pendingWindowActions.TryAdd(key,0)) _=DispatchWindowAction(window,action,key);
            }
            else Launch(offered, false); return;
        }
        // Every bar-item edit verb has to be routed here first: unknown verbs fall through
        // to the module switch below and would e.g. reopen the user menu.
        if (verb is "remove" or "move-left" or "move-right" or "edit-custom-icon" or "reset-icon" or "set-brand-icon" or "set-brand-device" or "set-brand-style" or "set-brand-label" or "set-brand-icon-hide" or "set-menu-layout" or "set-focused-separator" or "set-toolbar-drag-button-only") { await ConfigureBar(Text(offered, "nativeSurface", "dock"), command); return; }
        string module = Text(offered, "nativeModule");
        string kind = module switch { "magic.user" => "@magic/user-menu", "magic.power-menu" or "magic.battery" => "@magic/power-menu", "magic.clock" => "@magic/calendar-popup", "magic.tray" => "@magic/system-tray", "magic.bluetooth" => "@magic/bluetooth-popup", "magic.network" => "@magic/network-popup", "magic.keyboard" => "@magic/keyboard-selector", "magic.quick-settings" => "@magic/quick-settings", "magic.audio" or "magic.player" => "@magic/media-popup", "magic.notifications" => "@magic/notifications", "magic.start" => "@magic/apps-menu", _ => "" };
        if (module == "magic.desktop") { showDesktopCommand.Request(Notice); return; }
        if (module == "magic.recycle") { if (verb == "empty-bin") recycleBinCommand.Request(Notice); else OpenUri("shell:RecycleBinFolder"); return; }
        if(module=="magic.battery"){var trigger=JsonNode.Parse(command.GetRawText())!.AsObject();trigger["nativePowerView"]=true;await Open("@magic/quick-settings",JsonSerializer.SerializeToElement(trigger));return;}
        if (kind.Length > 0) await Open(kind, command);
        else if (module is "magic.cpu" or "magic.memory" or "magic.gpu") OpenUri(Path.Combine(Environment.SystemDirectory, "Taskmgr.exe"));
        else if (module == "magic.storage") OpenUri("ms-settings:storagesense");
        else throw new InvalidOperationException("当前组件未提供可执行动作。");
    }
    private async Task<NativeSystemPresentationSession.Published> MediaState(NativeSystemPresentationSession.Published state, CancellationToken cancellation) { var providers = new Dictionary<string, JsonElement>(state.Providers); providers["mixer"] = await backend.Request("mixer-read", new { }, cancellation); providers["microphone"] = await backend.Request("microphone-read", new { }, cancellation); return state with { Providers = providers }; }
    // popupGate serializes subscription transitions and receipt cleanup.
    private async Task SetMediaPopupSubscription(bool enabled){
        if(mediaPopupSubscribed==enabled)return;
        try{await backend.Request("mixer-subscribe",new{enabled},stop.Token);await backend.Request("microphone-subscribe",new{enabled},stop.Token);mediaPopupSubscribed=enabled;}
        catch{if(enabled&&!stop.IsCancellationRequested){foreach(string operation in new[]{"mixer-subscribe","microphone-subscribe"})try{await backend.Request(operation,new{enabled=false},stop.Token);}catch(Exception e)when(e is not OutOfMemoryException){log("independent_audio_panel_rollback",new{type=e.GetType().Name});}mediaPopupSubscribed=false;}throw;}
    }
    private readonly SemaphoreSlim keyboardChanges=new(1,1);
    private async Task SetKeyboardModes(ShortcutPreferences value,CancellationToken token)
    {
        var receipt=await backend.Request("shortcuts-config",new{win=value.Win,winA=value.WinA,production=true},token);
        if(Number(receipt,"shortcutModes",-1)!=value.Modes)throw new IOException("系统后台未确认快捷键设置。");
    }
    private async Task ChangeKeyboard(AppController controller,ShortcutPreferences value)
    {
        await keyboardChanges.WaitAsync(stop.Token);
        try {
            if(controller.Store.ReadOnly||ShortcutPreferences.IsReadOnly(controller.Root))throw new IOException("当前配置为只读。");
            var before=controller.KeyboardPreferences;
            try {
                await SetKeyboardModes(value,stop.Token);
                value.Save(controller.Root);
            } catch {
                try {await SetKeyboardModes(before,stop.Token);}
                catch(Exception e) when(e is not OutOfMemoryException){
                    await backend.Request("shortcuts-subscribe",new{enabled=false},stop.Token);
                    controller.KeyboardPreferences=new();controller.ApplyKeyboardPreferences=null;
                }
                throw;
            }
            controller.KeyboardPreferences=value;controller.SetStatus("快捷键设置已生效。");
        } finally {keyboardChanges.Release();}
    }
    private async Task Open(string kind, JsonElement trigger)
    {
        kind=NativePopupIds.Canonical(kind);
        if (!CompositionPopupSession.Supported.Contains(kind)) throw new InvalidDataException("Unknown panel.");
        var timing=Stopwatch.StartNew();await popupGate.WaitAsync(stop.Token);double queuedMs=timing.Elapsed.TotalMilliseconds;bool shown=false;
        try
        {
            networkDetails.Hide(); bluetoothDetails.Hide();
            long generation = Interlocked.Increment(ref popupGeneration);
            // Capture before any provider awaits or popup takes focus. This is
            // independent of the Dock list, which deliberately excludes desktop windows.
            // Capture on entry, while the application still owns foreground.
            // A switch from another bar panel can then reuse its validated target.
            JsonElement inputTarget=default;
            if(Text(trigger,"id")!="__measure-popup"){
                try{inputTarget=await backend.Request("input-target",new{popupProcessId=renderer},stop.Token);}
                catch(NativeSystemRequestException e){log("input_target_unavailable",new{kind,e.Message});}
            }
            if (kind == "@magic/notifications") await backend.Request("notifications-subscribe", new { enabled = true }, stop.Token);
            bool media = kind == "@magic/media-popup"; await SetMediaPopupSubscription(media);
            var state = media ? await MediaState(presentation.State, stop.Token) : presentation.State;
            if (kind == "@magic/keyboard-selector")
            {
                var inputProviders = new Dictionary<string, JsonElement>(state.Providers);
                if(Flag(inputTarget,"available")){
                    try{inputProviders["input"]=await backend.Request("input-read",new{windowId=Text(inputTarget,"id")},stop.Token);}
                    catch(NativeSystemRequestException){inputTarget=default;}
                }
                if(!Flag(inputTarget,"available"))inputProviders["input"]=await backend.Request("input-read",new{},stop.Token);
                state = state with { Providers = inputProviders };
            }
            if(kind=="@magic/quick-settings"&&!Flag(trigger,"nativePowerView")){
                var displayProviders=new Dictionary<string,JsonElement>(state.Providers);
                displayProviders["brightness"]=await backend.Request("brightness-read",new{refresh=true},stop.Token);
                state=state with{Providers=displayProviders};
            }
            double providerMs=timing.Elapsed.TotalMilliseconds;var panel = kind == "@magic/apps-menu" ? await AppsPanel(opening: true) : NativeSystemPanel.Build(kind, NetworkState(state),Flag(trigger,"nativePowerView"),notificationQuiet,Flag(trigger,"nativeDisplayView"),kind=="@magic/user-menu"?CommunityPreferences.Load(deployment.Root):null);
            // The shared popup gate never waits for Wi-Fi enumeration. Show
            // current state now and refresh only the still-visible network token.
            if (kind == "@magic/network-popup") panel = NativeSystemPanel.Build(kind, NetworkPanelState(), notificationQuiet: notificationQuiet);
            panel.Data["hostPid"] = Environment.ProcessId;if(Text(trigger,"id")=="__measure-popup")panel.Data["measurement"]=true;
            string foreground = Text(inputTarget, "id");
            if(kind=="@magic/keyboard-selector"){
                panel.Data["inputTargetAvailable"]=foreground.Length>0;
                if(foreground.Length==0)panel.Data["title"]="键盘布局 · 请先激活输入窗口";
            }
            double x = Number(trigger, "x"), y = Number(trigger, "y"); if (x == 0 && y == 0) { GetCursorPos(out var point); x = point.X; y = point.Y; }
            // Bar input grants this adapter foreground rights. Return them to
            // our owned renderer before it activates the requested popup.
            bool delegated=AllowSetForegroundWindow((uint)renderer);double panelMs=timing.Elapsed.TotalMilliseconds;
            // The tuning panel's unsaved copy rides with the card's first packet too,
            // so a card opened while the panel stands is shaped from the draft.
            object? cardLook = kind == "@magic/user-menu" ? PreferencesView(Preferences()) : null;
            var opened = JsonSerializer.SerializeToElement(await popups.Open(JsonSerializer.SerializeToElement(new { widgetId = kind, payload = new { desiredPosition = new { x, y }, alignX = "Center", alignY = y > SystemParameters.PrimaryScreenHeight / 2 ? "End" : "Start" }, data = panel.Data, preferences = cardLook }), stop.Token));
            log(kind=="@magic/apps-menu"?"independent_apps_open_timing":"independent_popup_open_timing",new{kind,triggerSource=trigger.ValueKind==JsonValueKind.Undefined?"keyboard":"shell",delegated,queueMs=queuedMs,providerMs=providerMs-queuedMs,panelMs=panelMs-providerMs,rendererAckMs=timing.Elapsed.TotalMilliseconds-panelMs,totalAckMs=timing.Elapsed.TotalMilliseconds,accepted=Flag(opened,"accepted"),scope="Owned renderer receipt; not first DWM frame"});
            if (!Flag(opened, "accepted")){if(media)await SetMediaPopupSubscription(false);return;} string token = Text(opened, "token");shown=true;
            lock (currentGate) { currentToken = token; currentKind = kind; currentPanel = panel; }
            if (kind == "@magic/network-popup") networkDetails.Show(token);
            if (kind == "@magic/bluetooth-popup") bluetoothDetails.Show(token);
            _ = WaitPopup(token, kind, foreground, generation);
            if (kind == "@magic/apps-menu") { await MeasureAppsWindow(opened,token); _ = RefreshApplications(token, generation); }
        }
        finally {try{if(kind=="@magic/media-popup"&&!shown&&mediaPopupSubscribed&&!stop.IsCancellationRequested)try{await SetMediaPopupSubscription(false);}catch(Exception e)when(e is not OutOfMemoryException){log("independent_audio_panel_release",new{type=e.GetType().Name});}}finally{popupGate.Release();}}
    }
    private async Task WaitPopup(string token, string kind, string foreground, long generation)
    {
        try
        {
            while (!stop.IsCancellationRequested)
            {
                var received = JsonSerializer.SerializeToElement(await popups.Wait(JsonSerializer.SerializeToElement(new { token }), stop.Token));
                bool keep = Flag(received, "keepOpen"); var result = keep ? Object(received, "result") : received;
                if (Text(result, "action") == "native-command")
                {
                    Command? action; lock (currentGate) { action = currentToken == token ? currentPanel?.Commands.GetValueOrDefault(Text(result, "id")) : null; }
                    if (action is not null) try { await PanelCommand(action, result, token); } catch (Exception error) when (error is not OutOfMemoryException) { await Notice(error); }
                }
                else if (Text(result, "action") != "cancel") try { await FinalPopup(result, foreground); } catch (Exception error) when (error is not OutOfMemoryException) { await Notice(error); }
                if (!keep) break;
            }
        }
        catch (Exception error) when (error is not OutOfMemoryException) { if (!stop.IsCancellationRequested) log("independent_popup_ended", new { kind, type = error.GetType().Name }); }
        finally
        {
            if (kind == "@magic/network-popup") networkDetails.Hide(token);
            if (kind == "@magic/bluetooth-popup") bluetoothDetails.Hide(token);
            lock (currentGate) { if (currentToken == token) { currentToken = currentKind = null; currentPanel = null; } }
            if(kind=="@magic/media-popup"&&!stop.IsCancellationRequested){bool entered=false;try{await popupGate.WaitAsync(stop.Token);entered=true;if(Interlocked.Read(ref popupGeneration)==generation)await SetMediaPopupSubscription(false);}catch(Exception error)when(error is not OutOfMemoryException){log("independent_audio_panel_release",new{type=error.GetType().Name});}finally{if(entered)popupGate.Release();}}
        }
    }
    private async Task PanelCommand(Command action, JsonElement result, string token)
    {
        if (action.Local.Length > 0) { await LocalCommand(action, result, token); return; }
        var args = action.Arguments.DeepClone().AsObject();
        if (action.ValueField.Length > 0) { if (action.ValueField == "password") args["password"] = Text(Object(result, "fields"), "password"); else if (action.BooleanValue) { var v = result.GetProperty("value"); if (v.ValueKind is JsonValueKind.True or JsonValueKind.False) args[action.ValueField] = v.GetBoolean(); else { double n = v.GetDouble(); if (n != 0 && n != 1) throw new InvalidDataException("Toggle value outside 0/1."); args[action.ValueField] = n == 1; } } else args[action.ValueField] = Number(result, "value", double.NaN); }
        if(action.Operation is "wifi-action" or "bluetooth-action" or "bluetooth-unpair"){
            JsonObject? busy=null;lock(currentGate){if(currentToken==token&&currentPanel is not null)busy=currentPanel.Data.DeepClone().AsObject();}
            if(busy is not null){foreach(var row in busy["rows"]!.AsArray())row!["enabled"]=false;await popups.Update(JsonSerializer.SerializeToElement(new{token,data=busy}),stop.Token);}
            try{var deviceResult=await backend.Request(action.Operation,args,stop.Token);if(deviceResult.TryGetProperty("accepted",out var acceptedDevice)&&acceptedDevice.ValueKind==JsonValueKind.False)throw new InvalidOperationException("Windows 尚未确认设备操作，已重新读取实际状态。");}
            finally{
                string topic=action.Operation.StartsWith("wifi-",StringComparison.Ordinal)?"wifi":"bluetooth";
                if (topic == "wifi") NetworkActionCompleted(token);
                else {
                    bluetoothDetails.Invalidate();
                    lock(currentGate) if(currentToken==token && currentKind=="@magic/bluetooth-popup") bluetoothDetails.Show(token);
                }
            }
            return;
        }
        var response = await backend.Request(action.Operation, args, stop.Token);
        if (action.Operation is "notifications-clear" or "notifications-remove")
        {
            var state = presentation.State; var providers = new Dictionary<string, JsonElement>(state.Providers);
            providers["notifications"] = await backend.Request("notifications-read", new { }, stop.Token);
            var panel = NativeSystemPanel.Build("@magic/notifications", state with { Providers = providers },notificationQuiet:notificationQuiet); panel.Data["hostPid"] = Environment.ProcessId;
            lock (currentGate) { if (currentToken != token) return; currentPanel = panel; }
            await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), stop.Token);
            if(response.TryGetProperty("accepted",out var done)&&done.ValueKind==JsonValueKind.False)
                throw new NativeSystemRequestException($"Windows 尚未移除 {Number(response,"remaining")} 条通知，已刷新列表。",response.TryGetProperty("errorCode",out var errorCode)?errorCode.GetInt32():null,action.Operation);
        }
        else if (response.TryGetProperty("accepted", out var accepted) && accepted.ValueKind == JsonValueKind.False) throw new InvalidOperationException("Windows 未完成该操作，请查看设备当前状态。");
    }
    private async Task FinalPopup(JsonElement result, string foreground)
    {
        string action = Text(result, "action");
        if (action == "keyboard-select") {
            string layout = Text(result, "id");
            if (foreground.Length == 0 || layout.Length == 0) throw new InvalidOperationException("输入目标或键盘布局已变化，请重新打开面板。");
            AllowSetForegroundWindow((uint)backend.ProcessId);
            var selectionClock = Stopwatch.StartNew();
            var selected=await backend.Request("input-select", new { layoutId = layout, windowId = foreground }, stop.Token);
            log("independent_input_selection", new { accepted = Flag(selected,"accepted"), delivered = Flag(selected,"delivered"), requestMs = selectionClock.Elapsed.TotalMilliseconds, deliveryMs = Number(selected,"deliveryMs"), win32Error = Number(selected,"win32Error") });
            if(!Flag(selected,"accepted"))throw new InvalidOperationException("目标应用尚未确认输入法切换，请先激活目标应用；未重复发送切换请求。");
            return; }
        if (action == "keyboard-settings") { OpenUri("ms-settings:regionlanguage"); return; }
        if (action == "date-settings") { OpenUri("ms-settings:dateandtime"); return; }
        if (action is "settings" or "update" or "about" or "account" or "feedback" or "exit") { await ui.InvokeAsync(() => community(action)).Task.Unwrap(); return; }
        if (action == "lock") { if (!LockWorkStation()) throw new System.ComponentModel.Win32Exception(); return; }
        if (action == "sleep") { if (!SetSuspendState(false, false, false)) throw new System.ComponentModel.Win32Exception(); return; }
        if (action is "restart" or "shutdown" && Flag(result, "confirmed")) { var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "shutdown.exe")) { UseShellExecute = false, CreateNoWindow = true }; start.ArgumentList.Add(action == "restart" ? "/r" : "/s"); start.ArgumentList.Add("/t"); start.ArgumentList.Add("0"); Process.Start(start)?.Dispose(); return; }
        throw new InvalidDataException("Popup action is not offered.");
    }
    private static void OpenUri(string uri) { Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true })?.Dispose(); }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref closing, 1) != 0) return;
        lyrics.Dispose();
        if (host is { } borrowed) { borrowed.ReadBarModules = null; borrowed.SetBarModule = null; borrowed.RefreshPreferences = null; borrowed.RefreshCard = null; borrowed.CardWindow = null; borrowed.HoldCard = null; borrowed.CloseCard = null; borrowed.CardDraft = null; host = null; }
        try { string path = Path.Combine(deployment.Root, "runtime/system/native-ready.json"); if (File.Exists(path) && new FileInfo(path).Length < 8192) { var ready = JsonNode.Parse(File.ReadAllText(path))!.AsObject(); if (ready["nonce"]?.GetValue<string>() == deployment.Nonce && ready["adapterPid"]?.GetValue<int>() == Environment.ProcessId) { ready["active"] = false; ready["stoppedUtc"] = DateTimeOffset.UtcNow.ToString("o"); StateStore.AtomicWrite(path, ready.ToJsonString(), null); } } } catch (Exception error) { log("independent_native_readiness_close", new { type = error.GetType().Name }); }
        lock(jumpGate)jumpLists.Clear();MediaCardChanged=null;networkPortal.Dispose();networkDetails.Dispose();bluetoothDetails.Dispose();stop.Cancel(); elevatedWindows.Dispose(); taskbar?.RequestStop(); shell.BarReady -= BarReady; shell.InlineImage = null; popups.InlineImage = null; popups.TuningVisible = null; Volatile.Write(ref tuning, null);shell.ExtensionState=null; Volatile.Write(ref images, new Dictionary<string, string>());
        Released = Task.Run(async () => {
            try { if (taskbar is not null) await taskbar.DisposeAsync(); }
            finally {
                try { await extensions.DisposeAsync(); await presentation.DisposeAsync(); }
                finally { try { await backend.DisposeAsync(); } finally { stop.Dispose(); } }
            }
        });
        _ = Released.ContinueWith(task => log("independent_native_release", new { error = task.Exception!.GetBaseException().GetType().Name }), CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.Default);
    }
    [StructLayout(LayoutKind.Sequential)] private struct Point { internal int X, Y; }
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern bool AllowSetForegroundWindow(uint processId);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern bool LockWorkStation();
    [DllImport("powrprof.dll", SetLastError = true)] private static extern bool SetSuspendState(bool hibernate, bool force, bool disableWake);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string cls, string? title);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr h, uint message, IntPtr w, IntPtr l);
}
