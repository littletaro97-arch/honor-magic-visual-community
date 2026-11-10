#include "providers.h"
#include <wbemidl.h>
#include <cmath>
#include <functional>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <powrprof.h>
#include <cwctype>
#pragma comment(lib,"dxva2.lib")
#pragma comment(lib,"wbemuuid.lib")
#pragma comment(lib,"powrprof.lib")
// Public Windows WMI monitor classes. No polling, screen overlay or gamma ramp.
namespace magic::system {
namespace {
struct WmiString{BSTR value;explicit WmiString(wchar_t const *text):value(SysAllocString(text)){if(!value)winrt::throw_hresult(E_OUTOFMEMORY);}~WmiString(){SysFreeString(value);}BSTR get()const{return value;}};
std::wstring BrightnessMonitorKey(std::wstring value){
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    if(value.starts_with(L"\\\\?\\"))value.erase(0,4);
    std::replace(value.begin(),value.end(),L'#',L'\\');
    if(auto guid=value.find(L"\\{");guid!=value.npos)value.resize(guid);
    if(value.ends_with(L"_0"))value.resize(value.size()-2);return value;
}
DWORD PhysicalBrightnessValue(double value,DWORD minimum,DWORD maximum){
    if(!std::isfinite(value)||value<0||value>1||maximum<=minimum)throw std::invalid_argument("Invalid physical brightness range");
    return minimum+static_cast<DWORD>(std::llround(value*static_cast<double>(maximum-minimum)));
}
struct BrightnessPhysical{HANDLE handle{};explicit BrightnessPhysical(HANDLE h):handle(h){}~BrightnessPhysical(){if(handle)DestroyPhysicalMonitor(handle);}};
struct BrightnessPhysicalBatch{std::vector<PHYSICAL_MONITOR> values;~BrightnessPhysicalBatch(){for(auto const &m:values)if(m.hPhysicalMonitor)DestroyPhysicalMonitor(m.hPhysicalMonitor);}};
// Some monitors reject their capabilities table while still implementing
// standard brightness. A successful bounded read is the capability proof.
bool QueryPhysicalBrightness(HANDLE handle,DWORD &minimum,DWORD &current,DWORD &maximum){
    return GetMonitorBrightness(handle,&minimum,&current,&maximum)&&maximum>minimum&&current>=minimum&&current<=maximum;
}
double ApplyPhysicalBrightness(BrightnessPhysical const &physical,std::wstring const &monitor,std::wstring const &expectedKey,double value){
    DISPLAY_DEVICEW device{};device.cb=sizeof(device);
    if(!EnumDisplayDevicesW(monitor.c_str(),0,&device,EDD_GET_DEVICE_INTERFACE_NAME)||BrightnessMonitorKey(device.DeviceID)!=expectedKey)throw std::invalid_argument("Brightness display identity changed");
    DWORD minimum=0,current=0,maximum=0;
    if(!QueryPhysicalBrightness(physical.handle,minimum,current,maximum))throw std::runtime_error("External brightness display is unavailable");
    auto target=PhysicalBrightnessValue(value,minimum,maximum);
    if(!SetMonitorBrightness(physical.handle,target))winrt::throw_last_error();
    if(!QueryPhysicalBrightness(physical.handle,minimum,current,maximum))throw std::runtime_error("External brightness readback failed");
    return (current-minimum)/static_cast<double>(maximum-minimum);
}
// Windows reports a brightness change without saying who asked for it: the WMI event
// carries only Active/Brightness, and the ambient sensor moves the very same value the
// brightness keys do. So the shell keeps quiet unless the write was ours, and the flag
// below is what tells a hand from a sensor when someone asks. Read it per power source,
// because the setting is stored twice.
//
// The scheme has to be named explicitly: on this Windows build a null SchemeGuid comes
// back as ERROR_INVALID_PARAMETER, which silently reads as "adaptive is off". Unknown
// answers report "off" so a machine without the setting still gets an honest value.
bool AdaptiveBrightness(){
    GUID *scheme=nullptr;
    if(PowerGetActiveScheme(nullptr,&scheme)!=ERROR_SUCCESS||!scheme)return false;
    SYSTEM_POWER_STATUS power{};
    bool ac=GetSystemPowerStatus(&power)&&power.ACLineStatus!=0;
    GUID subgroup{0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99}};
    GUID setting{0xfbd9aa66,0x9553,0x4097,{0xba,0x44,0xed,0x6e,0x9d,0x65,0xea,0xb8}};
    DWORD index=0;
    HRESULT result=ac?PowerReadACValueIndex(nullptr,scheme,&subgroup,&setting,&index)
                     :PowerReadDCValueIndex(nullptr,scheme,&subgroup,&setting,&index);
    LocalFree(scheme);
    return SUCCEEDED(result)&&index!=0;
}
struct BrightnessState{
    std::mutex gate;std::atomic<bool> active{false};std::shared_ptr<ChangeState> changes;
    struct Item{
        std::wstring id,path;unsigned value=0;
        // Wall clock of the last write we made, so the read that follows can say
        // "this one was the user" instead of leaving the shell to guess.
        ULONGLONG manualUntil=0;
        std::wstring monitor,name,monitorKey;bool primary=false,available=true;double inches=0;LONG width=0,height=0;
        std::shared_ptr<BrightnessPhysical> physical;DWORD minimum=0,maximum=100;
    };
    std::map<std::wstring,Item> items;std::atomic<unsigned> callbacks{0};
};
std::wstring Property(IWbemClassObject *object,wchar_t const *key){
    if(!object)return L"";
    VARIANT value{};if(FAILED(object->Get(key,0,&value,nullptr,nullptr)))return L"";
    std::wstring result=value.vt==VT_BSTR&&value.bstrVal?value.bstrVal:L"";VariantClear(&value);return result;
}
unsigned Integer(IWbemClassObject *object,wchar_t const *key){
    if(!object)winrt::throw_hresult(E_POINTER);
    VARIANT value{},converted{};if(FAILED(object->Get(key,0,&value,nullptr,nullptr)))return 0;
    unsigned n=0;if(SUCCEEDED(VariantChangeType(&converted,&value,0,VT_UI4)))n=converted.ulVal;
    VariantClear(&converted);VariantClear(&value);return n;
}
void ValidateBrightnessResult(IWbemClassObject *signature,IWbemClassObject *output){
    // Some monitor providers declare this method with no output parameters.
    // ExecMethod can succeed with a null output object in that case.
    if(!signature&&!output)return;
    if(!output)throw std::runtime_error("Brightness provider omitted its declared result");
    struct Value{VARIANT value{};~Value(){VariantClear(&value);}} status,converted;
    winrt::check_hresult(output->Get(L"ReturnValue",0,&status.value,nullptr,nullptr));
    if(status.value.vt==VT_EMPTY||status.value.vt==VT_NULL)throw std::runtime_error("Brightness provider returned no status");
    winrt::check_hresult(VariantChangeType(&converted.value,&status.value,0,VT_UI4));
    if(converted.value.ulVal!=0)throw std::runtime_error("Windows did not apply brightness");
}
class BrightnessEvents final:public IWbemObjectSink {
    std::atomic<ULONG> refs{1};std::shared_ptr<BrightnessState> state;
  public:
    explicit BrightnessEvents(std::shared_ptr<BrightnessState> value):state(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out)override{if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown)&&id!=__uuidof(IWbemObjectSink))return E_NOINTERFACE;*out=static_cast<IWbemObjectSink*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Indicate(LONG count,IWbemClassObject **objects)override{
        try{if(!state->active)return S_OK;bool changed=false;{
            std::scoped_lock lock(state->gate);
            for(LONG i=0;i<count&&i<64;i++){auto name=Property(objects[i],L"InstanceName");auto entry=state->items.find(name);
                unsigned n=Integer(objects[i],L"Brightness");if(entry!=state->items.end()&&n<=100&&entry->second.value!=n){entry->second.value=n;changed=true;}}
        }if(changed){++state->callbacks;state->changes->Notify(2048);}}catch(...){return E_FAIL;}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStatus(LONG,HRESULT,BSTR,IWbemClassObject*)override{return S_OK;}
};
}
struct BrightnessProvider::Impl{
    std::mutex gate;std::shared_ptr<BrightnessState> state=std::make_shared<BrightnessState>();
    winrt::com_ptr<IWbemServices> service;winrt::com_ptr<BrightnessEvents> sink;bool events=false;HRESULT error=S_OK;
    explicit Impl(std::shared_ptr<ChangeState> changes){state->changes=std::move(changes);}
    void Stop(){state->active=false;if(service&&sink&&events)service->CancelAsyncCall(sink.get());events=false;sink=nullptr;service=nullptr;std::scoped_lock lock(state->gate);state->items.clear();}
    void Query(wchar_t const *query,std::function<void(IWbemClassObject*)> visit){
        winrt::com_ptr<IEnumWbemClassObject> rows;
        winrt::check_hresult(service->ExecQuery(WmiString(L"WQL").get(),WmiString(query).get(),WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,rows.put()));
        auto deadline=GetTickCount64()+3000;
        for(int i=0;i<64&&GetTickCount64()<deadline;i++){winrt::com_ptr<IWbemClassObject> row;ULONG count=0;auto hr=rows->Next(500,1,row.put(),&count);winrt::check_hresult(hr);if(count)visit(row.get());else if(hr==WBEM_S_FALSE)break;}
    }
    unsigned generation=0;
    void Collect(){
        std::map<std::wstring,BrightnessState::Item> found;
        std::map<std::wstring,double> sizes;
        if(service)try{
            Query(L"SELECT * FROM WmiMonitorBrightness WHERE Active=TRUE",[&](auto row){auto id=Property(row,L"InstanceName");auto n=Integer(row,L"CurrentBrightness");if(!id.empty()&&n<=100)found[id]={id,L"",n};});
            Query(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE",[&](auto row){auto id=Property(row,L"InstanceName");if(found.contains(id))found[id].path=Property(row,L"__PATH");});
            std::erase_if(found,[](auto const &entry){return entry.second.path.empty();});
        }catch(winrt::hresult_error const &e){error=e.code();found.clear();}
        if(service)try{
            Query(L"SELECT * FROM WmiMonitorBasicDisplayParams WHERE Active=TRUE",[&](auto row){double x=Integer(row,L"MaxHorizontalImageSize"),y=Integer(row,L"MaxVerticalImageSize");if(x>0&&y>0&&x<400&&y<400)sizes[BrightnessMonitorKey(Property(row,L"InstanceName"))]=std::hypot(x,y)/2.54;});
        }catch(winrt::hresult_error const &){ }
        struct Context{decltype(found)*items;decltype(sizes)*sizes;unsigned generation,count=0;};
        Context context{&found,&sizes,++generation};
        EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL{
            auto &context=*reinterpret_cast<Context*>(data);if(++context.count>16)return FALSE;
            MONITORINFOEXW info{};info.cbSize=sizeof(info);if(!GetMonitorInfoW(monitor,&info))return TRUE;
            DISPLAY_DEVICEW device{};device.cb=sizeof(device);EnumDisplayDevicesW(info.szDevice,0,&device,EDD_GET_DEVICE_INTERFACE_NAME);
            auto key=BrightnessMonitorKey(device.DeviceID);double inches=context.sizes->contains(key)?context.sizes->at(key):0;
            auto metadata=[&](BrightnessState::Item &item){item.monitor=info.szDevice;item.monitorKey=key;item.name=device.DeviceString;item.primary=(info.dwFlags&MONITORINFOF_PRIMARY)!=0;item.inches=inches;item.width=info.rcMonitor.right-info.rcMonitor.left;item.height=info.rcMonitor.bottom-info.rcMonitor.top;};
            bool wmi=false;for(auto &[id,item]:*context.items)if(BrightnessMonitorKey(id)==key){metadata(item);wmi=true;}
            if(wmi)return TRUE;
            DWORD count=0;BrightnessPhysicalBatch batch;auto &monitors=batch.values;
            if(GetNumberOfPhysicalMonitorsFromHMONITOR(monitor,&count)&&count>0&&count<=8){monitors.resize(count);if(!GetPhysicalMonitorsFromHMONITOR(monitor,count,monitors.data())){for(auto &m:monitors)if(m.hPhysicalMonitor){DestroyPhysicalMonitor(m.hPhysicalMonitor);m.hPhysicalMonitor=nullptr;}monitors.clear();}}
            if(monitors.empty()){
                BrightnessState::Item item;item.id=L"unsupported:"+std::wstring(info.szDevice);item.available=false;metadata(item);(*context.items)[item.id]=std::move(item);return TRUE;
            }
            for(size_t i=0;i<monitors.size();++i){
                auto handle=std::make_shared<BrightnessPhysical>(monitors[i].hPhysicalMonitor);monitors[i].hPhysicalMonitor=nullptr;
                BrightnessState::Item item;item.id=L"ddc:"+std::to_wstring(context.generation)+L":"+info.szDevice+L":"+std::to_wstring(i);metadata(item);item.name=monitors[i].szPhysicalMonitorDescription;
                DWORD current=0;
                item.available=QueryPhysicalBrightness(handle->handle,item.minimum,current,item.maximum);
                if(item.available){item.physical=handle;item.value=static_cast<unsigned>(std::lround((current-item.minimum)*100.0/(item.maximum-item.minimum)));}
                (*context.items)[item.id]=std::move(item);
            }
            return TRUE;
        },reinterpret_cast<LPARAM>(&context));
        // Destruction of old physical handles happens outside the data lock.
        {std::scoped_lock lock(state->gate);state->items.swap(found);}
    }
    void Start(){
        if(state->active)return;
        try{
            winrt::com_ptr<IWbemLocator> locator;winrt::check_hresult(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(locator.put())));
            winrt::check_hresult(locator->ConnectServer(WmiString(L"ROOT\\WMI").get(),nullptr,nullptr,nullptr,WBEM_FLAG_CONNECT_USE_MAX_WAIT,nullptr,nullptr,service.put()));
            winrt::check_hresult(CoSetProxyBlanket(service.get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE));
        }catch(winrt::hresult_error const &e){error=e.code();service=nullptr;}
        Collect();state->active=true;
        if(service){sink.attach(new BrightnessEvents(state));auto hr=service->ExecNotificationQueryAsync(WmiString(L"WQL").get(),WmiString(L"SELECT * FROM WmiMonitorBrightnessEvent").get(),0,nullptr,sink.get());events=SUCCEEDED(hr);error=hr;}
    }
};
BrightnessProvider::BrightnessProvider(std::shared_ptr<ChangeState> state):impl(std::make_unique<Impl>(std::move(state))){}
BrightnessProvider::~BrightnessProvider(){impl->Stop();}
JsonObject BrightnessProvider::Read(bool refresh){
    if(refresh){{std::scoped_lock lock(impl->gate);if(impl->state->active)impl->Collect();else impl->Start();}impl->state->changes->Notify(2048);}
    JsonObject result;JsonArray displays;std::scoped_lock lock(impl->state->gate);
    ULONGLONG now=GetTickCount64();
    bool available=false;for(auto const &[id,item]:impl->state->items){JsonObject row;Text(row,L"id",id);Number(row,L"value",item.value/100.0);Text(row,L"monitorId",item.monitor);Text(row,L"name",item.name);Flag(row,L"primary",item.primary);Flag(row,L"available",item.available);Text(row,L"transport",item.physical?L"ddc":item.path.empty()?L"unsupported":L"wmi");Number(row,L"diagonalInches",item.inches);Number(row,L"widthPx",item.width);Number(row,L"heightPx",item.height);Flag(row,L"manual",item.manualUntil>now);available|=item.available;displays.Append(row);}
    Flag(result,L"available",impl->state->active&&available);Flag(result,L"events",impl->events);Flag(result,L"adaptive",AdaptiveBrightness());result.Insert(L"displays",displays);return result;
}
JsonObject BrightnessProvider::Subscribe(bool enabled){
    std::scoped_lock lock(impl->gate);
    if(!enabled)impl->Stop();else if(!impl->service)try{impl->Start();}catch(winrt::hresult_error const &error){impl->error=error.code();impl->Stop();}
    return Read();
}
JsonObject BrightnessProvider::Health()const{JsonObject r;Flag(r,L"subscribed",impl->state->active);Flag(r,L"events",impl->events);Number(r,L"callbacks",impl->state->callbacks);{std::scoped_lock lock(impl->state->gate);Number(r,L"physicalHandles",std::count_if(impl->state->items.begin(),impl->state->items.end(),[](auto const &i){return static_cast<bool>(i.second.physical);}));}Number(r,L"hresult",static_cast<LONG>(impl->error));Flag(r,L"polling",false);return r;}
JsonObject BrightnessProvider::Set(JsonObject const &args){
    double value=args.GetNamedNumber(L"value",-1);if(!std::isfinite(value)||value<0||value>1)throw std::invalid_argument("Brightness value outside 0..1");
    auto id=RequiredText(args,L"displayId",1024);std::scoped_lock lock(impl->gate);BrightnessState::Item item;
    {std::scoped_lock data(impl->state->gate);auto it=impl->state->items.find(id);if(it==impl->state->items.end()||!it->second.available)throw std::invalid_argument("Brightness display is no longer available");item=it->second;}
    if(item.physical){
        double actual=ApplyPhysicalBrightness(*item.physical,item.monitor,item.monitorKey,value);
        {std::scoped_lock data(impl->state->gate);if(impl->state->items.contains(id)){impl->state->items[id].value=static_cast<unsigned>(std::lround(actual*100));impl->state->items[id].manualUntil=GetTickCount64()+2500;}}
        impl->state->changes->Notify(2048);JsonObject result;Flag(result,L"accepted",true);Number(result,L"value",actual);return result;
    }
    if(!impl->service||item.path.empty())throw std::invalid_argument("Brightness display is no longer available");
    winrt::com_ptr<IWbemClassObject> type,parameters,input,output,outputSignature;
    winrt::check_hresult(impl->service->GetObject(WmiString(L"WmiMonitorBrightnessMethods").get(),0,nullptr,type.put(),nullptr));
    if(!type)winrt::throw_hresult(WBEM_E_INVALID_CLASS);
    winrt::check_hresult(type->GetMethod(L"WmiSetBrightness",0,parameters.put(),outputSignature.put()));
    if(!parameters)winrt::throw_hresult(WBEM_E_INVALID_METHOD_PARAMETERS);
    winrt::check_hresult(parameters->SpawnInstance(0,input.put()));if(!input)winrt::throw_hresult(E_POINTER);
    VARIANT timeout{};timeout.vt=VT_I4;timeout.lVal=0;winrt::check_hresult(input->Put(L"Timeout",0,&timeout,0));
    VARIANT level{};level.vt=VT_UI1;level.bVal=static_cast<BYTE>(std::lround(value*100));winrt::check_hresult(input->Put(L"Brightness",0,&level,0));
    winrt::check_hresult(impl->service->ExecMethod(WmiString(item.path.c_str()).get(),WmiString(L"WmiSetBrightness").get(),0,nullptr,input.get(),output.put(),nullptr));
    ValidateBrightnessResult(outputSignature.get(),output.get());
    {std::scoped_lock data(impl->state->gate);if(impl->state->items.contains(id)){impl->state->items[id].value=level.bVal;impl->state->items[id].manualUntil=GetTickCount64()+2500;}}
    impl->state->changes->Notify(2048);JsonObject result;Flag(result,L"accepted",true);Number(result,L"value",level.bVal/100.0);return result;
}
}
