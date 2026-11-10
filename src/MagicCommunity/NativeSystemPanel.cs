using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MagicCommunity;

// Renderer rows carry opaque offered command IDs. Windows identities and action
// arguments remain in this adapter; click payloads cannot choose HWNDs or APIs.
internal sealed class NativeSystemPanel
{
    internal sealed record Command(string Operation, JsonObject Arguments, string Local = "", string ValueField = "", bool BooleanValue = false);
    internal JsonObject Data { get; } = new() { ["rows"] = new JsonArray() };
    internal Dictionary<string, Command> Commands { get; } = new(StringComparer.Ordinal);
    private JsonArray Rows => Data["rows"]!.AsArray();
    internal void Add(string id, string label, string detail = "", Command? command = null, string control = "button", bool active = false, double value = 0, string? image = null, string labelEn = "", bool? enabledOverride = null)
    {
        if (Rows.Count >= 192) return;
        if(control=="slider")control="range"; if(control=="radio")control="button"; if(control=="toggle")value=active?1:0;
        if (label.Length > 256) label = label[..256]; if (detail.Length > 512) detail = detail[..512]; if (labelEn.Length > 256) labelEn = labelEn[..256];
        var row = new JsonObject { ["id"] = id, ["label"] = label, ["detail"] = detail, ["enabled"] = enabledOverride ?? (command is not null), ["control"] = control, ["active"] = active, ["value"] = value, ["image"] = image ?? "" };
        // An English label rides beside the Chinese one; the renderer reads it only
        // when the popup language is English. Absent labels are omitted so the other
        // panels keep their payload size.
        if (labelEn.Length > 0) row["labelEn"] = labelEn;
        Rows.Add(row);
        if (command is not null) Commands.Add(id, command);
    }
    internal static string Text(JsonElement value, string key, string fallback = "") => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.String ? field.GetString() ?? fallback : fallback;
    internal static bool Flag(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.True;
    internal static double Number(JsonElement value, string key, double fallback = 0) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Number && field.TryGetDouble(out double n) && double.IsFinite(n) ? n : fallback;
    internal static JsonElement[] Array(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Array ? field.EnumerateArray().ToArray() : [];
    internal static JsonElement Object(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var field) && field.ValueKind == JsonValueKind.Object ? field : default;
    internal static Command Backend(string operation, object arguments, string valueField = "", bool boolean = false) => new(operation, JsonSerializer.SerializeToNode(arguments)!.AsObject(), ValueField: valueField, BooleanValue: boolean);
    internal static Command Local(string action, object? arguments = null) => new("", arguments is null ? new() : JsonSerializer.SerializeToNode(arguments)!.AsObject(), action);
    // The ten doors MyMagicBar's MenuSettings carried, in its order and with its
    // targets. Each is optional and all default to off, which is what keeps the card
    // to a couple of rows until the reader asks for more.
    internal static IEnumerable<(string Id, string Label, string Target)> AccountEntries(CommunityPreferences card)
    {
        if (card.AccountEntrySystem) yield return ("account:system", "系统设置", "ms-settings:");
        if (card.AccountEntryTaskManager) yield return ("account:taskmgr", "任务管理器", "taskmgr");
        if (card.AccountEntryDisplay) yield return ("account:display", "显示设置", "ms-settings:display");
        if (card.AccountEntrySound) yield return ("account:sound", "声音设置", "ms-settings:sound");
        if (card.AccountEntryNetwork) yield return ("account:network", "网络设置", "ms-settings:network");
        if (card.AccountEntryPower) yield return ("account:power", "电源设置", "ms-settings:powersleep");
        if (card.AccountEntryBluetooth) yield return ("account:bluetooth", "蓝牙设置", "ms-settings:bluetooth");
        if (card.AccountEntryNotification) yield return ("account:notify", "通知设置", "ms-settings:notifications");
        if (card.AccountEntryLanguage) yield return ("account:language", "语言设置", "ms-settings:regionlanguage");
        if (card.AccountEntryLocation) yield return ("account:location", "位置隐私", "ms-settings:privacy-location");
    }
    internal static NativeSystemPanel Build(string kind, NativeSystemPresentationSession.Published state, bool powerView = false, bool notificationQuiet = false, bool displayView = false, CommunityPreferences? accountCard = null)
    {
        kind=NativePopupIds.Canonical(kind);
        var panel = new NativeSystemPanel();
        panel.Data["title"]=kind switch{"@magic/system-tray"=>"系统托盘","@magic/keyboard-selector"=>"键盘布局","@magic/calendar-popup"=>"日历","@magic/user-menu"=>"用户菜单","@magic/notifications"=>"通知中心","@magic/bluetooth-popup"=>"蓝牙","@magic/network-popup"=>"网络","@magic/media-popup"=>"音量与媒体","@magic/quick-settings"=>"快捷设置","@magic/power-menu"=>"电源","@magic/task-switcher"=>"窗口切换",_=>"系统面板"};
        JsonElement Provider(string name) => state.Providers.GetValueOrDefault(name);
        void Setting(string id, string label, string uri) => panel.Add(id, label, command: Local("settings-uri", new { uri }));
        var audio = Provider("audio");
        void Audio()
        {
            string device = Text(audio, "defaultDeviceId");
            if (Flag(audio, "available"))
            {
                panel.Add("volume:" + device, "音量", Text(audio, "defaultDeviceName"), Backend("audio-action", new { action = "set-volume", deviceId = device }, "value"), "slider", value: Number(audio, "volume"));
                panel.Add("mute:" + device, "静音", command: Backend("audio-action", new { action = "set-mute", deviceId = device }, "value", true), control: "toggle", active: Flag(audio, "muted"));
                foreach (var item in Array(audio, "devices")) panel.Add("default:" + Text(item, "id"), Text(item, "name"), Text(item, "id") == device ? "当前输出设备" : "", Flag(audio, "canSelectDefault") ? Backend("audio-default-device", new { deviceId = Text(item, "id") }) : null, "radio", Text(item, "id") == device);
            }
            else panel.Add("audio-unavailable", "音频设备暂不可用");
        }
        void Brightness()
        {
            var brightness=Provider("brightness");var displays=Array(brightness,"displays").OrderByDescending(d=>Flag(d,"primary")).ThenBy(d=>Text(d,"monitorId"),StringComparer.Ordinal).ToArray();
            int secondary=0,secondaryCount=displays.Count(d=>!Flag(d,"primary"));
            foreach(var display in displays){
                string id=Text(display,"id");if(id.Length==0)continue;
                bool primary=Flag(display,"primary")||!display.TryGetProperty("primary",out _)&&displays.Length==1;
                string name=primary?"亮度-主屏幕":"亮度-副屏"+(secondaryCount>1?(++secondary).ToString():"");
                double inches=Number(display,"diagonalInches");int w=(int)Number(display,"widthPx"),h=(int)Number(display,"heightPx");
                if(!primary)name+=inches is >0 and <=150?"（约"+inches.ToString("0.#",System.Globalization.CultureInfo.InvariantCulture)+" 英寸）":w>0&&h>0?$"（{w}×{h}）":"";
                double level=Number(display,"value",double.NaN);bool available=double.IsFinite(level)&&level>=0&&level<=1&&(display.TryGetProperty("available",out _)?Flag(display,"available"):Flag(brightness,"available"));
                panel.Add("brightness:"+id,name,available?"":"此屏幕未提供可用硬件亮度接口",available?Backend("brightness-action",new{displayId=id},"value"):null,"range",value:available?level:0);
            }
            if(!displays.Any())panel.Add("brightness-unavailable","亮度调节暂不可用","此显示器未提供受支持的亮度接口；可打开显示设置调整");
        }
        switch (kind)
        {
            case "@magic/system-tray":
                foreach (var icon in Array(Provider("tray"), "icons")) { panel.Add("tray:" + Text(icon, "id"), Text(icon, "tooltip"), command: Local("tray", new { iconId = Text(icon, "id"), stableId = Text(icon, "stableId") }), control: "tray", active: Flag(icon, "promoted"), image: "native-tray"); var row=panel.Data["rows"]!.AsArray().Last()!; row["imageVersion"] = Text(icon, "imageVersion"); row["ownerHwnd"]=ulong.TryParse(Text(icon,"ownerHwnd"),out var hwnd)?hwnd:0; }
                break;
            case "@magic/keyboard-selector":
                panel.Data.Remove("rows"); var layouts = new JsonArray(); foreach (var item in Array(Provider("input"), "layouts")) layouts.Add(new JsonObject { ["id"] = Text(item, "id"), ["handle"] = Text(item, "id"), ["language"] = Text(item, "name"), ["name"] = Text(item, "locale"), ["active"] = Flag(item, "selected") }); panel.Data["layouts"] = layouts; break;
            case "@magic/calendar-popup": panel.Data.Remove("rows"); break;
            case "@magic/user-menu":
                {
                    // The whole account card, in the order MyMagicBar carried it: the
                    // account door, the system doors the reader switched on, the
                    // application group, the session group, and one row that opens the
                    // tuning panel. The session rows carry no command table entry on
                    // purpose - the renderer emits the action name itself and
                    // NativeSystemUiSession.FinalPopup owns it, so a stale command
                    // mapping can never reach PanelCommand. No Remove("rows"): Rows is
                    // Data["rows"]! unguarded.
                    var entries = AccountEntries(accountCard ?? new CommunityPreferences()).ToArray();
                    panel.Add("account", "荣耀账号", labelEn: "HONOR account", enabledOverride: HonorPcManager.Find() is not null);
                    panel.Add("@divider:1", "", control: "label");
                    foreach (var (entryId, entryLabel, entryTarget) in entries)
                        panel.Add(entryId, entryLabel, command: Local("account-open", new { target = entryTarget }));
                    if (entries.Length > 0) panel.Add("@divider:2", "", control: "label");
                    panel.Add("settings", "设置", labelEn: "Settings", enabledOverride: true);
                    panel.Add("update", "检查更新", labelEn: "Check for updates", enabledOverride: true);
                    panel.Add("feedback", "问题反馈", labelEn: "Feedback", enabledOverride: true);
                    panel.Add("about", "关于", labelEn: "About", enabledOverride: true);
                    panel.Add("@divider:3", "", control: "label");
                    panel.Add("restart", "重新启动", labelEn: "Restart", enabledOverride: true);
                    panel.Add("sleep", "睡眠", labelEn: "Sleep", enabledOverride: true);
                    panel.Add("shutdown", "关机", labelEn: "Shut down", enabledOverride: true);
                    panel.Add("lock", "锁定", labelEn: "Lock", enabledOverride: true);
                    panel.Add("@divider:4", "", control: "label");
                    panel.Add("account-tune", "菜单卡片设置", "尺寸、位置、动画和系统入口", Local("account-tune"));
                    panel.Add("exit", "退出社区版", labelEn: "Exit Magic Community", enabledOverride: true);
                }
                break;
            case "@magic/notifications":
                var notifications = Provider("notifications");
                panel.Add("focus",notificationQuiet?"免打扰-开":"免打扰-magic视界",command:Local("notification-quiet",new{enabled=!notificationQuiet}),active:notificationQuiet);
                if (Flag(notifications, "available") && Flag(notifications, "canClearAll") && Text(notifications, "access") == "allowed")
                    panel.Add("clear", "清除全部", command: Backend("notifications-clear", new { confirmed = true }));
                if (Text(notifications, "access") != "allowed") panel.Add("authorize", "允许 Magic 视界读取通知", "确认后使用 Windows 通知读取权限", Local("notifications-authorize"));
                foreach (var item in Array(notifications, "notifications"))
                {
                    if(panel.Rows.Count>=190)break; // Keep the dismissal pair and settings footer inside the existing row bound.
                    string id = Text(item, "id"); var text = Array(item, "text").Select(x => x.GetString() ?? "").ToArray();
                    // A toast we could read but that carries no text would otherwise
                    // draw as an empty card, which looks exactly like a notification
                    // that never arrived. Say which one it is.
                    string body = text.Length > 0 ? string.Join("\n", text) : "（这条通知没有可读取的文字）";
                    panel.Add("notification:"+id, Text(item, "appName"), body, Text(item, "appId").Length > 0 ? Local("open-application", new { aumid = Text(item, "appId") }) : null);
                    var notice=panel.Rows.Last()!.AsObject();notice["iconUmid"]=Text(item,"appId");notice["createdFileTime"]=Text(item,"createdFileTime");
                    // A row kept after its sender withdrew it no longer exists in
                    // Windows, so offering to clear it there would only fail.
                    if (!Flag(item, "transient")) panel.Add("dismiss:" + id, "清除此通知", command: Backend("notifications-remove", new { notificationId = id }));
                }
                Setting("notification-settings", "通知设置", "ms-settings:notifications"); break;
            case "@magic/bluetooth-popup":
                var bluetooth = Provider("bluetooth"); panel.Add("bluetooth", Flag(bluetooth,"radioAvailable")?"蓝牙":"蓝牙（状态未知）", command: Flag(bluetooth,"radioAvailable")?Backend("bluetooth-action", new { action = "radio" }, "value", true):null, control: "toggle", active: Flag(bluetooth, "radioOn"));
                if (!Flag(bluetooth, "radioAvailable")) panel.Add("bt-status", "蓝牙状态暂不可用", "点击刷新重新读取，当前不代表蓝牙已关闭");
                else if (Flag(bluetooth, "devicesIncomplete")) panel.Add("bt-status", "部分设备暂未响应", "蓝牙开关状态已更新，可刷新设备列表");
                foreach (var item in Array(bluetooth, "devices"))
                {
                    string id = Text(item, "id"); bool connected = Flag(item, "connected"), paired = Flag(item, "paired");
                    panel.Add("device:" + id, Text(item, "name"), connected ? "已连接" : paired ? "已配对" : "可用设备");
                    var deviceRow=panel.Rows.Last()!.AsObject();deviceRow["deviceIcon"]=Text(item,"deviceIcon","TbBluetooth");
                    double deviceBattery=Number(item,"batteryPercent",-1);if(deviceBattery>=0&&deviceBattery<=100)deviceRow["batteryPercent"]=deviceBattery;
                    if (paired) panel.Add("forget:" + id, "删除设备", command: Backend("bluetooth-unpair", new { deviceId = id }));
                    if (!paired) panel.Add("pair:" + id, "配对", command: Backend("bluetooth-action", new { action = "pair", deviceId = id }));
                    else if (connected) panel.Add("disconnect:" + id, "断开连接", command: Backend("bluetooth-action", new { action = "disconnect", deviceId = id }));
                    else panel.Add("connect:" + id, "连接", command: Backend("bluetooth-action", new { action = "connect", deviceId = id }));
                }
                panel.Add("bt-discover", "刷新设备", command: Local("bluetooth-refresh"));
                panel.Add("bt-find", "查找新设备", command: Backend("bluetooth-action", new { action = "discover" }));
                Setting("bt-settings", "蓝牙设置", "ms-settings:bluetooth"); break;
            case "@magic/network-popup":
                var wifi = Provider("wifi");
                if (!Flag(wifi, "networkDetailsRequested"))
                    panel.Add("network-details-status", Flag(wifi, "networkDetailsFailed") ? "网络列表暂不可用，请刷新" : "正在读取网络…");
                foreach(var limited in Array(wifi,"interfaces").Where(i=>Number(i,"state")==1&&Text(i,"portalHint").Length>0).Take(1))panel.Add("network-auth",Text(limited,"portalHint"),"点击打开默认浏览器完成网络认证",Local("wifi-auth",new{}));
                var wifiGroups = new Dictionary<string, JsonObject>(StringComparer.Ordinal);
                foreach (var item in Array(wifi, "interfaces").OrderByDescending(i => Array(i,"networks").Any(n => Flag(n,"connected"))))
                {
                    if(panel.Rows.Count>185)break;
                    string adapter = Text(item, "id"); bool radioKnown=item.TryGetProperty("radioOn",out var radioState)&&radioState.ValueKind is JsonValueKind.True or JsonValueKind.False; panel.Add("radio:" + adapter, radioKnown?"Wi-Fi":"Wi-Fi（状态未知）", command: radioKnown?Backend("wifi-action", new { action = "radio", interfaceId = adapter }, "value", true):null, control: "toggle", active: Flag(item, "radioOn")); panel.Add("adapter:" + adapter, Text(item, "name"), Text(item, "state"));
                    panel.Add("scan:" + adapter, "刷新网络", command: Backend("wifi-action", new { action = "scan", interfaceId = adapter }));
                    // Windows can return the same SSID for several saved
                    // profiles/security modes, and can repeat identical rows.
                    // Keep distinct connection targets; collapse only exact
                    // duplicates, preferring the connected/strongest record.
                    foreach (var network in Array(item, "networks")
                        .GroupBy(n => (Ssid: Text(n, "ssidHex"), Auth: Number(n, "authentication"), Profile: Text(n, "profile")))
                        .Select(g => g.OrderByDescending(n => Flag(n, "connected")).ThenByDescending(n => Number(n, "signal")).First()))
                    {
                        if(panel.Rows.Count>187)break; // Reserve room for the group, actions and settings.
                        string hex = Text(network, "ssidHex"), profile = Text(network, "profile"); string name = "";
                        string identity = hex + ":" + Number(network, "authentication").ToString(System.Globalization.CultureInfo.InvariantCulture) + ":" + Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(profile)));
                        if (name.Length == 0) try { name = System.Text.Encoding.UTF8.GetString(Convert.FromHexString(hex)); } catch (FormatException) { name = hex; }
                        string groupId="wifi-group:"+hex;
                        if(!wifiGroups.TryGetValue(groupId,out var group)){
                            panel.Add(groupId,name+(Flag(network,"connected")&&Text(item,"portalHint").Length>0?" · "+Text(item,"portalHint"):""),control:"button");group=panel.Rows.Last()!.AsObject();group["networkGroup"]=groupId;group["connected"]=false;wifiGroups.Add(groupId,group);
                        }
                        if(Flag(network,"connected")){group["connected"]=true;if(Text(item,"portalHint").Length>0)group["label"]=name+" · "+Text(item,"portalHint");}
                        panel.Add("network:" + adapter + ":" + identity, Flag(network,"connected")?"断开连接":"连接", Flag(network, "connected") ? (Text(item,"portalHint").Length>0?Text(item,"portalHint"):"已连接") : Flag(network, "secured") ? "安全网络" : "开放网络", Flag(network, "connected") ? Backend("wifi-action", new { action = "disconnect", interfaceId = adapter }) : Local("wifi-connect", new { interfaceId = adapter, profile, ssidHex = hex, secured = Flag(network, "secured") }));
                        panel.Rows.Last()!["networkGroup"]=groupId;panel.Rows.Last()!["profileLabel"]=profile;
                        if (profile.Length > 0) {panel.Add("forget-network:" + adapter + ":" + identity, "忘记此网络", command: Backend("wifi-action", new { action = "forget-profile", interfaceId = adapter, profile }));panel.Rows.Last()!["networkGroup"]=groupId;}
                        // Multiple saved profiles remain distinct authenticated targets.
                        if(profile.Length>0)panel.Data["rows"]!.AsArray().Last()!["profileLabel"]=profile;
                    }
                }
                foreach(var group in wifiGroups){
                    var actions=panel.Rows.Where(r=>r?["networkGroup"]?.GetValue<string>()==group.Key&&r?["id"]?.GetValue<string>().StartsWith("network:")==true).ToArray();
                    if(actions.Length>1)foreach(var action in panel.Rows.Where(r=>r?["networkGroup"]?.GetValue<string>()==group.Key&&r?["id"]?.GetValue<string>()!=group.Key)){
                        string profileName=action?["profileLabel"]?.GetValue<string>()??"";
                        action!["label"]=(action["label"]?.GetValue<string>()??"")+" · "+(profileName.Length>0?profileName:action["detail"]?.GetValue<string>()??"网络配置");
                    }
                }
                Setting("network-settings", "网络设置", "ms-settings:network-wifi"); break;
            case "@magic/media-popup":
                foreach (var item in Array(Provider("media"), "sessions"))
                {
                    string id = Text(item, "id"); panel.Add("track:" + id, Text(item, "title"), Text(item, "artist"));
                    panel.Add("player:" + id + ":prev", "上一首", command: Flag(item, "canPrevious") ? Backend("media-action", new { sessionId = id, action = "previous" }) : null);
                    panel.Add("player:" + id + ":play", Number(item, "status") == 4 ? "暂停" : "播放", command: Backend("media-action", new { sessionId = id, action = "toggle" }), active: Number(item, "status") == 4);
                    panel.Add("player:" + id + ":next", "下一首", command: Flag(item, "canNext") ? Backend("media-action", new { sessionId = id, action = "next" }) : null);
                }
                Audio();
                var microphone = Provider("microphone"); if (Flag(microphone, "available")) { string input = Text(microphone, "defaultDeviceId"); panel.Add("volume:microphone", "麦克风音量", Text(microphone, "defaultDeviceName"), Backend("microphone-action", new { action = "set-volume", deviceId = input }, "value"), "slider", value: Number(microphone, "volume")); panel.Add("mute:microphone", "麦克风静音", command: Backend("microphone-action", new { action = "set-mute", deviceId = input }, "value", true), control: "toggle", active: Flag(microphone, "muted")); }
                var mixer = Provider("mixer"); foreach (var session in Array(mixer, "sessions")) { string id = Text(session, "id"), device = Text(mixer, "deviceId"); panel.Add("session:" + id, Text(session, "name"), command: Backend("mixer-action", new { action = "set-volume", deviceId = device, sessionId = id }, "value"), control: "slider", value: Number(session, "volume")); panel.Add("mute:session:" + id, "静音", command: Backend("mixer-action", new { action = "set-mute", deviceId = device, sessionId = id }, "value", true), control: "toggle", active: Flag(session, "muted")); }
                Setting("audio-settings", "声音设置", "ms-settings:sound"); break;
            case "@magic/quick-settings" when powerView:
                var power=Object(Object(state.Snapshot,"system"),"power");double battery=Number(power,"batteryPercent",-1);
                panel.Data["nativePowerView"]=true;panel.Data["view"]="power";
                panel.Add("power-status",battery<0?"电源状态":battery.ToString("0")+"%",Number(power,"acLineStatus")==1?"已连接电源":"使用电池");
                panel.Add("power-mode","Windows 电源模式",Text(power,"mode") switch{"efficiency"=>"更长续航","balanced"=>"平衡","performance"=>"最佳性能",_=>"暂不可用"},Local("settings-uri",new{uri="ms-settings:powersleep"}));
                string saver=power.ValueKind==JsonValueKind.Object&&power.TryGetProperty("powerSaver",out var saverValue)&&saverValue.ValueKind==JsonValueKind.Number&&saverValue.TryGetInt32(out int saverFlag)&&saverFlag is 0 or 1?(saverFlag==1?"已开启":"已关闭"):"暂不可用";
                panel.Add("saver","Windows 节电模式",saver,Local("settings-uri",new{uri="ms-settings:powersleep"}));
                panel.Add("honor-mode","荣耀性能模式","暂不可用");
                foreach(var item in Array(power,"batteries")){
                    string name=Text(item,"name");double health=Number(item,"healthPercent",-1),cycles=Number(item,"cycleCount",-1);
                    panel.Add("battery:"+Text(item,"id"),name.Length>0?name:"电池信息",$"电池健康度 {(health>=0?health.ToString("0")+"%":"暂不可用")} · 循环次数 {(cycles>=0?cycles.ToString("0"):"暂不可用")}");
                }
                if(!Array(power,"batteries").Any()&&Number(power,"batteryPercent",-1)>=0)panel.Add("battery:unknown","电池信息","电池健康度 暂不可用 · 循环次数 暂不可用");
                panel.Add("honor-manager","荣耀电脑管家","打开管家",Local("honor-power"));
                panel.Add("quick-back","快捷设置",command:Local("panel",new{kind="@magic/quick-settings"}));Setting("system-power","电源设置","ms-settings:powersleep");break;
            case "@magic/quick-settings":
                if(displayView){
                    panel.Data["nativeDisplayView"]=true;panel.Data["title"]="显示与亮度";
                    bool dark=NativeAppearanceSettings.IsDark;
                    panel.Add("appearance-theme",dark?"深色模式":"浅色模式","与 Windows 及设置页同步",Local("appearance-theme",new{dark=!dark}),"toggle",dark);
                    Brightness();
                    Setting("night-settings","Windows 夜间模式设置","ms-settings:nightlight");
                    panel.Add("display-back","返回控制中心",command:Local("panel",new{kind="@magic/quick-settings"}));break;
                }
                Brightness(); Audio(); panel.Add("display-settings", "显示与亮度", command:Local("panel",new{kind="@magic/quick-settings",nativeDisplayView=true})); panel.Add("focus-settings","通知与勿扰",command:Local("panel",new{kind="@magic/notifications"}));
                var adapters = Array(Provider("wifi"), "interfaces");
                var bt = Provider("bluetooth");
                panel.Add("quick-network", adapters.Any(x => x.TryGetProperty("radioOn", out _)) ? "网络" : "网络（状态未知）", command: Local("panel", new { kind = "@magic/network-popup" }), active: adapters.Any(x => Flag(x, "radioOn")));
                panel.Add("quick-bluetooth", Flag(bt, "radioAvailable") ? "蓝牙" : "蓝牙（状态未知）", command: Local("panel", new { kind = "@magic/bluetooth-popup" }), active: Flag(bt, "radioAvailable") && Flag(bt, "radioOn"));
                panel.Add("quick-power", "电源", command: Local("panel", new { kind = "@magic/power-menu" })); Setting("quick-settings", "系统设置", "ms-settings:"); break;
            case "@magic/power-menu":
                foreach(var action in new[]{("lock","锁定"),("sleep","睡眠"),("restart","重新启动"),("shutdown","关机"),("settings","设置"),("exit","退出社区版")})panel.Add(action.Item1,action.Item2,command:Local("power",new{action=action.Item1}));break;
            case "@magic/task-switcher":
                foreach (var item in Array(state.Snapshot, "windows")) panel.Add("window:" + Text(item, "hwnd"), Text(item, "title"), command: Backend("window-action", new { windowId = Text(item, "id"), action = "activate" }), image: Text(item, "executable")); break;
            default: throw new InvalidDataException("Unsupported independent panel.");
        }
        return panel;
    }
}
