using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

internal sealed partial class NativeSystemUiSession
{
    private WindowsStartCatalog? startCatalog;
    private StartApplication[] pinnedApps = [];
    private StartApplication[] applicationEntries => startCatalog?.All ?? [];
    private readonly Dictionary<string, StartApplication> offeredApps = new(StringComparer.Ordinal);
    private readonly object appScanGate = new();
    private Task<WindowsStartCatalog>? appScan;
    private DateTimeOffset appsRead;
    private long startLayoutStamp;
    private bool appsAll;
    private string appsSearch = "";
    private int appsPage;
    private double appsWidth = 760, appsHeight = 520;
    private readonly Dictionary<string, bool> trayPromoted = new(StringComparer.Ordinal);
    private readonly List<string> trayOrder = [];

    private Task<NativeSystemPanel> AppsPanel(bool opening = false)
    {
        Task<WindowsStartCatalog> scan;
        lock (appScanGate)
        {
            long stamp = opening ? WindowsStartCatalog.LayoutStamp() : startLayoutStamp;
            if (appScan is null || opening && appScan.IsCompleted &&
                (stamp != startLayoutStamp || DateTimeOffset.UtcNow - appsRead > TimeSpan.FromMinutes(1) || appScan.IsFaulted || startCatalog?.PinReadError is not null))
            {
                startLayoutStamp = stamp;
                appsRead = DateTimeOffset.UtcNow;
                appScan = WindowsStartCatalog.Read(stop.Token);
            }
            scan = appScan;
            if (scan.IsCompletedSuccessfully)
            {
                startCatalog = scan.Result;
                if (startCatalog.Pins is not null) pinnedApps = startCatalog.Pins;
            }
        }
        if (opening) { appsAll = false; appsSearch = ""; appsPage = 0; }
        var panel = new NativeSystemPanel();
        panel.Data["appsView"] = appsAll ? "all" : "favorites";
        panel.Data["hostPid"] = Environment.ProcessId;
        panel.Add("search", "搜索应用", command: Local("apps-control", new { kind = "search" }), control: "text");
        panel.Add("apps-view", appsAll ? "已固定" : "全部应用", command: Local("apps-control", new { kind = "view" }));
        panel.Add("apps-power", "电源", command: Local("panel", new { kind = "@magic/power-menu" }));
        IEnumerable<StartApplication> items = appsAll ? applicationEntries : pinnedApps;
        if (appsAll && appsSearch.Length > 0)
            items = items.Concat(startCatalog?.SearchOnly ?? []).Where(a => ApplicationSearchFiles.Matches(a.Name, a.Target, appsSearch));
        var list = items.ToArray();
        int capacity = NativeAppsLayout.Grid(appsWidth, appsHeight).Capacity;
        int pages = Math.Max(1, (list.Length + capacity - 1) / capacity);
        appsPage = Math.Clamp(appsPage, 0, pages - 1);
        panel.Data["appsPageSize"] = capacity;
        panel.Data["page"] = appsPage;
        panel.Data["pages"] = pages;
        offeredApps.Clear();
        foreach (var app in list.Skip(appsPage * capacity).Take(capacity))
        {
            string rowId = app.Id;
            // Two distinct Windows pins can point to the same app. Retain both
            // positions without reusing the renderer command key.
            if (!offeredApps.TryAdd(rowId, app))
            {
                rowId += "-" + offeredApps.Count;
                offeredApps.Add(rowId, app);
            }
            var item = new { id = app.Id, rowId, path = app.Target, location = app.Location, umid = app.Aumid, label = app.Name };
            panel.Add(rowId, app.Name, command: app.Available ? Local("apps-item", item) : null, image: app.Target);
            var row = panel.Data["rows"]!.AsArray().Last()!;
            row["iconUmid"] = app.Aumid;
            row["hasPath"] = app.Target.Length > 0;
            row["hasLocation"] = app.Location.Length > 0;
            row["secondaryTile"] = app.TileArguments is not null;
        }
        if (!scan.IsCompleted && list.Length == 0) panel.Add("apps-loading", "正在读取 Windows 开始菜单…");
        else if (scan.IsFaulted || scan.IsCanceled || !appsAll && startCatalog?.PinReadError is not null)
            panel.Add("apps-unavailable", appsAll ? "Windows 应用目录暂不可用" : "暂时无法读取 Windows 已固定", "重新打开开始菜单可重试。");
        else if (list.Length == 0) panel.Add("apps-empty", appsAll ? "没有匹配的应用" : "Windows 开始菜单暂无已固定项目");
        if (pages > 1)
        {
            panel.Add("apps-previous", "上一页", command: appsPage > 0 ? Local("apps-control", new { kind = "previous" }) : null);
            panel.Add("apps-next", "下一页", command: appsPage + 1 < pages ? Local("apps-control", new { kind = "next" }) : null);
        }
        return Task.FromResult(panel);
    }
    private async Task RefreshApplications(string token, long generation)
    {
        Task<WindowsStartCatalog>? scan; lock (appScanGate) scan = appScan;
        if (scan is null) return;
        try { await scan; }
        catch (Exception error) when (error is not OutOfMemoryException) { if (stop.IsCancellationRequested) return; log("independent_apps_catalog_failed", new { type = error.GetType().Name }); }
        bool entered = false;
        try
        {
            await popupGate.WaitAsync(stop.Token); entered = true;
            lock (currentGate) if (currentToken != token || popupGeneration != generation || currentKind != "@magic/apps-menu") return;
            if(scan.IsCompletedSuccessfully&&ReferenceEquals(startCatalog,scan.Result))return;
            var panel = await AppsPanel();
            lock (currentGate) { if (currentToken != token || popupGeneration != generation) return; currentPanel = panel; }
            await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), stop.Token);
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
        finally { if (entered) popupGate.Release(); }
    }
    private async Task MeasureAppsWindow(JsonElement opened,string token){
        var hwnd=new IntPtr((long)Number(opened,"window"));if(hwnd==IntPtr.Zero)return;
        if(GetAppsClientRect(hwnd,out var rect)){uint dpi=GetAppsDpi(hwnd);if(dpi==0)dpi=96;double w=(rect.Right-rect.Left)*96d/dpi,h=(rect.Bottom-rect.Top)*96d/dpi;
            if(w>=240&&h>=240&&w<=1920&&h<=1440){int previous=NativeAppsLayout.Grid(appsWidth,appsHeight).Capacity;appsWidth=w;appsHeight=h;if(previous==NativeAppsLayout.Grid(w,h).Capacity)return;var updated=await AppsPanel();lock(currentGate){if(currentToken!=token)return;currentPanel=updated;}await popups.Update(JsonSerializer.SerializeToElement(new{token,data=updated.Data}),stop.Token);}
        }
    }
    [StructLayout(LayoutKind.Sequential)]private struct AppsRect{public int Left,Top,Right,Bottom;}
    [DllImport("user32.dll",EntryPoint="GetClientRect")]private static extern bool GetAppsClientRect(IntPtr h,out AppsRect r);
    [DllImport("user32.dll",EntryPoint="GetDpiForWindow")]private static extern uint GetAppsDpi(IntPtr h);
    private async Task AppsCommand(Command action, JsonElement result, string token)
    {
        var args = JsonSerializer.SerializeToElement(action.Arguments);
        if (action.Local == "apps-control")
        {
            switch (Text(args, "kind"))
            {
                case "view": appsAll = !appsAll; appsSearch = ""; appsPage = 0; break;
                case "search": appsSearch = Text(Object(result, "fields"), "search"); appsPage = 0; break;
                case "previous": --appsPage; break;
                case "next": ++appsPage; break;
            }
        }
        else
        {
            if (!offeredApps.TryGetValue(Text(args, "rowId"), out var app) || app.Id != Text(args, "id") || !app.Available)
                throw new InvalidOperationException("应用目录已变化，请重新打开。");
            int button = (int)Number(result, "button");
            if (button == 0)
            {
                await popups.Close(stop.Token);
                if (app.TileArguments is not null) NativeJumpLists.ActivateStartTile(app.Aumid, app.TileArguments);
                else Launch(args, false);
                return;
            }
            if (app.TileArguments is not null) return;
            switch (button)
            {
                case 11: await ApplicationCommand(args, "pin"); break;
                case 12: await popups.Close(stop.Token); Launch(args, true); break;
                case 13: await ApplicationCommand(args, "open-location"); break;
                case 14: OpenUri("ms-settings:appsfeatures"); break;
                case 15: await EditIcon(app.Id); break;
            }
            return;
        }
        var panel = await AppsPanel();
        lock (currentGate) { if (currentToken != token) return; currentPanel = panel; }
        await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), stop.Token);
    }

    internal static async Task<object> ProbeApplications()
    {
        var timer = Stopwatch.StartNew();
        var catalog = await WindowsStartCatalog.Read(CancellationToken.None);
        return new { milliseconds = timer.ElapsedMilliseconds, all = catalog.All.Length,
            unique = catalog.All.Select(a => a.Id).Distinct().Count(), searchOnly = catalog.SearchOnly.Length,
            pins = catalog.Pins?.Length, available = catalog.Pins?.Count(a => a.Available),
            systemAppIds = catalog.All.Select(a => a.Id),
            orderedPins = catalog.Pins?.Select(a => new { a.Id, path = a.Target.Length > 0 ? WindowsStartCatalog.Id(a.Target) : null }),
            searchIds = catalog.SearchOnly.Select(a => a.Id),
            catalog.PinReadError, scope = "Read-only Windows catalog. No application launched." };
    }
    private JsonElement ProjectTray(JsonElement input)
    {
        var data = JsonNode.Parse(input.GetRawText())!.AsObject(); var icons = data["icons"]!.AsArray(); lock (trayPromoted) { foreach (var item in icons) if (trayPromoted.TryGetValue(item!["stableId"]!.GetValue<string>(), out bool promoted)) item["promoted"] = promoted; var sorted = icons.OrderBy(x => { int n = trayOrder.IndexOf(x!["stableId"]!.GetValue<string>()); return n < 0 ? int.MaxValue : n; }).Select(x => x!.DeepClone()).ToArray(); icons.Clear(); foreach (var item in sorted) icons.Add(item); }
        return JsonSerializer.SerializeToElement(data);
    }
    private async Task TrayPreference(JsonElement args, JsonElement result, string token)
    {
        string receipt = Text(result, "reorderReceipt");
        try
        {
            string id = Text(args, "stableId"), targetId = "";
            bool pin = (int)Number(result, "button") == 4;
            var live = Array(await backend.Request("tray-read", new { }, stop.Token), "icons");
            lock (currentGate)
            {
                if (currentToken != token || currentKind != "@magic/system-tray") return;
                if (!pin)
                {
                    var target = currentPanel?.Commands.GetValueOrDefault(Text(result, "destination"));
                    if (target is null || target.Local != "tray") throw new InvalidDataException("Tray reorder target expired.");
                    targetId = Text(JsonSerializer.SerializeToElement(target.Arguments), "stableId");
                }
            }
            if (!live.Any(icon => Text(icon, "stableId") == id)) throw new InvalidDataException("Tray source expired.");
            lock (trayPromoted)
            {
                var promoted = new Dictionary<string, bool>(trayPromoted, StringComparer.Ordinal);
                var order = trayOrder.ToList();
                if (pin) promoted[id] = !(promoted.TryGetValue(id, out bool saved) ? saved : live.Any(icon => Text(icon, "stableId") == id && Flag(icon, "promoted")));
                else
                {
                    var visible = live.Select(icon => Text(icon, "stableId")).Distinct(StringComparer.Ordinal)
                        .OrderBy(key => { int n = order.IndexOf(key); return n < 0 ? int.MaxValue : n; }).ToArray();
                    var moved = NativeTrayOrder.Move(visible, id, targetId, Flag(result, "after"));
                    moved.AddRange(order.Where(key => !visible.Contains(key, StringComparer.Ordinal)));
                    order = moved;
                }
                if (promoted.Count > 256 || order.Count > 256) throw new InvalidOperationException("托盘偏好已达到上限。");
                StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "tray-preferences.json"), JsonSerializer.Serialize(new { promoted, order }), null);
                trayPromoted.Clear(); foreach (var pair in promoted) trayPromoted.Add(pair.Key, pair.Value);
                trayOrder.Clear(); trayOrder.AddRange(order);
            }
            await backend.Request("tray-refresh", new { }, stop.Token);
        }
        finally
        {
            // A queued renderer command is not a save acknowledgement. Return
            // authoritative order even on rejection, so the preview can settle
            // or roll back without changing pin/hidden state or invoking an app.
            if (receipt.Length > 0)
            {
                var state = presentation.State;
                var providers = new Dictionary<string, JsonElement>(state.Providers);
                providers["tray"] = ProjectTray(await backend.Request("tray-read", new { }, stop.Token));
                var panel = NativeSystemPanel.Build("@magic/system-tray", state with { Providers = providers });
                panel.Data["hostPid"] = Environment.ProcessId; panel.Data["trayReorderReceipt"] = receipt;
                bool current;
                lock (currentGate) { current = currentToken == token && currentKind == "@magic/system-tray"; if (current) currentPanel = panel; }
                if (current) await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), stop.Token);
            }
        }
    }
}
