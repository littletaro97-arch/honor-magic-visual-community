#pragma once
#include "native-weather-hours.h"
// Presentation only: shares NativeWeather's visible-card, bounded request/cache lifecycle.
inline std::pair<UINT32,UINT32> NativeWeatherSky(int code,bool day){
    if(!day)return {0x17223a,0x354764};if(code<=1)return {0x2f7ec6,0x8fc6ec};if(code==2)return {0x46769f,0xa3c1da};
    if(code==45||code==48)return {0x5c6672,0xa7b0b9};if((code>=71&&code<=77)||code==85||code==86)return {0x5b7189,0xb3c4d2};
    if(code>=95&&code<=99)return {0x232a46,0x4d4569};if((code>=51&&code<=67)||(code>=80&&code<=82))return {0x2e4c66,0x6a8298};return {0x566a7e,0x9daab7};
}
inline void WeatherIcon(NativePaint &p,float x,float y,float size,int code,bool day=true){
    float s=size/44;auto ink=D2D1::ColorF(0xffffff,.94f),rain=D2D1::ColorF(0xc7e9ff),sun=D2D1::ColorF(0xffde83);
    auto point=[&](float a,float b){return D2D1::Point2F(x+a*s,y+b*s);};
    auto line=[&](float a,float b,float c,float d,D2D1_COLOR_F color){p.Line(x+a*s,y+b*s,x+c*s,y+d*s,color,2*s);};
    if(code<=1&&day){p.c->DrawEllipse({point(22,22),8*s,8*s},p.Color(sun),2*s);for(int i=0;i<8;i++){float a=i*3.14159265f/4;line(22+12*std::cos(a),22+12*std::sin(a),22+16*std::cos(a),22+16*std::sin(a),sun);}return;}
    com_ptr<ID2D1Factory> factory;p.c->GetFactory(factory.put());com_ptr<ID2D1PathGeometry> path;check_hresult(factory->CreatePathGeometry(path.put()));com_ptr<ID2D1GeometrySink> sink;check_hresult(path->Open(sink.put()));
    if(code<=1){sink->BeginFigure(point(28,6),D2D1_FIGURE_BEGIN_FILLED);sink->AddBezier({point(3,3),point(0,37),point(26,38)});sink->AddBezier({point(33,38),point(39,33),point(40,28)});sink->AddBezier({point(21,35),point(17,13),point(28,6)});sink->EndFigure(D2D1_FIGURE_END_CLOSED);check_hresult(sink->Close());p.c->FillGeometry(path.get(),p.Color(ink));return;}
    sink->BeginFigure(point(11,29),D2D1_FIGURE_BEGIN_HOLLOW);sink->AddBezier({point(1,29),point(1,17),point(11,16)});sink->AddBezier({point(11,4),point(28,4),point(30,16)});sink->AddBezier({point(41,14),point(44,29),point(34,29)});sink->AddLine(point(11,29));sink->EndFigure(D2D1_FIGURE_END_CLOSED);check_hresult(sink->Close());p.c->DrawGeometry(path.get(),p.Color(ink),2*s);
    if((code>=71&&code<=77)||code==85||code==86){for(float dx:{14.f,27.f}){line(dx,33,dx,40,ink);line(dx-3,35,dx+3,38,ink);line(dx-3,38,dx+3,35,ink);}}
    else if(code>=51){line(14,33,12,39,rain);line(27,33,25,39,rain);if(code>=95){line(23,28,18,35,sun);line(18,35,23,35,sun);line(23,35,19,42,sun);}}
    else if(code==45||code==48){line(7,34,35,34,ink);line(11,39,31,39,ink);}
}
inline std::wstring WeatherLabel(int code){
    if(code==0)return L"晴";if(code==1)return L"大致晴朗";if(code==2)return L"多云";if(code==3)return L"阴";if(code==45||code==48)return L"雾";
    if(code>=95)return L"雷雨";if(code==85||code==86)return L"阵雪";if(code>=80)return L"阵雨";if(code>=71&&code<=77)return L"雪";if(code>=61)return L"雨";if(code>=51)return L"毛毛雨";return L"天气";
}
inline std::wstring NativeWeatherDay(std::wstring const &date,unsigned index){
    if(index==0)return L"今天";if(index==1)return L"明天";int y=0,m=0,d=0;if(swscanf_s(date.c_str(),L"%d-%d-%d",&y,&m,&d)!=3)return L"";
    using namespace std::chrono;year_month_day value{year{y},month{static_cast<unsigned>(m)},day{static_cast<unsigned>(d)}};if(!value.ok())return L"";
    auto week=weekday{sys_days{value}}.c_encoding();return L"周"+std::wstring(1,L"日一二三四五六"[week]);
}
inline void PaintNativeWeather(Window &window,NativePaint &p,Tile const &tile,float w,float h,float radius){
    JsonObject settings=tile.card.GetNamedObject(L"settings",JsonObject{}),entry=nullptr,data=nullptr;auto city=Str(settings,L"city");
    if(std::abs(Num(settings,L"latitude",1000))>90||std::abs(Num(settings,L"longitude",1000))>180)city.clear();wchar_t key[80];swprintf_s(key,L"%.5f,%.5f",Num(settings,L"latitude"),Num(settings,L"longitude"));
    if(window.app.model.weatherData){auto v=window.app.model.weatherData.TryLookup(key);if(v&&v.ValueType()==JsonValueType::Object){entry=v.GetObject();auto d=entry.TryLookup(L"data");if(d&&d.ValueType()==JsonValueType::Object)data=d.GetObject();}}
    bool slim=h<100,wide=w>300;auto ink=p.theme.Ink(),muted=p.theme.MaterialMuted();
    auto text=[&](std::wstring const &value,D2D1_RECT_F bounds,float size=12.f,bool dim=false,bool center=false,int weight=400){p.Text(value,bounds,size,dim?muted:ink,weight,center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING,0,true);};
    if(city.empty()||(!data&&entry&&Bool(entry,L"error"))){p.Icon(L"location",{w/2-12,h/2-34,w/2+12,h/2-10},muted);auto code=entry?Str(entry,L"errorCode"):L"";bool network=code==L"dns"||code==L"tls"||code==L"proxy"||code==L"connection"||code==L"network"||code==L"timeout";
        text(city.empty()?L"还没选城市":network?L"天气网络连接失败":code==L"rate_limited"?L"天气服务限流":L"天气暂不可用",{12,h/2-1,w-12,h/2+20},12,false,true);text(city.empty()?L"右键设置城市后显示天气":L"右键定位信息可查看原因",{12,h/2+21,w-12,h/2+43},10.5f,true,true);return;}
    if(!data){auto skeleton=[&](D2D1_RECT_F r,float rad=4){auto color=muted;color.a=p.theme.dark?.32f:.28f;p.Fill(r,color,rad);};
        if(slim){skeleton({14,28,44,58});skeleton({53,29,105,57});skeleton({122,25,166,35});skeleton({133,47,166,57});}
        else{if(wide){skeleton({14,14,80,25});skeleton({14,36,102,72});skeleton({w-77,16,w-14,39});}else{skeleton({13,20,94,58});skeleton({143,13,169,39});skeleton({119,48,169,58});}int count=wide?5:3;float cell=(w-28)/count;for(int i=0;i<count;i++){float x=14+cell*i;skeleton({x+3,96,x+cell-5,104});skeleton({x+cell/2-9,112,x+cell/2+9,130});skeleton({x+3,146,x+cell-5,156});}}
        return;
    }
    auto current=data.GetNamedObject(L"current"),daily=data.GetNamedObject(L"daily");int code=static_cast<int>(Num(current,L"weather_code"));bool day=Num(current,L"is_day")>0;
    auto colors=NativeWeatherSky(code,day);D2D1_GRADIENT_STOP stops[]{{0,D2D1::ColorF(colors.first)},{1,D2D1::ColorF(colors.second)}};com_ptr<ID2D1GradientStopCollection> collection;check_hresult(p.c->CreateGradientStopCollection(stops,2,D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,collection.put()));com_ptr<ID2D1LinearGradientBrush> brush;check_hresult(p.c->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0,0},{w,h}),collection.get(),brush.put()));p.c->FillRoundedRectangle(D2D1::RoundedRect({0,0,w,h},radius,radius),brush.get());
    ink=D2D1::ColorF(0xffffff);muted=D2D1::ColorF(0xffffff,.75f);
    auto temp=[](double n){return std::isfinite(n)?(n<-.49?L"−":L"")+CardNumber(std::abs(n))+L"°":L"—";};
    bool hourly=Str(settings,L"forecastView")==L"hourly";
    if(slim){WeatherIcon(p,12,hourly?10.f:27.f,32,code,day);p.Text(temp(Num(current,L"temperature_2m")),{50,hourly?6.f:23.f,112,hourly?48.f:65.f},26,ink,600);p.Text(city,{111,hourly?8.f:20.f,w-14,hourly?28.f:40.f},12,ink,400,DWRITE_TEXT_ALIGNMENT_TRAILING);if(!hourly)p.Text(WeatherLabel(code),{111,43,w-14,66},11.5f,muted,400,DWRITE_TEXT_ALIGNMENT_TRAILING);}
    else{
        if(wide){p.Icon(L"location",{14,12,26,24},ink);text(city,{30,10,w-123,29},12,false,false,500);text(temp(Num(current,L"temperature_2m")),{14,29,200,77},42);p.Text(WeatherLabel(code),{w-152,13,w-49,41},13.5f,ink,400,DWRITE_TEXT_ALIGNMENT_TRAILING);WeatherIcon(p,w-42,15,27,code,day);}
        else{text(temp(Num(current,L"temperature_2m")),{13,13,116,67},46);WeatherIcon(p,w-39,13,25,code,day);p.Text(city,{95,43,w-13,62},11,ink,400,DWRITE_TEXT_ALIGNMENT_TRAILING);}
        p.Line(14,wide?83.f:77.f,w-14,wide?83.f:77.f,D2D1::ColorF(0xffffff,.24f),1);
        if(!hourly){auto dates=daily.GetNamedArray(L"time",JsonArray{}),codes=daily.GetNamedArray(L"weather_code",JsonArray{}),high=daily.GetNamedArray(L"temperature_2m_max",JsonArray{}),low=daily.GetNamedArray(L"temperature_2m_min",JsonArray{});
        unsigned count=wide?5u:3u;float cell=(w-28)/count;
        auto number=[](JsonArray const &v,unsigned i){return i<v.Size()&&v.GetAt(i).ValueType()==JsonValueType::Number?v.GetAt(i).GetNumber():NAN;};
        for(unsigned i=0;i<count;i++){float x=14+cell*i;auto label=i<dates.Size()?NativeWeatherDay(std::wstring(dates.GetAt(i).GetString()),i):L"—";text(label,{x,wide?91.f:86.f,x+cell,wide?108.f:101.f},wide?10.5f:9.5f,false,true);
            double forecast=number(codes,i);if(std::isfinite(forecast))WeatherIcon(p,x+cell/2-10,wide?112.f:106.f,wide?20.f:19.f,static_cast<int>(forecast));
            if(wide)text(temp(number(low,i))+L" / "+temp(number(high,i)),{x,139,x+cell,167},12,false,true,500);
            else{text(temp(number(high,i)),{x,131,x+cell,151},13,false,true,600);text(temp(number(low,i)),{x,153,x+cell,171},10.5f,true,true);}
        }}
    }
    if(hourly){auto hours=NativeWeatherHourPage(window,tile,data,w,h);
        if(hours.values.empty())text(L"暂无小时天气",{12,slim?44.f:104.f,w-12,slim?62.f:143.f},11,false,true);
        else {float cell=(w-28)/hours.count;for(unsigned slot=0;slot<hours.count;slot++){unsigned i=hours.page*hours.count+slot;if(i>=hours.values.size())break;auto const &hour=hours.values[i];float x=14+cell*slot;
            if(slim)text(hour.time.substr(11,5)+L" "+temp(hour.temperature),{x,43,x+cell,61},10.5f,false,true);
            else {text(hour.time.substr(11,5),{x,wide?91.f:86.f,x+cell,wide?108.f:102.f},11,false,true);if(std::isfinite(hour.code))WeatherIcon(p,x+cell/2-10,wide?112.f:106.f,20,static_cast<int>(hour.code),hour.day);text(temp(hour.temperature),{x,134,x+cell,157},13,false,true,500);}
        }}
        text(L"今日"+(hours.pages?L" · "+std::to_wstring(hours.page+1)+L"/"+std::to_wstring(hours.pages):L""),{14,h-23,w-57,h-3},9.5f,true);
        if(hours.pages>1){for(bool next:{false,true}){auto box=next?NativeWeatherHours::Next(w,h):NativeWeatherHours::Previous(w,h);float cx=(box.left+box.right)/2,cy=(box.top+box.bottom)/2,dx=next?2.5f:-2.5f;auto color=(next?hours.page+1<hours.pages:hours.page>0)?ink:muted;p.Line(cx-dx,cy-4,cx+dx,cy,color,1.5f);p.Line(cx+dx,cy,cx-dx,cy+4,color,1.5f);}}
    }
    D2D1_RECT_F status=hourly?D2D1_RECT_F{w-72,h-22,w-56,h-4}:D2D1_RECT_F{w-24,h-22,w-8,h-4};
    if(entry&&Bool(entry,L"loading"))p.Text(L"…",status,13,ink,600,DWRITE_TEXT_ALIGNMENT_CENTER);else if(entry&&Bool(entry,L"error"))p.Text(L"!",status,13,ink,600,DWRITE_TEXT_ALIGNMENT_CENTER);
}
