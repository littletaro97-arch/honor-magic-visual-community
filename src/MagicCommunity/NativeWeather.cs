using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
namespace MagicCommunity;
// One in-memory subscription per rounded coordinate, shared by every visible card.
internal sealed partial class NativeWeather : IDisposable
{
    internal sealed record City(string Name,string Region,double Latitude,double Longitude,string Timezone)
    {
        public string Key=>Latitude.ToString("F5",CultureInfo.InvariantCulture)+","+Longitude.ToString("F5",CultureInfo.InvariantCulture);
        public override string ToString()=>string.Join(" · ",new[]{Name,Region}.Where(s=>s.Length>0));
    }
    private sealed class Entry(City city){public City City=city;public JsonElement? Data;public bool Error,Loading,Notify;public int Failures;public DateTimeOffset Last,LastManual,LastSuccess,Due;public WeatherFailure? Failure;public CancellationTokenSource? Cancel;}
    private readonly HttpClient http;
    private readonly bool ownsHttp;
    private readonly TimeProvider time;
    private readonly Action<string>? notice;
    private readonly Action<string,object>? log;
    private readonly Dictionary<string,Entry> entries=new();
    private readonly System.Windows.Threading.DispatcherTimer timer=new();
    private readonly Action<object> emit;
    private readonly SemaphoreSlim slots=new(2);
    private HashSet<string> active=[];
    private bool disposed;
    private Window? cityEditor;private string editorCard="";
    public NativeWeather(Action<object> emit,HttpClient? client=null,TimeProvider? clock=null,Action<string>? notice=null,Action<string,object>? log=null){this.emit=emit;http=client??new(new HttpClientHandler{AllowAutoRedirect=false}){Timeout=TimeSpan.FromSeconds(10),MaxResponseContentBufferSize=256*1024};ownsHttp=client is null;time=clock??TimeProvider.System;this.notice=notice;this.log=log;timer.Tick+=(_,_)=>Tick();}
    public static City? Read(JsonElement settings)
    {
        try{double lat=settings.GetProperty("latitude").GetDouble(),lon=settings.GetProperty("longitude").GetDouble();string name=settings.GetProperty("city").GetString()??"";
            if(!double.IsFinite(lat)||!double.IsFinite(lon)||Math.Abs(lat)>90||Math.Abs(lon)>180||name.Length is <1 or >100)return null;
            return new(name,Text(settings,"region"),Math.Round(lat,5),Math.Round(lon,5),Text(settings,"timezone"));}
        catch(Exception e)when(e is KeyNotFoundException or InvalidOperationException or FormatException){return null;}
    }
    private static string Text(JsonElement v,string name)=>v.TryGetProperty(name,out var x)&&x.ValueKind==JsonValueKind.String?(x.GetString()??"")[..Math.Min(x.GetString()!.Length,100)]:"";
    public void Demand(IEnumerable<DesktopCard> cards)
    {
        if(disposed)return;
        var cities=cards.Where(c=>c.Type is "system.weather" or "system.time-weather").Select(c=>Read(c.Settings)).OfType<City>().DistinctBy(c=>c.Key).Take(64).ToArray();
        var next=cities.Select(c=>c.Key).ToHashSet();
        if(active.SetEquals(next))return;
        foreach(var key in active.Except(next)){entries[key].Cancel?.Cancel();}
        foreach(var city in cities){if(!entries.TryGetValue(city.Key,out var entry)){if(entries.Count>=64){var old=entries.Keys.FirstOrDefault(k=>!next.Contains(k));if(old is not null){entries[old].Cancel?.Cancel();entries.Remove(old);}}entries[city.Key]=entry=new(city);}
            if(!active.Contains(city.Key))entry.Due=DateTimeOffset.MinValue;}
        active=next;Tick();Publish();
    }
    private void Tick(){timer.Stop();foreach(var key in active.ToArray()){var e=entries[key];if(!e.Loading&&e.Due<=time.GetUtcNow())_=Fetch(e);}Schedule();}
    private void Schedule(){timer.Stop();var due=active.Select(k=>entries[k]).Where(e=>!e.Loading).Select(e=>e.Due).DefaultIfEmpty(DateTimeOffset.MaxValue).Min();if(due==DateTimeOffset.MaxValue||disposed)return;timer.Interval=TimeSpan.FromSeconds(Math.Clamp((due-time.GetUtcNow()).TotalSeconds,1,1800));timer.Start();}
    private async Task<JsonElement> Get(string url,CancellationToken token){
        using var deadline=CancellationTokenSource.CreateLinkedTokenSource(token);deadline.CancelAfter(TimeSpan.FromSeconds(10));
        try{using var response=await http.GetAsync(url,HttpCompletionOption.ResponseHeadersRead,deadline.Token);
            if(!response.IsSuccessStatusCode){DateTimeOffset? retry=null;if((int)response.StatusCode==429){var limit=response.Headers.RetryAfter;retry=limit?.Date??(limit?.Delta is {} delta?time.GetUtcNow()+delta:null);if(retry is {} at)retry=time.GetUtcNow().AddSeconds(Math.Clamp((at-time.GetUtcNow()).TotalSeconds,1,86400));}throw new WeatherRequestFailure(HttpFailure((int)response.StatusCode,retry));}
            await response.Content.LoadIntoBufferAsync(256*1024,deadline.Token);using var json=JsonDocument.Parse(await response.Content.ReadAsByteArrayAsync(deadline.Token),new JsonDocumentOptions{MaxDepth=24});return json.RootElement.Clone();
        }catch(OperationCanceledException)when(!token.IsCancellationRequested){throw new WeatherRequestFailure(new("timeout","天气请求超时（10 秒），请检查网络或代理连接。"));}
    }
    private async Task Fetch(Entry e)
    {
        if(e.Loading||disposed)return;e.Loading=true;e.Last=time.GetUtcNow();var cancel=new CancellationTokenSource();e.Cancel=cancel;Publish();bool acquired=false;var duration=System.Diagnostics.Stopwatch.StartNew();
        try{if(e.Notify)await Task.Yield();await slots.WaitAsync(cancel.Token);acquired=true;e.Last=time.GetUtcNow();
            var v=await Get(NativeWeatherForecast.Request(e.City.Latitude,e.City.Longitude),cancel.Token);
            NativeWeatherForecast.Validate(v);e.Data=v;e.Error=false;e.Failure=null;e.Failures=0;e.LastSuccess=time.GetUtcNow();e.Due=time.GetUtcNow().AddMinutes(30);
        }catch(OperationCanceledException)when(cancel.IsCancellationRequested){}
        catch(Exception ex)when(ex is HttpRequestException or TaskCanceledException or JsonException or InvalidOperationException or KeyNotFoundException or IOException or ObjectDisposedException){e.Error=true;e.Failure=Explain(ex);e.Failures++;e.Due=time.GetUtcNow().AddMinutes(e.Failures==1?1:e.Failures==2?5:15);if(e.Failure.RetryAfter is {} retry&&retry>e.Due)e.Due=retry;log?.Invoke("weather_fetch_failed",new{provider="Open-Meteo",code=e.Failure.Code,httpStatus=e.Failure.HttpStatus,milliseconds=duration.Elapsed.TotalMilliseconds,manual=e.Notify});}
        finally{if(acquired)slots.Release();bool wasCancelled=cancel.IsCancellationRequested;e.Loading=false;e.Cancel=null;cancel.Dispose();bool report=e.Notify;e.Notify=false;if(!disposed){Publish();Schedule();if(report&&active.Contains(e.City.Key)&&!wasCancelled)notice?.Invoke(e.Error?"天气刷新失败："+e.Failure?.Message:"天气已更新");}}
    }
    public string Refresh(DesktopCard card){var city=Read(card.Settings);if(city is null)return "请先设置城市";if(entries.TryGetValue(city.Key,out var e)&&active.Contains(city.Key)){if(e.Loading){if(!e.Notify)e.LastManual=time.GetUtcNow();e.Notify=true;return "正在获取天气，完成后将显示结果";}if(e.Failure?.RetryAfter is {} retry&&retry>time.GetUtcNow())return $"天气服务已限流（HTTP 429），还需等待 {Math.Ceiling((retry-time.GetUtcNow()).TotalSeconds):0} 秒。";double remaining=30-(time.GetUtcNow()-e.LastManual).TotalSeconds;if(remaining>0)return $"手动刷新间隔为 30 秒，还需 {Math.Ceiling(remaining):0} 秒。";e.LastManual=time.GetUtcNow();e.Notify=true;_=Fetch(e);return "正在刷新天气";}return "卡片当前不可见";}
    private void Publish(){if(!disposed)emit(entries.Where(p=>active.Contains(p.Key)).ToDictionary(p=>p.Key,p=>(object)new{loading=p.Value.Loading,error=p.Value.Error,errorCode=p.Value.Failure?.Code,errorMessage=p.Value.Failure?.Message,data=p.Value.Data}));}
    private NativeWeatherInfoWindow? informationWindow;
    public void ShowInformation(DesktopCard card){if(informationWindow is {} existing){existing.UpdateInformation(Information(card));return;}var window=new NativeWeatherInfoWindow(Information(card));informationWindow=window;window.Closed+=(_,_)=>{if(informationWindow==window)informationWindow=null;};window.Show();}
    public string Information(DesktopCard card){var city=Read(card.Settings);if(city is null)return "还没选城市";var entry=entries.GetValueOrDefault(city.Key);var value=entry?.Data;string failure=entry?.Failure is {} error?$"\n最近获取失败：{error.Message}\n失败类型：{error.Code}":"";return $"城市：{city}\n经纬度：{city.Key}\n时区：{(value?.GetProperty("timezone").GetString()??city.Timezone)}\n更新于：{(value?.GetProperty("current").GetProperty("time").GetString()??"尚未更新")}{failure}\n\n天气 Open-Meteo · 城市 GeoNames\n城市为手动搜索选定，不读取系统定位";}
    public void Choose(AppController controller,string board,string id)
    {
        if(cityEditor is not null){if(editorCard==id){cityEditor.Activate();return;}cityEditor.Close();}
        var dialog=new NativeWeatherCityWindow();
        cityEditor=dialog;editorCard=id;dialog.Closed+=(_,_)=>{if(cityEditor==dialog){cityEditor=null;editorCard="";}};
        var query=dialog.Query;var search=dialog.Search;var state=dialog.State;var list=dialog.Cities;var choose=dialog.ChooseCity;
        var lifetime=new CancellationTokenSource();bool closed=false;dialog.Closed+=(_,_)=>{closed=true;lifetime.Cancel();lifetime.Dispose();};
        search.Click+=async(_,_)=>{if(query.Text.Trim().Length<2){state.Text="请输入至少两个字";return;}search.IsEnabled=false;list.Items.Clear();state.Text="正在搜索…";
            try{var result=await Get("https://geocoding-api.open-meteo.com/v1/search?count=8&language=zh&format=json&name="+Uri.EscapeDataString(query.Text.Trim()),lifetime.Token);if(closed)return;
                if(result.TryGetProperty("results",out var results))foreach(var c in results.EnumerateArray().Take(8)){var city=new City(Text(c,"name"),Text(c,"admin1"),c.GetProperty("latitude").GetDouble(),c.GetProperty("longitude").GetDouble(),Text(c,"timezone"));list.Items.Add(city);}state.Text=list.Items.Count==0?"没有找到城市":"选择一个城市";
            }catch(Exception ex)when(ex is HttpRequestException or OperationCanceledException or JsonException or InvalidOperationException or KeyNotFoundException or IOException or ObjectDisposedException){if(!closed)state.Text="城市搜索失败："+Explain(ex).Message;}finally{if(!closed)search.IsEnabled=true;}};
        choose.Click+=(_,_)=>{if(list.SelectedItem is not City city)return;try{controller.Commit(s=>{var target=s.Boards.Single(b=>b.Id==board);DesktopCards.Configure(target,id,NativeWeatherForecast.MergeCity(DesktopCards.Require(target,id).Settings,JsonSerializer.SerializeToElement(new{city=city.Name,region=city.Region,latitude=city.Latitude,longitude=city.Longitude,timezone=city.Timezone})));},"天气城市已保存。");dialog.Close();}catch(Exception ex){state.Text=ex.Message;}};
        dialog.Show();
    }
    public void Dispose(){disposed=true;informationWindow?.Close();cityEditor?.Close();timer.Stop();foreach(var e in entries.Values)e.Cancel?.Cancel();entries.Clear();if(ownsHttp)http.Dispose();}
}
