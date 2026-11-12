namespace MagicCommunity;

// Per-monitor positions are small, bounded data, shared by every transient hint.
public sealed class NotificationPlacementPreferences
{
    public bool AutoSwitch { get; set; } = true;
    public string FixedMonitor { get; set; } = "primary";
    public Dictionary<string, NotificationAnchor> Monitors { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public void Normalize()
    {
        if (string.IsNullOrWhiteSpace(FixedMonitor) || FixedMonitor.Length > 512) FixedMonitor = "primary";
        Monitors = (Monitors ?? []).Where(p => !string.IsNullOrWhiteSpace(p.Key) && p.Key.Length <= 512 && p.Value is not null)
            .GroupBy(p => p.Key,StringComparer.OrdinalIgnoreCase).Take(16).ToDictionary(g => g.Key, g => g.First().Value, StringComparer.OrdinalIgnoreCase);
        foreach (var value in Monitors.Values) value.Normalize();
    }
    public NotificationAnchor For(string monitor) => Monitors.TryGetValue(monitor, out var value) ? value : new();
    public void Set(string monitor, NotificationAnchor value)
    {
        if (string.IsNullOrWhiteSpace(monitor) || monitor.Length > 512) throw new ArgumentException("显示器标识无效。");
        value.Normalize();
        if (!Monitors.ContainsKey(monitor) && Monitors.Count >= 16)
            throw new InvalidOperationException("已保存的显示器达到 16 块，请先恢复通知位置默认值。");
        Monitors[monitor] = value;
    }
}
public sealed class NotificationAnchor
{
    public string Anchor { get; set; } = "top-center";
    public int X { get; set; }
    public int Y { get; set; }
    public void Normalize()
    {
        if (Anchor is not ("top-left" or "top-center" or "top-right" or "bottom-left" or "bottom-center" or "bottom-right")) Anchor = "top-center";
        X = Math.Clamp(X, -4096, 4096); Y = Math.Clamp(Y, -4096, 4096);
    }
}
