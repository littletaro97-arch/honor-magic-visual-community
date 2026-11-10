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
    // Which bar item opened this cascade, and what that item looked like when it did.
    // A standing menu is rebuilt from the item that owns it, never from a guessed one.
    std::wstring itemKey,itemStamp;HWND itemBar=nullptr;
    std::function<void(size_t,std::wstring const&)> choose,prepare;
    std::function<void()> dismiss;
    std::function<bool(ID2D1DeviceContext*,std::wstring const&,D2D1_RECT_F)> icon;
    explicit NativeMenus(App &app):app(app){}
    ~NativeMenus(){Close(false);}
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
        closing=true;NativeOwnSurfaceVeil(app,origin,false);auto callback=std::move(dismiss);choose={};prepare={};HideFrom(0);icon={};itemKey.clear();itemStamp.clear();itemBar=nullptr;closing=false;
        if(notify&&callback)callback();
    }
    void Begin(POINT point,std::wstring menuStyle,std::function<void(size_t,std::wstring const&)> action,std::function<void()> cancel){
        Close();rootAlignX=rootAlignY=L"Start";origin=point;style=std::move(menuStyle);choose=std::move(action);dismiss=std::move(cancel);
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
            // Repaint before reporting: the reader sees the new tick under the cursor,
            // and the adapter's answer only has to agree with it.
            if(Bool(row,L"keepOpen")&&row.HasKey(L"checked"))Tick(depth,id);
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
        // Rebuilding the root used to cut every level below it. The caller that
        // refreshes a standing menu now keeps the reader where they are.
        Rebuild(values);
    }
    // A tick row is one alternative in a small set. Reporting the click and closing
    // turns a run of tries into "reopen, find the submenu, try again", so a row that
    // carries keepOpen repaints with the new tick and leaves the level standing.
    bool Persistent(size_t depth,std::wstring const &id)const{
        if(depth>=visibleLevels||depth>=levels.size())return false;
        for(auto const &v:levels[depth].values){auto row=v.GetObject();if(Str(row,L"id")==id)return Bool(row,L"keepOpen");}
        return false;
    }
    void Tick(size_t depth,std::wstring const &id){
        if(depth>=visibleLevels||depth>=levels.size())return;
        auto &level=levels[depth];
        // A solo row is a state switch that shares a level with a radio group (the
        // colour/glyph switch sits above the 39-vendor list): picking it must not
        // clear the vendor tick, and picking a vendor must not clear it.
        bool solo=false;for(auto const &v:level.values){auto row=v.GetObject();if(Str(row,L"id")==id&&Bool(row,L"independent"))solo=true;}
        for(auto const &v:level.values){auto row=v.GetObject();if(!row.HasKey(L"checked"))continue;
            // A row outside the level's radio group keeps its own state when a sibling
            // is picked; a two-state row flips itself instead of claiming the click as
            // a new selection.
            bool clicked=Str(row,L"id")==id,independent=Bool(row,L"independent");
            bool next=clicked?(Bool(row,L"toggle")?!Bool(row,L"checked"):true):(solo||independent?Bool(row,L"checked"):false);
            row.Insert(L"checked",JsonValue::CreateBooleanValue(next));}
        // The same in-place re-layout Rebuild() uses: the reader picked a row in a
        // list they had scrolled to, so the level must come back at the same offset
        // instead of jumping to the top under the cursor.
        float scroll=level.popup?level.popup->ContextScrollOffset():0.f;bool shown=level.popup&&level.popup->visible;
        if(level.popup)level.popup->SetContextRows(level.values);
        if(shown)level.popup->SetContextScrollOffset(scroll);
    }
    struct LevelAnchor{POINT point{};RECT parentRect{};float rowAnchor=0;};
    // Where a level hangs from: the root sits on the pointer, a child on the right
    // edge of the row that opened it. Show() and Rebuild() share this, so a rebuilt
    // level lands exactly where reopening it would.
    LevelAnchor Anchor(size_t depth){
        LevelAnchor result;result.point=origin;result.rowAnchor=static_cast<float>(origin.y);
        if(!depth||depth>levels.size())return result;
        auto &parent=*levels[depth-1].popup;RECT rect{};if(parent.AnimatedContext())rect=parent.ContentScreenRect();else GetWindowRect(parent.hwnd,&rect);
        result.point={rect.right-static_cast<LONG>(4*parent.dpi/96),rect.top};
        for(auto const &row:parent.rows)if(row.id==levels[depth-1].opened)result.point.y+=static_cast<LONG>((row.rect.top-10)*parent.dpi/96);
        result.parentRect=rect; // The popup selects its side after measuring its final multi-column width.
        result.rowAnchor=result.point.y+22*parent.dpi/96;
        return result;
    }
    // Rebuild every open level from freshly generated root values while keeping the
    // submenu each level has open. The ids are generated by the same code in the same
    // order, so following the parent's opened id into the new children restores the
    // stack. A level whose parent row no longer has children ends the stack there.
    void Rebuild(JsonArray const &values){
        if(levels.empty()||!levels[0].popup||!levels[0].popup->visible)return;
        std::vector<std::wstring> trail;for(size_t i=0;i<visibleLevels&&i<levels.size();++i)trail.push_back(levels[i].opened);
        JsonArray current=values;size_t keep=0;
        for(size_t depth=0;depth<trail.size()&&depth<levels.size();++depth){
            auto &level=levels[depth];
            level.values=current;level.opened=trail[depth];level.opening=false;
            // Re-anchor from the parent as it stands now, not from where it stood when
            // the level first opened: an in-place rebuild must land like a reopen.
            if(depth&&level.popup){
                auto anchor=Anchor(depth);auto payload=level.popup->packet.GetNamedObject(L"payload");JsonObject position;
                Put(position,L"x",static_cast<double>(anchor.point.x));Put(position,L"y",static_cast<double>(anchor.point.y));payload.Insert(L"desiredPosition",position);
                Put(payload,L"contextAnchorY",static_cast<double>(anchor.rowAnchor));
                Put(payload,L"contextParentLeft",static_cast<double>(anchor.parentRect.left));Put(payload,L"contextParentRight",static_cast<double>(anchor.parentRect.right));
            }
            float scroll=level.popup?level.popup->ContextScrollOffset():0.f;bool shown=level.popup&&level.popup->visible;
            if(level.popup)level.popup->SetContextRows(current);
            if(shown)level.popup->SetContextScrollOffset(scroll);
            ++keep;
            if(trail[depth].empty())break;
            JsonArray children{};bool found=false;
            for(auto const &v:current){auto row=v.GetObject();if(Str(row,L"id")!=trail[depth])continue;
                if(auto value=row.TryLookup(L"children");value&&value.ValueType()==JsonValueType::Array){children=value.GetArray();found=children.Size()>0;}break;}
            if(!found){level.opened.clear();break;}
            current=children;
        }
        if(keep<visibleLevels)HideFrom(keep);
    }
    void Show(size_t depth,JsonArray const &values){
        if(depth>16||!values.Size())return;
        if(depth&&depth<=levels.size())levels[depth-1].opening=false;
        HideFrom(depth+1);
        while(levels.size()<=depth){Level level;level.popup=std::make_unique<NativePopup>(app);levels.push_back(std::move(level));}
        auto &p=*levels[depth].popup;
        auto anchor=Anchor(depth);POINT point=anchor.point;RECT parentRect=anchor.parentRect;bool flipped=false;float rowAnchor=anchor.rowAnchor;
        p.contextDismiss={};p.contextFamily=[this](HWND hwnd){return Family(hwnd);};
        levels[depth].values=values;visibleLevels=std::max(visibleLevels,depth+1);
        JsonObject packet,data,payload,position;
        Put(packet,L"kind",std::wstring(L"@magic/context-menu"));Put(packet,L"token",L"local-menu-"+std::to_wstring(depth));
        Put(position,L"x",static_cast<double>(point.x));Put(position,L"y",static_cast<double>(point.y));payload.Insert(L"desiredPosition",position);
        Put(payload,L"alignX",depth?std::wstring(L"Start"):rootAlignX);Put(payload,L"alignY",depth?std::wstring(L"Start"):rootAlignY);
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
        p.contextIcon=icon;p.Draw();
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
    add(12,L"以管理员身份运行");add(13,L"打开文件所在位置",Bool(value,L"hasPath"));add(14,L"卸载（Windows 应用设置）");add(15,L"编辑图标");return result;
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
