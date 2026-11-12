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
    private static readonly HashSet<string> SettingsUris = new(StringComparer.Ordinal) { "ms-settings:", "ms-settings:bluetooth", "ms-settings:network-wifi", "ms-settings:sound", "ms-settings:powersleep", "ms-settings:notifications", "ms-settings:regionlanguage", "ms-settings:dateandtime", "ms-settings:privacy-notifications", "ms-settings:display", "ms-settings:nightlight" };
    private async Task LocalCommand(Command action, JsonElement result, string token)
    {
        var args = JsonSerializer.SerializeToElement(action.Arguments);
        switch (action.Local)
        {
            case "music-pin": case "music-reorder":
                string destination=Text(result,"destination");
                if(action.Local=="music-reorder"&&!destination.StartsWith("music-source:",StringComparison.Ordinal))break;
                music.Change(Text(args,"sessionId"),action.Local=="music-pin"?null:destination[13..]);
                QueueExtensionProjection();break;
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
            case "settings-uri": string uri = Text(args, "uri"); if (!SettingsUris.Contains(uri)) throw new InvalidDataException("Settings destination differs."); OpenUri(uri); break;
            case "panel": await popups.Close(stop.Token); await Open(Text(args, "kind"), args); break;
            case "appearance-theme":
                await Task.Run(()=>NativeAppearanceSettings.SetDark(Flag(args,"dark")),stop.Token);
                var themePanel=NativeSystemPanel.Build("@magic/quick-settings",presentation.State,notificationQuiet:notificationQuiet,displayView:true);themePanel.Data["hostPid"]=Environment.ProcessId;
                lock(currentGate){if(currentToken!=token)break;currentPanel=themePanel;}
                await popups.Update(JsonSerializer.SerializeToElement(new{token,data=themePanel.Data}),stop.Token);break;
            case "honor-power": HonorPcManager.RequireSignedIn(); OpenUri(HonorPcManager.Find() ?? throw new InvalidOperationException("未找到荣耀电脑管家。")); break;
            case "tray":
                int button = (int)Number(result, "button");
                if (button is 4 or 5) { await TrayPreference(args, result, token); break; }
                // The native tray stays open across activation and double clicks.
                // Capture coordinates at the gesture, not after an async dispatch.
                GetCursorPos(out var point); await TrayAction(Text(args, "iconId"), button == 2 ? "context" : button == 3 ? "double-click" : button == 1 ? "middle" : "activate", result.TryGetProperty("x",out _)?Number(result,"x"):point.X, result.TryGetProperty("y",out _)?Number(result,"y"):point.Y); break;
            case "open-application": await popups.Close(stop.Token); await OpenNotificationApplication(Text(args,"aumid")); break;
            case "notifications-authorize": await AuthorizeNotifications(); break;
            case "bluetooth-refresh": bluetoothDetails.Invalidate(); bluetoothDetails.Show(token); break;
            case "wifi-auth": OpenUri(NativeNetworkPortal.AuthenticationUri);break;
            case "wifi-connect": await ConnectWifi(args, token); break;
            case "app-launch": await popups.Close(stop.Token); Launch(args, false); break;
            default: throw new InvalidDataException("Local action is not offered.");
        }
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
    private readonly SemaphoreSlim applicationActivation = new(1,1);
    private async Task OpenNotificationApplication(string aumid)
    {
        // Repeated clicks must not toggle a tray-owned window open and closed.
        if(!await applicationActivation.WaitAsync(0,stop.Token))return;
        try { await OpenNotificationApplicationCore(aumid); }
        finally { applicationActivation.Release(); }
    }
    private async Task OpenNotificationApplicationCore(string aumid)
    {
        var state=presentation.State;
        var resolved=Array(state.Providers.GetValueOrDefault("launches"),"items").ToDictionary(item=>Text(item,"id"),item=>Text(item,"target"));
        var targets=new List<string>();
        foreach(string surface in new[]{"toolbar","dock"})foreach(string section in new[]{"left","center","right"})
            foreach(var item in Array(Object(Object(state.Configuration,surface),"sections"),section))
                if(Text(item,"aumid").Equals(aumid,StringComparison.OrdinalIgnoreCase))targets.Add(resolved.GetValueOrDefault(Text(item,"id"),Text(Object(item,"launch"),"executable",Text(item,"target"))));
        targets.AddRange(applicationEntries.Where(entry=>entry.Aumid.Equals(aumid,StringComparison.OrdinalIgnoreCase)).Select(entry=>entry.Target));
        var snapshot=await backend.Request("snapshot",new{},stop.Token);
        string launchId = aumid;
        if (notificationWindows.Find(aumid,snapshot,targets,NativeNotificationWindows.WindowAppId).ValueKind != JsonValueKind.Object)
        {
            // A tray-resident app is already installed and running. Resolve its
            // live instance before an AppsFolder lookup can fail or launch it.
            if(NativeNotificationWindows.CanRestoreResident(aumid,targets)&&await ActivateResidentApplication(aumid,targets))return;
            var registered = await RegisteredApplication.ResolveAsync(aumid, stop.Token);
            launchId = registered.Aumid;
            if (registered.Executable.Length > 0) targets.Add(registered.Executable);
        }
        await notificationWindows.Open(aumid,snapshot,targets,window=>PerformWindowAction(JsonSerializer.SerializeToElement(new{nativeWindowId=Text(window,"id"),hwnd=long.Parse(Text(window,"hwnd"),System.Globalization.CultureInfo.InvariantCulture)}),"activate"),_=>OpenApplication(launchId),NativeNotificationWindows.WindowAppId,()=>ActivateResidentApplication(aumid,targets));
    }
    private async Task<bool> ActivateResidentApplication(string appId,IEnumerable<string> targets)
    {
        var paths=NativeNotificationWindows.ResidentPaths(targets);
        // Process existence alone does not imply a tray-resident application:
        // Explorer, for example, owns the desktop even with no folder open.
        // Use the captured tray identity, whose PID/creation time is rechecked
        // by TrayAction, rather than blocking every background process.
        var tray=await backend.Request("tray-read",new{},stop.Token);
        var ids=NativeNotificationWindows.ResidentTrayIds(appId,tray,paths);
        if(ids.Length==0){
            // Preserve the existing QQ guard when capture has not seen its
            // icon yet. The generic path above is based on real tray identity;
            // process existence is not a universal single-instance contract.
            if(NativeNotificationWindows.IsQq(appId,paths)&&QqProcessRunning())
                throw new InvalidOperationException("QQ 正在运行。请从系统托盘打开对应的 QQ 账号。");
            return false;
        }
        string? id=ids.Length==1?ids[0]:null;
        if(id is not null){
            var snapshot=await backend.Request("snapshot",new{},stop.Token);
            var existing=notificationWindows.Find(appId,snapshot,paths,NativeNotificationWindows.WindowAppId);
            if(existing.ValueKind==JsonValueKind.Object){
                await PerformWindowAction(JsonSerializer.SerializeToElement(new{nativeWindowId=Text(existing,"id"),hwnd=long.Parse(Text(existing,"hwnd"),System.Globalization.CultureInfo.InvariantCulture)}),"activate");
                return true;
            }
            GetCursorPos(out var point);
            var target=await NativeNotificationWindows.RestoreTrayWindow(
                action=>TrayAction(id,action,point.X,point.Y),
                ()=>notificationWindows.WaitForWindow(appId,paths,
                    ()=>backend.Request("snapshot",new{},stop.Token),
                    ms=>Task.Delay(ms,stop.Token),NativeNotificationWindows.WindowAppId));
            if(target.ValueKind==JsonValueKind.Object){
                await PerformWindowAction(JsonSerializer.SerializeToElement(new{nativeWindowId=Text(target,"id"),hwnd=long.Parse(Text(target,"hwnd"),System.Globalization.CultureInfo.InvariantCulture)}),"activate");
                return true;
            }
            log("notification_resident_window_missing",new{application=appId});
        }
        throw new InvalidOperationException("应用正在运行。请从系统托盘打开对应的已有窗口。");
    }
    private static bool QqProcessRunning()
    {
        using var current=Process.GetCurrentProcess();var processes=Process.GetProcessesByName("QQ");
        try{
            foreach(var process in processes){
                try{if(process.SessionId==current.SessionId)return true;}
                catch(InvalidOperationException){/* Process exited during the snapshot. */}
            }
            return false;
        }finally{foreach(var process in processes)process.Dispose();}
    }
    private async Task OpenDockApplication(JsonElement item)
    {
        string appId=Text(item,"umid",Text(item,"aumid"));
        string[] targets=[Text(Object(item,"nativeLaunch"),"executable"),Text(item,"iconExecutable"),Text(item,"path",Text(item,"target"))];
        var launch=Object(item,"nativeLaunch");
        // File/folder links and explicit arguments retain their launch intent
        // (e.g. an Explorer location or a browser profile). A background process
        // without a matching tray icon is not evidence of a resident instance.
        var arguments=launch.ValueKind==JsonValueKind.Object&&launch.TryGetProperty("arguments",out var args)?args:default;
        if(!NativeNotificationWindows.ResidentDockLaunch(Text(launch,"executable",Text(item,"path",Text(item,"target"))),arguments)||!NativeNotificationWindows.CanRestoreResident(appId,targets)){Launch(item,false);return;}
        // Dock and notification clicks share the in-flight guard, so two entry
        // points cannot toggle a tray window twice or race a cold launch.
        if(!await applicationActivation.WaitAsync(0,stop.Token))return;
        try{
            if(!await ActivateResidentApplication(appId,targets))Launch(item,false);
        }finally{applicationActivation.Release();}
    }
    private async Task DispatchDockApplication(JsonElement item)
    {
        try{await OpenDockApplication(item);}
        catch(Exception error)when(error is not OutOfMemoryException){await Notice(error);}
    }
    private static void Launch(JsonElement item, bool administrator)
    {
        string aumid = Text(item, "umid", Text(item, "aumid")); var launch = Object(item, "nativeLaunch"); string target = Text(launch, "executable", Text(item, "path", Text(item, "target")));
        if ((target.Length == 0 || !File.Exists(target) && !Directory.Exists(target)) && aumid.Length > 0) { if (administrator) throw new InvalidOperationException("此打包应用不支持此管理员启动方式。"); OpenApplication(aumid); return; }
        if (!Path.IsPathFullyQualified(target) || target.StartsWith(@"\\", StringComparison.Ordinal) || (!File.Exists(target) && !Directory.Exists(target))) throw new FileNotFoundException("程序或文件已移动。", target);
        var start = new ProcessStartInfo(target) { UseShellExecute = true }; if (administrator) start.Verb = "runas";
        string working = Text(launch, "workingDirectory"); if (working.Length > 0 && Directory.Exists(working)) start.WorkingDirectory = working;
        else if(Path.GetExtension(target).Equals(".exe",StringComparison.OrdinalIgnoreCase))start.WorkingDirectory=Path.GetDirectoryName(target)!;
        if (launch.ValueKind == JsonValueKind.Object && launch.TryGetProperty("arguments", out var arguments)) { if (arguments.ValueKind == JsonValueKind.Array) foreach (var argument in arguments.EnumerateArray()) start.ArgumentList.Add(argument.GetString() ?? ""); else if (arguments.ValueKind == JsonValueKind.String) start.Arguments = arguments.GetString() ?? ""; }
        Process.Start(start)?.Dispose();
    }
    private async Task ApplicationCommand(JsonElement item, string verb)
    {
        if (verb == "launch") { _=DispatchDockApplication(item); return; }
        if (verb == "new-instance") { Launch(item, false); return; }
        if (verb is "runas" or "run-as") { Launch(item, true); return; }
        if (verb == "open-location")
        {
            string path = Text(item, "location", Text(item, "path")); if (!Path.IsPathFullyQualified(path)) throw new InvalidOperationException("此应用没有可打开的本地位置。");
            if (!File.Exists(path) && !Directory.Exists(path)) throw new FileNotFoundException("应用所在位置已变化，请重新打开开始菜单。", path);
            await popups.Close(stop.Token);
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
                var catalog = await backend.Request("configuration-catalog", new { }, stop.Token); if (!Array(catalog, "modules").Any(m => m.ValueKind == JsonValueKind.String ? m.GetString() == module : Text(m, "id") == module)) throw new InvalidDataException("模块不在内置目录中。");
                bool removed = false; foreach (string group in new[] { "left", "center", "right" }) { var list = sections[group]!.AsArray(); for (int n = list.Count - 1; n >= 0; --n) if (list[n]!["module"]?.GetValue<string>() == module) { list.RemoveAt(n); removed = true; } }
                if (!removed) sections[surface == "toolbar" ? "right" : "center"]!.AsArray().Add(new JsonObject { ["id"] = surface + ":" + module, ["kind"] = "module", ["module"] = module });
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
            else throw new InvalidOperationException("此菜单动作尚未连接独立配置。");
            await SaveConfiguration(config); if (token.Length > 0) { lock (orderReceipts) { orderReceipts.Add(new() { ["token"] = token, ["ok"] = true,["revision"]=config["revision"]!.GetValue<long>() }); while (orderReceipts.Count > 32) orderReceipts.RemoveAt(0); } }
        }
        catch (Exception error) { if (token.Length > 0) { lock (orderReceipts) { orderReceipts.Add(new() { ["token"] = token, ["ok"] = false, ["error"] = error.Message }); while (orderReceipts.Count > 32) orderReceipts.RemoveAt(0); } } throw; }
        finally { configurationGate.Release(); }
    }
    private async Task SaveConfiguration(JsonObject config)
    {
        long prior = config["revision"]!.GetValue<long>(); config["revision"] = prior + 1;
        await backend.Request("configuration-write", new { expectedRevision = prior, configuration = config }, stop.Token);
    }
    private async Task EditIcon(string id)
    {
        string? path = await ui.InvokeAsync(() => { var picker = new OpenFileDialog { Title = "选择图标", Filter = "图标或图片|*.ico;*.png;*.jpg;*.jpeg;*.webp;*.exe;*.lnk", CheckFileExists = true }; return picker.ShowDialog() == true ? picker.FileName : null; }); if (path is null) return;
        lock (customIcons) { if (customIcons.Count >= 256 && !customIcons.ContainsKey(id)) throw new InvalidOperationException("自定义图标已达到数量上限。"); customIcons[id] = path; StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "custom-icons.json"), JsonSerializer.Serialize(customIcons), null); }
        await send(new { type = "icons-invalidated", keys = new[] { id } });
    }
}
