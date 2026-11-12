using MagicCommunity;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

internal static class ToolbarMusicChecks
{
    private sealed class Handler(Func<int,HttpRequestMessage,HttpResponseMessage> reply):HttpMessageHandler{
        internal int Count;internal readonly List<string> Paths=[];
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request,CancellationToken token){Paths.Add(request.RequestUri!.AbsolutePath);return Task.FromResult(reply(++Count,request));}
    }
    private static JsonElement Record(string title="我不曾忘记",string artist="花玲、张安琪、沐霏",double duration=231,string album="我不曾忘记") => JsonSerializer.SerializeToElement(new{trackName=title,artistName=artist,duration,albumName=album,syncedLyrics="[00:00]Owned fixture line",instrumental=false});
    internal static async Task Run(string output){
        var checks=new Dictionary<string,bool>();void Check(string name,bool pass){checks[name]=pass;if(!pass)throw new InvalidDataException(name);}
        var config=NativeSystemBarDefaults.CreateInstalledConfiguration();
        var left=config["toolbar"]!["sections"]!["left"]!.AsArray();var center=config["toolbar"]!["sections"]!["center"]!.AsArray();
        left.Add(new JsonObject{["id"]="former-media",["kind"]="module",["module"]="magic.player"});
        center.Add(new JsonObject{["id"]="former-lyrics",["kind"]="module",["module"]="magic.lyrics"});
        center.Add(new JsonObject{["id"]="external",["kind"]="compatibility",["sourceModuleId"]="external/test"});
        config["dock"]!["sections"]!["right"]!.AsArray().Add(new JsonObject{["id"]="dock-media",["kind"]="module",["module"]="magic.player"});
        var snapshot=JsonSerializer.SerializeToElement(new{protocol="Magic.System.Native/1",displays=Array.Empty<object>(),windows=Array.Empty<object>()});
        var media=JsonSerializer.SerializeToElement(new{available=true,sessions=new[]{new{id="background",trackId="other",current=false,status=4,title="Other",canPlay=true,canPause=true,canPrevious=true,canNext=true},new{id="chosen",trackId="version",current=true,status=5,title="Chosen",canPlay=true,canPause=false,canPrevious=false,canNext=true}}});
        var projected=NativeSystemPresentation.Project(snapshot,JsonSerializer.SerializeToElement(config),new Dictionary<string,JsonElement>{{"media",media}},true);
        var rows=new[]{"left","center","right"}.SelectMany(s=>projected["toolbar"]![s]!.AsArray()).ToArray();
        var music=rows.Single(r=>r?["nativeModule"]?.GetValue<string>()=="magic.music")!;
        Check("legacy-projection-single-music-retains-lyric-position",music["id"]!.GetValue<string>()=="former-lyrics"&&projected["toolbar"]!["center"]![1]!["id"]!.GetValue<string>()=="former-lyrics");
        Check("actions-and-lyrics-choose-the-same-current-session",music["nativeSessionId"]!.GetValue<string>()=="chosen"&&music["nativeTrackId"]!.GetValue<string>()=="version"&&!music["playing"]!.GetValue<bool>()&&!music["canPrevious"]!.GetValue<bool>());
        Check("toolbar-catalog-has-only-music",!projected["toolbarModules"]!.AsArray().Any(r=>r!["id"]!.GetValue<string>() is "magic.player" or "magic.lyrics"));
        var originalDock=config["dock"]!.ToJsonString();NativeSystemBarDefaults.NormalizeToolbarMusic(config);
        Check("migration-keeps-dock-and-external-order",config["dock"]!.ToJsonString()==originalDock&&center[1]!["id"]!.GetValue<string>()=="former-lyrics"&&center[2]!["id"]!.GetValue<string>()=="external"&&!left.Any(r=>r!["id"]!.GetValue<string>()=="former-media"));
        var once=config.ToJsonString();NativeSystemBarDefaults.NormalizeToolbarMusic(config);Check("migration-idempotent",once==config.ToJsonString());
        center.RemoveAt(1);left.Add(new JsonObject{["id"]="media-only",["kind"]="module",["module"]="magic.player"});NativeSystemBarDefaults.NormalizeToolbarMusic(config);Check("media-only-configuration-retains-position",left[^1]!["id"]!.GetValue<string>()=="media-only"&&left[^1]!["module"]!.GetValue<string>()=="magic.music");
        var track=new NativeToolbarLyrics.Track("我不曾忘记","花玲/张安琪/沐霏","",231);
        foreach(var separator in new[]{"、",", "," & ","／"})Check("artist-separator-"+separator,NativeToolbarLyrics.ReadResponse(track,Record(artist:string.Join(separator,new[]{"沐霏","花玲","张安琪"}))).Length==1);
        Check("wrong-performer-rejected",NativeToolbarLyrics.ReadResponse(track,Record(artist:"花玲")).Length==0);
        Check("different-duration-rejected",NativeToolbarLyrics.ReadResponse(track,Record(duration:289)).Length==0);
        Check("different-live-version-rejected",NativeToolbarLyrics.ReadResponse(track,Record(title:"我不曾忘记 (Live)")).Length==0);
        var star=new NativeToolbarLyrics.Track("提瓦特民谣 (星空版)","鹿喑kana / 多多poi / Mace / 陶典 / 孙晔 / 花玲","",214);
        Check("version-brackets-only-do-not-block-match",NativeToolbarLyrics.ReadResponse(star,Record(title:"提瓦特民谣 星空版",artist:"鹿喑kana; 多多poi; Mace; 陶典; 孙晔; 花玲",duration:214)).Length==1);
        Check("version-words-still-required",NativeToolbarLyrics.ReadResponse(star,Record(title:"提瓦特民谣",artist:star.Artist,duration:214)).Length==0);
        Check("search-preserves-version-words",Uri.UnescapeDataString(NativeToolbarLyrics.SearchQuery(star).Query).Contains("星空版")&&!NativeToolbarLyrics.SearchQuery(star).Query.Contains("%28"));
        Check("soundtrack-annotation-matches-with-known-duration",NativeToolbarLyrics.ReadResponse(track,Record(title:"我不曾忘记 (游戏《原神》2023新春会同人曲)")).Length==1);
        Check("annotation-not-relaxed-without-duration",NativeToolbarLyrics.ReadResponse(track with{DurationSeconds=0},Record(title:"我不曾忘记 (游戏《原神》2023新春会同人曲)")).Length==0);
        Check("ordinary-parenthetical-title-preserved",NativeToolbarLyrics.SearchTitle("Song (Part II)")=="Song (Part II)");
        var search=JsonSerializer.SerializeToElement(new[]{Record(artist:"Someone else"),Record(duration:289),Record()});
        Check("search-revalidates-identity-and-duration",NativeToolbarLyrics.ReadSearch(track,search).Length==1);
        Check("unknown-duration-ambiguous-records-hidden",NativeToolbarLyrics.ReadSearch(track with{DurationSeconds=0},JsonSerializer.SerializeToElement(new[]{Record(duration:230),Record(duration:289)})).Length==0);
        static HttpResponseMessage Json(object value)=>new(HttpStatusCode.OK){Content=new StringContent(JsonSerializer.Serialize(value),Encoding.UTF8,"application/json")};
        using(var handler=new Handler((n,_)=>n==1?new(HttpStatusCode.NotFound):Json(new[]{Record()})))using(var http=new HttpClient(handler))using(var lyrics=new NativeToolbarLyrics(()=>{},http:http)){
            Check("failed-exact-request-has-one-search-fallback",(await lyrics.Fetch(track,CancellationToken.None)).Length==1&&handler.Count==2&&handler.Paths.SequenceEqual(new[]{"/api/get","/api/search"}));
        }
        using(var handler=new Handler((_,_)=>Json(Record())))using(var http=new HttpClient(handler))using(var lyrics=new NativeToolbarLyrics(()=>{},http:http)){
            Check("exact-hit-does-not-add-network-request",(await lyrics.Fetch(track,CancellationToken.None)).Length==1&&handler.Count==1);
        }
        using(var handler=new Handler((_,_)=>new(HttpStatusCode.TooManyRequests)))using(var http=new HttpClient(handler))using(var lyrics=new NativeToolbarLyrics(()=>{},http:http)){
            bool limited=false;try{await lyrics.Fetch(track,CancellationToken.None);}catch(HttpRequestException){limited=true;}
            Check("rate-limit-does-not-fan-out-search-or-report-missing",limited&&handler.Count==1);
        }
        File.WriteAllText(output,JsonSerializer.Serialize(new{allPass=true,checks,scope="Owned configuration, snapshots and mock HTTP. No playback, network or installed settings changed."},new JsonSerializerOptions{WriteIndented=true}));
    }
    internal static async Task Live(string output){
        var result=new List<object>();using var lyrics=new NativeToolbarLyrics(()=>{});
        foreach(var track in new[]{new NativeToolbarLyrics.Track("提瓦特民谣 (星空版)","鹿喑kana / 多多poi / Mace / 陶典 / 孙晔 / 花玲","",214),new NativeToolbarLyrics.Track("昔涟","张韶涵 / HOYO-MiX","",186),new NativeToolbarLyrics.Track("我不曾忘记","花玲/张安琪/沐霏","我不曾忘记",231),new NativeToolbarLyrics.Track("我不曾忘记 (游戏《原神》2023新春会同人曲)","花玲/张安琪/沐霏","我不曾忘记 (游戏《原神》2023新春会同人曲)",231),new NativeToolbarLyrics.Track("La vaguelette","HOYO-MiX","Genshin Impact - La vaguelette (Original Game Soundtrack)",150)}){
            var lines=await lyrics.Fetch(track,CancellationToken.None);result.Add(new{track.Title,track.Artist,track.DurationSeconds,matched=lines.Length>0,lineCount=lines.Length});
        }
        File.WriteAllText(output,JsonSerializer.Serialize(new{samples=result,scope="Only user-specified public track metadata. Durations from supplied screenshots or public catalogue, not live QQ playback. Lyrics bodies not retained."},new JsonSerializerOptions{WriteIndented=true}));
    }
}
