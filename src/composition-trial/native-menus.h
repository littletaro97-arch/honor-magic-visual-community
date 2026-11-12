#pragma once
#include "native-popup.h"
// Shared transient cascade host. Each level keeps its own HWND and hit rows;
// hidden levels release drawing surfaces and never retain timers or Shell COM.
inline void NativeOwnSurfaceVeil(App &app,POINT origin,bool show){
    for(auto const &window:app.windows){auto &w=*window;if(!w.root)continue;bool active=show&&w.hwnd&&MonitorFromWindow(w.hwnd,MONITOR_DEFAULTTONEAREST)==MonitorFromPoint(origin,MONITOR_DEFAULTTONEAREST);
        if(active&&!w.surfaceVeil){w.surfaceVeil=app.graphics.compositor.CreateSpriteVisual();w.surfaceVeil.Brush(app.graphics.compositor.CreateColorBrush({14,10,12,18}));w.root.Children().InsertAtTop(w.surfaceVeil);}
        if(w.surfaceVeil){w.surfaceVeil.Size({w.width*w.dpi/96,w.height*w.dpi/96});w.surfaceVeil.IsVisible(active);}
    }
}
struct NativeMenus {
    struct Level {std::unique_ptr<NativePopup> popup;JsonArray values{};std::wstring opened;bool opening=false;};
    App &app;
    std::vector<Level> levels;
    size_t visibleLevels=0;
    POINT origin{};
    bool closing=false;
    std::wstring style,rootAlignX=L"Start",rootAlignY=L"Start";
    std::wstring barSide;
    int barBoundary=0;
    HHOOK outsideHook=nullptr;
    uint64_t outsideGeneration=0;
    bool outsideQueued=false;
    inline static NativeMenus *outsideOwner=nullptr;
    std::function<void(size_t,std::wstring const&)> choose,prepare;
    std::function<void()> dismiss;
    std::function<bool(ID2D1DeviceContext*,std::wstring const&,D2D1_RECT_F)> icon;
    explicit NativeMenus(App &app):app(app){}
    ~NativeMenus(){Close(false);}
    bool ContainsPoint(POINT point)const{
        for(size_t i=0;i<visibleLevels;i++){
            auto &popup=*levels[i].popup;if(!popup.visible||popup.dismissing)continue;
            auto rect=popup.ContentScreenRect();float scale=popup.dpi/96;
            if(NativeContextMotion::Contains({(point.x-rect.left)/scale,(point.y-rect.top)/scale},{popup.width,popup.height},popup.MaterialRadius()))return true;
        }return false;
    }
    void OutsidePointer(POINT point){
        if(!visibleLevels||outsideQueued||ContainsPoint(point))return;
        // Do not close windows or steal the click from the low-level callback.
        outsideQueued=PostMessageW(levels[0].popup->hwnd,NativePopup::ContextOutsideMessage,static_cast<WPARAM>(outsideGeneration),0)!=FALSE;
    }
    void OutsideMessage(uint64_t generation){if(generation==outsideGeneration&&outsideQueued){outsideQueued=false;Close();}}
    static LRESULT CALLBACK ObserveMouse(int code,WPARAM message,LPARAM data){
        if(code==HC_ACTION&&outsideOwner&&data&&(message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN||message==WM_XBUTTONDOWN))
            outsideOwner->OutsidePointer(reinterpret_cast<MSLLHOOKSTRUCT const*>(data)->pt);
        return CallNextHookEx(nullptr,code,message,data);
    }
    void WatchOutside(){
        // An inactive popup may never receive WA_INACTIVE when the user clicks
        // its already-foreground full-screen app. Observe only while open.
        if(outsideHook||app.model.synthetic||app.popupCycles)return;
        if(outsideOwner&&outsideOwner!=this)outsideOwner->StopOutside();
        outsideOwner=this;outsideHook=SetWindowsHookExW(WH_MOUSE_LL,ObserveMouse,GetModuleHandleW(nullptr),0);
        if(!outsideHook)outsideOwner=nullptr;
    }
    void StopOutside(){
        if(outsideHook){UnhookWindowsHookEx(outsideHook);outsideHook=nullptr;}
        if(outsideOwner==this)outsideOwner=nullptr;
        ++outsideGeneration;outsideQueued=false;
    }
    void RootPlacement(JsonObject const &payload){
        JsonObject position;Put(position,L"x",static_cast<double>(origin.x));Put(position,L"y",static_cast<double>(origin.y));payload.Insert(L"desiredPosition",position);
        Put(payload,L"alignX",rootAlignX);Put(payload,L"alignY",rootAlignY);
        Put(payload,L"contextBarSide",barSide);Put(payload,L"contextBarBoundary",static_cast<double>(barBoundary));
    }
    bool Family(HWND window)const {
        for(auto const &level:levels)if(level.popup->hwnd==window||IsChild(level.popup->hwnd,window))return true;
        return false;
    }
    void HideFrom(size_t depth){
        for(size_t i=visibleLevels;i>depth;--i){auto &p=*levels[i-1].popup;p.contextDismiss={};p.Hide(false);levels[i-1].opened.clear();levels[i-1].opening=false;}
        if(visibleLevels>depth&&depth){levels[depth-1].opened.clear();levels[depth-1].opening=false;}
        visibleLevels=std::min(visibleLevels,depth);
    }
    void Close(bool notify=true){
        if(closing)return;
        closing=true;StopOutside();NativeOwnSurfaceVeil(app,origin,false);auto callback=std::move(dismiss);choose={};prepare={};HideFrom(0);icon={};closing=false;
        if(notify&&callback)callback();
    }
    void Begin(POINT point,std::wstring menuStyle,std::function<void(size_t,std::wstring const&)> action,std::function<void()> cancel){
        Close();rootAlignX=rootAlignY=L"Start";barSide.clear();barBoundary=0;origin=point;style=std::move(menuStyle);choose=std::move(action);dismiss=std::move(cancel);
    }
    void Back(size_t depth){
        if(depth==0){Close();return;}
        HideFrom(depth);auto &parent=*levels[depth-1].popup;
        SetForegroundWindow(parent.hwnd);SetFocus(parent.hwnd);
    }
    void Dispatch(size_t depth,std::wstring const &id){
        if(depth>=visibleLevels)return;
        auto &level=levels[depth];
        for(auto const &v:level.values){auto row=v.GetObject();if(Str(row,L"id")!=id||!Bool(row,L"enabled",true))continue;
            if((style==L"desktop"||style==L"toolbar")&&Str(row,L"control")==L"submenu"&&level.opened==id&&(level.opening||visibleLevels>depth+1))return;
            level.opened=id;
            level.opening=Str(row,L"control")==L"submenu";
            if(auto children=row.TryLookup(L"children");children&&children.ValueType()==JsonValueType::Array){Show(depth+1,children.GetArray());return;}
            auto callback=choose;if(callback)callback(depth,id);return;
        }
    }
    void Hover(size_t depth,std::wstring const &id){
        if(depth>=visibleLevels)return;
        // Desktop and bar cascades open explicitly. A separate one-shot dwell
        // callback may prepare one child without revealing it.
        if(style==L"desktop"||style==L"toolbar")return;
        for(auto const &v:levels[depth].values){auto row=v.GetObject();if(Str(row,L"id")!=id)continue;
            if(Str(row,L"control")==L"submenu"){
                if(levels[depth].opened!=id)Dispatch(depth,id);
            }else{HideFrom(depth+1);levels[depth].opened.clear();}
            return;
        }
    }
    void ReplaceRoot(JsonArray const &values){
        if(levels.empty()||!levels[0].popup->visible){Show(0,values);return;}
        HideFrom(1);auto &level=levels[0];level.values=values;auto &popup=*level.popup;
        RootPlacement(popup.packet.GetNamedObject(L"payload"));popup.contextIcon=icon;popup.SetContextRows(values);
    }
    void Show(size_t depth,JsonArray const &values){
        if(depth>16||!values.Size())return;
        if(depth&&depth<=levels.size())levels[depth-1].opening=false;
        HideFrom(depth+1);
        while(levels.size()<=depth){Level level;level.popup=std::make_unique<NativePopup>(app);levels.push_back(std::move(level));}
        auto &p=*levels[depth].popup;
        POINT point=origin;RECT parentRect{};bool flipped=false;float rowAnchor=static_cast<float>(origin.y);
        if(depth){
            auto &parent=*levels[depth-1].popup;RECT rect{};if(parent.AnimatedContext())rect=parent.ContentScreenRect();else GetWindowRect(parent.hwnd,&rect);
            point={rect.right-static_cast<LONG>(4*parent.dpi/96),rect.top};
            for(auto const &row:parent.rows)if(row.id==levels[depth-1].opened)point.y+=static_cast<LONG>((row.rect.top-10)*parent.dpi/96);
            parentRect=rect; // The popup selects its side after measuring its final multi-column width.
            rowAnchor=point.y+22*parent.dpi/96;
        }
        p.contextDismiss={};p.contextFamily=[this](HWND hwnd){return Family(hwnd);};
        levels[depth].values=values;visibleLevels=std::max(visibleLevels,depth+1);
        JsonObject packet,data,payload,position;
        Put(packet,L"kind",std::wstring(L"@magic/context-menu"));Put(packet,L"token",L"local-menu-"+std::to_wstring(depth));
        Put(position,L"x",static_cast<double>(point.x));Put(position,L"y",static_cast<double>(point.y));payload.Insert(L"desiredPosition",position);
        Put(payload,L"alignX",depth?std::wstring(L"Start"):rootAlignX);Put(payload,L"alignY",depth?std::wstring(L"Start"):rootAlignY);
        Put(payload,L"contextBarSide",barSide);Put(payload,L"contextBarBoundary",static_cast<double>(barBoundary));
        Put(payload,L"contextDepth",static_cast<double>(depth));payload.Insert(L"contextFlipped",JsonValue::CreateBooleanValue(flipped));Put(payload,L"contextAnchorY",static_cast<double>(rowAnchor));
        if(depth){Put(payload,L"contextParentLeft",static_cast<double>(parentRect.left));Put(payload,L"contextParentRight",static_cast<double>(parentRect.right));}
        data.Insert(L"rows",values);data.Insert(L"dark",JsonValue::CreateBooleanValue(Preview70Theme::systemDark));Put(data,L"menuStyle",style);
        packet.Insert(L"payload",payload);packet.Insert(L"data",data);packet.Insert(L"account",JsonObject{});
        // Context menus do not dim unrelated desktop content.
        p.Show(packet);NativeOwnSurfaceVeil(app,origin,false);
        p.contextAction=[this,depth](std::wstring const &id){Dispatch(depth,id);};
        p.contextHover=[this,depth](std::wstring const &id){Hover(depth,id);};
        p.contextPrepare=[this,depth](std::wstring const &id){if(depth<visibleLevels&&prepare)prepare(depth,id);};
        p.contextBack=[this,depth](){Back(depth);};
        p.contextDismiss=[this](){Close();};
        p.contextOutside=[this](uint64_t generation){OutsideMessage(generation);};
        p.contextIcon=icon;p.Draw();
        WatchOutside();
    }
};
inline NativeMenus &Menus(App &app){if(!app.menus)app.menus=std::make_unique<NativeMenus>(app);return *app.menus;}
inline JsonArray NativePopup::AppMenuRows(int index){
    JsonArray result;if(index<0||index>=static_cast<int>(rows.size())||!rows[index].id.starts_with(L"app:"))return result;
    auto value=Data().GetNamedArray(L"rows").GetAt(index).GetObject();
    auto add=[&](int command,std::wstring const &label,bool enabled=true){JsonObject row;Put(row,L"id",std::to_wstring(command));Put(row,L"label",label);row.Insert(L"enabled",JsonValue::CreateBooleanValue(enabled));result.Append(row);};
    if(!Bool(value,L"enabled",true)||Bool(value,L"secondaryTile"))return result;
    add(11,L"固定到 Dock");
    JsonObject separator;Put(separator,L"id",std::wstring(L"separator"));Put(separator,L"control",std::wstring(L"separator"));separator.Insert(L"enabled",JsonValue::CreateBooleanValue(false));result.Append(separator);
    add(12,L"以管理员身份运行");add(13,L"打开文件所在位置",Bool(value,L"hasLocation",Bool(value,L"hasPath")));add(14,L"卸载（Windows 应用设置）");add(15,L"编辑图标");return result;
}
inline void NativePopup::CloseAppMenu(){if(contextMenuOpen&&app.menus){contextMenuOpen=false;app.menus->Close(false);}}
inline void NativePopup::AppMenu(int index,POINT point){
    auto values=AppMenuRows(index);if(!values.Size())return;auto owner=life;auto key=rows[index].id;
    auto &menus=Menus(app);menus.Begin(point,L"desktop",[owner,key](size_t,std::wstring const &command){
        if(!owner||!owner->owner)return;auto &popup=*owner->owner;
        popup.CloseAppMenu();if(!popup.visible||popup.dismissing)return;
        SetForegroundWindow(popup.hwnd);popup.FocusReadyInput();
        auto found=std::find_if(popup.rows.begin(),popup.rows.end(),[&](auto const &row){return row.id==key;});
        if(found!=popup.rows.end())popup.Command(*found,-1,_wtoi(command.c_str()));
    },[owner](){
        if(!owner||!owner->owner)return;auto &popup=*owner->owner;popup.contextMenuOpen=false;
        if(!popup.visible||popup.dismissing)return;auto active=GetForegroundWindow();
        if(popup.app.menus&&popup.app.menus->Family(active)){SetForegroundWindow(popup.hwnd);popup.FocusReadyInput();}
        else if(active!=popup.hwnd&&!IsChild(popup.hwnd,active)&&!popup.InputFamily(active))popup.Hide();
    });contextMenuOpen=true;menus.Show(0,values);
}
