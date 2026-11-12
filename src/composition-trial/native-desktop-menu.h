#pragma once
// Compact, GPU-drawn menu; every Shell verb remains in the bounded STA worker.
// One shared popup is used only while a menu is visible. No shell extensions in
// the renderer.
inline void NativePopup::BuildContextRows() {
  rows.clear();
  auto values = Data().GetNamedArray(L"rows");
  contextTotal=0;contextScroll=0;
  bool toolbar=Str(Data(),L"menuStyle")==L"toolbar";
  float top=toolbar?8.f:10.f,y=top,x=0,columnWidth=contextColumnWidth>0?contextColumnWidth:width;
  for (UINT i = 0; i < values.Size() && i < 256; i++) {
    auto v = values.GetAt(i).GetObject();
    PopupRow row;
    row.id = row.key = Str(v, L"id");
    row.label = Str(v, L"label");
    row.detail = Str(v, L"detail");
    row.control = Str(v, L"control");
    row.enabled = Bool(v, L"enabled", true) && row.control != L"separator" && row.control != L"header";
    row.active = Bool(v, L"checked");row.dimmed=Bool(v,L"dimmed");
    row.action = L"context";
    float h = ContextRowHeight(v);
    if(contextColumns>1&&y>top&&y-top+h>ContextViewport()+.1f){contextTotal=std::max(contextTotal,y-top);y=top;x+=columnWidth;}
    row.rect = {x+8, y, x+columnWidth - 8, y + h-(row.control!=L"separator"?(toolbar?8.f:2.f):0.f)};
    row.offscreen=row.rect.bottom<=10||row.rect.top>=height-10;
    y += h;
    contextTotal=std::max(contextTotal,y-top);
    rows.push_back(row);
    auto png = Str(v, L"icon");
    if (png.rfind(L"data:image/png;base64,", 0) == 0 &&
        !appearanceIcons.values.contains(row.id))
      appearanceIcons.Load(app.graphics, row.id, png.substr(22));
  }
}
inline void NativePopup::PaintContext(ID2D1DeviceContext *c) {
  NativePaint p(c, app.graphics, dark);
  auto &t = p.theme;
  bool toolbar=Str(Data(),L"menuStyle")==L"toolbar";
  if(!NativeLowPower(app.model))p.Shadow(MaterialBounds(),toolbar?10:NativeMetrics::FloatingRadius);
  p.Material(MaterialBounds(),NativeMaterial::Menu,Glass(),toolbar?10:NativeMetrics::FloatingRadius);
  c->PushAxisAlignedClip({6, 6, width - 6, height - 6},
                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  for (int i = 0; i < static_cast<int>(rows.size()); i++) {
    auto const &row = rows[i];
    if (row.offscreen)
      continue;
    auto rect = row.rect;
    if (row.control == L"separator") {
      p.Line(rect.left+6, (rect.top + rect.bottom) / 2, rect.right-6,
             (rect.top + rect.bottom) / 2, t.Gray(900,.10f));
      continue;
    }
    p.Interaction(InteractionRegion(i),false,materialPreview&&i==hover,i==focus);
    auto ink = row.control==L"danger"?t.Danger():row.enabled&&!row.dimmed?t.Ink():t.MaterialMuted();
    if (!(contextIcon&&contextIcon(c,row.id,{rect.left+8,rect.top+8,rect.left+24,rect.top+24}))&&!appearanceIcons.Draw(
            c, row.id,
            {rect.left + 8, rect.top + 8, rect.left + 24, rect.top + 24})) {
      if (row.active)
        p.Icon(L"check",
               {rect.left + 8, rect.top + 8, rect.left + 24, rect.top + 24},
               ink);
      else if(!row.detail.empty()&&row.id.rfind(L"shell:",0)!=0)
        p.Icon(row.detail,
               {rect.left + 8, rect.top + 8, rect.left + 24, rect.top + 24},
               ink);
    }
    auto label = row.label;
    auto tab = label.find(L'\t');
    std::wstring shortcut;
    if (tab != label.npos) {
      shortcut = label.substr(tab + 1);
      label.resize(tab);
    }
    p.Text(label, {rect.left + (toolbar?32:34), rect.top, rect.right - (shortcut.empty()?25:112), rect.bottom}, row.control==L"header"?11.f:toolbar?12.8f:13.f,
           ink, (Str(Data(),L"menuStyle")==L"shell"||toolbar)?600:400);
    if (!shortcut.empty())
      p.Text(shortcut,
             {rect.right - 112, rect.top, rect.right - (row.control==L"submenu"?30:10), rect.bottom}, 11,
             t.MaterialMuted(), 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if(toolbar&&row.active)p.Icon(L"check",{rect.right-20,rect.top+9,rect.right-6,rect.top+23},ink);
    if (row.control == L"submenu")
      p.Icon(L"right",
             {rect.right - 23, rect.top + 9, rect.right - 9, rect.top + 23},
             ink);
  }
  c->PopAxisAlignedClip();
  float view=ContextViewport();
  if(ContextOverflows()){
    float thumb=std::max(24.f,view*view/contextTotal),top=10+contextScroll/(contextTotal-view)*(view-thumb);
    float inkWidth=contextScrollbarHover||contextDragging?6.f:3.f;
    auto ink=t.Ink();ink.a=contextScrollbarHover||contextDragging?.43f:.25f;
    p.Fill({width-6-inkWidth/2,top,width-6+inkWidth/2,top+thumb},ink,inkWidth/2);
  }
}
struct NativeDesktopMenu : std::enable_shared_from_this<NativeDesktopMenu> {
  App &app;
  HWND owner;
  std::wstring board, requestId, automatic;
  POINT point;
  POINT cell{};
  JsonArray keys{};
  JsonArray rootEntries{}, entries{}, moreEntries{};
  bool optimized=false,fullLoading=false,prefetchPending=false;
  JsonObject readyFull{nullptr};
  struct PreparedChild {JsonObject reply{nullptr};std::vector<std::function<void(JsonObject const&)>> readers;};
  std::map<std::vector<int>,PreparedChild> preparedChildren;
  size_t preparingChildren=0;
  unsigned long long childGeneration=0;
  long long fullNavigation=0;
  std::vector<int> path;
  std::vector<std::pair<JsonArray, std::vector<int>>> parents;
  std::vector<std::pair<JsonArray,std::vector<int>>> levels;
  std::map<std::wstring, JsonObject> shellEntries;
  std::map<std::wstring, std::wstring> destinations;
  bool live = true, physical = false;
  long long entrySerial = 0, navigation = 0;
  NativeDesktopMenu(Window &w, POINT p)
      : app(w.app), owner(w.hwnd), board(w.boardId), point(p) {
    POINT client=p;ScreenToClient(w.hwnd,&client);auto b=app.model.Find(board);
    if(b)cell={static_cast<LONG>(std::floor((client.x*96/w.dpi-b->originX)/b->stepX)),static_cast<LONG>(std::floor((client.y*96/w.dpi-b->originY)/b->stepY))};
    requestId=NewRequestId();
    for (auto const &key : w.selected)
      keys.Append(::Text(key));
  }
  static std::wstring NewRequestId(){
    std::wstring id;
    GUID guid{};
    check_hresult(CoCreateGuid(&guid));
    wchar_t text[40];
    StringFromGUID2(guid, text, 40);
    for (wchar_t c : std::wstring(text))
      if (iswxdigit(c))
        id += c;
    return id;
  }
  Window *WindowNow() {
    for (auto const &w : app.windows)
      if (w->hwnd == owner && w->boardId == board)
        return w.get();
    return nullptr;
  }
  static JsonObject Row(std::wstring id, std::wstring label,
                        std::wstring icon = L"", bool enabled = true,
                        std::wstring control = L"") {
    JsonObject row;
    Put(row, L"id", id);
    Put(row, L"label", label);
    Put(row, L"detail", icon);
    Put(row, L"control", control);
    row.Insert(L"enabled", JsonValue::CreateBooleanValue(enabled));
    return row;
  }
  void Request(std::wstring action, JsonArray commandPath = {},
               std::function<void(JsonObject const &)> reply = {}) {
    JsonObject r;
    Put(r, L"op", std::wstring(L"context-menu"));
    Put(r, L"board", board);
    Put(r, L"requestId", requestId);
    Put(r, L"action", action);
    r.Insert(L"keys", keys);
    r.Insert(L"path", commandPath);
    Put(r, L"x", static_cast<double>(point.x));
    Put(r, L"y", static_cast<double>(point.y));
    Put(r,L"cellX",static_cast<double>(cell.x));Put(r,L"cellY",static_cast<double>(cell.y));
    app.Request(r, std::move(reply));
  }
  void Cancel() {
    if (!live)
      return;
    live = false;ResetPreparedChildren();
    Menus(app).Close(false);
    if (physical)
      Request(L"dismiss");
  }
  void Close() {
    Menus(app).Close(false);
    live = false;ResetPreparedChildren();
  }
  void Display(JsonArray const &values) {
    if (!live)
      return;
    entries = values;
    auto depth=parents.size();levels.resize(depth+1);levels[depth]={values,path};
    Menus(app).Show(depth,values);
  }
  void ActionAt(size_t depth,std::wstring const &id){
    if(depth>=levels.size())return;
    entries=levels[depth].first;path=levels[depth].second;
    parents.assign(levels.begin(),levels.begin()+depth);Action(id);
  }
  void ResetPreparedChildren(){++childGeneration;preparedChildren.clear();preparingChildren=0;}
  void Children(std::vector<int> const &next,std::function<void(JsonObject const&)> reader={}){
    if(auto it=preparedChildren.find(next);it!=preparedChildren.end()){
      if(it->second.reply){if(reader)reader(it->second.reply);}
      else if(reader)it->second.readers.push_back(std::move(reader));
      return;
    }
    // At most one speculative request. Explicit navigation may queue behind
    // that one, never behind an eager walk of all installed extension menus.
    if(!reader&&(preparingChildren||preparedChildren.size()>=16))return;
    auto &entry=preparedChildren[next];if(reader)entry.readers.push_back(std::move(reader));
    ++preparingChildren;JsonArray commandPath;for(auto n:next)commandPath.Append(JsonValue::CreateNumberValue(n));
    auto self=shared_from_this();auto lease=requestId;auto generation=childGeneration;
    Request(L"items",commandPath,[self,next,lease,generation](JsonObject const &reply){
      if(!self->live||self->requestId!=lease||self->childGeneration!=generation)return;
      if(self->preparingChildren)--self->preparingChildren;
      auto it=self->preparedChildren.find(next);if(it==self->preparedChildren.end())return;
      auto readers=std::move(it->second.readers);
      bool valid=Bool(reply,L"ok");
      if(valid){auto payload=reply.TryLookup(L"result");valid=payload&&payload.ValueType()==JsonValueType::Object;if(valid){auto items=payload.GetObject().TryLookup(L"items");valid=items&&items.ValueType()==JsonValueType::Array;}}
      if(valid)it->second.reply=reply;else self->preparedChildren.erase(it); // failures are retryable
      auto response=reply;
      if(!valid&&Bool(reply,L"ok")){response=JsonObject::Parse(reply.Stringify());response.Insert(L"ok",JsonValue::CreateBooleanValue(false));Put(response,L"error",std::wstring(L"Windows 子菜单返回了无效内容，可重试。"));}
      for(auto &callback:readers)callback(response);
    });
  }
  void PrepareAt(size_t depth,std::wstring const &id){
    if(!live||depth>=levels.size()||preparingChildren)return;
    auto found=shellEntries.find(id);if(found==shellEntries.end()||!Bool(found->second,L"submenu")||Bool(found->second,L"disabled"))return;
    auto next=levels[depth].second;if(next.size()>=16)return;
    next.push_back(static_cast<int>(Num(found->second,L"index")));Children(next);
  }
  void Extras(JsonArray &rows) {
    auto w = WindowNow();
    if (!w)
      return;
    auto tile = w->Find(w->focusKey);
    auto b = app.model.Find(board);
    if(rows.Size())rows.Append(Row(L"separator", L"", L"", false, L"separator"));
    if (tile && tile->card) {
      auto type = Str(tile->card, L"type");
      rows.Append(Row(L"card-info",L"卡片 · "+std::to_wstring(static_cast<int>(Num(tile->card,L"rows",2)))+L" 行 × "+std::to_wstring(static_cast<int>(Num(tile->card,L"columns",2)))+L" 列",L"",false,L"header"));
      for (auto const &v : app.model.cardDefinitions) {
        auto d = v.GetObject();
        if (Str(d, L"type") != type)
          continue;
        for (auto const &s : d.GetNamedArray(L"sizes")) {
          auto size = s.GetObject();
          auto id = L"size:" +
                    std::to_wstring(static_cast<int>(Num(size, L"columns"))) +
                    L":" +
                    std::to_wstring(static_cast<int>(Num(size, L"rows")));
          rows.Append(
              Row(id,
                  std::to_wstring(static_cast<int>(Num(size, L"rows"))) +
                      L" 行 × " +
                      std::to_wstring(static_cast<int>(Num(size, L"columns"))) +
                      L" 列",
                  L"resize"));
          rows.GetAt(rows.Size()-1).GetObject().Insert(L"checked",JsonValue::CreateBooleanValue(Num(tile->card,L"columns")==Num(size,L"columns")&&Num(tile->card,L"rows")==Num(size,L"rows")));
        }
      }
      if (type == L"system.clock") {
        auto settings=tile->card.GetNamedObject(L"settings",JsonObject{});int font=static_cast<int>(Num(settings,L"fontSize",82)),weight=static_cast<int>(Num(settings,L"fontWeight",900));bool compact=Num(tile->card,L"columns",4)<4;
        rows.Append(Row(L"clock-font-header",L"时间字号",L"",false,L"header"));
        int sizes[]{56,72,82,92,108};wchar_t const *labels[]{L"极小时间",L"小号时间",L"中号时间",L"大号时间",L"特大时间"};
        for(int i=0;i<5;i++){auto row=Row(L"font:"+std::to_wstring(sizes[i]),labels[i],L"clock");row.Insert(L"checked",JsonValue::CreateBooleanValue((compact?56:font)==sizes[i]));row.Insert(L"dimmed",JsonValue::CreateBooleanValue(compact&&sizes[i]>56));rows.Append(row);}
        if(compact)rows.Append(Row(L"clock-font-hint",L"这个尺寸只放得下 56 号时间",L"",false,L"header"));
        rows.Append(Row(L"clock-weight-header",L"字重",L"",false,L"header"));
        for(auto value:{300,900}){auto row=Row(L"weight:"+std::to_wstring(value),value==300?L"细体":L"粗体",L"clock");row.Insert(L"checked",JsonValue::CreateBooleanValue((weight==300?300:900)==value));rows.Append(row);}
      }
      if (type == L"system.storage" && app.model.honorSignedIn)
        rows.Append(Row(L"card-cleanup", L"打开管家清理", L"storage"));
      if (type == L"system.clipboard-image") {
        rows.Append(Row(L"image-copy", L"复制图片", L"copy", static_cast<bool>(app.clipboardBitmap)));
        rows.Append(Row(L"image-save", L"另存为…", L"save", static_cast<bool>(app.clipboardBitmap)));
        rows.Append(Row(L"image-clear", L"清除剪贴板", L"delete"));
      }
      if (type == L"system.weather") {
        bool hourly=Str(tile->card.GetNamedObject(L"settings",JsonObject{}),L"forecastView")==L"hourly";
        for(bool today:{false,true}){auto row=Row(today?L"weather-view:hourly":L"weather-view:daily",today?L"今日天气":L"多日天气",today?L"clock":L"calendar");row.Insert(L"checked",JsonValue::CreateBooleanValue(today==hourly));rows.Append(row);}
        rows.Append(Row(L"weather-refresh", L"立即刷新", L"refresh"));
        rows.Append(Row(L"weather-city", L"设置城市…", L"location"));
        rows.Append(Row(L"weather-info", L"定位信息", L"info"));
      }
      if(type==L"system.handoff"){rows.Append(Row(L"handoff-edit",L"编辑交接内容",L"edit"));rows.Append(Row(L"handoff-text",L"仅复制文本",L"copy"));}
      if(type==L"system.honor-service"&&app.model.honorSignedIn){rows.Append(Row(L"honor-refresh",L"刷新服务状态",L"refresh"));rows.Append(Row(L"honor-restore",L"全部恢复",L"undo"));}
      if(type==L"system.todo"||type==L"system.week"){
        auto state=NativeTodoData(*w,*tile);bool completed=Bool(state,L"hasCompleted"),undo=Bool(state,L"canUndo");
        if(type==L"system.week")rows.Append(Row(L"todo-today",L"回到今天",L"calendar"));
        rows.Append(Row(L"todo-edit",type==L"system.week"?L"编辑这一天的待办":L"编辑待办事项",L"edit"));rows.Append(Row(L"todo-completed",L"查看已完成",L"check",completed));
        rows.Append(Row(L"todo-clear",L"清除已完成",L"delete",completed));rows.Append(Row(L"todo-undo",L"撤销待办操作",L"undo",undo));
      }
      if (type == L"system.encodeck") {
        bool connected=app.model.encodeckData&&Bool(app.model.encodeckData,L"available");
        rows.Append(Row(L"encodeck-refresh", connected?L"刷新耳机状态":L"连接 EncoDeck", L"refresh"));
        rows.Append(Row(L"encodeck-configure",L"配置 EncoDeck",L"settings",true,L"submenu"));
      }
      if (auto custom=tile->card.TryLookup(L"custom"); custom && custom.ValueType()==JsonValueType::Object && Str(custom.GetObject(),L"kind")==L"shortcut") {
        int index=0;
        for(auto const &value:custom.GetObject().GetNamedObject(L"content").GetNamedArray(L"items"))
          rows.Append(Row(L"shortcut:"+std::to_wstring(index++),L"打开 "+Str(value.GetObject(),L"label"),L"open"));
      }
      rows.Append(Row(L"card-divider",L"",L"",false,L"separator"));
      rows.Append(Row(L"card-remove", L"移除卡片", L"delete",true,L"danger"));
    } else if (tile && !tile->folder.empty()) {
      rows.Append(Row(L"folder-open", L"打开", L"open"));
      rows.Append(Row(L"folder-count",tile->showItemCount?L"隐藏文件数量标识":L"显示文件数量标识",L"info"));
      rows.Append(Row(L"rename", L"重命名\tF2", L"rename"));
      rows.Append(Row(L"size:2:2", L"2 行 × 2 列", L"resize"));
      rows.Append(Row(L"size:4:2", L"2 行 × 4 列", L"resize"));
      rows.Append(Row(L"size:4:4", L"4 行 × 4 列", L"resize"));
      rows.Append(
          Row(L"folder-dissolve", L"解散大文件夹（保留文件）", L"folder"));
    } else if (!tile) {
      if(w->folderId.empty()){
        rows.Append(Row(L"grid-settings",L"桌面图标设置",L"settings"));
        rows.Append(Row(L"desktop-sort",L"排序方式",L"sort",true,L"submenu"));
      }
      rows.Append(Row(L"new-folder", L"新建大文件夹", L"folder"));
      rows.Append(Row(L"new-card", L"新建卡片", L"add"));
      if(b&&b->paged)rows.Append(Row(L"page-divider",L"",L"",false,L"separator"));
      if (b && b->paged)
        rows.Append(Row(L"pages", L"桌面页面", L"pages", true, L"submenu"));
      if (b && b->paged) {
        rows.Append(Row(L"page-delete", L"删除当前空页", L"delete",
                        b->pages.size() > 1 && b->pageCounts[b->pageId] == 0,L"danger"));
      }
      if (b && !b->unplaced.empty())
        rows.Append(
            Row(L"unplaced",
                L"待放置项目（" + std::to_wstring(b->unplaced.size()) + L"）",
                L"folder", true, L"submenu"));
      rows.Append(Row(L"system-divider",L"",L"",false,L"separator"));
      rows.Append(Row(L"settings", L"社区版设置", L"settings"));
      rows.Append(Row(L"feedback", L"反馈问题", L"feedback"));
    }
    if (tile) {
      if (!w->folderId.empty())
        rows.Append(Row(L"move-out", L"移出到桌面", L"folder"));
      if (!tile->card && tile->folder.empty())
        rows.Append(
            Row(L"folders", L"移入大文件夹", L"folder", true, L"submenu"));
      if (app.model.boards.size() > 1)
        rows.Append(
            Row(L"screens", L"移动到屏幕", L"display", true, L"submenu"));
    }
  }
  JsonArray Compact(JsonArray const &source) {
    JsonArray out;
    for (auto const &value : source) {
      auto v = value.GetObject();
      auto key = Str(v, L"key");
      if(keys.Size()==0&&path.empty()&&(key==L"view"||key==L"sort"||key==L"sortby"))continue;
      auto index = static_cast<int>(Num(v, L"index"));
      auto id = L"shell:" + std::to_wstring(++entrySerial) + L":" +
                std::to_wstring(index);
      if (shellEntries.size() >= 512)
        break;
      JsonObject offered;Put(offered,L"key",key);Put(offered,L"index",static_cast<double>(index));offered.Insert(L"disabled",JsonValue::CreateBooleanValue(Bool(v,L"disabled")));offered.Insert(L"submenu",JsonValue::CreateBooleanValue(Bool(v,L"submenu")));shellEntries[id]=offered;
      auto label = Str(v, L"label");
      for (size_t i = 0; (i = label.find(L'&', i)) != label.npos;)
        label.erase(i, 1);
      auto row = Row(id, label, key, !Bool(v, L"disabled"),
                     Bool(v,L"separator")?L"separator":Bool(v, L"submenu") ? L"submenu" : L"");
      row.Insert(L"checked",
                 JsonValue::CreateBooleanValue(Bool(v, L"checked")));
      if (!Str(v, L"icon").empty())
        Put(row, L"icon", Str(v, L"icon"));
      out.Append(row);
    }
    return out;
  }
  void EnsureDesktopCommands(JsonArray &rows) {
    if(keys.Size()!=0)return;
    auto ensure=[&](std::wstring key,std::wstring label,std::wstring icon,std::wstring control=L""){
      for(auto const &value:rows){
        auto entry=shellEntries.find(Str(value.GetObject(),L"id"));
        if(entry!=shellEntries.end()&&Str(entry->second,L"key")==key)return;
      }
      rows.Append(Row(L"desktop-"+key,label,icon,true,control));
    };
    ensure(L"refresh",L"刷新(E)",L"refresh");
    ensure(L"new",L"新建(W)",L"new",L"submenu");
    ensure(L"display",L"显示设置(D)",L"display");
    ensure(L"personalize",L"个性化(R)",L"settings");
  }
  void Root() {
    auto w = WindowNow();
    if (!w) {
      Cancel();
      return;
    }
    auto tile = w->Find(w->focusKey);
    physical = app.model.desktopRuntime &&
               (!tile || (!tile->card && tile->folder.empty()));
    if (!physical || !app.model.desktopRuntime) {
      JsonArray rows;
      Extras(rows);
      rootEntries = rows;
      Display(rows);
      return;
    }
    if(automatic==L"native")automatic=L"more";
    auto self = shared_from_this();
    auto sequence = ++navigation;
    auto epoch=app.inputEpoch;auto foreground=GetForegroundWindow();
    Request(L"open", {}, [self, sequence, epoch, foreground](JsonObject const &reply) {
      if (!self->live || self->navigation != sequence)
        return;
      if(self->app.inputEpoch!=epoch||GetForegroundWindow()!=foreground){self->Cancel();return;}
      JsonArray rows;
      auto result=self->ReadFullReply(reply);
      bool loaded=result&&!Bool(result,L"cancelled");
      if (loaded){
        rows = self->Compact(result.GetNamedArray(L"compact"));
        self->moreEntries=result.GetNamedArray(L"more");self->optimized=Bool(result,L"optimizedFile");
      }
      self->EnsureDesktopCommands(rows);
      if(!loaded){
        self->optimized=true; // Explicit More gets its own read even if the initial query failed.
        rows.Append(Row(L"retry", L"重试读取 Windows 菜单", L"refresh"));
      }
      if (!self->automatic.empty())
        for (auto const &v : rows) {
          auto row = v.GetObject();
          auto found = self->shellEntries.find(Str(row, L"id"));
          if (found != self->shellEntries.end() &&
              Str(found->second, L"key") == self->automatic &&
              Bool(row, L"enabled")) {
            self->Action(Str(row, L"id"));
            return;
          }
        }
      rows.Append(Row(L"more", L"显示更多选项\tShift+F10", L"more",true,L"submenu"));
      self->Extras(rows);
      self->rootEntries = rows;
      self->Display(rows);
      if(self->automatic==L"more"){self->Action(L"more");return;}
      // Start only after showing the fast page. Optional extensions live under
      // More; completion must never resize or reorder the menu under a pointer.
      // The first page's command table stays valid while this independent query
      // runs. A failed speculative query is silent and an explicit click retries.
      if(self->optimized&&loaded){
        self->prefetchPending=true;
        self->Request(L"prefetch",{},[self,lease=self->requestId](JsonObject const &reply){
          if(!self->live||self->requestId!=lease)return;
          self->prefetchPending=false;
          self->readyFull=self->ReadFullReply(reply);
          if(self->fullLoading&&self->fullNavigation==self->navigation){
            if(self->readyFull)self->ShowFull(self->readyFull);
            else self->LoadFull();
          }
        });
      }
    });
  }
  JsonObject ReadFullReply(JsonObject const &reply) const {
    if(!Bool(reply,L"ok"))return nullptr;
    auto payload=reply.TryLookup(L"result");
    if(!payload||payload.ValueType()!=JsonValueType::Object)return nullptr;
    auto result=payload.GetObject();
    if(Str(result,L"requestId")!=requestId||Bool(result,L"cancelled"))return nullptr;
    for(auto key:{L"compact",L"more"}){
      auto value=result.TryLookup(key);
      if(!value||value.ValueType()!=JsonValueType::Array)return nullptr;
      for(auto const &row:value.GetArray())if(row.ValueType()!=JsonValueType::Object)return nullptr;
    }
    return result;
  }
  void ShowFull(JsonObject const &result){
    fullLoading=false;optimized=false;moreEntries=result.GetNamedArray(L"more");
    // The full query owns a separate command table. Never promote its classic
    // extensions into the already displayed first level (or change its IDs).
    parents.clear();path.clear();entries=rootEntries;levels.resize(1);levels[0]={rootEntries,{}};
    auto rows=Compact(moreEntries);
    if(!rows.Size())rows.Append(Row(L"empty",L"没有更多可用选项",L"",false));
    parents.push_back({rootEntries,{}});Display(rows);readyFull=nullptr;
  }
  void LoadFull(){
    auto self=shared_from_this();auto sequence=fullNavigation;
    Request(L"full",{},[self,sequence](JsonObject const &reply){
      if(!self->live||self->navigation!=sequence)return;
      self->fullLoading=false;
      if(auto result=self->ReadFullReply(reply)){self->ShowFull(result);return;}
      // Failure in the separate full-menu worker must not disable root commands.
      JsonArray error;error.Append(Row(L"retry-more",L"重试加载更多选项",L"refresh"));self->Display(error);
    });
  }
  void Action(std::wstring const &id) {
    auto w = WindowNow();
    if (!live || !w || (id==L"retry-more"&&fullLoading))
      return;
    app.inputEpoch++;
    ++navigation;fullLoading=false;
    if(id==L"retry-more"){
      fullLoading=true;fullNavigation=navigation;LoadFull();return;
    }
    if(id==L"grid-settings"){
      Close();if(physical)Request(L"dismiss");w->BeginGridEdit();return;
    }
    if(id.rfind(L"todo-",0)==0){auto tile=w->Find(w->focusKey);if(!tile||!tile->card||(Str(tile->card,L"type")!=L"system.todo"&&Str(tile->card,L"type")!=L"system.week"))return;Close();
        if(id==L"todo-today"){w->todoSelectedDays[tile->key]=NativeTodoToday();w->todoScrolls[tile->key]=0;w->DrawCards(true,L"system.week");}else w->CardAction(tile->key,id,-1);return;}
    if(id==L"encodeck-configure"){
      auto tile=w->Find(w->focusKey);if(!tile||!tile->card||Str(tile->card,L"type")!=L"system.encodeck")return;
      parents.push_back({entries,path});JsonArray rows;
      rows.Append(Row(L"encodeck-location",L"指定 EncoDeck 安装位置",L"folder"));
      rows.Append(Row(L"encodeck-restart",L"重启 EncoDeck",L"refresh"));
      rows.Append(Row(L"encodeck-open",L"打开主界面",L"open"));
      Display(rows);return;
    }
    if(id==L"desktop-sort"){
      parents.push_back({entries,path});JsonArray rows;auto b=app.model.Find(board);
      auto checked=[&](std::wstring const &id,std::wstring const &label,bool on){auto row=Row(id,label,L"");row.Insert(L"checked",JsonValue::CreateBooleanValue(on));rows.Append(row);};
      checked(L"sort-mode:custom",L"magic 视界自定义排序",!b||b->sortMode==L"custom");
      rows.Append(Row(L"sort-divider",L"",L"",false,L"separator"));
      for(auto const &[mode,label]:std::vector<std::pair<std::wstring,std::wstring>>{{L"name",L"名称"},{L"size",L"大小"},{L"type",L"项目类型"},{L"modified",L"修改日期"}})checked(L"sort-mode:"+mode,label,b&&b->sortMode==mode);
      rows.Append(Row(L"direction-divider",L"",L"",false,L"separator"));
      checked(L"sort-ascending",L"递增",!b||!b->sortDescending);checked(L"sort-descending",L"递减",b&&b->sortDescending);
      rows.Append(Row(L"sort-help",L"逐页排序 · 卡片与大文件夹固定 · Ctrl+Z 撤销",L"",false,L"header"));
      Display(rows);return;
    }
    if(id.rfind(L"sort-mode:",0)==0||id==L"sort-ascending"||id==L"sort-descending"){
      auto b=app.model.Find(board);if(!b)return;
      auto mode=id.rfind(L"sort-mode:",0)==0?id.substr(10):b->sortMode;
      bool descending=id==L"sort-descending"?true:id==L"sort-ascending"?false:b->sortDescending;
      Close();if(physical)Request(L"dismiss");JsonObject r;Put(r,L"op",std::wstring(L"desktop-sort"));Put(r,L"board",board);Put(r,L"mode",mode);r.Insert(L"descending",JsonValue::CreateBooleanValue(descending));app.Request(r);return;
    }
    if (id == L"retry") {
      Request(L"dismiss");
      requestId=NewRequestId();
      readyFull=nullptr;prefetchPending=false;optimized=false;ResetPreparedChildren();shellEntries.clear();moreEntries=JsonArray{};
      Root();
      return;
    }
    if(id==L"desktop-new"){
      parents.push_back({entries,path});JsonArray rows;
      rows.Append(Row(L"desktop-create-folder",L"文件夹",L"folder"));rows.Append(Row(L"desktop-create-text",L"文本文档",L"document"));Display(rows);return;
    }
    if(id==L"desktop-create-folder"||id==L"desktop-create-text"){
      Close();Request(L"dismiss");JsonObject r;Put(r,L"op",std::wstring(L"desktop-create"));Put(r,L"board",board);
      Put(r,L"action",std::wstring(id==L"desktop-create-folder"?L"folder":L"text"));Put(r,L"x",static_cast<double>(cell.x));Put(r,L"y",static_cast<double>(cell.y));app.Request(r);return;
    }
    if(id==L"desktop-refresh"||id==L"desktop-display"||id==L"desktop-personalize"){
      if(id!=L"desktop-refresh"){AllowSetForegroundWindow(app.parent);SetWindowPos(w->hwnd,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}
      Close();Request(L"dismiss");JsonObject r;
      Put(r,L"op",std::wstring(L"desktop-action"));Put(r,L"board",board);
      Put(r,L"kind",std::wstring(id==L"desktop-refresh"?L"refresh":id==L"desktop-display"?L"display":L"personalize"));app.Request(r);return;
    }
    if (id == L"back") {
      if (parents.empty()) {
        path.clear();
        Display(rootEntries);
      } else {
        auto prior = parents.back();
        parents.pop_back();
        path = prior.second;
        Display(prior.first);
      }
      return;
    }
    if (id == L"pages" || id == L"screens" || id == L"folders" ||
        id == L"unplaced" || id.rfind(L"pending-page:", 0) == 0) {
      parents.push_back({entries, path});
      JsonArray rows;
      auto b = app.model.Find(board);
      int i = 0;
      if (b && (id == L"unplaced" || id.rfind(L"pending-page:", 0) == 0)) {
        if (id == L"unplaced" && b->unplaced.size() > 120) {
          for (size_t start = 0; start < b->unplaced.size(); start += 120)
            rows.Append(Row(
                L"pending-page:" + std::to_wstring(start),
                L"项目 " + std::to_wstring(start + 1) + L"–" +
                    std::to_wstring(std::min(start + 120, b->unplaced.size())),
                L"folder", true, L"submenu"));
        } else {
          size_t start =
              id == L"unplaced"
                  ? 0
                  : static_cast<size_t>(_wtoi(id.substr(13).c_str()));
          for (size_t j = start; j < b->unplaced.size() && j < start + 120;
               j++) {
            auto const &[key, name] = b->unplaced[j];
            auto rowId = L"restore:" + std::to_wstring(j);
            destinations[rowId] = key;
            rows.Append(Row(rowId, L"恢复 · " + name, L"add"));
          }
        }
      } else if (id == L"pages" && b) {
        rows.Append(Row(L"page-before",L"在前面新建页面",L"add",b->pages.size()<static_cast<size_t>(b->pageLimit)));
        for (auto const &p : b->pages) {
          auto key = L"page:" + std::to_wstring(i++);
          destinations[key] = p;
          auto row = Row(key, L"第 " + std::to_wstring(i) + L" 页", L"pages");
          row.Insert(L"checked", JsonValue::CreateBooleanValue(p == b->pageId));
          rows.Append(row);
        }
        rows.Append(Row(L"page-after",L"在后面新建页面",L"add",b->pages.size()<static_cast<size_t>(b->pageLimit)));
      } else if (id == L"screens") {
        for (auto const &other : app.model.boards)
          if (other.id != board) {
            auto key = L"screen:" + std::to_wstring(i++);
            destinations[key] = other.id;
            rows.Append(Row(key, other.name, L"display"));
          }
      } else if (id == L"folders" && b) {
        for (auto const &t : b->tiles)
          if (!t.folder.empty() && t.folder != w->folderId) {
            auto key = L"folder:" + std::to_wstring(i++);
            destinations[key] = t.folder;
            rows.Append(Row(key, t.name, L"folder"));
          }
      }
      if (rows.Size() == 0)
        rows.Append(Row(L"empty", L"暂无可用目标", L"", false));
      Display(rows);
      return;
    }
    if (shellEntries.contains(id)) {
      auto v = shellEntries[id];
      if (Bool(v, L"disabled"))
        return;
      if (Str(v, L"key") == L"refresh") {
        Close();
        Request(L"dismiss");
        JsonObject r;
        Put(r, L"op", std::wstring(L"desktop-action"));
        Put(r, L"board", board);
        Put(r, L"kind", std::wstring(L"refresh"));
        app.Request(r);
        return;
      }
      if (Str(v, L"key") == L"rename") {
        Close();
        Request(L"dismiss");
        w->Rename();
        return;
      }
      if(Str(v,L"key")==L"paste"&&keys.Size()==0){
        Close();Request(L"dismiss");JsonObject r;Put(r,L"op",std::wstring(L"clipboard"));Put(r,L"board",board);Put(r,L"action",std::wstring(L"paste"));r.Insert(L"keys",keys);Put(r,L"x",static_cast<double>(cell.x));Put(r,L"y",static_cast<double>(cell.y));auto state=w->providerState;app.Request(r,[state](JsonObject const &reply){if(auto window=Live(state);window&&Bool(reply,L"ok")){auto data=reply.GetNamedObject(L"result");window->pendingCreatedPage=Str(data,L"page");window->pendingCreatedKeys.clear();for(auto const &key:data.GetNamedArray(L"keys"))window->pendingCreatedKeys.insert(std::wstring(key.GetString()));window->Apply();}});return;
      }
      auto next = path;
      next.push_back(static_cast<int>(Num(v, L"index")));
      JsonArray commandPath;
      for (auto n : next)
        commandPath.Append(JsonValue::CreateNumberValue(n));
      if (Bool(v, L"submenu")) {
        if (path.size() >= 16)
          return;
        parents.push_back({entries, path});
        auto self = shared_from_this();
        auto sequence = navigation;
        Children(next,
            [self, next, sequence](JsonObject const &reply) {
              if (!self->live || self->navigation != sequence)
                return;
              JsonArray rows;
              if (Bool(reply, L"ok")) {
                self->path = next;
                auto values = self->Compact(
                    reply.GetNamedObject(L"result").GetNamedArray(L"items"));
                for (auto const &v : values)
                  rows.Append(v);
              } else
                rows.Append(Row(L"error", Str(reply, L"error"), L"", false));
              if(!rows.Size())rows.Append(Row(L"empty",L"没有可用的子选项",L"",false));
              self->Display(rows);
            });
        return;
      }
      if(keys.Size()==0&&(Str(v,L"key")==L"personalize"||Str(v,L"key")==L"display")){
        AllowSetForegroundWindow(app.parent);
        SetWindowPos(w->hwnd,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
      }
      Close();
      Request(L"invoke", commandPath);
      return;
    }
    if (id == L"more") {
      parents.push_back({entries,path});path.clear();
      if(!optimized){
        bool recycle=std::any_of(moreEntries.begin(),moreEntries.end(),[](auto const &v){return Str(v.GetObject(),L"key")==L"empty-bin";});
        if(!recycle){Display(Compact(moreEntries));return;}
        auto self=shared_from_this();auto sequence=navigation;
        Request(L"items",{},[self,sequence](JsonObject const &reply){
          if(!self->live||self->navigation!=sequence)return;
          if(Bool(reply,L"ok"))self->moreEntries=reply.GetNamedObject(L"result").GetNamedArray(L"items");
          self->Display(self->Compact(self->moreEntries));
        });return;
      }
      // A completed preload is already in the renderer. No extra round trip,
      // loading placeholder, second layout or restarted reveal animation.
      if(readyFull){ShowFull(readyFull);return;}
      fullLoading=true;fullNavigation=navigation;
      if(!prefetchPending)LoadFull();
      return;
    }
    JsonObject r;
    Put(r, L"board", board);
    Put(r,L"x",static_cast<double>(cell.x));Put(r,L"y",static_cast<double>(cell.y));
    auto tile = w->Find(w->focusKey);
    if (id == L"new-card") {
      Close();
      if (physical)
        Request(L"dismiss");
      JsonObject p, data, account, payload, anchor;
      Put(p, L"kind", std::wstring(L"@magic/card-picker"));
      Put(p, L"token", std::wstring(L"local-card-picker"));
      Put(anchor, L"x", static_cast<double>(point.x));
      Put(anchor, L"y", static_cast<double>(point.y));
      payload.Insert(L"desiredPosition", anchor);
      Put(payload, L"alignX", std::wstring(L"Center"));
      Put(payload, L"alignY", std::wstring(L"Center"));
      data.Insert(L"dark",
                  JsonValue::CreateBooleanValue(Preview70Theme::systemDark));
      data.Insert(L"rows", JsonArray{});
      Put(data, L"title", std::wstring(L"新建卡片"));
      p.Insert(L"data", data);
      p.Insert(L"account", account);
      p.Insert(L"payload", payload);
      app.popup->pickerBoard = board;
      app.popup->pickerCell=cell;app.popup->pickerHasCell=true;
      app.popup->Show(p);
      return;
    }
    Close();
    if (physical)
      Request(L"dismiss");
    if (id == L"rename") {
      w->Rename();
      return;
    }
    if (id == L"folder-open") {
      w->Activate(w->focusKey);
      return;
    }
    if (id == L"move-out") {
      w->MoveTo(board, true);
      return;
    }
    if (id.rfind(L"screen:", 0) == 0) {
      w->MoveTo(destinations[id]);
      return;
    }
    if (id.rfind(L"folder:", 0) == 0) {
      Put(r, L"op", std::wstring(L"moveAuto"));
      r.Insert(L"drag", w->DragPayload(w->focusKey));
      Put(r, L"destination", board);
      Put(r, L"destinationFolder", destinations[id]);
      app.Request(r);
      return;
    }
    if (id.rfind(L"restore:", 0) == 0) {
      Put(r, L"op", std::wstring(L"unplaced-action"));
      Put(r, L"kind", std::wstring(L"restore"));
      Put(r, L"key", destinations[id]);
    } else if (id == L"new-folder")
      Put(r, L"op", std::wstring(L"new-folder"));
    else if (id == L"settings" || id == L"feedback") {
      Put(r, L"op", std::wstring(L"desktop-action"));
      Put(r, L"kind", id);
    } else if (id == L"page-before" || id == L"page-after") {
      Put(r, L"op", std::wstring(L"page-add"));
      r.Insert(L"before", JsonValue::CreateBooleanValue(id == L"page-before"));
      r.Insert(L"edge",JsonValue::CreateBooleanValue(true));
    } else if (id == L"page-delete") {
      Put(r, L"op", std::wstring(L"page-delete"));
      Put(r, L"page", app.model.Find(board)->pageId);
    } else if (id.rfind(L"page:", 0) == 0) {
      Put(r, L"op", std::wstring(L"page"));
      Put(r, L"page", destinations[id]);
    } else if (tile && tile->card) {
      if (id==L"handoff-edit" || id==L"handoff-text" || id == L"honor-refresh" || id==L"honor-restore" || id == L"image-copy" || id == L"image-save" || id == L"image-clear" || id == L"weather-refresh" || id == L"weather-city" || id == L"weather-info" || id == L"encodeck-refresh" || id == L"encodeck-location" || id == L"encodeck-restart" || id == L"encodeck-open") {
        w->CardAction(tile->key,id);
        return;
      }
      if(id.rfind(L"shortcut:",0)==0){
        w->CardAction(tile->key,L"shortcut",_wtoi(id.substr(9).c_str()));
        return;
      }
      if (id == L"card-cleanup" || id == L"card-remove") {
        w->CardAction(tile->key,
                      id == L"card-cleanup" ? L"cleanup" : L"remove");
        return;
      }
      Put(r, L"op", std::wstring(L"card-action"));
      Put(r, L"cardId", Str(tile->card, L"id"));
      if((id==L"weather-view:hourly"||id==L"weather-view:daily") && Str(tile->card,L"type")==L"system.weather"){
        Put(r,L"kind",std::wstring(L"configure"));auto settings=JsonObject::Parse(tile->card.GetNamedObject(L"settings",JsonObject{}).Stringify());
        Put(settings,L"forecastView",id.substr(13));r.Insert(L"settings",settings);w->weatherHourPages.erase(tile->key);
      } else if (id.rfind(L"font:", 0) == 0 || id.rfind(L"weight:",0)==0) {
        Put(r, L"kind", std::wstring(L"configure"));
        auto settings = tile->card.GetNamedObject(L"settings");
        bool weight=id.rfind(L"weight:",0)==0;
        Put(settings, weight?L"fontWeight":L"fontSize",
            static_cast<double>(_wtoi(id.substr(weight?7:5).c_str())));
        r.Insert(L"settings", settings);
      } else if (id.rfind(L"size:", 0) == 0) {
        Put(r, L"kind", std::wstring(L"resize"));
        Put(r, L"columns", static_cast<double>(_wtoi(id.substr(5).c_str())));
        Put(r, L"rows",
            static_cast<double>(
                _wtoi(id.substr(id.find(L':', 5) + 1).c_str())));
      } else
        return;
    } else if (tile && !tile->folder.empty()) {
      Put(r, L"op", std::wstring(L"folder-action"));
      Put(r, L"folderId", tile->folder);
      Put(r, L"kind",
          std::wstring(id == L"folder-dissolve" ? L"dissolve" : id==L"folder-count"?L"folder-count":L"resize"));
      if(id==L"folder-count")r.Insert(L"show",JsonValue::CreateBooleanValue(!tile->showItemCount));
      else if (id != L"folder-dissolve") {
        Put(r, L"columns", static_cast<double>(_wtoi(id.substr(5).c_str())));
        Put(r, L"rows",
            static_cast<double>(
                _wtoi(id.substr(id.find(L':', 5) + 1).c_str())));
      }
    } else
      return;
    app.Request(r);
  }
  static void Open(Window &window, POINT point, std::wstring automatic = L"") {
    auto state = std::make_shared<NativeDesktopMenu>(window, point);
    state->automatic = automatic;
    auto &app = window.app;
    if (!app.popup)
      app.popup = std::make_unique<NativePopup>(app);
    app.popup->Hide();
    Menus(app).Begin(point,L"desktop",[state](size_t depth,std::wstring const &id){state->ActionAt(depth,id);},[state](){state->Cancel();});
    Menus(app).prepare=[state](size_t depth,std::wstring const &id){state->PrepareAt(depth,id);};
    state->Root();
  }
};
