using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

internal sealed partial class NativeSystemUiSession
{
    private void LoadPreferences()
    {
        JsonDocument? Read(string name) { string path = Path.Combine(deployment.DataDirectory, name); if (!File.Exists(path)) return null; if (new FileInfo(path).Length > 262144 || (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Independent preferences exceed bounds or are a link."); return JsonDocument.Parse(File.ReadAllBytes(path), new JsonDocumentOptions { MaxDepth = 8 }); }
        using (var brands = Read("brand-icons.json")) if (brands is not null) foreach (var item in brands.RootElement.EnumerateObject())
        {
            if (brandIcons.Count >= 256 || item.Name.Length > 96) throw new InvalidDataException("Brand icon bound.");
            // Older files stored a bare slug; keep reading them.
            if (item.Value.ValueKind == JsonValueKind.String)
            {
                string legacy = item.Value.GetString() ?? "";
                if (legacy.Length > 64) throw new InvalidDataException("Brand icon bound.");
                brandIcons.Add(item.Name, new BrandOverride(legacy, false, "brand", "", "mono", ""));
                continue;
            }
            string? icon = item.Value.TryGetProperty("Icon", out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
            if (icon is { Length: > 64 }) throw new InvalidDataException("Brand icon bound.");
            bool hidden = item.Value.TryGetProperty("IconHidden", out var flag) && flag.ValueKind == JsonValueKind.True;
            string label = item.Value.TryGetProperty("Label", out var mode) && mode.ValueKind == JsonValueKind.String ? mode.GetString() ?? "brand" : "brand";
            string text = item.Value.TryGetProperty("Text", out var typed) && typed.ValueKind == JsonValueKind.String ? typed.GetString() ?? "" : "";
            if (text.Length > 32) throw new InvalidDataException("Brand wordmark bound.");
            string style = item.Value.TryGetProperty("Style", out var paint) && paint.ValueKind == JsonValueKind.String ? paint.GetString() ?? "mono" : "mono";
            string device = item.Value.TryGetProperty("Device", out var shape) && shape.ValueKind == JsonValueKind.String ? shape.GetString() ?? "" : "";
            brandIcons.Add(item.Name, new BrandOverride(icon, hidden, label, text, style == "color" ? "color" : "mono", device == "desktop" ? "desktop" : ""));
        }
        using (var icons = Read("custom-icons.json")) if (icons is not null) foreach (var item in icons.RootElement.EnumerateObject()) { if (customIcons.Count >= 256 || item.Name.Length > 96 || item.Value.GetString()!.Length > 32768) throw new InvalidDataException("Custom icon bound."); customIcons.Add(item.Name, item.Value.GetString()!); }
        try{notificationQuiet=NativeNotificationPreferences.Read(deployment.DataDirectory);}catch(Exception e)when(e is IOException or InvalidDataException or JsonException or UnauthorizedAccessException or InvalidOperationException or KeyNotFoundException or FormatException){notificationQuiet=false;log("notification_preference_unavailable",new{type=e.GetType().Name});}
        using (var tray = Read("tray-preferences.json")) if (tray is not null) { foreach (var item in tray.RootElement.GetProperty("promoted").EnumerateObject()) { if (trayPromoted.Count >= 256 || item.Name.Length > 33280) throw new InvalidDataException("Tray preference bound."); trayPromoted.Add(item.Name, item.Value.GetBoolean()); } foreach (var item in Array(tray.RootElement, "order")) { if (trayOrder.Count >= 256 || item.GetString()!.Length > 33280) throw new InvalidDataException("Tray order bound."); trayOrder.Add(item.GetString()!); } }
    }
    private async Task AuthorizeNotifications()
    {
        await popups.Close(stop.Token);
        var before = await backend.Request("notifications-status", new { }, stop.Token);
        string consentScope = Text(before, "consentScope");
        if (consentScope.Length == 0) throw new InvalidOperationException("当前运行身份不支持通知授权。");
        string executable = Path.Combine(deployment.CodeDirectory, "MagicNotificationPermission.exe");
        if (!deployment.Files.TryGetValue("MagicNotificationPermission.exe", out var expected)) throw new InvalidDataException("Independent notification permission helper is absent.");
        using (var file = File.OpenRead(executable)) if (!Convert.ToHexString(SHA256.HashData(file)).Equals(expected, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Permission helper hash differs.");
        var start = new ProcessStartInfo(executable) { UseShellExecute = false }; using var process = Process.Start(start) ?? throw new IOException("通知授权窗口未启动。");
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(stop.Token); deadline.CancelAfter(TimeSpan.FromMinutes(3)); await process.WaitForExitAsync(deadline.Token);
        if (process.ExitCode != 0) throw new InvalidOperationException("通知授权未完成。");
        using (var identity = System.Security.Principal.WindowsIdentity.GetCurrent()) StateStore.AtomicWrite(Path.Combine(deployment.DataDirectory, "notification-consent.json"), JsonSerializer.Serialize(new { protocol = "Magic.Notification.Consent/1", family = consentScope, userSid = identity.User!.Value, flow = "explicit-user-interface", requestedUtc = DateTimeOffset.UtcNow }), null);
        var status = await backend.Request("notifications-status", new { }, stop.Token); if (!Flag(status, "available") || Text(status, "access") != "allowed") throw new InvalidOperationException("通知权限尚未授予；可在 Windows 设置中单独允许 Magic 视界社区版。");
        await backend.Request("notifications-subscribe", new { enabled = true }, stop.Token);
    }
    private async Task ConnectWifi(JsonElement args, string token)
    {
        string adapter = Text(args, "interfaceId"), profile = Text(args, "profile");
        if (profile.Length > 0) { try { await backend.Request("wifi-action", new { action = "connect-profile", interfaceId = adapter, profile }, stop.Token); } finally { NetworkActionCompleted(token); } return; }
        bool secured = Flag(args, "secured"); string hex = Text(args, "ssidHex");
        if (!secured) { try { await backend.Request("wifi-action", new { action = "connect-network", interfaceId = adapter, ssidHex = hex, password = "" }, stop.Token); } finally { NetworkActionCompleted(token); } return; }
        networkDetails.Hide(token);
        var panel = new NativeSystemPanel(); panel.Data["nativeNetworkPassword"] = true; panel.Add("password", "网络密码", control: "password");
        panel.Add("connect-network", "连接", command: new Command("wifi-action", new JsonObject { ["action"] = "connect-network", ["interfaceId"] = adapter, ["ssidHex"] = hex }, ValueField: "password"));
        lock (currentGate) { if (currentToken != token) return; currentPanel = panel; }
        await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), stop.Token);
    }
}
