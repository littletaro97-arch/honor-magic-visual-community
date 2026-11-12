using System.Globalization;
using System.Text.Json;
using MagicCommunity;

internal static class WeatherHoursChecks
{
    internal static void Run(string path)
    {
        var checks=new Dictionary<string,bool>();
        void Check(string name,bool value){checks[name]=value;if(!value)throw new InvalidOperationException(name);}
        var culture=CultureInfo.CurrentCulture;
        try {
            CultureInfo.CurrentCulture=CultureInfo.GetCultureInfo("de-DE");
            var request=NativeWeatherForecast.Request(37.50914,122.11356);
            Check("coordinate-format-independent-of-Windows-locale",request.Contains("latitude=37.50914&longitude=122.11356"));
            Check("one-request-contains-both-views",request.Contains("&daily=")&&request.Contains("&hourly=temperature_2m,weather_code,is_day"));
            Check("city-time-zone-and-five-days-preserved",request.EndsWith("&forecast_days=5&timezone=auto"));
        } finally {CultureInfo.CurrentCulture=culture;}
        var previous=JsonSerializer.SerializeToElement(new {city="旧城市",latitude=0,longitude=0,forecastView="hourly",extension=new {kept=true}});
        var city=JsonSerializer.SerializeToElement(new {city="威海",region="山东",latitude=37.50914,longitude=122.11356,timezone="Asia/Shanghai"});
        var updated=NativeWeatherForecast.MergeCity(previous,city);
        Check("changing-city-keeps-hourly-view",updated.GetProperty("forecastView").GetString()=="hourly");
        Check("changing-city-keeps-other-settings",updated.GetProperty("extension").GetProperty("kept").GetBoolean());
        Check("coordinates-and-timezone-updated",updated.GetProperty("latitude").GetDouble()==37.50914&&updated.GetProperty("timezone").GetString()=="Asia/Shanghai");
        var restored=JsonSerializer.Deserialize<JsonElement>(JsonSerializer.Serialize(updated));
        Check("view-survives-save-reload",restored.GetProperty("forecastView").GetString()=="hourly");
        Check("source-settings-not-mutated",previous.GetProperty("city").GetString()=="旧城市");
        var legacy=NativeWeatherForecast.MergeCity(default,city);Check("legacy-settings-still-supported",legacy.GetProperty("city").GetString()=="威海");
        const string daily="\"daily\":{\"time\":[\"2026-11-10\",\"2026-11-11\",\"2026-11-12\",\"2026-11-13\",\"2026-11-14\"],\"weather_code\":[0,1,2,3,0],\"temperature_2m_max\":[15,16,14,12,11],\"temperature_2m_min\":[3,4,2,1,0]}";
        foreach(var hourly in new[]{"",",\"hourly\":null",",\"hourly\":{\"temperature_2m\":[null,12]}",",\"hourly\":\"malformed\""}){
            NativeWeatherForecast.Validate(JsonDocument.Parse("{\"current\":{\"temperature_2m\":12},"+daily+hourly+"}").RootElement);
        }
        Check("absent-or-partial-hours-do-not-discard-daily-weather",true);
        bool invalid=false;try{NativeWeatherForecast.Validate(JsonDocument.Parse("{\"current\":{\"temperature_2m\":12},\"daily\":{\"time\":[]}}").RootElement);}catch(JsonException){invalid=true;}
        Check("incomplete-daily-response-still-rejected",invalid);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path,JsonSerializer.Serialize(new{allPass=checks.Values.All(x=>x),checks,scope="Synthetic API payloads and preference persistence; no user data or desktop changes."},new JsonSerializerOptions{WriteIndented=true}));
    }
}
