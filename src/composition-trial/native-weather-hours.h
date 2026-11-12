#pragma once
// Hour labels and the selected date use the provider's city-local ISO time, never PC time.
struct NativeWeatherHour{std::wstring time;double temperature=NAN,code=NAN;bool day=true;};
struct NativeWeatherHours{
    std::wstring date;std::vector<NativeWeatherHour> values;unsigned page=0,pages=0,count=5;
    static JsonArray Array(JsonObject const &o,wchar_t const *key){auto v=o.TryLookup(key);return v&&v.ValueType()==JsonValueType::Array?v.GetArray():JsonArray{};}
    static double Number(JsonArray const &a,unsigned i){return i<a.Size()&&a.GetAt(i).ValueType()==JsonValueType::Number?a.GetAt(i).GetNumber():NAN;}
    static NativeWeatherHours Read(JsonObject const &data,unsigned count,int page=-1){
        NativeWeatherHours result;result.count=std::max(1u,count);if(!data)return result;
        auto current=Str(data.GetNamedObject(L"current",JsonObject{}),L"time");if(current.size()<16)return result;result.date=current.substr(0,10);
        auto value=data.TryLookup(L"hourly");if(!value||value.ValueType()!=JsonValueType::Object)return result;auto hourly=value.GetObject();
        auto times=Array(hourly,L"time"),temperatures=Array(hourly,L"temperature_2m"),codes=Array(hourly,L"weather_code"),days=Array(hourly,L"is_day");
        int currentIndex=0;
        for(unsigned i=0;i<times.Size()&&i<144&&result.values.size()<25;i++){
            auto stamp=times.GetAt(i);if(stamp.ValueType()!=JsonValueType::String)continue;auto time=std::wstring(stamp.GetString());
            if(time.size()!=16||time.substr(0,10)!=result.date||time[10]!=L'T'||time[13]!=L':'||time[11]<L'0'||time[11]>L'2'||time[12]<L'0'||time[12]>L'9'||_wtoi(time.substr(11,2).c_str())>23)continue;
            if(time<=current)currentIndex=static_cast<int>(result.values.size());
            result.values.push_back({time,Number(temperatures,i),Number(codes,i),Number(days,i)!=0});
        }
        result.pages=static_cast<unsigned>((result.values.size()+result.count-1)/result.count);
        if(result.pages)result.page=std::clamp(page<0?currentIndex/static_cast<int>(result.count):page,0,static_cast<int>(result.pages)-1);return result;
    }
    static D2D1_RECT_F Previous(float w,float h){return {w-51,h-24,w-29,h-3};}
    static D2D1_RECT_F Next(float w,float h){return {w-28,h-24,w-6,h-3};}
    static int Hit(float x,float y,float w,float h){auto has=[&](D2D1_RECT_F r){return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;};return has(Previous(w,h))?-1:has(Next(w,h))?1:0;}
};
inline JsonObject NativeWeatherData(Window &window,Tile const &tile){
    auto settings=tile.card.GetNamedObject(L"settings",JsonObject{});wchar_t key[80];swprintf_s(key,L"%.5f,%.5f",Num(settings,L"latitude"),Num(settings,L"longitude"));
    auto entry=window.app.model.weatherData?window.app.model.weatherData.TryLookup(key):nullptr;if(!entry||entry.ValueType()!=JsonValueType::Object)return nullptr;
    auto data=entry.GetObject().TryLookup(L"data");return data&&data.ValueType()==JsonValueType::Object?data.GetObject():nullptr;
}
inline NativeWeatherHours NativeWeatherHourPage(Window &window,Tile const &tile,JsonObject const &data,float w,float h){
    auto result=NativeWeatherHours::Read(data,h<100?2:w>300?5:3);auto found=window.weatherHourPages.find(tile.key);
    if(found!=window.weatherHourPages.end()&&found->second.first==result.date&&result.pages)result.page=std::clamp(found->second.second,0,static_cast<int>(result.pages)-1);return result;
}
inline void NativeWeatherTurn(Window &window,Tile const &tile,int step,float w,float h){
    if(!step||Str(tile.card.GetNamedObject(L"settings",JsonObject{}),L"forecastView")!=L"hourly")return;
    auto page=NativeWeatherHourPage(window,tile,NativeWeatherData(window,tile),w,h);if(!page.pages)return;
    auto next=std::clamp(static_cast<int>(page.page)+step,0,static_cast<int>(page.pages)-1);if(next==static_cast<int>(page.page))return;
    window.weatherHourPages[tile.key]={page.date,next};window.DrawCards(true,L"system.weather");
}
