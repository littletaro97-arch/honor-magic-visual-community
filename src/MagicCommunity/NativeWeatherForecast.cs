using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace MagicCommunity;

// Daily and hourly views share one existing forecast request and city cache.
internal static class NativeWeatherForecast
{
    internal static string Request(double latitude,double longitude)=>
        "https://api.open-meteo.com/v1/forecast?latitude="+latitude.ToString("F5",CultureInfo.InvariantCulture)+
        "&longitude="+longitude.ToString("F5",CultureInfo.InvariantCulture)+
        "&current=temperature_2m,weather_code,is_day&daily=weather_code,temperature_2m_max,temperature_2m_min"+
        "&hourly=temperature_2m,weather_code,is_day&forecast_days=5&timezone=auto";

    internal static void Validate(JsonElement data)
    {
        if(!data.GetProperty("current").GetProperty("temperature_2m").TryGetDouble(out double current)||!double.IsFinite(current))throw new JsonException();
        var daily=data.GetProperty("daily");
        foreach(string name in new[]{"time","weather_code","temperature_2m_max","temperature_2m_min"}){
            var values=daily.GetProperty(name);if(values.GetArrayLength()<5)throw new JsonException();
            foreach(var value in values.EnumerateArray().Take(5)){
                if(name=="time"){if(!DateOnly.TryParseExact(value.GetString(),"yyyy-MM-dd",CultureInfo.InvariantCulture,DateTimeStyles.None,out _))throw new JsonException();}
                else if(!value.TryGetDouble(out double number)||!double.IsFinite(number))throw new JsonException();
            }
        }
        // Null hourly values are valid missing observations; render them as unavailable.
        // A malformed or absent hourly block must not discard an otherwise usable daily view.
    }

    internal static JsonElement MergeCity(JsonElement previous,JsonElement city)
    {
        var settings=previous.ValueKind==JsonValueKind.Object?JsonNode.Parse(previous.GetRawText())!.AsObject():new JsonObject();
        foreach(string name in new[]{"city","region","latitude","longitude","timezone"})settings[name]=JsonNode.Parse(city.GetProperty(name).GetRawText());
        return JsonSerializer.SerializeToElement(settings);
    }
}
