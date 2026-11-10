#pragma once
#include <filesystem>
#include "native-honor-assets.h"
// Ordinary local resources only. No user supplied path reaches a decoder.
inline std::wstring HonorScheme(JsonObject const &preferences){
    return Str(preferences,L"iconScheme")==L"honor-translucent"?L"honor-translucent":L"honor-white";
}
inline std::wstring HonorStem(std::wstring value){
    auto slash=value.find_last_of(L"/\\");if(slash!=value.npos)value=value.substr(slash+1);
    auto dot=value.find_last_of(L'.');if(dot!=value.npos)value.resize(dot);
    std::transform(value.begin(),value.end(),value.begin(),::towlower);return value;
}
inline bool HonorCustomIcon(JsonObject const &item){
    if(Bool(item,L"customIcon"))return true;
    auto source=Str(item,L"icon");auto dot=source.find_last_of(L'.');
    if(dot==source.npos)return false;auto ext=source.substr(dot);std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
    return ext==L".png"||ext==L".ico"||ext==L".jpg"||ext==L".jpeg"||ext==L".bmp";
}
inline HonorAppAsset const *HonorApplicationPath(std::wstring const &path){
    auto dot=path.find_last_of(L'.');if(dot==path.npos)return nullptr;
    auto ext=path.substr(dot);std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
    if(ext!=L".exe"&&ext!=L".lnk")return nullptr;
    static auto aliases=[](){std::map<std::wstring,HonorAppAsset const*> result;for(auto const &asset:HonorAppAssets)result.emplace(asset.alias,&asset);return result;}();
    auto found=aliases.find(HonorStem(path));return found==aliases.end()?nullptr:found->second;
}
inline HonorAppAsset const *HonorApplication(JsonObject const &item){
    if(Str(item,L"type")!=L"AppOrFile"||HonorCustomIcon(item))return nullptr;
    auto path=Str(item,L"iconExecutable");if(path.empty())path=Str(item,L"path");
    return HonorApplicationPath(path);
}
inline std::wstring HonorDesktopFile(Tile const &item,std::wstring const &scheme,bool recycleFull){
    if(item.isFolder||item.card||!item.folder.empty())return L"";
    bool white=scheme!=L"honor-translucent";
    if(item.isVirtual){
        if(item.target==L"::{645FF040-5081-101B-9F08-00AA002F954E}"){
            if(recycleFull)return L"library/系统图标/RecycleBin/RecycleBin_NotEmpty.png";
            return white?L"library/DockIcon-白底/RecycleBin_ Empty.png":L"library/DockIcon-通透/RecycleBin_ Empty.png";
        }
        if(item.target==L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")return white?L"library/系统图标/MyComputer/MyComputer_White.png":L"library/系统图标/MyComputer/MyComputer_Transparent.png";
        return L"";
    }
    auto asset=HonorApplicationPath(item.target);return asset?(white?asset->white:asset->translucent):L"";
}
inline std::wstring HonorAppFile(JsonObject const &item,std::wstring const &scheme){
    auto asset=HonorApplication(item);return asset?(scheme==L"honor-translucent"?asset->translucent:asset->white):L"";
}
inline std::wstring HonorReferenceGlyph(std::wstring const &name){
    // The extracted recycle SVG embeds a PNG pattern unsupported by D2D SVG.
    // Menu glyphs use the bundled native outline instead of silently drawing nothing.
    if(name==L"PiTrashDuotone")return L"FaRegTrashAlt";
    static const std::map<std::wstring,std::wstring> names={
        {L"FaRegCircleUser",L"Account"},{L"FaWifi",L"WiFi4"},{L"TbWorldCancel",L"WiFiOff"},
        {L"FaComputer",L"matebook"},{L"IoDesktopOutline",L"matebook"},{L"BsKeyboard",L"keyboard"},
        {L"TbBluetooth",L"Bluetooth"},{L"TbBluetoothConnected",L"Bluetooth"},
        {L"IoVolumeHighOutline",L"Speaker3"},{L"IoVolumeMediumOutline",L"Speaker2"},
        {L"IoVolumeLowOutline",L"Speaker1"},{L"IoVolumeOffOutline",L"Speaker0"},{L"IoVolumeMuteOutline",L"SpeakerMute"},
        {L"IoChevronDownCircleOutline",L"tray"},{L"IoIosArrowDropdown",L"tray"},
        {L"LuSettings2",L"controlCenter"},{L"RiSettings4Fill",L"setting"},
        {L"TbZzz",L"sleep"},{L"MdNotificationsActive",L"havenotify"},{L"MdOutlineNotifications",L"nonotify"},
        {L"IoNotificationsOutline",L"notification"},{L"IoPower",L"shutdown"},{L"BsWindows",L"start-menu"},
        {L"IoTimeOutline",L"clock1"},{L"PiMusicNotesPlusFill",L"music1"},
        {L"TbPlugConnected",L"BatteryCharge"},
        {L"TbRefresh",L"rotate"},{L"IoClose",L"close"},{L"VscLock",L"lock"},
        {L"MagicDock_start_menu",L"start-menu"},{L"MagicDock_start_menu_dark",L"start-menu"},
        {L"MagicDock_desktop_light",L"matebook"},{L"MagicDock_desktop_dark",L"matebook"},
        {L"MagicDock_missing",L"APP"},{L"MagicDock_music_thumbnail",L"music1"}
    };
    if(name.starts_with(L"Honor:"))return name;
    if(name.starts_with(L"MagicBatteryNormal"))return L"Honor:Battery"+(name.substr(18)==L"0"?L"1":name.substr(18));
    if(name.starts_with(L"MagicBatteryCharge"))return L"Honor:BatteryAC"+(name.substr(18)==L"0"?L"1":name.substr(18));
    auto found=names.find(name);return found==names.end()?name:L"Honor:"+found->second;
}
inline std::wstring HonorModuleGlyph(JsonObject const &item){
    auto module=Str(item,L"nativeModule"),plugin=CanonicalNativeBarPlugin(Str(item,L"plugin"));
    if(module==L"magic.power-menu")return L"Honor:shutdown";
    if(module==L"magic.battery"||plugin==L"@default/weg-power"){
        if(!Bool(item,L"dataAvailable",true))return L"Honor:battery";
        int level=std::clamp(static_cast<int>(std::ceil(Num(item,L"batteryPercent",100)/10)),1,10);
        return std::wstring(Num(item,L"acLineStatus")==1?L"Honor:BatteryAC":L"Honor:Battery")+std::to_wstring(level);
    }
    if(module==L"magic.audio"||plugin==L"@magic/dock-media-popup"){
        if(Bool(item,L"dockMuted"))return L"Honor:SpeakerMute";
        int level=std::clamp(static_cast<int>(std::ceil(Num(item,L"dockVolume")*3)),0,3);
        return L"Honor:Speaker"+std::to_wstring(level);
    }
    if(module==L"magic.bluetooth"||plugin==L"@magic/dock-bluetooth-popup")return Bool(item,L"bluetoothOn")?L"Honor:Bluetooth":L"Honor:BluetoothOff";
    if(module==L"magic.network"||plugin==L"@magic/dock-network-popup"){
        if(!Bool(item,L"networkStateKnown",true))return L"Honor:WiFi4";
        if(!Bool(item,L"dockOnline"))return L"Honor:WiFiOff";
        if(!Bool(item,L"dockWifi"))return L"Honor:NetShare";
        int level=std::clamp(static_cast<int>(std::ceil(Num(item,L"wifiSignal",100)/25)),1,4);
        return L"Honor:WiFi"+std::to_wstring(level);
    }
    if(module==L"magic.quick-settings")return L"Honor:controlCenter";
    if(module==L"magic.user")return L"Honor:HONOR";
    if(module==L"magic.notifications"||plugin==L"@magic/dock-notifications")return Bool(item,L"hasNotifications")?L"Honor:havenotify":L"Honor:nonotify";
    if(module==L"magic.keyboard"||plugin==L"@magic/dock-keyboard-selector")return L"Honor:keyboard";
    if(module==L"magic.clock"||plugin==L"@magic/dock-calendar-popup")return L"Honor:clock1";
    if(module==L"magic.desktop"||plugin==L"@default/weg-show-desktop")return L"Honor:matebook";
    if(module==L"magic.tray"||plugin==L"@magic/dock-system-tray")return L"Honor:tray";
    if(module==L"magic.start"||plugin==L"@default/weg-start-menu")return L"Honor:start-menu";
    return L"";
}
inline std::wstring HonorDockFile(JsonObject const &item,std::wstring const &scheme){
    auto app=HonorAppFile(item,scheme);if(!app.empty())return app;
    if(HonorCustomIcon(item))return L"";
    auto plugin=CanonicalNativeBarPlugin(Str(item,L"plugin"));bool white=scheme!=L"honor-translucent";
    if(plugin==L"@default/weg-start-menu")return white?L"library/TopBar/start-menu.png":L"library/TopBar/start-menu-transparent.png";
    if(plugin==L"@default/weg-trash-bin"){
        if(Num(item,L"trashCount")>0)return L"library/系统图标/RecycleBin/RecycleBin_NotEmpty.png";
        return white?L"library/DockIcon-白底/RecycleBin_ Empty.png":L"library/DockIcon-通透/RecycleBin_ Empty.png";
    }
    if(plugin==L"@default/weg-show-desktop")return white?L"library/系统图标/MyComputer/MyComputer_White.png":L"library/系统图标/MyComputer/MyComputer_Transparent.png";
    return L"";
}
// Only reviewed white-plate fallbacks may use the native alpha conversion.
// Arbitrary/custom artwork is never keyed by color or modified on disk.
inline bool HonorWhitePlateFallback(std::wstring const &file){return file==L"library/DockIcon-白底/ChatGPT.png"||file==L"library/DockIcon-白底/资源管理器.png";}
inline void HonorTransparentPixels(std::vector<BYTE> &pixels,bool monochrome,UINT width=0){
    for(size_t i=0;i+3<pixels.size();i+=4){float a=pixels[i+3]/255.f;if(a<=0)continue;
        float lo=255,hi=0;for(int j=0;j<3;++j){float v=std::min(255.f,pixels[i+j]/a);lo=std::min(lo,v);hi=std::max(hi,v);}
        if(monochrome&&width){auto x=(i/4)%width,y=(i/4)/width;auto height=pixels.size()/4/width;if(x<width*.12f||x>width*.88f||y<height*.12f||y>height*.88f){pixels[i]=pixels[i+1]=pixels[i+2]=pixels[i+3]=0;continue;}}
        if(monochrome){float next=a*std::clamp((238.f-lo)/238.f,0.f,1.f);pixels[i]=pixels[i+1]=pixels[i+2]=0;pixels[i+3]=static_cast<BYTE>(std::round(next*255));}
        else if(hi-lo<12){float k=0;for(int j=0;j<4;++j)pixels[i+j]=static_cast<BYTE>(std::round(pixels[i+j]*k));}
    }
}
// One cache shared by all bars, <= 48 decoded textures / 4 MiB, max 256px.
// SVG and bitmap objects are released on scheme switch/device reset/session exit.
struct NativeHonorBitmapCache{
    struct Entry{com_ptr<ID2D1Bitmap1> bitmap;size_t bytes;unsigned long long age;};
    std::map<std::wstring,Entry> entries;std::set<std::wstring> failed;
    size_t bytes=0;unsigned long long clock=0,decodes=0;std::wstring scheme;
    void Clear(){entries.clear();failed.clear();bytes=0;}
    void Scheme(std::wstring const &value){if(scheme!=value){Clear();scheme=value;}}
    bool Draw(Graphics &g,ID2D1DeviceContext *c,std::wstring const &file,D2D1_RECT_F rect,bool lightGlyph=false){
        if(file.empty()||!file.starts_with(L"library/")||file.find(L"..")!=file.npos)return false;
        float dx=96,dy=96;c->GetDpi(&dx,&dy);UINT pixels=static_cast<UINT>(std::clamp(std::ceil(std::max(rect.right-rect.left,rect.bottom-rect.top)*dx/96),16.f,256.f));
        bool monochrome=scheme==L"honor-translucent"&&file.ends_with(L"/ChatGPT.png");
        auto key=file+L":"+std::to_wstring(pixels)+(monochrome&&lightGlyph?L":light":L"");if(failed.contains(key))return false;auto found=entries.find(key);
        if(found==entries.end()){
            try{
                wchar_t module[32768]{};if(!GetModuleFileNameW(nullptr,module,32768))return false;
                auto path=std::filesystem::path(module).parent_path()/L"honor-icons"/std::filesystem::path(file);
                std::error_code error;auto length=std::filesystem::file_size(path,error);if(error||length>2*1024*1024)return false;
                com_ptr<IWICBitmapDecoder> decoder;check_hresult(g.wic->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,decoder.put()));
                com_ptr<IWICBitmapFrameDecode> frame;check_hresult(decoder->GetFrame(0,frame.put()));UINT w=0,h=0;check_hresult(frame->GetSize(&w,&h));if(!w||!h||w>2048||h>2048)return false;
                float scale=std::min(1.f,static_cast<float>(pixels)/std::max(w,h));UINT pw=std::max(1u,static_cast<UINT>(w*scale)),ph=std::max(1u,static_cast<UINT>(h*scale));
                com_ptr<IWICBitmapScaler> scaler;check_hresult(g.wic->CreateBitmapScaler(scaler.put()));check_hresult(scaler->Initialize(frame.get(),pw,ph,WICBitmapInterpolationModeFant));
                com_ptr<IWICFormatConverter> converter;check_hresult(g.wic->CreateFormatConverter(converter.put()));check_hresult(converter->Initialize(scaler.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
                Entry entry;entry.bytes=static_cast<size_t>(pw)*ph*4;entry.age=++clock;
                if(scheme==L"honor-translucent"&&HonorWhitePlateFallback(file)){
                    std::vector<BYTE> pixels(entry.bytes);check_hresult(converter->CopyPixels(nullptr,pw*4,static_cast<UINT>(pixels.size()),pixels.data()));HonorTransparentPixels(pixels,monochrome,pw);if(monochrome&&lightGlyph)for(size_t i=0;i<pixels.size();i+=4)pixels[i]=pixels[i+1]=pixels[i+2]=pixels[i+3];
                    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));check_hresult(c->CreateBitmap({pw,ph},pixels.data(),pw*4,&props,entry.bitmap.put()));
                }else check_hresult(c->CreateBitmapFromWicBitmap(converter.get(),nullptr,entry.bitmap.put()));
                while(!entries.empty()&&(entries.size()>=48||bytes+entry.bytes>4*1024*1024)){auto oldest=std::min_element(entries.begin(),entries.end(),[](auto const &a,auto const &b){return a.second.age<b.second.age;});bytes-=oldest->second.bytes;entries.erase(oldest);}
                bytes+=entry.bytes;found=entries.emplace(key,std::move(entry)).first;++decodes;
            }catch(hresult_error const &){if(failed.size()<64)failed.insert(key);return false;}
        }
        found->second.age=++clock;
        auto size=found->second.bitmap->GetSize();float fit=std::min((rect.right-rect.left)/size.width,(rect.bottom-rect.top)/size.height),cx=(rect.left+rect.right)/2,cy=(rect.top+rect.bottom)/2;
        c->DrawBitmap(found->second.bitmap.get(),{cx-size.width*fit/2,cy-size.height*fit/2,cx+size.width*fit/2,cy+size.height*fit/2},1,D2D1_INTERPOLATION_MODE_LINEAR);return true;
    }
};
