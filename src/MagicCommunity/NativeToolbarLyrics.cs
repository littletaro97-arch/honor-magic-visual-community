using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace MagicCommunity;

// Opt-in toolbar module. Fetch only on track changes; schedule once per lyric
// boundary instead of polling the player or refreshing the toolbar each frame.
internal sealed class NativeToolbarLyrics : IDisposable
{
    internal sealed record Track(string Title, string Artist, string Album, int DurationSeconds);
    internal sealed record Line(double Milliseconds, string Text);
    private sealed record Cached(Line[] Lines, DateTimeOffset Expires);
    private static readonly HttpClient Http = new(new SocketsHttpHandler { AllowAutoRedirect = false, PooledConnectionLifetime = TimeSpan.FromMinutes(5) });
    private static readonly Regex Stamp = new(@"\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\]", RegexOptions.CultureInvariant | RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(50));
    private static readonly Regex Offset = new(@"\[offset:([+-]?\d{1,7})\]", RegexOptions.CultureInvariant | RegexOptions.IgnoreCase | RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(50));
    private readonly HttpClient http;
    private readonly object gate = new();
    private readonly Action changed;
    private readonly TimeProvider clock;
    private readonly Func<Track, CancellationToken, Task<Line[]>> fetch;
    private readonly ITimer timer;
    private readonly SemaphoreSlim networkGate = new(1, 1);
    private readonly Dictionary<Track, Cached> cache = [];
    private readonly Queue<Track> cacheOrder = new();
    private CancellationTokenSource? lookup;
    private Track? track;
    private Line[] lines = [];
    private string timeline = "", text = "", lookupState = "idle";
    private long generation, positionTimestamp;
    private double position, duration, rate = 1;
    private bool playing, enabled, disposed;
    private DateTimeOffset nextRequest;
    internal string Text { get { lock (gate) return text; } }
    internal string Status { get { lock (gate) return lookupState; } }

    internal NativeToolbarLyrics(Action changed, TimeProvider? clock = null, Func<Track, CancellationToken, Task<Line[]>>? fetch = null, HttpClient? http = null)
    {
        this.changed = changed; this.clock = clock ?? TimeProvider.System; this.http = http ?? Http; this.fetch = fetch ?? Fetch;
        timer = this.clock.CreateTimer(_ => Tick(), null, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
    }

    internal void Observe(bool wanted, JsonElement media)
    {
        if (!wanted)
        {
            lock (gate)
            {
                if (disposed || !enabled && track is null && lookup is null) return;
                enabled = playing = false; ++generation; lookup?.Cancel(); lookup = null;
                track = null; lines = []; text = timeline = ""; lookupState = "idle"; cache.Clear(); cacheOrder.Clear();
                timer.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
            }
            return;
        }
        var row = NativeSystemPresentation.CurrentMedia(media);
        string title = NativeSystemPanel.Text(row, "title").Trim(), artist = NativeSystemPanel.Text(row, "artist").Trim();
        double ms = NativeSystemPanel.Number(row, "durationMs");
        var next = wanted && NativeSystemPanel.Flag(media, "available") && title.Length is > 0 and <= 256 && artist.Length is > 0 and <= 256
            ? new Track(title, artist, NativeSystemPanel.Text(row, "album")[..Math.Min(256, NativeSystemPanel.Text(row, "album").Length)], ms is >= 1000 and <= 3600000 ? (int)Math.Round(ms / 1000) : 0) : null;
        lock (gate)
        {
            if (disposed) return;
            enabled = wanted;
            if (next != track)
            {
                ++generation; lookup?.Cancel(); lookup = null; track = next; lines = []; text = ""; timeline = ""; lookupState = "idle";
                if (next is not null)
                {
                    if (cache.TryGetValue(next, out var hit) && hit.Expires > clock.GetUtcNow()) { lines = hit.Lines; lookupState = lines.Length > 0 ? "ready" : "missing"; }
                    else { lookupState = "loading"; lookup = new(); _ = Load(next, generation, lookup); }
                }
            }
            // The same projection can be republished by a lyric callback. Do not
            // reset its monotonic position anchor to an older media snapshot.
            string signature = row.ValueKind == JsonValueKind.Object ? row.GetRawText() : "";
            if (signature != timeline)
            {
                timeline = signature; duration = Math.Max(0, ms); position = Math.Max(0, NativeSystemPanel.Number(row, "positionMs"));
                playing = NativeSystemPanel.Number(row, "status") == 4;
                rate = Math.Clamp(NativeSystemPanel.Number(row, "playbackRate", 1), .25, 4);
                double updated = NativeSystemPanel.Number(row, "positionUpdatedUnixMs");
                double age = clock.GetUtcNow().ToUnixTimeMilliseconds() - updated;
                if (playing && updated > 0 && age > 0 && age < 3600000) position += age * rate;
                positionTimestamp = clock.GetTimestamp();
            }
            Advance();
        }
    }

    internal void Project(JsonObject view, JsonElement media, bool cardDemand = false)
    {
        var rows = new[] { "left", "center", "right" }.SelectMany(s => view["toolbar"]?[s]?.AsArray() ?? []);
        var targets = rows.Where(r => r?["nativeModule"]?.GetValue<string>() == "magic.music").ToArray();
        bool visible = view["monitors"]?.AsArray().Any(m => m?["toolbar"]?.GetValue<bool>() == true) == true;
        Observe(cardDemand || visible && targets.Length != 0, media);
        string current, state; lock(gate) { current = text; state = lookupState; }
        foreach (var row in targets)
        {
            row!["lyricsText"] = row["playing"]?.GetValue<bool>()==true?current:"";
            string title=row["label"]?.GetValue<string>()??"", artist=row["artist"]?.GetValue<string>()??"";
            row["lyricsStatus"] = state;
            string status=state switch { "loading"=>"正在查找同步歌词", "missing"=>"暂无匹配的同步歌词", "error"=>"歌词源暂时不可用", _=>"" };
            row["tooltip"] = string.Join(" · ",new[]{title,artist,status,row["playing"]?.GetValue<bool>()==true?"点击左侧暂停":"点击左侧播放"}.Where(s=>s.Length>0));
        }
    }

    private async Task Load(Track requested, long revision, CancellationTokenSource source)
    {
        try
        {
            // Keep rapid track switching off the UI thread and coalesce it.
            await Task.Delay(TimeSpan.FromMilliseconds(350), clock, source.Token).ConfigureAwait(false);
            Line[] found; bool failed = false;
            try { found = await fetch(requested, source.Token).ConfigureAwait(false); }
            catch (Exception e) when (e is HttpRequestException or IOException or JsonException or InvalidDataException or OperationCanceledException or RegexMatchTimeoutException or ArgumentException)
            { if (source.IsCancellationRequested) return; found = []; failed = true; }
            lock (gate)
            {
                if (disposed || source.IsCancellationRequested || revision != generation || track != requested) return;
                if (!failed) {
                    if (!cache.ContainsKey(requested)) cacheOrder.Enqueue(requested);
                    cache[requested] = new(found, clock.GetUtcNow().AddMinutes(found.Length > 0 ? 30 : 5));
                }
                while (cacheOrder.Count > 16) cache.Remove(cacheOrder.Dequeue());
                lines = found; lookupState = failed ? "error" : found.Length > 0 ? "ready" : "missing"; Advance();
            }
            changed(); // Publish missing/error too, even when there was no lyric text.
        }
        catch (OperationCanceledException) { }
        finally { lock (gate) { if (ReferenceEquals(lookup, source)) lookup = null; } source.Dispose(); }
    }

    private bool Advance()
    {
        timer.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
        double now = position + (playing ? clock.GetElapsedTime(positionTimestamp).TotalMilliseconds * rate : 0);
        if (duration > 0) now = Math.Min(now, duration);
        int index = -1;
        if (enabled && track is not null) { for (int i = 0; i < lines.Length && lines[i].Milliseconds <= now; ++i) index = i; }
        string next = index >= 0 && (duration <= 0 || now < duration) ? lines[index].Text : "";
        bool different = text != next; text = next;
        if (enabled && playing && track is not null && lines.Length > 0 && (duration <= 0 || now < duration))
        {
            double boundary = index + 1 < lines.Length ? lines[index + 1].Milliseconds : duration;
            if (duration > 0) boundary = Math.Min(boundary, duration);
            if (boundary > now) timer.Change(TimeSpan.FromMilliseconds(Math.Clamp((boundary - now) / rate, 25, 3600000)), Timeout.InfiniteTimeSpan);
        }
        return different;
    }
    private void Tick() { bool notify; lock (gate) { if (disposed) return; notify = Advance(); } if (notify) changed(); }

    internal static string Limit(string value)
    {
        var clean = string.Concat(value.EnumerateRunes().Where(r => !Rune.IsControl(r) && (Rune.GetUnicodeCategory(r) != UnicodeCategory.Format || r.Value is 0x200C or 0x200D)).Select(r => r.ToString())).Trim();
        int[] starts = StringInfo.ParseCombiningCharacters(clean);
        // Bound transport/cache memory; visible ellipsis is measured by DirectWrite.
        return starts.Length <= 256 ? clean : clean[..starts[255]] + "…";
    }
    internal static Line[] ParseLrc(string value)
    {
        if (value.Length > 131072) return [];
        double offset = 0; var tag = Offset.Match(value); if (tag.Success) double.TryParse(tag.Groups[1].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out offset);
        var result = new List<Line>();
        foreach (string raw in value.Split('\n'))
        {
            if (raw.Length > 4096) continue;
            string row = raw.Trim(); var stamps = Stamp.Matches(row); if (stamps.Count == 0 || stamps[0].Index != 0) continue;
            int end = 0; var times = new List<double>();
            foreach (Match stamp in stamps)
            {
                if (stamp.Index != end || times.Count >= 16) break;
                end = stamp.Index + stamp.Length;
                double seconds = double.Parse(stamp.Groups[2].Value, CultureInfo.InvariantCulture); if (seconds >= 60) continue;
                double fraction = stamp.Groups[3].Success ? double.Parse("0." + stamp.Groups[3].Value, CultureInfo.InvariantCulture) : 0;
                double at = (double.Parse(stamp.Groups[1].Value, CultureInfo.InvariantCulture) * 60 + seconds + fraction) * 1000 - offset;
                if (at is >= 0 and <= 3600000) times.Add(at);
            }
            string lyric = Limit(row[end..]);
            foreach (double at in times) { if (result.Count >= 1024) return result.OrderBy(l => l.Milliseconds).ToArray(); result.Add(new(at, lyric)); }
        }
        return result.OrderBy(l => l.Milliseconds).GroupBy(l => l.Milliseconds).Select(g => g.Last()).ToArray();
    }

    internal static Uri Query(Track value)
    {
        string query = "https://lrclib.net/api/get?track_name=" + Uri.EscapeDataString(value.Title) + "&artist_name=" + Uri.EscapeDataString(value.Artist);
        if (value.Album.Length > 0) query += "&album_name=" + Uri.EscapeDataString(value.Album);
        if (value.DurationSeconds > 0) query += "&duration=" + value.DurationSeconds.ToString(CultureInfo.InvariantCulture);
        return new(query);
    }
    private static string MatchKey(string value) => string.Concat(value.Normalize(NormalizationForm.FormKC).Where(c=>!char.IsWhiteSpace(c))).ToUpperInvariant();
    // Catalogue punctuation may differ; retain every word inside the brackets
    // so a named version can never fall through to a different recording.
    private static string TitleKey(string value) => MatchKey(value).Replace("(", "").Replace(")", "");
    private static string[] Artists(string value) => value.Normalize(NormalizationForm.FormKC)
        .Split(['/', '、', ',', '&', ';', '•'],StringSplitOptions.RemoveEmptyEntries).Select(MatchKey).Where(s=>s.Length>0).Distinct().Order().ToArray();
    internal static string SearchTitle(string value)
    {
        // Only descriptive soundtrack annotations; never strip Live/Remix/cover/version labels.
        string title=value.Normalize(NormalizationForm.FormKC).Trim();int at=title.LastIndexOf('(');
        if(at>0&&title.EndsWith(')')){
            string suffix=title[(at+1)..^1];
            if(new[]{"主题曲","片尾曲","插曲","同人曲"}.Any(suffix.Contains)&&
                !Regex.IsMatch(suffix,@"live|remix|伴奏|翻唱|现场|改编",RegexOptions.IgnoreCase|RegexOptions.CultureInvariant))title=title[..at].Trim();
        }
        return title;
    }
    internal static Uri SearchQuery(Track value) => new("https://lrclib.net/api/search?track_name="+Uri.EscapeDataString(string.Join(" ",SearchTitle(value.Title).Normalize(NormalizationForm.FormKC).Replace("("," ").Replace(")"," ").Split((char[]?)null,StringSplitOptions.RemoveEmptyEntries))));
    internal static Line[] ReadResponse(Track requested, JsonElement response)
    {
        string title=NativeSystemPanel.Text(response,"trackName");
        bool exact=TitleKey(requested.Title)==TitleKey(title);
        bool annotation=requested.DurationSeconds>0&&TitleKey(SearchTitle(requested.Title))==TitleKey(SearchTitle(title));
        if ((!exact&&!annotation) || !Artists(requested.Artist).SequenceEqual(Artists(NativeSystemPanel.Text(response,"artistName"))) ||
            requested.DurationSeconds > 0 && Math.Abs(NativeSystemPanel.Number(response, "duration") - requested.DurationSeconds) > 2 ||
            NativeSystemPanel.Flag(response, "instrumental")) return [];
        return ParseLrc(NativeSystemPanel.Text(response, "syncedLyrics"));
    }
    internal static Line[] ReadSearch(Track requested,JsonElement response)
    {
        if(response.ValueKind!=JsonValueKind.Array)return [];
        var candidates=response.EnumerateArray().Take(20).Select(r=>new{Row=r,Lines=ReadResponse(requested,r)})
            .Where(r=>r.Lines.Length>0).OrderByDescending(r=>MatchKey(NativeSystemPanel.Text(r.Row,"trackName"))==MatchKey(requested.Title))
            .ThenByDescending(r=>requested.Album.Length>0&&MatchKey(NativeSystemPanel.Text(r.Row,"albumName"))==MatchKey(requested.Album))
            .ThenBy(r=>Math.Abs(NativeSystemPanel.Number(r.Row,"duration")-requested.DurationSeconds)).ToArray();
        if(candidates.Length==0)return [];
        if(requested.DurationSeconds==0&&candidates.Max(r=>NativeSystemPanel.Number(r.Row,"duration"))-candidates.Min(r=>NativeSystemPanel.Number(r.Row,"duration"))>2)return [];
        return candidates[0].Lines;
    }
    private async Task<(JsonElement Value,bool CanSearch)> ReadHttp(Uri uri,int limit,CancellationToken cancellation)
    {
        TimeSpan delay;lock(gate)delay=nextRequest-clock.GetUtcNow();
        if(delay>TimeSpan.FromMinutes(5))throw new HttpRequestException("Lyrics source is in Retry-After backoff.");
        if(delay>TimeSpan.Zero)await Task.Delay(delay,clock,cancellation).ConfigureAwait(false);
        lock(gate)nextRequest=clock.GetUtcNow().AddMilliseconds(500);
        using var timeout=CancellationTokenSource.CreateLinkedTokenSource(cancellation);timeout.CancelAfter(TimeSpan.FromSeconds(8));
        using var request=new HttpRequestMessage(HttpMethod.Get,uri);
        string version=typeof(NativeToolbarLyrics).Assembly.GetName().Version?.ToString()??"0.0";
        request.Headers.UserAgent.ParseAdd("MagicVisualCommunity/"+version+" (+https://github.com/littletaro97-arch/honor-magic-visual-community)");
        using var response=await http.SendAsync(request,HttpCompletionOption.ResponseHeadersRead,timeout.Token).ConfigureAwait(false);
        if(response.StatusCode is HttpStatusCode.TooManyRequests or HttpStatusCode.ServiceUnavailable){
            var retry=response.Headers.RetryAfter;var backoff=retry?.Delta??(retry?.Date is {} date?date-clock.GetUtcNow():TimeSpan.FromMinutes(1));
            lock(gate){var now=clock.GetUtcNow();double seconds=Math.Max(backoff.TotalSeconds,1)+Random.Shared.NextDouble();nextRequest=seconds<(DateTimeOffset.MaxValue-now).TotalSeconds?now.AddSeconds(seconds):DateTimeOffset.MaxValue;}
            throw new HttpRequestException("Lyrics source temporarily unavailable.",null,response.StatusCode);
        }
        if(response.StatusCode==HttpStatusCode.NotFound)return(default,true);
        response.EnsureSuccessStatusCode();
        if(response.Content.Headers.ContentLength>limit||!string.Equals(response.Content.Headers.ContentType?.MediaType,"application/json",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Invalid lyrics response.");
        using var stream=await response.Content.ReadAsStreamAsync(timeout.Token).ConfigureAwait(false);
        using var buffer=new MemoryStream();byte[] bytes=new byte[8192];int read;
        while((read=await stream.ReadAsync(bytes,timeout.Token).ConfigureAwait(false))!=0){if(buffer.Length+read>limit)throw new InvalidDataException("Lyrics response exceeds limit.");buffer.Write(bytes,0,read);}
        using var json=JsonDocument.Parse(buffer.ToArray(),new JsonDocumentOptions{MaxDepth=8});return(json.RootElement.Clone(),true);
    }
    internal async Task<Line[]> Fetch(Track value, CancellationToken cancellation)
    {
        await networkGate.WaitAsync(cancellation).ConfigureAwait(false);
        try{
            var exact=await ReadHttp(Query(value),262144,cancellation).ConfigureAwait(false);
            var lines=ReadResponse(value,exact.Value);if(lines.Length>0||!exact.CanSearch)return lines;
            // One bounded fallback per new track. Search tolerates catalogue album
            // and artist separators; identity/version/duration are rechecked locally.
            var search=await ReadHttp(SearchQuery(value),1048576,cancellation).ConfigureAwait(false);
            return ReadSearch(value,search.Value);
        }finally{networkGate.Release();}
    }
    public void Dispose()
    {
        lock (gate) { if (disposed) return; disposed = true; ++generation; lookup?.Cancel(); lookup = null; timer.Dispose(); lines = []; text = ""; cache.Clear(); cacheOrder.Clear(); }
    }
}
