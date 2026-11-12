using MagicCommunity;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

internal static class ToolbarLyricsChecks
{
    private sealed class Clock : TimeProvider
    {
        private readonly object gate = new();
        private long ticks;
        private readonly List<Alarm> alarms = [];
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;
        public override long GetTimestamp() { lock(gate)return ticks; }
        public override DateTimeOffset GetUtcNow() => new DateTimeOffset(2026,11,7,0,0,0,TimeSpan.Zero).AddTicks(GetTimestamp());
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan due, TimeSpan period)
        { var alarm = new Alarm(this,callback,state);lock(gate)alarms.Add(alarm);alarm.Change(due,period);return alarm; }
        internal int Active {get{lock(gate)return alarms.Count(a=>a.Due!=long.MaxValue);}}
        internal void Advance(int ms)
        {
            long end=GetTimestamp()+TimeSpan.FromMilliseconds(ms).Ticks;
            while(true){Alarm? next;lock(gate){next=alarms.Where(a=>a.Due<=end).OrderBy(a=>a.Due).FirstOrDefault();if(next is null){ticks=end;return;}ticks=next.Due;next.Due=long.MaxValue;}next.Fire();}
        }
        private sealed class Alarm(Clock owner,TimerCallback callback,object? state):ITimer
        {
            internal long Due=long.MaxValue;private bool disposed;
            internal void Fire()=>callback(state);
            public bool Change(TimeSpan due,TimeSpan period){lock(owner.gate){if(disposed)return false;Due=due==Timeout.InfiniteTimeSpan?long.MaxValue:owner.ticks+due.Ticks;return true;}}
            public void Dispose(){lock(owner.gate){disposed=true;Due=long.MaxValue;owner.alarms.Remove(this);}}
            public ValueTask DisposeAsync(){Dispose();return ValueTask.CompletedTask;}
        }
    }
    internal static async Task Run(string output)
    {
        var checks=new Dictionary<string,bool>();void Check(string name,bool pass){checks[name]=pass;if(!pass)throw new InvalidDataException(name);}
        async Task Until(Func<bool> condition){using var limit=new CancellationTokenSource(2000);while(!condition())await Task.Delay(5,limit.Token);}
        var parsed=NativeToolbarLyrics.ParseLrc("[ar:fixture]\n[offset:100]\n[00:00.10]First\n[00:01.10][00:02.10]Repeated\n[00:03.10]\n[99:99]bad");
        Check("lrc-offset-and-repeated-timestamps",parsed.Length==4&&parsed[0].Milliseconds==0&&parsed[2].Milliseconds==2000);
        Check("blank-lyric-clears-instrumental-gap",parsed[3].Text=="");
        Check("unmatched-or-plain-text-hidden",NativeToolbarLyrics.ParseLrc("No timestamps").Length==0);
        Check("oversize-response-bounded",NativeToolbarLyrics.ParseLrc(new string('x',131073)).Length==0);
        Check("line-count-bounded",NativeToolbarLyrics.ParseLrc(string.Join('\n',Enumerable.Range(0,1100).Select(i=>$"[{i/60:00}:{i%60:00}]fixture"))).Length==1024);
        string longText=string.Concat(Enumerable.Repeat("👩‍💻",300));string limited=NativeToolbarLyrics.Limit(longText);
        Check("transport-256-graphemes-with-intact-emoji",new StringInfo(limited).LengthInTextElements==256&&limited.EndsWith("…")&&limited.StartsWith("👩‍💻"));
        Check("ordinary-30-character-line-not-prematurely-trimmed",NativeToolbarLyrics.Limit(new string('a',30))==new string('a',30));
        Check("control-and-directional-text-removed",NativeToolbarLyrics.Limit("a\n\u202Eb")=="ab");
        var uri=NativeToolbarLyrics.Query(new("song & test?","artist/one","album#two",234));
        Check("metadata-only-https-query-escaped",uri.Scheme=="https"&&uri.Host=="lrclib.net"&&uri.Query.Contains("%26")&&uri.Query.Contains("duration=234")&&uri.Fragment=="");
        var request=new NativeToolbarLyrics.Track("Fixture","Artist","Album",120);
        var response=JsonSerializer.SerializeToElement(new{trackName="Fixture",artistName="Artist",duration=120,instrumental=false,syncedLyrics="[00:00]Original fixture text"});
        Check("matching-track-response-accepted",NativeToolbarLyrics.ReadResponse(request,response).Length==1);
        Check("wrong-artist-response-rejected",NativeToolbarLyrics.ReadResponse(request with{Artist="Other"},response).Length==0);
        Check("wrong-duration-response-rejected",NativeToolbarLyrics.ReadResponse(request with{DurationSeconds=180},response).Length==0);
        var clock=new Clock();int requests=0,changes=0;
        JsonElement Media(string title="Test",int position=0,int status=4)=>JsonSerializer.SerializeToElement(new{available=true,sessions=new[]{new{id="session",trackId="track:"+title,current=true,title,artist="Fixture",album="",durationMs=100000,positionMs=position,status,playbackRate=1,positionUpdatedUnixMs=clock.GetUtcNow().ToUnixTimeMilliseconds()}}});
        NativeToolbarLyrics.Line[] Lines()=>[new(0,"first"),new(1000,"second"),new(2000,""),new(3000,"fourth")];
        using(var service=new NativeToolbarLyrics(()=>Interlocked.Increment(ref changes),clock,(track,ct)=>{Interlocked.Increment(ref requests);return Task.FromResult(Lines());}))
        {
            service.Observe(false,Media());clock.Advance(10000);Check("disabled-module-does-not-query-or-tick",requests==0&&clock.Active==0);
            var initial=Media();service.Observe(true,initial);clock.Advance(350);await Until(()=>service.Text=="first");
            Check("track-query-runs-once",requests==1);clock.Advance(650);Check("line-boundary-scheduled-once",service.Text=="second");
            service.Observe(true,initial);Check("projection-does-not-reset-playback-anchor",service.Text=="second"&&requests==1);
            clock.Advance(1000);Check("gap-hides-previous-line",service.Text=="");
            service.Observe(true,Media(position:1500,status:5));clock.Advance(10000);Check("paused-playback-has-no-timer",service.Text=="second"&&clock.Active==0);
            service.Observe(true,Media(position:3000,status:5));Check("seek-resynchronizes-without-refetch",service.Text=="fourth"&&requests==1);
            service.Observe(true,Media("Other"));Check("track-change-clears-old-lyric-immediately",service.Text=="");clock.Advance(350);await Until(()=>requests==2&&service.Text=="first");
            service.Observe(true,Media());Check("recent-track-uses-bounded-cache",requests==2&&service.Text=="first");
            service.Observe(true,Media(position:99900));clock.Advance(100);Check("track-end-clears-final-line",service.Text==""&&clock.Active==0);
            service.Observe(false,Media());int seen=changes;clock.Advance(10000);Check("disable-releases-timer-and-text",service.Text==""&&clock.Active==0&&changes==seen);
        }
        Check("dispose-releases-timers",clock.Active==0);
        var pending=new TaskCompletionSource<NativeToolbarLyrics.Line[]>(TaskCreationOptions.RunContinuationsAsynchronously);int late=0;
        using(var service=new NativeToolbarLyrics(()=>{},clock,(track,ct)=>{Interlocked.Increment(ref late);return track.Title=="Old"?pending.Task:Task.FromResult<NativeToolbarLyrics.Line[]>([new(0,"new")]);}))
        {
            service.Observe(true,Media("Old"));clock.Advance(350);await Until(()=>late==1);
            service.Observe(true,Media("New"));clock.Advance(350);await Until(()=>service.Text=="new");
            pending.SetResult([new(0,"stale")]);await Task.Delay(30);Check("late-network-result-cannot-replace-new-track",service.Text=="new");
        }
        using(var service=new NativeToolbarLyrics(()=>{},clock,(_,_)=>Task.FromException<NativeToolbarLyrics.Line[]>(new HttpRequestException("fixture"))))
        {service.Observe(true,Media());clock.Advance(350);await Task.Delay(30);Check("offline-keeps-text-hidden-without-polling",service.Text==""&&clock.Active==0);}
        var view=JsonNode.Parse("""{"monitors":[{"toolbar":true}],"toolbar":{"left":[],"center":[{"nativeModule":"magic.music","playing":true}],"right":[]}}""")!.AsObject();
        using(var service=new NativeToolbarLyrics(()=>{},clock,(_,_)=>Task.FromResult(Lines())))
        {service.Project(view,Media());clock.Advance(350);await Until(()=>service.Text=="first");service.Project(view,Media());Check("projection-carries-display-text-only",view["toolbar"]!["center"]![0]!["lyricsText"]!.GetValue<string>()=="first");view["toolbar"]!["center"]![0]!["playing"]=false;service.Project(view,Media(status:5));Check("paused-music-hides-lyric",view["toolbar"]!["center"]![0]!["lyricsText"]!.GetValue<string>()==""&&clock.Active==0);}
        var projection=new NativeMusicCardProjection();var timeline=Media(position:1500);long utc=clock.GetUtcNow().ToUnixTimeMilliseconds();
        var card=projection.Project(timeline,"second",1000,utc);
        Check("card-current-session-matches-toolbar",card["id"]!.GetValue<string>()=="session"&&card["lyricsText"]!.GetValue<string>()=="second");
        var lyricOnly=projection.Project(timeline,"fourth",2500,utc+1500);
        Check("card-lyric-change-retains-position-anchor",lyricOnly["positionMs"]!.GetValue<double>()==1500&&lyricOnly["observedTick"]!.GetValue<long>()==1000);
        var paused=projection.Project(Media(position:3000,status:5),"fourth",3000,utc);
        Check("card-paused-hides-lyrics",paused["lyricsText"]!.GetValue<string>()==""&&paused["positionMs"]!.GetValue<double>()==3000);
        var empty=projection.Project(default,"stale",4000,utc);Check("card-no-session-clears-stale-track",!empty["hasSession"]!.GetValue<bool>()&&empty["lyricsText"]!.GetValue<string>()=="");
        var onlyCard=JsonNode.Parse("""{"monitors":[],"toolbar":{"left":[],"center":[],"right":[]}}""")!.AsObject();
        int cardRequests=0;using(var service=new NativeToolbarLyrics(()=>{},clock,(_,_)=>{cardRequests++;return Task.FromResult(Lines());})){
            var state=Media();service.Project(onlyCard,state,true);clock.Advance(350);await Until(()=>service.Text=="first");
            service.Project(view,state,true);service.Project(view,state,true);Check("card-and-toolbar-share-one-lookup",cardRequests==1);
            service.Project(onlyCard,state,true);Check("card-only-retains-shared-service",service.Text=="first"&&clock.Active==1);
            service.Project(onlyCard,state,false);Check("last-consumer-removal-releases-service",service.Text==""&&clock.Active==0);
        }
        foreach(bool offline in new[]{false,true}){
            int notices=0;using var service=new NativeToolbarLyrics(()=>Interlocked.Increment(ref notices),clock,(_,_)=>offline?Task.FromException<NativeToolbarLyrics.Line[]>(new HttpRequestException("fixture")):Task.FromResult<NativeToolbarLyrics.Line[]>([]));
            var row=view["toolbar"]!["center"]![0]!;row["playing"]=true;row["label"]="Known song";row["artist"]="Known artist";
            service.Project(view,Media());Check("lookup-state-visible-"+offline,row["lyricsStatus"]!.GetValue<string>()=="loading");clock.Advance(350);await Until(()=>notices>0);service.Project(view,Media());
            Check("missing-and-offline-distinct-"+offline,row["lyricsStatus"]!.GetValue<string>()==(offline?"error":"missing"));
            Check("recognized-track-retained-without-lyrics-"+offline,row["label"]!.GetValue<string>()=="Known song"&&row["tooltip"]!.GetValue<string>().Contains("Known artist")&&clock.Active==0);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
        File.WriteAllText(output,JsonSerializer.Serialize(new{allPass=checks.Values.All(x=>x),scope="Owned media snapshots, fake time and HTTP responses; no live playback or network access",checks},new JsonSerializerOptions{WriteIndented=true}));
    }
    internal static async Task Live(string output)
    {
        // The provider's published documentation example, never user listening history.
        using var service=new NativeToolbarLyrics(()=>{});
        service.Observe(true,JsonSerializer.SerializeToElement(new{available=true,sessions=new[]{new{id="documentation-example",current=true,title="I Want to Live",artist="Borislav Slavov",album="Baldur's Gate 3 (Original Game Soundtrack)",durationMs=233000,positionMs=18000,status=5}}}));
        var timer=System.Diagnostics.Stopwatch.StartNew();while(timer.ElapsedMilliseconds<10000&&service.Text.Length==0)await Task.Delay(50);
        bool available=service.Text.Length>0;int characters=new StringInfo(service.Text).LengthInTextElements;
        File.WriteAllText(output,JsonSerializer.Serialize(new{allPass=available&&characters<=256,available,characters,elapsedMs=timer.ElapsedMilliseconds,source="https://lrclib.net/docs",scope="One documented public track via production HTTP client; no lyrics body stored; no user media read"},new JsonSerializerOptions{WriteIndented=true}));
        if(!available)throw new IOException("Public lyric example was unavailable; see live report.");
    }
}
