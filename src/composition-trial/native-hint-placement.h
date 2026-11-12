#pragma once
namespace NativeHintPlacement {
struct Monitor { HMONITOR handle{};RECT full{},work{};std::wstring id;bool primary=false;UINT dpi=96; };
inline BOOL CALLBACK Enumerate(HMONITOR handle,HDC,LPRECT,LPARAM context){
    auto &screens=*reinterpret_cast<std::vector<Monitor>*>(context);MONITORINFOEXW info{};info.cbSize=sizeof(info);
    if(screens.size()>=16||!GetMonitorInfoW(handle,&info))return TRUE;
    DISPLAY_DEVICEW device{};device.cb=sizeof(device);
    std::wstring id=EnumDisplayDevicesW(info.szDevice,0,&device,EDD_GET_DEVICE_INTERFACE_NAME)&&device.DeviceID[0]?device.DeviceID:std::wstring(L"temporary:")+info.szDevice;
    screens.push_back({handle,info.rcMonitor,info.rcWork,id,(info.dwFlags&MONITORINFOF_PRIMARY)!=0,96});return TRUE;
}
inline size_t Select(std::vector<Monitor> const &screens,JsonObject const &settings,HMONITOR interaction){
    size_t primary=0;for(size_t i=0;i<screens.size();++i)if(screens[i].primary){primary=i;break;}
    if(Bool(settings,L"autoSwitch",true)){
        for(size_t i=0;i<screens.size();++i)if(screens[i].handle==interaction)return i;
    }else{auto target=Str(settings,L"fixedMonitor");if(target.empty())target=L"primary";if(target!=L"primary")for(size_t i=0;i<screens.size();++i)if(!_wcsicmp(screens[i].id.c_str(),target.c_str()))return i;}
    return primary;
}
inline HMONITOR Interaction(){
    auto window=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(window,&pid);wchar_t name[128]{};GetClassNameW(window,name,128);
    if(window&&pid!=GetCurrentProcessId()&&wcscmp(name,L"Progman")&&wcscmp(name,L"WorkerW"))return MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    POINT cursor{};GetCursorPos(&cursor);return MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST);
}
// Reuse fast-changing volume/brightness hints only on the same destination.
// A live slider gesture keeps its window stable until the user releases it.
inline bool Reuse(bool dragging,bool automatic,bool sameSettings,HMONITOR shown,HMONITOR interaction){
    return dragging||(sameSettings&&(!automatic||shown==interaction));
}
inline JsonObject Position(JsonObject const &settings,Monitor const &screen){
    auto saved=settings.GetNamedObject(L"monitors",JsonObject{});JsonObject value;
    for(auto const &entry:saved)if(!_wcsicmp(entry.Key().c_str(),screen.id.c_str())&&entry.Value().ValueType()==JsonValueType::Object){value=entry.Value().GetObject();break;}
    auto anchor=Str(value,L"anchor");if(anchor.empty())anchor=L"top-center";bool bottom=anchor.starts_with(L"bottom-");
    auto alignX=anchor.ends_with(L"-left")?L"Start":anchor.ends_with(L"-right")?L"End":L"Center";
    int inset=std::max(1,static_cast<int>(12*screen.dpi/96));auto const &work=screen.work;
    JsonObject payload,point;Put(point,L"x",static_cast<double>(alignX==std::wstring(L"Start")?work.left+inset:alignX==std::wstring(L"End")?work.right-inset:(work.left+work.right)/2));
    Put(point,L"y",static_cast<double>(bottom?work.bottom-inset:work.top+inset));payload.Insert(L"desiredPosition",point);
    Put(payload,L"alignX",std::wstring(alignX));Put(payload,L"alignY",std::wstring(bottom?L"End":L"Start"));
    Put(payload,L"hintOffsetX",std::clamp(Num(value,L"x"),-4096.,4096.));Put(payload,L"hintOffsetY",std::clamp(Num(value,L"y"),-4096.,4096.));return payload;
}
inline POINT Origin(JsonObject const &payload,RECT work,int width,int height){
    auto point=payload.GetNamedObject(L"desiredPosition",JsonObject{});
    auto offset=[](std::wstring align,int size){return align==L"Center"?size/2:align==L"End"?size:0;};
    int x=static_cast<int>(Num(point,L"x"))-offset(Str(payload,L"alignX"),width)+static_cast<int>(std::clamp(Num(payload,L"hintOffsetX"),-4096.,4096.));
    int y=static_cast<int>(Num(point,L"y"))-offset(Str(payload,L"alignY"),height)+static_cast<int>(std::clamp(Num(payload,L"hintOffsetY"),-4096.,4096.));
    return {std::clamp(x,static_cast<int>(work.left),std::max(static_cast<int>(work.left),static_cast<int>(work.right)-width)),std::clamp(y,static_cast<int>(work.top),std::max(static_cast<int>(work.top),static_cast<int>(work.bottom)-height))};
}
// Coordinates are in DIPs. Use the same saved anchor as placement, including
// the bottom edge after a notification expands; offsets never change its edge.
struct MotionPose { float x,y,enterY,exitY; };
inline MotionPose Motion(JsonObject const &payload,float width,float height){
    auto align=Str(payload,L"alignX");bool bottom=Str(payload,L"alignY")==L"End";
    return {align==L"Start"?0.f:align==L"End"?width:width/2,bottom?height:0.f,bottom?14.f:-14.f,bottom?12.f:-12.f};
}
}
