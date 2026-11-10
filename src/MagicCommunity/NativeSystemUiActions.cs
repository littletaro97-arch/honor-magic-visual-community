using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using Microsoft.Win32;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

internal sealed partial class NativeSystemUiSession
{
    private readonly List<JsonObject> orderReceipts = [];
    private readonly Dictionary<string, string> customIcons = new(StringComparer.Ordinal);
    // Row id -> how the left cell is composed: which bundled mark (assets/brand-icons),
    // whether the mark is hidden, and where the wordmark comes from. Kept apart from
    // customIcons because one is a compiled-in name and the other a file on disk.
    internal sealed record BrandOverride(string? Icon, bool IconHidden, string Label, string Text, string Style, string Device)
    {
        internal static readonly BrandOverride Empty = new(null, false, "brand", "", "mono", "");
    }
    private readonly Dictionary<string, BrandOverride> brandIcons = new(StringComparer.Ordinal);
    // The corner is drawn from the published view, so a brand edit has to go out as a
    // new view. Evicting the bitmap cache alone only ever refreshed imported images,
    // which is why picking a bundled mark used to do nothing until something else
    // happened to republish.
    private async Task RefreshBrand(string id)
    {
        BrandOverride? current = null; lock (brandIcons) brandIcons.TryGetValue(id, out current);
        log("brand_refresh", new { id, icon = current?.Icon, label = current?.Label, style = current?.Style, device = current?.Device, hidden = current?.IconHidden });
        if (CurrentView.ValueKind == JsonValueKind.Object) await Publish(JsonNode.Parse(CurrentView.GetRawText())!.AsObject(), stop.Token);
        string? publishedIcon = null, publishedLabel = null, publishedStyle = null;
        if (CurrentView.ValueKind == JsonValueKind.Object)
            foreach (var row in Array(Object(CurrentView, "toolbar"), "left"))
                if (Text(row, "id") == id) { publishedIcon = Text(row, "brandIcon"); publishedLabel = Text(row, "brandLabel"); publishedStyle = Text(row, "brandStyle"); }
        log("brand_published", new { icon = publishedIcon, label = publishedLabel, style = publishedStyle, publishes = Interlocked.Read(ref shellPublishes), duplicates = Interlocked.Read(ref shellDuplicates) });
        await send(new { type = "icons-invalidated", keys = new[] { id } });
    }
    private void StoreBrand(string id, Func<BrandOverride, BrandOverride> change)
    {
        lock (brandIcons)
        {
            brandIcons.TryGetValue(id, out var current);
            brandIcons[id] = change(current ?? BrandOverride.Empty);
            StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "brand-icons.json"), JsonSerializer.Serialize(brandIcons), null);
        }
    }
    private static readonly HashSet<string> SettingsUris = new(StringComparer.Ordinal) { "ms-settings:", "ms-settings:bluetooth", "ms-settings:network-wifi", "ms-settings:sound", "ms-settings:powersleep", "ms-settings:notifications", "ms-settings:regionlanguage", "ms-settings:dateandtime", "ms-settings:privacy-notifications", "ms-settings:display", "ms-settings:nightlight", "ms-settings:network", "ms-settings:privacy-location" };
    private async Task LocalCommand(Command action, JsonElement result, string token)
    {
        var args = JsonSerializer.SerializeToElement(action.Arguments);
        switch (action.Local)
        {
            case "notification-quiet":
                bool quiet=args.GetProperty("enabled").GetBoolean();
                NativeNotificationPreferences.Save(deployment.DataDirectory,quiet);
                notificationQuiet=quiet;
                if(CurrentView.ValueKind==JsonValueKind.Object)await Publish(JsonNode.Parse(CurrentView.GetRawText())!.AsObject(),stop.Token);
                if(currentKind=="@magic/notifications"){
                    var updated=NativeSystemPanel.Build("@magic/notifications",presentation.State,notificationQuiet:quiet);updated.Data["hostPid"]=Environment.ProcessId;
                    lock(currentGate){if(currentToken!=token)break;currentPanel=updated;}
                    await popups.Update(JsonSerializer.SerializeToElement(new{token,data=updated.Data}),stop.Token);
                }
                break;
            case "apps-item": case "apps-control": await AppsCommand(action, result, token); break;
            case "account-open":
                {
                    string target = Text(args, "target");
                    // One row can be a settings page or the task manager; both are
                    // vetted here rather than in the renderer, which only knows a label.
                    if (target == "taskmgr")
                        OpenUri(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "Taskmgr.exe"));
                    else if (!SettingsUris.Contains(target)) throw new InvalidDataException("Settings destination differs.");
                    else OpenUri(target);
                    break;
                }
            case "account-tune":
                // Shape and position are judged against the card, so the panel opens
                // beside it rather than in the settings window. The command runs off
                // the popup pipeline, which is not the UI thread, and a Window cannot
                // be created on anything but STA - hence the dispatch.
                await OpenAccountTune();
                break;
            case "settings-uri": string uri = Text(args, "uri"); if (!SettingsUris.Contains(uri)) throw new InvalidDataException("Settings destination differs."); OpenUri(uri); break;
            case "panel": await popups.Close(stop.Token); await Open(Text(args, "kind"), args); break;
            case "appearance-theme":
                await Task.Run(()=>NativeAppearanceSettings.SetDark(Flag(args,"dark")),stop.Token);
                var themePanel=NativeSystemPanel.Build("@magic/quick-settings",presentation.State,notificationQuiet:notificationQuiet,displayView:true);themePanel.Data["hostPid"]=Environment.ProcessId;
                lock(currentGate){if(currentToken!=token)break;currentPanel=themePanel;}
                await popups.Update(JsonSerializer.SerializeToElement(new{token,data=themePanel.Data}),stop.Token);break;
            case "honor-power": OpenUri(HonorPcManager.Find() ?? throw new InvalidOperationException("未找到荣耀电脑管家。")); break;
            case "tray":
                int button = (int)Number(result, "button");
                if (button is 4 or 5) { await TrayPreference(args, result); break; }
                await popups.Close(stop.Token); GetCursorPos(out var point); await TrayAction(Text(args, "iconId"), button == 2 ? "context" : button == 3 ? "double-click" : button == 1 ? "middle" : "activate", point.X, point.Y); break;
            case "open-application": await popups.Close(stop.Token); await OpenNotificationApplication(Text(args,"aumid")); break;
            case "notifications-authorize": await AuthorizeNotifications(); break;
            case "bluetooth-refresh": bluetoothDetails.Invalidate(); bluetoothDetails.Show(token); break;
            case "wifi-auth": OpenUri(NativeNetworkPortal.AuthenticationUri);break;
            case "wifi-connect": await ConnectWifi(args, token); break;
            case "app-launch": await popups.Close(stop.Token); Launch(args, false); break;
            default: throw new InvalidDataException("Local action is not offered.");
        }
    }
    // The panel's window is created without activating it, then the card is told to
    // hold it, and only then is it shown. Activation before that hold would read as
    // the reader clicking away and dismiss the card this panel exists to tune.
    private async Task OpenAccountTune()
    {
        if (host is null) return;
        AccountCardSettingsWindow? window = null;
        long handle = await ui.InvokeAsync(() =>
        {
            window = new AccountCardSettingsWindow(host);
            return new System.Windows.Interop.WindowInteropHelper(window).EnsureHandle().ToInt64();
        });
        if (window is null) return;
        var opened = window;
        // The card holds the panel's process, not just this handle: WPF may replace the
        // HWND between EnsureHandle and Show, and the panel is still the same reader.
        Volatile.Write(ref tuning, opened);
        opened.Closed += (_, _) => { if (ReferenceEquals(Volatile.Read(ref tuning), opened)) Volatile.Write(ref tuning, null); };
        // The handle is named before Show can take focus; a card that is not shown yet
        // inherits the same hold in its first packet once its receipt lands.
        if (handle != 0 && host.HoldCard is { } hold) await hold(handle);
        try { await ui.InvokeAsync(() => opened.Show()); }
        catch { if (ReferenceEquals(Volatile.Read(ref tuning), opened)) Volatile.Write(ref tuning, null); throw; }
    }
    private async Task<bool> TrayAction(string iconId, string action, double x, double y)
    {
        // The clicked renderer grants this adapter input rights. Delegate only
        // to our owned backend; it validates the live icon owner before granting
        // that process permission to show its own window/menu.
        bool delegated = AllowSetForegroundWindow((uint)backend.ProcessId);
        var response = await backend.Request("tray-action", new { iconId, action, x, y }, stop.Token);
        log("independent_native_tray_action", new { action, delegated, callbackPosted = Flag(response, "accepted"), ownerForegroundGranted = Flag(response, "ownerForegroundGranted") });
        return Flag(response,"accepted");
    }
    private static void OpenApplication(string aumid)
    {
        if (aumid.Length is < 1 or > 512 || aumid.Contains('"') || aumid.Contains('\0')) throw new InvalidDataException("Application identity differs.");
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe")) { UseShellExecute = false }; start.ArgumentList.Add("shell:AppsFolder\\" + aumid); Process.Start(start)?.Dispose();
    }
    private async Task OpenNotificationApplication(string aumid)
    {
        var state=presentation.State;
        var resolved=Array(state.Providers.GetValueOrDefault("launches"),"items").ToDictionary(item=>Text(item,"id"),item=>Text(item,"target"));
        var targets=new List<string>();
        foreach(string surface in new[]{"toolbar","dock"})foreach(string section in new[]{"left","center","right"})
            foreach(var item in Array(Object(Object(state.Configuration,surface),"sections"),section))
                if(Text(item,"aumid").Equals(aumid,StringComparison.OrdinalIgnoreCase))targets.Add(resolved.GetValueOrDefault(Text(item,"id"),Text(Object(item,"launch"),"executable",Text(item,"target"))));
        targets.AddRange(applicationEntries.Where(entry=>entry.Aumid.Equals(aumid,StringComparison.OrdinalIgnoreCase)).Select(entry=>entry.Target));
        var snapshot=await backend.Request("snapshot",new{},stop.Token);
        await notificationWindows.Open(aumid,snapshot,targets,window=>PerformWindowAction(JsonSerializer.SerializeToElement(new{nativeWindowId=Text(window,"id"),hwnd=long.Parse(Text(window,"hwnd"),System.Globalization.CultureInfo.InvariantCulture)}),"activate"),OpenApplication,NativeNotificationWindows.WindowAppId,()=>ActivateResidentQq(targets));
    }
    private async Task<bool> ActivateResidentQq(IEnumerable<string> targets)
    {
        using var current=Process.GetCurrentProcess();
        var processes=Process.GetProcessesByName("QQ");
        try{
            bool running=false;
            foreach(var process in processes){
                try{if(process.SessionId==current.SessionId){running=true;break;}}
                catch(InvalidOperationException){/* Process exited during the snapshot. */}
            }
            if(!running)return false;
        }finally{foreach(var process in processes)process.Dispose();}
        // A resident QQNT often has no task window. Activating its registered
        // AppsFolder entry in this state starts a second sign-in window.
        var tray=await backend.Request("tray-read",new{},stop.Token);
        string? id=NativeNotificationWindows.QqTrayIcon(tray,targets);
        if(id is not null){
            GetCursorPos(out var point);
            if(await TrayAction(id,"double-click",point.X,point.Y))return true;
        }
        throw new InvalidOperationException("QQ 正在运行。请从系统托盘打开对应的 QQ 账号。");
    }
    private static void Launch(JsonElement item, bool administrator)
    {
        string aumid = Text(item, "umid", Text(item, "aumid")); var launch = Object(item, "nativeLaunch"); string target = Text(launch, "executable", Text(item, "path", Text(item, "target")));
        if ((target.Length == 0 || !File.Exists(target) && !Directory.Exists(target)) && aumid.Length > 0) { if (administrator) throw new InvalidOperationException("此打包应用不支持此管理员启动方式。"); OpenApplication(aumid); return; }
        if (!Path.IsPathFullyQualified(target) || target.StartsWith(@"\\", StringComparison.Ordinal) || (!File.Exists(target) && !Directory.Exists(target))) throw new FileNotFoundException("程序或文件已移动。", target);
        var start = new ProcessStartInfo(target) { UseShellExecute = true }; if (administrator) start.Verb = "runas";
        string working = Text(launch, "workingDirectory"); if (working.Length > 0 && Directory.Exists(working)) start.WorkingDirectory = working;
        if (launch.ValueKind == JsonValueKind.Object && launch.TryGetProperty("arguments", out var arguments)) { if (arguments.ValueKind == JsonValueKind.Array) foreach (var argument in arguments.EnumerateArray()) start.ArgumentList.Add(argument.GetString() ?? ""); else if (arguments.ValueKind == JsonValueKind.String) start.Arguments = arguments.GetString() ?? ""; }
        Process.Start(start)?.Dispose();
    }
    private async Task ApplicationCommand(JsonElement item, string verb)
    {
        if (verb is "launch" or "new-instance") { Launch(item, false); return; }
        if (verb is "runas" or "run-as") { Launch(item, true); return; }
        if (verb == "open-location")
        {
            string path = Text(item, "path"); if (!Path.IsPathFullyQualified(path)) throw new InvalidOperationException("此应用没有可打开的本地位置。");
            var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe")) { UseShellExecute = false }; start.ArgumentList.Add("/select,"); start.ArgumentList.Add(path); Process.Start(start)?.Dispose(); return;
        }
        if (verb == "edit-icon") { await EditIcon(Text(item, "id")); return; }
        await configurationGate.WaitAsync(stop.Token);
        try
        {
            var read = await backend.Request("configuration-read", new { }, stop.Token); var config = JsonNode.Parse(read.GetProperty("configuration").GetRawText())!.AsObject();
            string id = Text(item, "id"); bool found = false;
            foreach (string section in new[] { "left", "center", "right" })
            {
                var list = config["dock"]!["sections"]![section]!.AsArray(); for (int n = list.Count - 1; n >= 0; --n) if (list[n]!["id"]!.GetValue<string>() == id) { if (verb is "unpin" or "remove") list.RemoveAt(n); else list[n]!["pinned"] = true; found = true; }
            }
            if (!found && verb == "pin") config["dock"]!["sections"]!["center"]!.AsArray().Add(ConfigurationApplication(item, true));
            await SaveConfiguration(config);
        }
        finally { configurationGate.Release(); }
    }
    private static JsonObject ConfigurationApplication(JsonElement row, bool pinned)
    {
        string path = Text(row, "path"); string aumid = Text(row, "umid"); if (path.Length == 0 && aumid.Length == 0) throw new InvalidDataException("App launch identity is unavailable.");
        var item = new JsonObject { ["id"] = Text(row, "id"), ["kind"] = "application", ["displayName"] = Text(row, "label"), ["target"] = path, ["aumid"] = aumid, ["pinned"] = pinned, ["allowPin"] = true };
        if (row.TryGetProperty("nativeLaunch", out var launch)) item["launch"] = JsonNode.Parse(launch.GetRawText()); return item;
    }
    private async Task ConfigureBar(string surface, JsonElement command)
    {
        string verb = Text(command, "verb"), id = Text(command, "id"), destination = Text(command, "destination"), token = Text(command, "orderToken");
        if(verb=="toggle-extension"){if(surface!="toolbar")throw new InvalidOperationException("外部扩展仅支持顶栏。");await ToggleExtension(destination);return;}
        if (verb == "reset-icon") { await ResetCustomIcon(id); return; }
        if (verb == "set-brand-icon") { await SetBrandIcon(id, destination); return; }
        if (verb == "set-brand-device") { await SetBrandDevice(id, destination); return; }
        if (verb == "set-brand-style") { if (id.Length > 0) { StoreBrand(id, e => e with { Style = destination == "color" ? "color" : "mono" }); await RefreshBrand(id); } return; }
        if (verb == "set-brand-label") { await SetBrandLabel(id, destination); return; }
        if (verb == "set-brand-icon-hide") { if (id.Length > 0) { StoreBrand(id, e => e with { IconHidden = destination == "1" }); await RefreshBrand(id); } return; }
        // The layout switch lives in the bar's own menu, so it is a bar command rather
        // than a settings window toggle: write the preference, then republish the view.
        if (verb == "set-menu-layout") { var look = CommunityPreferences.Load(deployment.Root); look.ContextMenuColumns = destination == "columns"; look.Save(deployment.Root); await RefreshPreferences(stop.Token); return; }
        // The separator belongs to the focused rows, so its switch lives on them; it is
        // the same preference the settings window writes.
        if (verb == "set-focused-separator") { var look = CommunityPreferences.Load(deployment.Root); look.FocusedSeparator = destination is "bar" or "none" ? destination : "desktop"; look.Save(deployment.Root); await RefreshPreferences(stop.Token); return; }
        // T39: the switch lives on the tray button's own menu; the renderer reads it
        // from the preferences projection, so publishing the view is the whole write.
        if (verb == "set-toolbar-drag-button-only") { var look = CommunityPreferences.Load(deployment.Root); look.ToolbarDragButtonOnly = destination != "off"; look.Save(deployment.Root); await RefreshPreferences(stop.Token); return; }
        if (verb == "windows-settings") { OpenUri("ms-settings:"); return; }
        if (verb == "settings") { await ui.InvokeAsync(() => community("settings")).Task.Unwrap(); return; }
        if (verb is "edit-icon" or "edit-custom-icon") { await EditIcon(id); return; }
        if (verb == "pin-file")
        {
            string? file = await ui.InvokeAsync(() => { var picker = new OpenFileDialog { Title = "固定到 Dock", CheckFileExists = true }; return picker.ShowDialog() == true ? picker.FileName : null; });
            if (file is null) return; var row = JsonSerializer.SerializeToElement(new { id = "app:" + Guid.NewGuid().ToString("N"), path = file, label = Path.GetFileNameWithoutExtension(file), umid = "" }); await ApplicationCommand(row, "pin"); return;
        }
        await configurationGate.WaitAsync(stop.Token);
        try
        {
            var read = await backend.Request("configuration-read", new { }, stop.Token); var config = JsonNode.Parse(read.GetProperty("configuration").GetRawText())!.AsObject(); NativeSystemBarDefaults.NormalizeToolbarMusic(config); var bar = config[surface]!.AsObject(); var sections = bar["sections"]!.AsObject();
            if (verb == "restore") NativeSystemBarDefaults.RestoreToolbar(config,surface);
            else if (verb == "lock") bar["locked"] = !(bar["locked"]?.GetValue<bool>() ?? false);
            else if (verb == "order")
            {
                if (bar["locked"]?.GetValue<bool>() == true) throw new InvalidOperationException("请先解锁模块顺序。");
                var known = new Dictionary<string, JsonNode>(StringComparer.Ordinal); foreach (string section in new[] { "left", "center", "right" }) foreach (var item in sections[section]!.AsArray()) known.Add(item!["id"]!.GetValue<string>(), item.DeepClone()); var visible = Enumerable.Range(0, 1).SelectMany(_ => new[] { "left", "center", "right" }.SelectMany(g => Array(Object(CurrentView, surface), g))).Select(r => Text(r, "id")).ToHashSet(StringComparer.Ordinal); var required = known.Keys.Where(visible.Contains).ToHashSet(StringComparer.Ordinal);
                foreach (string section in new[] { "left", "center", "right" }) foreach (var row in Array(Object(CurrentView, surface), section)) if (Text(row, "type") == "AppOrFile" && !known.ContainsKey(Text(row, "id"))) known.Add(Text(row, "id"), ConfigurationApplication(row, false));
                var order = Array(command, "order").Select(x => x.GetString() ?? "").ToArray(); if (order.Length > 514 || order.Distinct().Count() != order.Length || order.Count(x => x == "@boundary:left") != 1 || order.Count(x => x == "@boundary:right") != 1) throw new InvalidDataException("模块顺序格式无效。");
                var next = new JsonObject { ["left"] = new JsonArray(), ["center"] = new JsonArray(), ["right"] = new JsonArray() }; string group = "left"; bool left = false, right = false; var seen = new HashSet<string>(StringComparer.Ordinal);
                foreach (string key in order) { if (key == "@boundary:left") { if (right) throw new InvalidDataException("Section boundary order differs."); left = true; group = "center"; } else if (key == "@boundary:right") { if (!left) throw new InvalidDataException("Section boundary order differs."); right = true; group = "right"; } else { if (!known.TryGetValue(key, out var row)) throw new InvalidDataException("模块已变化，请重试。"); seen.Add(key); next[group]!.AsArray().Add(row.DeepClone()); } }
                if (!required.IsSubsetOf(seen)) throw new InvalidDataException("顺序变更缺少原有组件。"); foreach (string oldSection in new[] { "left", "center", "right" }) foreach (var hidden in sections[oldSection]!.AsArray()) if (!seen.Contains(hidden!["id"]!.GetValue<string>())) next[oldSection]!.AsArray().Add(hidden.DeepClone()); bar["sections"] = next;
            }
            else if (verb is "toggle-module" or "toggle-media")
            {
                string module = verb == "toggle-media" ? "magic.player" : destination;
                await RequireModule(module);
                ApplyModule(sections, surface, module, !ModulePresent(sections, module));
            }
            else if (verb == "remove")
            {
                bool removed = false; foreach (string group in new[] { "left", "center", "right" }) { var list = sections[group]!.AsArray(); for (int n = list.Count - 1; n >= 0; --n) if (list[n]!["id"]!.GetValue<string>() == id) { list.RemoveAt(n); removed = true; } }
                if (!removed) throw new InvalidDataException("组件已移除。");
            }
            else if (verb == "separator")
            {
                string anchor = destination.StartsWith("before:", StringComparison.Ordinal) ? destination[7..] : destination.StartsWith("after:", StringComparison.Ordinal) ? destination[6..] : ""; var separator = new JsonObject { ["id"] = "separator:" + Guid.NewGuid().ToString("N"), ["kind"] = "separator" }; bool inserted = false;
                foreach (string group in new[] { "left", "center", "right" }) { var list = sections[group]!.AsArray(); for (int n = 0; n < list.Count; ++n) if (list[n]!["id"]!.GetValue<string>() == anchor) { list.Insert(n + (destination.StartsWith("after:", StringComparison.Ordinal) ? 1 : 0), separator); inserted = true; break; } if (inserted) break; }
                if (!inserted) sections["center"]!.AsArray().Add(separator);
            }
            else { log("bar_command_unhandled", new { verb, destination, id, surface }); throw new InvalidOperationException("此菜单动作尚未连接独立配置。"); }
            await SaveConfiguration(config); if (token.Length > 0) { lock (orderReceipts) { orderReceipts.Add(new() { ["token"] = token, ["ok"] = true,["revision"]=config["revision"]!.GetValue<long>() }); while (orderReceipts.Count > 32) orderReceipts.RemoveAt(0); } }
        }
        catch (Exception error) { if (token.Length > 0) { lock (orderReceipts) { orderReceipts.Add(new() { ["token"] = token, ["ok"] = false, ["error"] = error.Message }); while (orderReceipts.Count > 32) orderReceipts.RemoveAt(0); } } throw; }
        finally { configurationGate.Release(); }
    }
    // One place decides what "this module is on" means, so the bar menu and the
    // settings page cannot disagree about where a row lives.
    private static bool ModulePresent(JsonObject sections, string module)
    {
        foreach (string group in new[] { "left", "center", "right" })
            foreach (var row in sections[group]!.AsArray())
                if (row!["module"]?.GetValue<string>() == module) return true;
        return false;
    }
    private static void ApplyModule(JsonObject sections, string surface, string module, bool enabled)
    {
        foreach (string group in new[] { "left", "center", "right" })
        {
            var list = sections[group]!.AsArray();
            for (int n = list.Count - 1; n >= 0; --n) if (list[n]!["module"]?.GetValue<string>() == module) list.RemoveAt(n);
        }
        if (enabled) sections[surface == "toolbar" ? "right" : "center"]!.AsArray().Add(new JsonObject { ["id"] = surface + ":" + module, ["kind"] = "module", ["module"] = module });
    }
    private async Task RequireModule(string module)
    {
        var catalog = await backend.Request("configuration-catalog", new { }, stop.Token);
        if (!Array(catalog, "modules").Any(m => m.ValueKind == JsonValueKind.String ? m.GetString() == module : Text(m, "id") == module)) throw new InvalidDataException("模块不在内置目录中。");
    }
    // The settings window has no pipe of its own, so it borrows this one: the catalog
    // it lists and the state it shows come from the same configuration the bar edits.
    internal async Task<IReadOnlyList<BarModuleState>> ReadBarModules(CancellationToken token)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(token, stop.Token);
        await configurationGate.WaitAsync(linked.Token);
        try
        {
            var read = await backend.Request("configuration-read", new { }, linked.Token);
            var config = JsonNode.Parse(read.GetProperty("configuration").GetRawText())!.AsObject();
            var toolbar = config["toolbar"]!["sections"]!.AsObject(); var dock = config["dock"]!["sections"]!.AsObject();
            var rows = new List<BarModuleState>();
            foreach (var (id, details) in NativeSystemPresentation.ModuleCatalog)
                rows.Add(new BarModuleState(id, details.Label, details.Icon, ModulePresent(toolbar, id), ModulePresent(dock, id)));
            rows.Sort((a, b) => StringComparer.CurrentCulture.Compare(a.Label, b.Label));
            return rows;
        }
        finally { configurationGate.Release(); }
    }
    // A module can live on both surfaces, so the caller always names the one it means.
    internal async Task SetBarModule(string surface, string module, bool enabled, CancellationToken token)
    {
        if (surface is not ("toolbar" or "dock")) throw new InvalidDataException("只有顶栏和任务栏有模块。");
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(token, stop.Token);
        await RequireModule(module);
        await configurationGate.WaitAsync(linked.Token);
        try
        {
            var read = await backend.Request("configuration-read", new { }, linked.Token);
            var config = JsonNode.Parse(read.GetProperty("configuration").GetRawText())!.AsObject();
            var sections = config[surface]!["sections"]!.AsObject();
            if (ModulePresent(sections, module) == enabled) return;
            ApplyModule(sections, surface, module, enabled);
            await SaveConfiguration(config);
        }
        finally { configurationGate.Release(); }
    }
    private async Task SaveConfiguration(JsonObject config)
    {
        long prior = config["revision"]!.GetValue<long>(); config["revision"] = prior + 1;
        await backend.Request("configuration-write", new { expectedRevision = prior, configuration = config }, stop.Token);
    }
    // A bundled brand mark is a name, not a file, so the renderer can draw it from
    // its baked asset table and the user never manages an image.
    private async Task SetBrandIcon(string id, string slug)
    {
        if (id.Length == 0 || slug.Length == 0 || slug.Length > 64) return;
        // Picking a bundled mark supersedes an imported file for the same row.
        lock (customIcons) if (customIcons.Remove(id)) StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "custom-icons.json"), JsonSerializer.Serialize(customIcons), null);
        StoreBrand(id, e => e with { Icon = slug, IconHidden = false });
        await RefreshBrand(id);
    }
    // The default entry is not one glyph but a family: a laptop or a desktop. Choosing
    // either clears any vendor mark, so the corner falls back to the device shape.
    private async Task SetBrandDevice(string id, string device)
    {
        if (id.Length == 0) return;
        string shape = device == "desktop" ? "desktop" : "";
        lock (customIcons) if (customIcons.Remove(id)) StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "custom-icons.json"), JsonSerializer.Serialize(customIcons), null);
        StoreBrand(id, e => e with { Icon = null, Device = shape, Style = "mono", IconHidden = false });
        await RefreshBrand(id);
    }
    // Dropping the entry is enough: the next icon request resolves the built-in glyph
    // again, and icons-invalidated evicts the cached bitmap.
    // The wordmark is independent of the mark: vendor name, this PC's name, typed
    // text, or nothing at all.
    private async Task SetBrandLabel(string id, string mode)
    {
        if (id.Length == 0) return;
        if (mode == "custom")
        {
            string current; lock (brandIcons) current = brandIcons.TryGetValue(id, out var existing) ? existing.Text : "";
            string? text = await ui.InvokeAsync(() => PromptText("自定义字标", current));
            if (text is null) return;
            // An empty box is a deliberate "no wordmark", not a cancel.
            StoreBrand(id, e => e with { Label = text.Length == 0 ? "none" : "custom", Text = text });
        }
        else
        {
            if (mode is not ("brand" or "host" or "none")) return;
            StoreBrand(id, e => e with { Label = mode });
        }
        await RefreshBrand(id);
    }
    // Small modal prompt; the bar menus cannot collect text themselves. It has to be
    // topmost: this app has no visible owner window, so a plain dialog opens behind
    // whatever the user runs fullscreen and the modal loop then looks like a freeze.
    private static string? PromptText(string title, string initial)
    {
        string? result = null;
        var window = new System.Windows.Window
        {
            Title = title, Width = 380, SizeToContent = System.Windows.SizeToContent.Height,
            WindowStartupLocation = System.Windows.WindowStartupLocation.CenterScreen,
            ResizeMode = System.Windows.ResizeMode.NoResize, ShowInTaskbar = false,
            Topmost = true, ShowActivated = true,
        };
        var panel = new System.Windows.Controls.StackPanel { Margin = new System.Windows.Thickness(16) };
        var box = new System.Windows.Controls.TextBox { Text = initial, MaxLength = 32 };
        var ok = new System.Windows.Controls.Button { Content = "确定", IsDefault = true, Width = 88, Margin = new System.Windows.Thickness(0, 12, 0, 0), HorizontalAlignment = System.Windows.HorizontalAlignment.Right };
        ok.Click += (_, _) => { result = box.Text.Trim(); window.Close(); };
        window.PreviewKeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Escape) window.Close(); };
        window.Loaded += (_, _) => { window.Activate(); box.Focus(); };
        panel.Children.Add(new System.Windows.Controls.TextBlock { Text = "字标文字（留空表示不显示字标）", Margin = new System.Windows.Thickness(0, 0, 0, 8) });
        panel.Children.Add(box);
        panel.Children.Add(ok);
        window.Content = panel;
        box.SelectAll();
        window.ShowDialog();
        return result;
    }
    // Both stores are cleared: an imported image and a bundled mark are two ways to
    // override the same glyph, so "reset" has to drop whichever is present.
    private async Task ResetCustomIcon(string id)
    {
        if (id.Length == 0) return;
        bool removed = false;
        lock (brandIcons) if (brandIcons.Remove(id)) { removed = true; StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "brand-icons.json"), JsonSerializer.Serialize(brandIcons), null); }
        lock (customIcons) if (customIcons.Remove(id)) { removed = true; StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "custom-icons.json"), JsonSerializer.Serialize(customIcons), null); }
        if (!removed) return;
        await RefreshBrand(id);
    }
    // Bounds mirror what the renderer can consume: one decodable frame, at most 4096
    // pixels per edge, and a file small enough to ship as a data URI.
    private static void ValidateIconFile(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists) throw new InvalidDataException("图标文件不存在。");
        if (info.Length == 0) throw new InvalidDataException("图标文件是空的。");
        if (info.Length > 4 * 1024 * 1024) throw new InvalidDataException($"图标文件 {info.Length / 1024} KB，超过 4 MB 上限。");
        try
        {
            using var stream = File.OpenRead(path);
            var decoder = System.Windows.Media.Imaging.BitmapDecoder.Create(stream, System.Windows.Media.Imaging.BitmapCreateOptions.IgnoreColorProfile, System.Windows.Media.Imaging.BitmapCacheOption.OnLoad);
            if (decoder.Frames.Count == 0) throw new InvalidDataException("图标里没有可用的图像帧。");
            var frame = decoder.Frames[0];
            if (frame.PixelWidth is < 1 or > 4096 || frame.PixelHeight is < 1 or > 4096) throw new InvalidDataException($"图标尺寸 {frame.PixelWidth}×{frame.PixelHeight} 超出 1–4096 像素。");
        }
        catch (Exception e) when (e is not InvalidDataException and not OutOfMemoryException) { throw new InvalidDataException("无法解码这个文件，请换一张 PNG 或 ICO。"); }
    }
    private async Task EditIcon(string id)
    {
        string? path = await ui.InvokeAsync(() => { var picker = new OpenFileDialog { Title = "选择图标", Filter = "图标或图片|*.ico;*.png;*.jpg;*.jpeg;*.bmp", CheckFileExists = true }; return picker.ShowDialog() == true ? picker.FileName : null; }); if (path is null) return;
        // Validate before storing, otherwise an unreadable file looks like the import
        // silently did nothing.
        ValidateIconFile(path);
        lock (customIcons) { if (customIcons.Count >= 256 && !customIcons.ContainsKey(id)) throw new InvalidOperationException("自定义图标已达到数量上限。"); customIcons[id] = path; StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "custom-icons.json"), JsonSerializer.Serialize(customIcons), null); }
        await RefreshBrand(id);
    }
}
