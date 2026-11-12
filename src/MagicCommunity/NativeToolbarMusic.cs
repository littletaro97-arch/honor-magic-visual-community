using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

// Session IDs address controls; application IDs only restore a preference after
// a player restarts. Never merge multiple sessions from the same application.
internal sealed class NativeToolbarMusic : IDisposable
{
    private sealed record Preferences(string[] Order, string Pinned);
    private readonly object gate = new();
    private readonly string? path;
    private readonly Action<string, string>? write;
    private readonly Action changed;
    private readonly NativeToolbarLyrics lookup;
    private readonly Dictionary<string, NativeToolbarLyrics> lyrics = new(StringComparer.Ordinal);
    private List<string> order = [];
    private string[] savedOrder = [];
    private string pinned = "", pinnedApp = "";
    private JsonElement[] sources = [];
    private bool disposed, mediaAvailable;

    internal NativeToolbarMusic(Action changed, string? directory = null, Action<string, string>? write = null,
        Func<NativeToolbarLyrics.Track, CancellationToken, Task<NativeToolbarLyrics.Line[]>>? fetch = null)
    {
        this.changed = changed; this.write = write; lookup = new(() => { }, fetch: fetch);
        Fetch = fetch ?? lookup.Fetch;
        if (directory is null) return;
        path = Path.Combine(directory, "toolbar-music.json");
        try {
            // 32 bounded application IDs plus a pin, including JSON Unicode escaping.
            if (File.Exists(path) && new FileInfo(path).Length <= 131072 && (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0) {
                var stored = JsonSerializer.Deserialize<Preferences>(File.ReadAllText(path));
                savedOrder = (stored?.Order ?? []).Where(ValidApp).Distinct(StringComparer.Ordinal).Take(32).ToArray();
                pinnedApp = ValidApp(stored?.Pinned) ? stored!.Pinned : "";
            }
        } catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { }
    }
    private readonly Func<NativeToolbarLyrics.Track, CancellationToken, Task<NativeToolbarLyrics.Line[]>> Fetch;
    private static bool ValidApp(string? id) => id is { Length: > 0 and <= 512 } && !id.Any(char.IsControl);
    internal JsonElement[] Sources { get { lock (gate) return sources.ToArray(); } }
    internal string Pinned { get { lock (gate) return pinned; } }

    internal void Observe(JsonElement media, bool toolbarVisible, bool panelVisible)
    {
        lock (gate) {
            if (disposed) return;
            mediaAvailable = Flag(media, "available");
            var available = Array(media, "sessions").Where(r => Text(r, "id").Length > 0)
                .DistinctBy(r => Text(r, "id")).Take(32).ToArray();
            var ids = available.Select(r => Text(r, "id")).ToHashSet(StringComparer.Ordinal);
            order.RemoveAll(id => !ids.Contains(id));
            int Rank(JsonElement row) { int i = System.Array.IndexOf(savedOrder, Text(row, "appId")); return i < 0 ? int.MaxValue : i; }
            foreach (var row in available.OrderBy(Rank).ThenByDescending(r => Flag(r, "current")).ThenByDescending(r => Number(r, "status") == 4)) {
                string id = Text(row, "id"); if (!order.Contains(id)) order.Add(id);
            }
            if (!ids.Contains(pinned)) pinned = "";
            if (pinned.Length == 0 && pinnedApp.Length > 0)
                pinned = order.FirstOrDefault(id => available.Any(r => Text(r, "id") == id && Text(r, "appId") == pinnedApp)) ?? "";
            if (pinned.Length > 0) { order.Remove(pinned); order.Insert(0, pinned); }
            sources = order.Select(id => available.Single(r => Text(r, "id") == id)).ToArray();
            var wanted = sources.Where((r, i) => Number(r, "status") == 4 && (panelVisible || toolbarVisible && i == 0))
                .Select(r => Text(r, "id")).ToHashSet(StringComparer.Ordinal);
            foreach (string id in lyrics.Keys.Where(id => !wanted.Contains(id)).ToArray()) { lyrics[id].Dispose(); lyrics.Remove(id); }
            foreach (var row in sources.Where(r => wanted.Contains(Text(r, "id")))) {
                string id = Text(row, "id");
                if (!lyrics.TryGetValue(id, out var engine)) lyrics[id] = engine = new(changed, fetch: Fetch);
                // One shared, bounded network gate in lookup.Fetch; no fan-out of
                // HTTP requests. Each visible player ticks only at lyric boundaries.
                engine.Observe(true, JsonSerializer.SerializeToElement(new { available = true, sessions = new[] { row } }));
            }
        }
    }
    internal void Project(JsonObject view, JsonElement media, bool panelVisible, bool cardDemand = false)
    {
        var targets = new[] { "left", "center", "right" }.SelectMany(s => view["toolbar"]?[s]?.AsArray() ?? [])
            .Where(r => r?["nativeModule"]?.GetValue<string>() == "magic.music").ToArray();
        bool shown = targets.Length > 0 && view["monitors"]?.AsArray().Any(m => m?["toolbar"]?.GetValue<bool>() == true) == true;
        Observe(media, shown || cardDemand, panelVisible);
        lock (gate) foreach (var item in targets) {
            var source = sources.FirstOrDefault(); bool has = source.ValueKind == JsonValueKind.Object;
            item!["hasPlayer"] = has; item["playing"] = has && Number(source, "status") == 4;
            item["nativeSessionId"] = Text(source, "id"); item["nativeTrackId"] = Text(source, "trackId");
            item["label"] = has ? Title(source) : "暂无播放源"; item["artist"] = Text(source, "artist");
            item["lyricsText"] = has ? Display(source) : "暂无播放源";
            item["sourceCount"] = sources.Length;
            string status = lyrics.TryGetValue(Text(source,"id"),out var engine) ? engine.Status : "idle";
            item["lyricsStatus"] = status;
            string lookupHint = status switch { "loading"=>"正在查找同步歌词", "missing"=>"暂无匹配的同步歌词", "error"=>"歌词源暂时不可用", _=>"" };
            item["tooltip"] = string.Join(" · ",new[]{has?Title(source):"暂无播放源",Text(source,"artist"),lookupHint,"点击查看所有播放源"}.Where(x=>x.Length>0));
            foreach (string capability in new[] { "canPlay", "canPause", "canPrevious", "canNext" }) item[capability] = Flag(source, capability);
        }
    }
    internal (JsonElement Provider, string Lyrics) CardState()
    {
        lock(gate) {
            var source=sources.FirstOrDefault();bool has=source.ValueKind==JsonValueKind.Object;
            string line=has&&Number(source,"status")==4&&lyrics.TryGetValue(Text(source,"id"),out var engine)?engine.Text:"";
            return (JsonSerializer.SerializeToElement(new{available=mediaAvailable,sessions=has?new[]{source}:System.Array.Empty<JsonElement>()}),line);
        }
    }
    private static string Title(JsonElement row) => Text(row, "title").Length > 0 ? NativeToolbarLyrics.Limit(Text(row, "title")) : "未提供标题";
    private string Display(JsonElement row) => Number(row, "status") == 4 && lyrics.TryGetValue(Text(row, "id"), out var engine) && engine.Text.Length > 0 ? engine.Text : Title(row);
    internal NativeSystemPanel Panel()
    {
        lock (gate) {
            var panel = new NativeSystemPanel(); panel.Data["title"] = "音乐"; panel.Data["nativeMusicView"] = true;
            foreach (var source in sources) {
                string id = Text(source, "id"), trackId = Text(source, "trackId"); bool playing = Number(source, "status") == 4;
                string app = Text(source, "appId"), artist = Text(source, "artist");
                panel.Add("music-source:" + id, Display(source), artist.Length > 0 ? artist + " · " + app : app,
                    Local("music-reorder", new { sessionId = id }), "music-source", active: id == pinned);
                void Button(string name, string label, string capability, string action) => panel.Add("music-" + name + ":" + id, label,
                    command: Flag(source, capability) ? Backend("media-action", new { sessionId = id, trackId, action }) : null, control: "music-" + name);
                Button("play", playing ? "暂停" : "播放", playing ? "canPause" : "canPlay", playing ? "pause" : "play");
                panel.Data["rows"]!.AsArray().Last()!["playing"] = playing;
                Button("previous", "上一曲", "canPrevious", "previous"); Button("next", "下一曲", "canNext", "next");
                panel.Add("music-pin:" + id, id == pinned ? "取消固定" : "固定到顶栏", command: Local("music-pin", new { sessionId = id }), control: "music-pin", active: id == pinned);
            }
            if (sources.Length == 0) panel.Add("music-empty", "暂无播放源", "打开音乐或视频应用后，会在这里显示");
            return panel;
        }
    }
    internal void Change(string id, string? destination = null)
    {
        lock (gate) {
            if (!order.Contains(id)) throw new InvalidOperationException("播放源已关闭，请重新选择。");
            var next = order.ToList(); string nextPin = pinned, nextApp = pinnedApp;
            if (destination is null) {
                nextPin = id == pinned ? "" : id;
                nextApp = nextPin.Length > 0 ? Text(sources.Single(r => Text(r, "id") == id), "appId") : "";
                if (nextPin.Length > 0) { next.Remove(id); next.Insert(0, id); }
            } else {
                if (!order.Contains(destination)) throw new InvalidOperationException("排序目标已变化，请重试。");
                if (id == pinned || id == destination) return;
                int index = next.IndexOf(destination); next.Remove(id);
                next.Insert(Math.Clamp(index, pinned.Length > 0 ? 1 : 0, next.Count), id);
            }
            var apps = next.Select(key => Text(sources.Single(r => Text(r, "id") == key), "appId")).Where(ValidApp).Distinct(StringComparer.Ordinal).ToArray();
            if (path is not null && write is not null) write(path, JsonSerializer.Serialize(new Preferences(apps, nextApp)));
            order = next; pinned = nextPin; pinnedApp = nextApp; savedOrder = apps;
            sources = order.Select(key => sources.Single(r => Text(r, "id") == key)).ToArray();
        }
    }
    public void Dispose() { lock (gate) { disposed = true; foreach (var value in lyrics.Values) value.Dispose(); lyrics.Clear(); lookup.Dispose(); } }
}
