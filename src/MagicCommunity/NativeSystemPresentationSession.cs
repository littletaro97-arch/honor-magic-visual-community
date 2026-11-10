using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MagicCommunity;

// Explicit isolated session over an already-owned independent backend. The
// existing formal runtime does not construct this class or select this provider.
internal sealed class NativeSystemPresentationSession : IAsyncDisposable
{
    private readonly NativeSystemClient backend;
    private readonly bool interactive;
    internal sealed record Published(JsonElement Configuration,JsonElement Snapshot,IReadOnlyDictionary<string,JsonElement> Providers);
    internal Func<JsonElement,JsonElement>? TrayProjection;
    internal IReadOnlySet<string> ChangedTopics {get;private set;}=new HashSet<string>();
    private Published? published;
    internal Published State => Volatile.Read(ref published) ?? throw new InvalidOperationException("Native session is not initialized.");
    private readonly Dictionary<string, JsonElement> providers = new();
    // A toast can be shown and then withdrawn from the system history by the app that
    // sent it within the same second. Tailscale does exactly that: the read after the
    // click returns it once, the read a second later does not, and the centre then
    // falls back to the previous notification - which is why a fresh card looked like
    // it was covered by an old one. Everything we saw is kept for a short grace
    // period so the newest card stays on what the user actually triggered.
    private sealed record FadedToast(JsonObject Row, DateTimeOffset LastSeen);
    private readonly Dictionary<string, FadedToast> fadedToasts = new(StringComparer.Ordinal);
    private static readonly TimeSpan FadedToastGrace = TimeSpan.FromSeconds(20);
    // Highest creation stamp ever read from the listener, as decimal FILETIME ticks.
    private string newestFileTime = "";
    private readonly HashSet<string> modules = new(StringComparer.Ordinal);
    private readonly HashSet<string> subscribed = new(StringComparer.Ordinal);
    private readonly SemaphoreSlim gate = new(1, 1);
    private JsonElement configuration, snapshot;
    private long cursor;
    private long updates;private readonly System.Collections.Concurrent.ConcurrentDictionary<string,long> topicCounts=new();
    internal object Diagnostics {get{var state=Volatile.Read(ref published);return new { updates=Interlocked.Read(ref updates),topics=topicCounts.ToArray().ToDictionary(x=>x.Key,x=>x.Value),backend=state is null ? new Dictionary<string,double>() : new[]{"revision","windowEvents","systemSamples","nativeHooks","windowEnumerations"}.ToDictionary(x=>x,x=>state.Snapshot.TryGetProperty(x,out var v)&&v.TryGetDouble(out var n)?n:0)};}}
    private bool initialized, closed,mediaCardDemand;
    private int disposing;
    private readonly TaskCompletionSource disposed = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Dictionary<string, string> failures = new();
    private readonly Func<JsonObject>? firstRunConfiguration;
    internal NativeSystemPresentationSession(NativeSystemClient ownedBackend, bool interactive = false, Func<JsonObject>? firstRunConfiguration = null)
    {
        backend = ownedBackend;
        this.interactive = interactive;
        this.firstRunConfiguration = firstRunConfiguration;
    }

    internal async Task<JsonObject> Initialize(CancellationToken cancellation = default)
    {
        await gate.WaitAsync(cancellation);
        try
        {
            ObjectDisposedException.ThrowIf(closed || Volatile.Read(ref disposing) != 0, this);
            if (initialized) throw new InvalidOperationException("Presentation session already initialized.");
            await LoadConfiguration(cancellation);
            snapshot = await backend.Request("snapshot", new { includeSystem = true }, cancellation);
            cursor = snapshot.GetProperty("revision").GetInt64();
            await RefreshProviders(null, cancellation);
            initialized = true;
            return Project();
        }
        catch { try { await backend.Request("unsubscribe", new { }, CancellationToken.None); } catch (Exception cleanup) when (cleanup is IOException or ObjectDisposedException or OperationCanceledException) { } subscribed.Clear(); throw; }
        finally { gate.Release(); }
    }
    internal async Task<JsonObject?> Next(int timeoutMs = 5000, CancellationToken cancellation = default)
    {
        if (timeoutMs < 1 || timeoutMs > 10000) throw new ArgumentOutOfRangeException(nameof(timeoutMs));
        await gate.WaitAsync(cancellation);
        try
        {
            ObjectDisposedException.ThrowIf(closed || Volatile.Read(ref disposing) != 0, this);
            if (!initialized) throw new InvalidOperationException("Presentation session not initialized.");
            var update = await backend.Request("next", new { afterRevision = cursor, timeoutMs }, cancellation);
            if (Volatile.Read(ref disposing) != 0) return null;
            cursor = update.GetProperty("revision").GetInt64();
            if (!update.GetProperty("changed").GetBoolean()) return null;
            var topics = update.GetProperty("changedTopics").EnumerateArray().Select(x => x.GetString()!).ToHashSet(StringComparer.Ordinal);
            ChangedTopics=topics;Interlocked.Increment(ref updates);foreach(string topic in topics)topicCounts.AddOrUpdate(topic,1,(_,value)=>value+1);
            bool reconfigured = topics.Contains("configuration");
            if (reconfigured) await LoadConfiguration(cancellation);
            var incoming = JsonNode.Parse(update.GetProperty("snapshot").GetRawText())!.AsObject();
            // A window-only event carries no new system sample. Retain the last
            // sample instead of making memory/power modules flicker unavailable.
            if (!incoming.ContainsKey("system") && snapshot.TryGetProperty("system", out var system)) incoming["system"] = JsonNode.Parse(system.GetRawText());
            using (var merged = JsonDocument.Parse(incoming.ToJsonString())) snapshot = merged.RootElement.Clone();
            await RefreshProviders(reconfigured ? null : topics, cancellation);
            return Project();
        }
        finally { gate.Release(); }
    }
    internal async Task<JsonObject> SetMediaCardDemand(bool wanted,CancellationToken cancellation){
        await gate.WaitAsync(cancellation);try{ObjectDisposedException.ThrowIf(closed||Volatile.Read(ref disposing)!=0,this);if(mediaCardDemand!=wanted){mediaCardDemand=wanted;await LoadConfiguration(cancellation);await RefreshProviders(new(StringComparer.Ordinal){"media"},cancellation);}return Project();}finally{gate.Release();}
    }
    private async Task LoadConfiguration(CancellationToken cancellation)
    {
        var read = await backend.Request("configuration-read", new { }, cancellation);
        if (!read.GetProperty("present").GetBoolean() && !initialized && firstRunConfiguration is not null)
        {
            try
            {
                await backend.Request("configuration-write", new { expectedRevision = 0, configuration = firstRunConfiguration() }, cancellation);
            }
            catch (NativeSystemRequestException)
            {
                // Another writer may have initialized this profile first. Preserve its revision.
                read = await backend.Request("configuration-read", new { }, cancellation);
                if (!read.GetProperty("present").GetBoolean()) throw;
            }
            if (!read.GetProperty("present").GetBoolean())
                read = await backend.Request("configuration-read", new { }, cancellation);
        }
        if (!read.GetProperty("present").GetBoolean()) throw new InvalidDataException("Independent configuration is absent; legacy fallback is not implicit.");
        configuration = read.GetProperty("configuration").Clone();
        providers["launches"]=await backend.Request("configuration-launch-targets",new{},cancellation);
        modules.Clear();
        foreach (string surface in new[] { "toolbar", "dock" }) foreach (string section in new[] { "left", "center", "right" })
            foreach (var row in configuration.GetProperty(surface).GetProperty("sections").GetProperty(section).EnumerateArray())
                if (row.GetProperty("kind").GetString() == "module") modules.Add(row.GetProperty("module").GetString()!);
        bool system = modules.Overlaps(["magic.cpu", "magic.memory", "magic.battery", "magic.network-usage","magic.gpu","magic.storage"]);
        await backend.Request("subscribe", new { windows = true, system, gpu=modules.Contains("magic.gpu"), storage=modules.Contains("magic.storage"), intervalMs = 10000 }, cancellation);
        var wanted = new HashSet<string>(StringComparer.Ordinal){"brightness"};
        if (modules.Overlaps(["magic.audio", "magic.quick-settings"])) wanted.Add("audio");
        if (mediaCardDemand||modules.Overlaps(["magic.player","magic.audio","magic.lyrics","magic.music"])) wanted.Add("media");
        if (modules.Overlaps(["magic.network", "magic.quick-settings"])) wanted.Add("wifi");
        if (modules.Overlaps(["magic.bluetooth", "magic.quick-settings"])) wanted.Add("bluetooth");
        if (modules.Contains("magic.notifications")) wanted.Add("notifications");
        if(interactive && modules.Contains("magic.tray"))wanted.Add("tray");
        foreach (string topic in subscribed.Except(wanted).ToArray()) { await backend.Request(topic + "-subscribe", new { enabled = false }, cancellation); subscribed.Remove(topic); providers.Remove(topic); failures.Remove(topic); }
        foreach (string topic in wanted.Except(subscribed).ToArray())
            try
            {
                var result = await backend.Request(topic + "-subscribe", topic=="tray" ? (object)new { enabled=true,production=true,initializeExisting=true } : new { enabled = true }, cancellation);
                if (result.TryGetProperty("available", out var available) && !available.GetBoolean()) failures[topic] = "unavailable";
                else subscribed.Add(topic);
            }
            catch (NativeSystemRequestException e) { failures[topic] = e.ErrorCode?.ToString() ?? "unavailable"; }
        if (!modules.Contains("magic.keyboard")) providers.Remove("input");
    }
    private async Task RefreshProviders(HashSet<string>? topics, CancellationToken cancellation)
    {
        if(topics?.Contains("notifications")==true&&modules.Contains("magic.notifications")){
            var status=await backend.Request("notifications-subscribe",new{enabled=true},cancellation);
            if(status.TryGetProperty("available",out var available)&&!available.GetBoolean()){subscribed.Remove("notifications");providers["notifications"]=await backend.Request("notifications-read",new{},cancellation);failures["notifications"]=providers["notifications"].GetProperty("access").GetString()??"unavailable";}
            else subscribed.Add("notifications");
        }
        foreach (string topic in subscribed.ToArray())
        {
            if (topics is not null && !topics.Contains(topic)) continue;
            try { providers[topic] = await backend.Request(topic + "-read", new { }, cancellation); if(topic=="tray"&&TrayProjection is not null)providers[topic]=TrayProjection(providers[topic]); failures.Remove(topic); }
            catch (NativeSystemRequestException e) { providers.Remove(topic); failures[topic] = e.ErrorCode?.ToString() ?? "unavailable"; }
        }
        if (providers.TryGetValue("notifications", out var notifications)) providers["notifications"] = KeepFadedToasts(notifications);
        if (modules.Contains("magic.keyboard") && (topics is null || topics.Contains("input") || topics.Contains("windows-system")))
            providers["input"] = await backend.Request("input-read", new { }, cancellation);
    }
    private JsonElement KeepFadedToasts(JsonElement notifications)
    {
        // A toast can be shown and then withdrawn from the system history by the app that
        // sent it, within the same second. Tailscale does exactly that with its "Copied
        // the IP address" confirmation: the read right after the click returns it, the
        // read a second later does not. Mirroring that verbatim dropped the card the user
        // had just triggered and let whatever was already in the centre take the banner
        // back, which reads as "my new card was covered by an old one".
        // Everything seen is therefore kept for a short grace period, and the newest
        // stamp ever seen is remembered so a fallback is never announced as news.
        if (notifications.ValueKind != JsonValueKind.Object || !notifications.TryGetProperty("notifications", out var listed) || listed.ValueKind != JsonValueKind.Array)
        {
            fadedToasts.Clear(); newestFileTime = "";
            return notifications;
        }
        var now = DateTimeOffset.UtcNow;
        var live = ReadNotifications(listed).ToArray();
        var alive = new HashSet<string>(StringComparer.Ordinal);
        var rows = new List<JsonObject>();
        foreach (var row in live)
        {
            string id = Text(row, "id");
            alive.Add(id); fadedToasts[id] = new FadedToast(row, now); rows.Add(row.DeepClone().AsObject());
            string created = Text(row, "createdFileTime");
            if (created.Length > newestFileTime.Length || created.Length == newestFileTime.Length && string.CompareOrdinal(created, newestFileTime) > 0) newestFileTime = created;
        }
        // An empty read that outlives the instant of a withdrawal means the centre was
        // cleared, not that one app took one toast back; ghosts must not survive that.
        bool cleared = live.Length == 0 && fadedToasts.Values.All(row => now - row.LastSeen > TimeSpan.FromSeconds(2));
        foreach (var (id, faded) in fadedToasts.ToArray())
        {
            if (alive.Contains(id)) continue;
            if (cleared || now - faded.LastSeen > FadedToastGrace) { fadedToasts.Remove(id); continue; }
            var ghost = faded.Row.DeepClone().AsObject();
            // Windows no longer holds this one, so it cannot be dismissed there either.
            ghost["transient"] = true;
            rows.Add(ghost);
        }
        while (fadedToasts.Count > 8) fadedToasts.Remove(fadedToasts.OrderBy(row => row.Value.LastSeen).First().Key);
        // Newest first, the order a notification centre is read in.
        rows.Sort((left, right) => string.CompareOrdinal(Text(right, "createdFileTime"), Text(left, "createdFileTime")));
        var copy = JsonNode.Parse(notifications.GetRawText())!.AsObject();
        var merged = new JsonArray(); foreach (var row in rows) merged.Add(row);
        copy["notifications"] = merged;
        // Only the row that is genuinely the newest ever seen may raise a banner. Once a
        // ghost expires the fallback row is older news and must stay silent.
        copy["announceLatest"] = rows.Count > 0 && Text(rows[0], "createdFileTime") == newestFileTime;
        return JsonSerializer.SerializeToElement(copy);
    }
    private static IEnumerable<JsonObject> ReadNotifications(JsonElement listed)
    {
        foreach (var row in listed.EnumerateArray())
            if (row.ValueKind == JsonValueKind.Object && JsonNode.Parse(row.GetRawText()) is JsonObject copy) yield return copy;
    }
    private static string Text(JsonObject row, string name)
    {
        if (!row.TryGetPropertyValue(name, out var value) || value is null) return "";
        if (value is JsonValue scalar && scalar.TryGetValue<string>(out var text)) return text;
        return value.ToJsonString().Trim('"');
    }
    private JsonObject Project()
    {
        Volatile.Write(ref published,new(configuration.Clone(),snapshot.Clone(),new Dictionary<string,JsonElement>(providers)));
        var view = NativeSystemPresentation.Project(snapshot, configuration, providers, interactive, backend.ProcessId);
        var errors = new JsonObject(); foreach (var (key, value) in failures) errors[key] = value;
        view["providerFailures"] = errors;
        return view;
    }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref disposing, 1) != 0) { await disposed.Task; return; }
        try
        {
            // Independent control transport wakes an outstanding next request;
            // waiting for the session gate first would delay release by 10 s.
            try { await backend.Request("unsubscribe", new { }, CancellationToken.None); }
            finally
            {
                await gate.WaitAsync();
                try
                {
                    closed = true;
                    try { await backend.Request("unsubscribe", new { }, CancellationToken.None); }
                    finally { subscribed.Clear(); providers.Clear(); failures.Clear(); modules.Clear(); configuration = default; snapshot = default; }
                }
                finally { gate.Release(); }
            }
        }
        finally { disposed.TrySetResult(); }
    }
}
