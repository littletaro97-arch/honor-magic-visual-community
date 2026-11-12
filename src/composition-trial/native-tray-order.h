#pragma once
#include <cfloat>
// Slots stay fixed for one gesture. The insertion index, not a moving neighbour,
// is the common contract for preview and the authenticated final request.
struct NativeTrayOrder {
    static std::vector<int> Build(int count,int from,int to){
        std::vector<int> order;if(from<0||to<0||from>=count||to>=count)return order;
        for(int i=0;i<count;++i)if(i!=from)order.push_back(i);
        order.insert(order.begin()+to,from);return order;
    }
    static int Nearest(std::vector<PopupRow> const &rows,float x,float y){
        int best=-1;float distance=FLT_MAX;
        for(int i=0;i<static_cast<int>(rows.size());++i){auto r=rows[i].rect;float dx=x-(r.left+r.right)/2,dy=y-(r.top+r.bottom)/2,d=dx*dx+dy*dy;
            if(d<distance){distance=d;best=i;}}
        return best;
    }
};
struct NativeTrayDrag {
    struct Item{SpriteVisual visual{nullptr};CompositionDrawingSurface surface{nullptr};int slot=-1;};
    std::vector<PopupRow> original;std::vector<int> order;std::vector<Item> items;
    ContainerVisual layer{nullptr};size_t bytes=0;
    int from=-1,to=-1;float2 grab{},pointer{};
    bool moving=true,awaiting=false,visualAttempted=false;
    ULONGLONG finishAt=0,deadline=0;std::wstring receipt;
    JsonObject deferred{nullptr};
    JsonObject Result()const{
        JsonObject result;Put(result,L"action",std::wstring(L"native-command"));Put(result,L"id",original[from].id);Put(result,L"button",5.0);
        Put(result,L"destination",original[to].id);result.Insert(L"after",JsonValue::CreateBooleanValue(to>from));Put(result,L"reorderReceipt",receipt);return result;
    }
};
