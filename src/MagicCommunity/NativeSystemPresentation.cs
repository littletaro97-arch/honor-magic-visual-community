using System.IO;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MagicCommunity;

// Pure data projection into the existing Composition renderer's contract.
// No backend selection, process startup, UI activation or command execution.
internal static class NativeSystemPresentation
{
    private static readonly object PreviewMotionLock=new();
    private static bool previewMotionRead;
    private static JsonNode? previewMotion;
    private static readonly string[] Sections = ["left", "center", "right"];
    private static readonly Dictionary<string, string> AppearanceNames = new()
    {
        ["enabled"] = "enabled",
        ["edge"] = "position",
        ["autoHide"] = "hideMode",
        ["outerMargin"] = "margin",
        ["innerPadding"] = "padding",
        ["showDelayMs"] = "delayToShow",
        ["hideDelayMs"] = "delayToHide",
        ["itemSize"] = "itemSize",
        ["iconSize"] = "size",
        ["itemGap"] = "spaceBetweenItems",
        ["widthMode"] = "mode",
        ["middleClickAction"] = "middleClickAction",
        ["pinnedVisibility"] = "pinnedItemsVisibility",
        ["runningVisibility"] = "temporalItemsVisibility",
        ["attentionReveal"] = "revealOnAttention",
        ["showEndTask"] = "showEndTask",
        ["showInstanceCount"] = "showInstanceCounter",
        ["showWindowTitle"] = "showWindowTitle",
        ["splitWindows"] = "splitWindows"
    };
    private static readonly Dictionary<string, (string Label, string Icon)> Modules = new()
    {
        // 账户与标识: the cross-brand reading of SeelenUI's "账户" module - the mark,
        // the machine name and the account menu all live behind this one cell.
        ["magic.user"] = ("账户与标识", "FaRegCircleUser"),
        ["magic.power-menu"] = ("电源", "IoPower"),
        ["magic.clock"] = ("时间", "IoTimeOutline"),
        ["magic.tray"] = ("系统托盘", "IoChevronDownCircleOutline"),
        ["magic.bluetooth"] = ("蓝牙", "TbBluetooth"),
        ["magic.keyboard"] = ("输入法", "BsKeyboard"),
        ["magic.battery"] = ("电池", "MagicBatteryNormal10"),
        ["magic.network"] = ("网络", "FaWifi"),
        ["magic.quick-settings"] = ("快捷设置", "LuSettings2"),
        ["magic.notifications"] = ("通知", "IoNotificationsOutline"),
        ["magic.audio"] = ("音量", "IoVolumeHighOutline"),
        ["magic.player"] = ("媒体", "MagicMediaVideo"),
        ["magic.music"] = ("音乐", "MagicMusicNote"),
        ["magic.start"] = ("开始", "BsWindows"),
        ["magic.desktop"] = ("显示桌面", "FaComputer"),
        ["magic.recycle"] = ("回收站", "PiTrashDuotone"),
        ["magic.cpu"] = ("CPU", "LuCpu"),
        ["magic.memory"] = ("内存", "FaMemory"),
        ["magic.gpu"] = ("GPU", "LuCpu"),
        ["magic.storage"] = ("存储", "BsDeviceHdd"),
        ["magic.network-usage"] = ("网速", "FaWifi"),
        ["magic.focused-app"] = ("当前应用", "IoDesktopOutline"),
        ["magic.focused-title"] = ("窗口标题", "IoDesktopOutline")
    };
    private static JsonNode? Copy(JsonElement value) => JsonNode.Parse(value.GetRawText());
    // The settings page lists exactly what the bar menu lists; reading one table keeps
    // the two in step when a module is added or renamed.
    internal static IReadOnlyDictionary<string, (string Label, string Icon)> ModuleCatalog => Modules;
    // "当前应用" and "窗口标题" lead with a mark that separates the name from what comes
    // before them. Which mark is a preference, so it is written into the copy the renderer
    // receives; the stored view keeps describing the row as it was built, which is what
    // lets the choice be switched back afterwards.
    internal static void ApplyFocusedSeparator(JsonObject view, string separator)
    {
        if (separator is not ("bar" or "none")) separator = "desktop";
        if (view["toolbar"] is not JsonObject groups) return;
        foreach (string section in Sections)
        {
            if (groups[section] is not JsonArray rows) continue;
            foreach (var node in rows)
            {
                if (node is not JsonObject row) continue;
                if (row["nativeModule"] is not JsonValue module || module.GetValue<string>() is not ("magic.focused-app" or "magic.focused-title")) continue;
                if (row["content"] is not JsonArray content || content.Count == 0 || content[0] is not JsonObject mark) continue;
                if (mark["kind"] is not JsonValue kind || kind.GetValue<string>() != "icon") continue;
                if (separator == "none") content.RemoveAt(0);
                else mark["value"] = separator == "bar" ? "MagicSeparatorBar" : "IoDesktopOutline";
            }
        }
    }
    private static string Text(JsonElement value, string key, string fallback = "") => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.String ? field.GetString()! : fallback;
    private static bool Flag(JsonElement value, string key, bool fallback = false) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind is JsonValueKind.True or JsonValueKind.False ? field.GetBoolean() : fallback;
    private static double Number(JsonElement value, string key, double fallback = 0) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Number && field.TryGetDouble(out double n) && double.IsFinite(n) ? n : fallback;
    private static JsonElement Object(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Object ? field : default;
    private static JsonElement[] Array(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Array ? field.EnumerateArray().ToArray() : [];
    private static JsonElement Provider(IReadOnlyDictionary<string, JsonElement> providers, string topic) => providers.TryGetValue(topic, out var value) ? value : default;
    private static string Percent(double n) => n.ToString("0", CultureInfo.InvariantCulture) + "%";
    private static string PathKey(string path) => path.Replace('/', '\\').TrimEnd('\\').ToUpperInvariant();

    internal static JsonObject Project(JsonElement snapshot, JsonElement configuration, IReadOnlyDictionary<string, JsonElement>? providers = null, bool interactive = false, int hostPid = 0)
    {
        providers ??= new Dictionary<string, JsonElement>();
        if (Text(snapshot, "protocol") != NativeSystemClient.Protocol || Text(configuration, "protocol") != "Magic.System.Configuration/1" || Number(configuration, "schemaVersion") != 1)
            throw new InvalidDataException("Native presentation protocol or schema differs.");
        var warnings = new JsonArray();
        var windows = Array(snapshot, "windows");
        var monitors = Array(snapshot, "displays");
        var focused = windows.FirstOrDefault(w => Flag(w, "focused"));
        string MonitorId(JsonElement window)
        {
            double x = (Number(window, "left") + Number(window, "right")) / 2, y = (Number(window, "top") + Number(window, "bottom")) / 2;
            return Text(monitors.FirstOrDefault(m => x >= Number(m, "left") && y >= Number(m, "top") && x < Number(m, "left") + Number(m, "width") && y < Number(m, "top") + Number(m, "height")), "id");
        }
        JsonObject Window(JsonElement window) => new()
        {
            ["hwnd"] = ulong.Parse(Text(window, "hwnd"), CultureInfo.InvariantCulture),
            ["nativeWindowId"] = Text(window, "id"),
            ["title"] = Text(window, "title"),
            ["monitor"] = MonitorId(window),
            ["minimized"] = Flag(window, "minimized")
        };
        var toolbar = Object(configuration, "toolbar"); var dock = Object(configuration, "dock");
        string musicOwner=NativeSystemBarDefaults.ToolbarMusicOwner(toolbar);
        var result = new JsonObject
        {
            ["providerProtocol"] = NativeSystemClient.Protocol,
            ["preview"] = !interactive,
            ["projectionOnly"] = !interactive,
            ["productionReady"] = false,
            ["commandDispatcherAttached"] = interactive,
            ["hostPid"] = hostPid,
            ["configurationRevision"] = Number(configuration,"revision"),
            ["windowMoving"] = Flag(snapshot,"windowMoving"),
            ["reorderToolbar"] = !Flag(Object(configuration,"toolbar"),"locked"),
            ["reorderDock"] = !Flag(Object(configuration,"dock"),"locked"),
            ["workspaces"] = new JsonObject(),
            ["toolbarSettings"] = Settings(toolbar),
            ["dockSettings"] = Settings(dock,true),
            ["monitors"] = new JsonArray(),
            ["appWindows"] = new JsonArray(windows.Select(window => (JsonNode)new JsonObject
            {
                ["hwnd"] = ulong.Parse(Text(window, "hwnd"), CultureInfo.InvariantCulture),
                ["monitor"] = MonitorId(window),
                ["isIconic"] = Flag(window, "minimized"),
                ["rect"] = new JsonObject { ["left"] = Number(window, "left"), ["top"] = Number(window, "top"), ["right"] = Number(window, "right"), ["bottom"] = Number(window, "bottom") }
            }).ToArray()),
            ["focused"] = new JsonObject { ["hwnd"] = 0, ["monitor"] = "", ["isFullscreened"] = false },
            ["flyouts"] = new JsonObject { ["enabled"] = interactive },
            ["warnings"] = warnings
        };
        if (focused.ValueKind == JsonValueKind.Object)
        {
            var rect = new JsonObject(); foreach (var edge in new[] { "left", "top", "right", "bottom" }) rect[edge] = Number(focused, edge);
            var focus = Window(focused); focus["rect"] = rect;
            var monitor = monitors.FirstOrDefault(m => Text(m, "id") == MonitorId(focused));
            focus["isFullscreened"] = !Flag(focused, "minimized") && monitor.ValueKind == JsonValueKind.Object &&
                Number(focused, "left") <= Number(monitor, "left") && Number(focused, "top") <= Number(monitor, "top") &&
                Number(focused, "right") >= Number(monitor, "left") + Number(monitor, "width") && Number(focused, "bottom") >= Number(monitor, "top") + Number(monitor, "height");
            result["focused"] = focus;
        }
        foreach (var screen in monitors)
            result["monitors"]!.AsArray().Add(new JsonObject
            {
                ["id"] = Text(screen, "id"),
                ["primary"] = Flag(screen, "primary"),
                ["scale"] = Number(screen, "dpi", 96) / 96,
                ["toolbar"] = Flag(Object(toolbar, "appearance"), "enabled", true),
                ["dock"] = Flag(Object(dock, "appearance"), "enabled", true),
                ["rect"] = new JsonObject { ["left"] = Number(screen, "left"), ["top"] = Number(screen, "top"), ["right"] = Number(screen, "left") + Number(screen, "width"), ["bottom"] = Number(screen, "top") + Number(screen, "height") }
            });
        var brightness=Provider(providers,"brightness");result["brightness"]=brightness.ValueKind==JsonValueKind.Object?Copy(brightness):new JsonObject{["available"]=false,["displays"]=new JsonArray()};
        var preferences = Object(configuration, "preferences");
        foreach (var key in new[] { "language", "dateFormat", "reduceMotion" }) if (preferences.ValueKind == JsonValueKind.Object && preferences.TryGetProperty(key, out var value)) result[key] = Copy(value);
        int windowLinksRemaining = 512;
        JsonArray WindowLinks(IEnumerable<JsonElement> values, string itemId)
        {
            var links = new JsonArray();
            foreach (var window in values)
            {
                if (windowLinksRemaining == 0) { warnings.Add(new JsonObject { ["item"] = itemId, ["reason"] = "window-link-budget-exhausted" }); break; }
                links.Add(Window(window)); --windowLinksRemaining;
            }
            return links;
        }
        var configuredPaths = new HashSet<string>();var configuredWindows=new HashSet<string>(StringComparer.Ordinal);
        var resolved=Array(Provider(providers,"launches"),"items").ToDictionary(x=>Text(x,"id"),x=>Text(x,"target"));
        var pinnedTargets=new HashSet<string>(StringComparer.Ordinal);foreach(var surface in new[]{toolbar,dock})foreach(string section in Sections)foreach(var item in Array(Object(surface,"sections"),section))if(Text(item,"kind")=="application"&&Flag(item,"pinned")){string path=resolved.GetValueOrDefault(Text(item,"id"),Text(Object(item,"launch"),"executable",Text(item,"target")));if(path.Length>0)pinnedTargets.Add(PathKey(path));}
        foreach (var (surfaceName, surface) in new[] { ("toolbar", toolbar), ("dock", dock) })
        {
            var groups = new JsonObject { ["isReorderDisabled"] = Flag(surface, "locked") };
            foreach (var section in Sections)
            {
                var rows = new JsonArray(); groups[section] = rows;
                foreach (var item in Array(Object(surface, "sections"), section))
                {
                    string id = Text(item, "id"), kind = Text(item, "kind");
                    var row = new JsonObject { ["id"] = id, ["key"] = id, ["nativeSurface"]=surfaceName, ["enabled"] = interactive, ["actionAvailable"] = interactive };
                    if (kind == "application")
                    {
                        string target = Text(item, "target"),executable=resolved.GetValueOrDefault(id,Text(Object(item,"launch"),"executable",target));
                        var matching=windows.Where(w=>executable.Length!=0&&(PathKey(Text(w,"executable"))==PathKey(executable)||Text(item,"aumid").Length>0&&Text(item,"aumid")==Text(w,"aumid"))).ToArray();
                        if(!Flag(item,"pinned")&&(matching.Length==0||pinnedTargets.Contains(PathKey(executable))))continue;
                        foreach(var window in matching)configuredWindows.Add(Text(window,"id"));if(executable.Length!=0)configuredPaths.Add(PathKey(executable));
                        row["icon"] = "native:application"; row["type"] = "AppOrFile"; row["path"] = target; row["iconExecutable"] = executable; row["umid"] = Text(item, "aumid"); row["label"] = Text(item, "displayName");
                        row["pinned"] = Flag(item, "pinned"); row["preventPinning"] = !Flag(item, "allowPin", true);
                        row["windows"] = WindowLinks(matching, id);
                        if (item.TryGetProperty("launch", out var launch)) row["nativeLaunch"] = Copy(launch);
                    }
                    else if (kind == "separator") row["type"] = "Separator";
                    else if (kind == "module")
                    {
                        string module = Text(item, "module");
                        if(surfaceName=="toolbar"&&NativeSystemBarDefaults.IsToolbarMusic(module)){
                            if(id!=musicOwner)continue; module="magic.music";
                        }
                        row["nativeModule"] = module; row["type"] = "Plugin"; row["plugin"] = module switch { "magic.start"=>"@default/weg-start-menu", "magic.desktop"=>"@default/weg-show-desktop", "magic.recycle"=>"@default/weg-trash-bin", "magic.tray"=>"@magic/dock-system-tray", "magic.clock"=>"@magic/dock-calendar-popup", "magic.audio"=>"@magic/dock-media-popup", "magic.bluetooth"=>"@magic/dock-bluetooth-popup", "magic.keyboard"=>"@magic/dock-keyboard-selector", "magic.notifications"=>"@magic/dock-notifications", "magic.network"=>"@magic/dock-network-popup", "magic.power-menu" or "magic.battery"=>"@default/weg-power", _=>module };
                        var details = Modules.GetValueOrDefault(module, (module, "TbDeviceUnknown"));
                        row["label"] = details.Item1; row["content"] = new JsonArray(new JsonObject { ["kind"] = "icon", ["value"] = details.Item2 });
                        bool available = FillModule(module, row, snapshot, focused, providers);
                        row["dataAvailable"] = available;
                        if (!available) warnings.Add(new JsonObject { ["item"] = id, ["module"] = module, ["reason"] = "provider-not-connected" });
                    }
                    else if (kind == "compatibility")
                    {
                        row["type"] = "Plugin"; row["plugin"] = Text(item, "sourceModuleId"); row["label"] = "兼容组件"; row["compatibilityMetadataOnly"] = true;
                        warnings.Add(new JsonObject { ["item"] = id, ["reason"] = "compatibility-execution-deferred" });
                    }
                    else throw new InvalidDataException("Unknown presentation item kind.");
                    bool rowAdded=false;
                    if(interactive && surfaceName=="toolbar" && kind=="module" && Text(item,"module")=="magic.tray")
                    {
                        row["module"]=id;
                        // The tray is one group whose button sits at its outer edge, so in the left
                        // group the block is mirrored: the button comes first and the icons follow
                        // in reverse. The renderer suppresses its own mirror while a drag is in
                        // flight, so both orders have to describe the same arrangement here or the
                        // button visibly jumps sides the moment the drop republishes.
                        var promoted=Array(Provider(providers,"tray"),"icons").Where(x=>Flag(x,"promoted")).ToArray();
                        bool mirrored=section=="left";
                        if(mirrored){rows.Add(row);rowAdded=true;}
                        IEnumerable<JsonElement> order=mirrored?promoted.Reverse():promoted;
                        foreach(var icon in order)
                        {
                            string trayId="tray:"+Text(icon,"id");
                            rows.Add(new JsonObject{["id"]=trayId,["key"]=trayId,["type"]="Tray",["module"]=id,["nativeTrayId"]=Text(icon,"id"),["stableId"]=Text(icon,"stableId"),["imageVersion"]=Text(icon,"imageVersion"),["label"]=Text(icon,"tooltip"),["tooltip"]=Text(icon,"tooltip"),["icon"]="native:tray",["path"]=Text(icon,"executable"),["ownerHwnd"]=ulong.Parse(Text(icon,"ownerHwnd"),CultureInfo.InvariantCulture),["enabled"]=Flag(icon,"actionAvailable")});
                        }
                    }
                    if(!rowAdded)rows.Add(row);
                }
            }
            result[surfaceName] = groups;
        }
        // Keep independent temporal entries when no executable is readable; do not group all protected windows into one empty-path item.
        foreach (var group in windows.Where(w => !configuredWindows.Contains(Text(w,"id"))).GroupBy(w => Text(w, "executable").Length == 0 ? Text(w, "id") : PathKey(Text(w, "executable"))))
        {
            var first = group.First(); string path = Text(first, "executable"), id = "native-running:" + Text(first, "id");
            result["dock"]!["center"]!.AsArray().Add(new JsonObject { ["id"] = id, ["key"] = id, ["type"] = "AppOrFile", ["icon"] = "native:application", ["path"] = path, ["label"] = Text(first, "title"), ["pinned"] = false, ["enabled"] = interactive, ["actionAvailable"] = interactive, ["windows"] = WindowLinks(group, id) });
        }
        if(interactive){result["dockSettings"]!["showEndTask"]=false;if(Flag(snapshot,"uacPending"))result["dock"]!["right"]!.AsArray().Add(new JsonObject{["id"]="native:authorization",["key"]="native:authorization",["type"]="Uac",["label"]="Windows 授权请求",["enabled"]=true});}
        var catalog=new JsonArray();foreach(var (name,details) in Modules)catalog.Add(new JsonObject{["id"]=name,["label"]=details.Label,["icon"]=details.Icon});
        result["toolbarModules"]=new JsonArray(catalog.Where(v=>v!["id"]!.GetValue<string>()!="magic.player").Select(v=>v!.DeepClone()).ToArray());result["dockModules"]=new JsonArray(catalog.Where(v=>v!["id"]!.GetValue<string>()!="magic.music").Select(v=>v!.DeepClone()).ToArray());
        // This package carries no package identity of its own on some machines, so the
        // listener answers "allowed" only when Windows granted the independent identity;
        // plain "available" was true even while every read came back empty.
        var notifications=Provider(providers,"notifications");result["notificationAccess"]=Flag(notifications,"independentIdentity")&&Text(notifications,"access")=="allowed";
        // A toast the sender withdrew must not be re-announced once its ghost expires.
        var latest=Flag(notifications,"announceLatest")?Array(notifications,"notifications").OrderByDescending(n=>Text(n,"createdFileTime")).FirstOrDefault():default;
        if(latest.ValueKind==JsonValueKind.Object)result["notification"]=new JsonObject{["key"]=Text(latest,"id"),["title"]=Text(latest,"appName"),["detail"]=string.Join("\n",Array(latest,"text").Select(n=>n.GetString()??""))};
        var audio = Provider(providers, "audio"); if (Flag(audio, "available")) result["volume"] = Number(audio, "volume");
        return result;
    }
    private static JsonObject Settings(JsonElement surface,bool dock=false)
    {
        var result = new JsonObject { ["isReorderDisabled"] = Flag(surface, "locked") };
        if(dock)lock(PreviewMotionLock){
            // Hidden tuning is sampled once per adapter lifetime. Later config
            // snapshots cannot alter the Dock settings signature through it.
            if(!previewMotionRead){var motion=Object(surface,"previewMotion");previewMotion=motion.ValueKind==JsonValueKind.Object?Copy(motion):null;previewMotionRead=true;}
            if(previewMotion is not null)result["previewMotion"]=previewMotion.DeepClone();
        }
        var appearance = Object(surface, "appearance");
        if (appearance.ValueKind == JsonValueKind.Object)
            foreach (var property in appearance.EnumerateObject())
                if (AppearanceNames.TryGetValue(property.Name, out var name)) result[name] = Copy(property.Value);
                else throw new InvalidDataException("Unknown native appearance key.");
        return result;
    }
    private static bool FillModule(string module, JsonObject row, JsonElement snapshot, JsonElement focused, IReadOnlyDictionary<string, JsonElement> providers)
    {
        var system = Object(snapshot, "system"); var audio = Provider(providers, "audio"); var input = Provider(providers, "input");
        string? label = null; bool available = false;
        switch (module)
        {
            case "magic.tray": available=Flag(Provider(providers,"tray"),"available"); break;
            case "magic.user": case "magic.power-menu": case "magic.quick-settings": case "magic.start": case "magic.desktop": case "magic.recycle": available=true; break;
            case "magic.clock": row["clock"] = true; row["dockClock"] = true; available = true; break;
            case "magic.memory": if (system.ValueKind == JsonValueKind.Object) { label = Percent(Number(system, "memoryLoadPercent")); available = true; } break;
            case "magic.cpu": if (system.ValueKind == JsonValueKind.Object && system.TryGetProperty("cpuPercent", out var cpu) && cpu.ValueKind == JsonValueKind.Number) { label = Percent(cpu.GetDouble()); available = true; } break;
            case "magic.gpu":var gpu=Object(Object(system,"metrics"),"gpu");if(Flag(gpu,"available")){label=Percent(Number(gpu,"percent"));available=true;}break;
            case "magic.storage":var disks=Array(Object(system,"metrics"),"storage");if(disks.Length>0){double total=disks.Sum(x=>Number(x,"totalBytes")),free=disks.Sum(x=>Number(x,"availableBytes"));label=total>0?Percent((1-free/total)*100):"—";available=total>0;}break;
            case "magic.network-usage":if(system.ValueKind==JsonValueKind.Object&&system.TryGetProperty("receivedBytesPerSecond",out _)){label="↓ "+TransferRate(Number(system,"receivedBytesPerSecond"))+" ↑ "+TransferRate(Number(system,"sentBytesPerSecond"));available=true;}break;
            case "magic.battery": var power = Object(system, "power"); if (power.ValueKind == JsonValueKind.Object && power.TryGetProperty("batteryPercent", out var battery) && battery.ValueKind == JsonValueKind.Number) { label = Percent(battery.GetDouble()); row["batteryPercent"]=battery.GetDouble(); row["acLineStatus"] = Number(power, "acLineStatus"); available = true; } break;
            case "magic.audio": if (Flag(audio, "available")) { label = Percent(Number(audio, "volume") * 100); row["dockVolume"] = Number(audio, "volume"); row["dockMuted"] = Flag(audio, "muted"); available = true; } break;
            case "magic.keyboard": var selected = Array(input, "layouts").FirstOrDefault(x => Flag(x, "selected")); if (selected.ValueKind == JsonValueKind.Object) { label = Text(selected, "locale");if(label.StartsWith("zh",StringComparison.OrdinalIgnoreCase)&&Flag(input,"imeAvailable"))label=Flag(input,"imeOpen")&&Flag(input,"imeNative")?"中":"英";else label=label.Split('-')[0].ToUpperInvariant(); row["dockLanguage"] = label; available = true; } break;
            case "magic.focused-title": label = Text(focused, "title"); available = focused.ValueKind == JsonValueKind.Object; row["inline"] = true; break;
            case "magic.focused-app": label = Path.GetFileNameWithoutExtension(Text(focused, "executable")); available = focused.ValueKind == JsonValueKind.Object; break;
            case "magic.bluetooth": var bluetooth=Provider(providers,"bluetooth"); available=Flag(bluetooth,"available");row["bluetoothOn"]=Flag(bluetooth,"radioOn");row["dockConnected"]=Array(bluetooth,"devices").Any(x=>Flag(x,"connected")); break;
            case "magic.network": var wifi=Provider(providers,"wifi");available=Flag(wifi,"available");var interfaces=Array(wifi,"interfaces");row["networkStateKnown"]=interfaces.Length>0;row["dockOnline"]=interfaces.Any(x=>Number(x,"state")==1);row["dockWifi"]=true;var connected=interfaces.SelectMany(x=>Array(x,"networks")).FirstOrDefault(x=>Flag(x,"connected"));row["wifiSignal"]=Number(connected,"signal",100);break;
            case "magic.notifications": var notifications=Provider(providers,"notifications");available=Flag(notifications,"available");row["hasNotifications"]=Array(notifications,"notifications").Length>0;break;
            case "magic.music": case "magic.player": var media=Provider(providers,"media");available=Flag(media,"available");var player=CurrentMedia(media);row["type"]=module=="magic.music"?"Music":"Media";row["lyricsText"]="";row["hasPlayer"]=player.ValueKind==JsonValueKind.Object;if(player.ValueKind==JsonValueKind.Object){row["nativeSessionId"]=Text(player,"id");row["nativeTrackId"]=Text(player,"trackId");foreach(string capability in new[]{"canPlay","canPause","canPrevious","canNext"})row[capability]=Flag(player,capability);row["label"]=Text(player,"title");row["artist"]=Text(player,"artist");row["playing"]=Number(player,"status")==4;}break;
        }
        if (label is not null) { row["label"] = label; row["content"]!.AsArray().Add(new JsonObject { ["kind"] = "text", ["value"] = label }); }
        return available;
    }
    internal static JsonElement CurrentMedia(JsonElement media) => Array(media,"sessions")
        .OrderByDescending(s=>Flag(s,"current")).ThenByDescending(s=>Number(s,"status")==4).FirstOrDefault();
    internal static string TransferRate(double bytes){
        if(!double.IsFinite(bytes)||bytes<0)bytes=0;
        string[] units=["KB/s","MB/s","GB/s","TB/s","PB/s","EB/s"];
        double value=bytes/1024;int unit=0;while(value>=1024&&unit<units.Length-1){value/=1024;unit++;}
        return value.ToString(unit==0?"0":"0.0",CultureInfo.InvariantCulture)+" "+units[unit];
    }
}
