#pragma once
// Visual grouping never rewrites the saved section/module/manual-separator order.
namespace NativeDockGroups {
inline constexpr auto Separator=L"@automatic:running";
inline JsonObject Project(JsonObject const &source,JsonObject const &settings,std::wstring const &monitor,bool primary,bool line){
    if(Bool(source,L"groupedRunningApps"))return source;
    JsonObject result;result.Insert(L"groupedRunningApps",JsonValue::CreateBooleanValue(true));
    std::vector<JsonObject> groups[3],running;int lastGroup=-1;size_t lastIndex=0;
    wchar_t const *names[]={L"left",L"center",L"right"};
    for(int s=0;s<3;++s)for(auto const &value:source.GetNamedArray(names[s],JsonArray{})){
        auto item=value.GetObject();
        if(Str(item,L"type")==L"AppOrFile"){
            bool pinned=Bool(item,L"pinned"),hasWindows=false;
            for(auto const &window:item.GetNamedArray(L"windows",JsonArray{}))
                if(Str(settings,L"temporalItemsVisibility")!=L"OnMonitor"||Str(window.GetObject(),L"monitor")==monitor){hasWindows=true;break;}
            if(!hasWindows&&(!pinned||(!primary&&Str(settings,L"pinnedItemsVisibility")==L"WhenPrimary")))continue;
            if(!pinned){running.push_back(item);continue;}
            lastGroup=s;lastIndex=groups[s].size()+1;
        }
        groups[s].push_back(item);
    }
    int target=lastGroup<0?1:lastGroup;size_t at=lastGroup<0?groups[target].size():lastIndex;
    if(lastGroup>=0&&!running.empty()){
        JsonObject marker;Put(marker,L"id",std::wstring(Separator));Put(marker,L"key",std::wstring(Separator));Put(marker,L"type",std::wstring(L"Separator"));
        marker.Insert(L"automaticRunningSeparator",JsonValue::CreateBooleanValue(true));marker.Insert(L"separatorVisible",JsonValue::CreateBooleanValue(line));
        groups[target].insert(groups[target].begin()+at++,marker);
    }
    groups[target].insert(groups[target].begin()+at,running.begin(),running.end());
    for(int s=0;s<3;++s){JsonArray rows;for(auto const &item:groups[s])rows.Append(item);result.Insert(names[s],rows);}
    return result;
}
}
