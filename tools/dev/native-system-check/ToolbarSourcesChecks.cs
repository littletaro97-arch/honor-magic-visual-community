using MagicCommunity;
using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;

internal static class ToolbarSourcesChecks
{
    private static JsonElement Source(string id, string app, bool playing = false, bool current = false,
        bool previous = true, string? title = null, string artist = "Owned Artist") => JsonSerializer.SerializeToElement(new {
            id, appId = app, trackId = id + ":revision", title = title ?? "Title " + id, artist, status = playing ? 4 : 5,
            current, canPlay = true, canPause = true, canPrevious = previous, canNext = true,
            durationMs = 120000, positionMs = 1000
        });
    private static JsonElement Media(params JsonElement[] rows) => JsonSerializer.SerializeToElement(new { available = true, sessions = rows });
    private static JsonObject View(bool visible = true) => JsonNode.Parse("""
        {"toolbar":{"left":[],"center":[{"id":"music","nativeModule":"magic.music"}],"right":[]},"monitors":[{"toolbar":true}]}
        """)!.AsObject().WithVisibility(visible);
    private static JsonObject WithVisibility(this JsonObject view, bool visible) { view["monitors"]![0]!["toolbar"] = visible; return view; }
    internal static async Task Run(string output)
    {
        var checks = new Dictionary<string, bool>();
        void Check(string name, bool pass) { checks[name] = pass; if (!pass) throw new InvalidDataException(name); }
        static string[] Ids(NativeToolbarMusic music) => music.Sources.Select(r => NativeSystemPanel.Text(r, "id")).ToArray();
        static string Label(NativeToolbarMusic music, string id) => music.Panel().Data["rows"]!.AsArray().Single(r => r!["id"]!.GetValue<string>() == "music-source:" + id)!["label"]!.GetValue<string>();
        static Task<NativeToolbarLyrics.Line[]> Empty(NativeToolbarLyrics.Track _, CancellationToken ct) => Task.FromResult<NativeToolbarLyrics.Line[]>([]);
        var a = Source("a", "player-a", current: true); var b = Source("b", "player-b"); var c = Source("c", "player-c"); var d = Source("d", "player-b");
        var state = Media(b, a, c, d); using var music = new NativeToolbarMusic(() => { }, fetch: Empty);
        var view = View(); music.Project(view, state, false);
        Check("current-initially-first-same-app-sessions-remain-distinct", Ids(music).SequenceEqual(new[] { "a", "b", "c", "d" }));
        Check("paused-source-uses-title-in-bar", view["toolbar"]!["center"]![0]!["lyricsText"]!.GetValue<string>() == "Title a");
        music.Change("c", "a"); music.Project(view, state, true);
        Check("drag-order-selects-bar-and-panel-same-source", Ids(music).SequenceEqual(new[] { "c", "a", "b", "d" }) && view["toolbar"]!["center"]![0]!["nativeSessionId"]!.GetValue<string>() == "c");
        music.Change("b"); music.Change("c", "b"); music.Change("b", "d"); music.Observe(state, true, false);
        Check("pin-first-and-locked-other-sources-still-reorder", Ids(music).SequenceEqual(new[] { "b", "c", "a", "d" }) && music.Pinned == "b");
        music.Change("b"); music.Change("d", "b");
        Check("unpin-restores-free-reorder", music.Pinned == "" && Ids(music).SequenceEqual(new[] { "d", "b", "c", "a" }));
        music.Change("c"); music.Change("a");
        Check("pin-another-replaces-single-pin", music.Pinned == "a" && Ids(music)[0] == "a" && music.Panel().Data["rows"]!.AsArray().Count(r => r!["control"]!.GetValue<string>() == "music-pin" && r["active"]!.GetValue<bool>()) == 1);
        music.Observe(Media(b, c, d), true, true);
        Check("closed-pinned-session-falls-back-to-live-source", music.Pinned == "" && Ids(music)[0] == "c" && !music.Panel().Commands.ContainsKey("music-play:a"));
        bool stale = false; try { music.Change("a"); } catch (InvalidOperationException) { stale = true; }
        Check("stale-drag-pin-rejected", stale);
        var a2 = Source("a2", "player-a"); music.Observe(Media(b, c, d, a2), true, true);
        Check("restarted-pinned-app-restores-with-new-session-id", music.Pinned == "a2" && Ids(music)[0] == "a2");
        var panel = music.Panel(); var play = panel.Commands["music-play:a2"];
        Check("transport-commands-bind-session-and-track", play.Operation == "media-action" && play.Arguments["sessionId"]!.GetValue<string>() == "a2" && play.Arguments["trackId"]!.GetValue<string>() == "a2:revision" && play.Arguments["action"]!.GetValue<string>() == "play");
        music.Observe(Media(Source("restricted", "player", previous: false)), false, true);
        Check("unavailable-transport-action-not-offered", !music.Panel().Commands.ContainsKey("music-previous:restricted") && music.Panel().Commands.ContainsKey("music-next:restricted"));
        music.Observe(Media(Enumerable.Range(0, 40).Select(i => Source("source-" + i, "app-" + i)).ToArray()), false, true);
        Check("native-source-bound-fits-command-row-limit", music.Sources.Length == 32 && music.Panel().Data["rows"]!.AsArray().Count == 160);
        music.Observe(Media(), true, true); music.Project(view, Media(), true);
        Check("empty-panel-and-bar-state", music.Panel().Data["rows"]!.AsArray().Count == 1 && !view["toolbar"]!["center"]![0]!["hasPlayer"]!.GetValue<bool>());

        string fixture = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(output))!, "preferences-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(fixture);
        using (var saved = new NativeToolbarMusic(() => { }, fixture, File.WriteAllText, Empty)) { saved.Observe(state, false, false); saved.Change("c", "a"); saved.Change("b"); }
        string json = File.ReadAllText(Path.Combine(fixture, "toolbar-music.json"));
        Check("preferences-persist-only-app-order-and-pin", json.Contains("player-b") && !json.Contains("revision") && !json.Contains("Title"));
        using (var restored = new NativeToolbarMusic(() => { }, fixture, File.WriteAllText, Empty)) {
            restored.Observe(Media(Source("new-a", "player-a"), Source("new-c", "player-c"), Source("new-b", "player-b")), false, false);
            Check("preference-roundtrip-across-session-restart", Ids(restored).SequenceEqual(new[] { "new-b", "new-c", "new-a" }) && restored.Pinned == "new-b");
        }
        using (var failing = new NativeToolbarMusic(() => { }, fixture, (_, _) => throw new IOException("Owned write failure"), Empty)) {
            failing.Observe(state, false, false); var before = Ids(failing); var pin = failing.Pinned; bool failed = false;
            try { failing.Change("a"); } catch (IOException) { failed = true; }
            Check("write-failure-leaves-order-and-pin-unchanged", failed && before.SequenceEqual(Ids(failing)) && pin == failing.Pinned);
        }
        File.WriteAllText(Path.Combine(fixture, "toolbar-music.json"), "{broken");
        using (var malformed = new NativeToolbarMusic(() => { }, fixture, fetch: Empty)) { malformed.Observe(state, false, false); Check("invalid-preference-file-safe-default", Ids(malformed)[0] == "a"); }
        var unicode = Media(Enumerable.Range(0, 32).Select(i => Source("unicode-" + i, new string('音', 500) + i)).ToArray());
        using (var saved = new NativeToolbarMusic(() => { }, fixture, File.WriteAllText, Empty)) { saved.Observe(unicode, false, false); saved.Change("unicode-31"); }
        using (var restored = new NativeToolbarMusic(() => { }, fixture, fetch: Empty)) { restored.Observe(unicode, false, false); Check("bounded-unicode-preferences-roundtrip", restored.Pinned == "unicode-31"); }

        var calls = new ConcurrentDictionary<string, int>(); int notifications = 0;
        using var lyrics = new NativeToolbarMusic(() => Interlocked.Increment(ref notifications), fetch: (track, ct) => {
            calls.AddOrUpdate(track.Title, 1, (_, n) => n + 1);
            return Task.FromResult<NativeToolbarLyrics.Line[]>([new(0, "Lyric " + track.Title), new(90000, "Next fixture line")]);
        });
        var active = Media(Source("one", "app-one", playing: true, current: true), Source("two", "app-two", playing: true), Source("paused", "app-three"));
        lyrics.Project(View(), active, false); await Task.Delay(600);
        Check("closed-panel-only-current-playing-source-fetches", calls.Count == 1 && calls.ContainsKey("Title one"));
        lyrics.Project(View(), active, true); await Task.Delay(600);
        Check("open-panel-playing-sources-have-isolated-lyrics", calls.Count == 2 && Label(lyrics, "one") == "Lyric Title one" && Label(lyrics, "two") == "Lyric Title two" && Label(lyrics, "paused") == "Title paused");
        Check("lyric-result-triggers-event-projection", notifications >= 2);
        var shared = lyrics.CardState();
        Check("card-and-bar-share-primary-source-and-lyrics", NativeSystemPanel.Flag(shared.Provider,"available") && NativeSystemPanel.Text(NativeSystemPanel.Array(shared.Provider,"sessions").Single(),"id")=="one" && shared.Lyrics=="Lyric Title one");
        lyrics.Change("two"); var pinnedView=View(); lyrics.Project(pinnedView,active,true);
        shared=lyrics.CardState();
        Check("card-follows-pinned-source-without-independent-lookup", NativeSystemPanel.Text(NativeSystemPanel.Array(shared.Provider,"sessions").Single(),"id")=="two" && shared.Lyrics==pinnedView["toolbar"]!["center"]![0]!["lyricsText"]!.GetValue<string>() && calls.Values.Sum()==2);
        lyrics.Project(View(false),active,false,cardDemand:true); await Task.Delay(450);
        Check("card-only-demand-retains-shared-lyric-engine", lyrics.CardState().Lyrics=="Lyric Title two" && calls.Values.Sum()==2);
        lyrics.Project(View(),Media(Source("two","app-two")),false,cardDemand:true);
        Check("paused-card-clears-lyrics-and-retains-media", lyrics.CardState().Lyrics=="" && NativeSystemPanel.Array(lyrics.CardState().Provider,"sessions").Count()==1);
        lyrics.Project(View(),JsonSerializer.SerializeToElement(new{available=false,sessions=System.Array.Empty<JsonElement>()}),false,cardDemand:true);
        Check("card-preserves-provider-unavailable-state", !NativeSystemPanel.Flag(lyrics.CardState().Provider,"available") && !NativeSystemPanel.Array(lyrics.CardState().Provider,"sessions").Any());
        foreach(bool offline in new[]{false,true}) {
            using var unavailable=new NativeToolbarMusic(()=>{},fetch:(_,_)=>offline?Task.FromException<NativeToolbarLyrics.Line[]>(new HttpRequestException("Owned offline fixture")):Empty(default!,CancellationToken.None));
            var missingView=View();var missingMedia=Media(Source("unavailable","test-player",playing:true));
            unavailable.Project(missingView,missingMedia,false);await Task.Delay(600);unavailable.Project(missingView,missingMedia,false);
            Check("multi-source-lookup-status-preserves-bug-fix-"+offline,missingView["toolbar"]!["center"]![0]!["lyricsStatus"]!.GetValue<string>()==(offline?"error":"missing"));
        }
        lyrics.Observe(Media(Source("missing", "app", playing: true, artist: "")), true, true);
        Check("missing-lyric-metadata-uses-title", Label(lyrics, "missing") == "Title missing");
        int requests = calls.Values.Sum(); lyrics.Project(View(false), Media(Source("hidden", "app", playing: true)), false); await Task.Delay(450);
        Check("hidden-module-no-lyric-network-work", calls.Values.Sum() == requests);
        using var cancelled = new NativeToolbarMusic(() => { }, fetch: (track, ct) => { calls.AddOrUpdate(track.Title, 1, (_, n) => n + 1); return Empty(track, ct); });
        cancelled.Observe(Media(Source("cancelled", "app", playing: true)), false, true); cancelled.Observe(Media(), false, false); await Task.Delay(450);
        Check("close-before-debounce-cancels-work", !calls.ContainsKey("Title cancelled"));
        File.WriteAllText(output, JsonSerializer.Serialize(new { allPass = true, checks, scope = "Owned snapshots, isolated preference files and mock lyrics; no live media commands, network or installed configuration changes." }, new JsonSerializerOptions { WriteIndented = true }));
    }
}
