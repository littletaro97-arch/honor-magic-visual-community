#include "trial-ole.h"
#include "trial-uia.h"
#include "trial.h"
#include "native-appearance.h"
#include "native-morph-frame.h"
#include "trial-cards.h"
#include "native-popup.h"
#include "native-popup-uia.h"
#include "native-menus.h"
#include "native-shell.h"
#include "native-backdrop.h"
#include "native-desktop-menu.h"
#include "native-grid-editor.h"
#include "native-grid-scene.h"
#include "native-power-policy.h"
#include "native-folder-drag.h"
#include "native-folder-morph.h"
#include "native-recovery.h"
#include "native-file-hover.h"
#include "native-honor-service-menu.h"
// The active authenticated session remains singular. A bounded closing HWND
// retains pixels only; all commands, inputs and DWM thumbnail relations end now.
void App::ClearRetiringPopups(){if(relay)KillTimer(relay,RetiringPopupTimer);for(auto &old:retiringPopups)old->FinishHide();retiringPopups.clear();}
void App::RetirePopup(std::wstring const &nextKind){
    if(!popup||!popup->visible||popup->kind==L"@magic/task-switcher"&&nextKind==popup->kind&&!popup->dismissing)return;
    ClearRetiringPopups();popup->Hide(!popup->dismissing);
    size_t bytes=popup->surfaceBytes+popup->glassSheet.bytes+popup->marqueeBytes+popup->refreshBytes;if(auto glass=NativeGlassStates().find(popup->hwnd);glass!=NativeGlassStates().end())bytes+=glass->second.bytes;
    if(popup->dismissing&&bytes<=24*1024*1024){
        for(auto const &[id,edit]:popup->edits){SetWindowTextW(edit,L"");DestroyWindow(edit);}popup->edits.clear();
        SetWindowLongPtrW(popup->hwnd,GWL_EXSTYLE,GetWindowLongPtrW(popup->hwnd,GWL_EXSTYLE)|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE);
        if(popup->life)popup->life->owner=nullptr;popup->appearanceIcons.Clear();popup->iconRequests.clear();
        retiringPopups.push_back(std::move(popup));
        if(!SetTimer(relay,RetiringPopupTimer,NativeInteractionMotion::CloseMs+32,nullptr))ClearRetiringPopups();
    }else popup->FinishHide();
}
App::~App(){Shutdown();}
Window::~Window()
{
    ClearMediaTitles();HideFileHover();fileTooltip.reset();EndRename();
    NativeGlassStates().erase(hwnd);
    if (editFont)
        DeleteObject(editFont);
}
void Window::Create(int index)
{
    auto b = app.model.Find(boardId);
    int x = b ? b->x + 32 + index * 24 : CW_USEDEFAULT, y = b ? b->y + 36 + index * 24 : CW_USEDEFAULT;
    bool live=app.model.desktopRuntime;
    if(live){x=b->x;y=b->y;}
    hwnd = CreateWindowExW(live?WS_EX_TOOLWINDOW:0, L"MagicCompositionTrialWindow", live?L"Magic 视界 · 桌面收纳":L"Magic Composition · 原生桌面隔离试点",
                           (live?WS_POPUP:WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN, x, y, live?b->width:1280, live?b->height:850, nullptr, nullptr,
                           GetModuleHandleW(nullptr), this);
    if (!hwnd)
        throw_last_error();
    if(live)return; // Existing bridge attaches/shows only verified ready windows.
    dpi = static_cast<float>(GetDpiForWindow(hwnd));
    RECT r{0, 0, static_cast<LONG>((app.cycles ? 1280 : 1000) * dpi / 96),
           static_cast<LONG>((app.cycles ? 850 : 700) * dpi / 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(dpi));
    if (b)
        SetWindowPos(hwnd, nullptr, x, y,
                     std::max(640, std::min(static_cast<int>(r.right - r.left), b->width - 64)),
                     std::max(480, std::min(static_cast<int>(r.bottom - r.top), b->height - 80)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}
void Window::Initialize()
{
    providerState->hwnd = hwnd;
    auto &g = app.graphics;
    check_hresult(
        g.compositor.as<abiComposition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
            hwnd, FALSE,
            reinterpret_cast<abiComposition::Desktop::IDesktopWindowTarget **>(put_abi(target))));
    root = g.compositor.CreateContainerVisual();
    target.Root(root);
    desktop = g.compositor.CreateSpriteVisual();
    pageLayer=g.compositor.CreateContainerVisual();root.Children().InsertAtBottom(pageLayer);pageLayer.Children().InsertAtTop(desktop);
    scrim = g.compositor.CreateSpriteVisual();
    scrim.Brush(g.compositor.CreateColorBrush({116, 16, 16, 16}));
    scrim.IsVisible(false);
    root.Children().InsertAtTop(scrim);
    folderMotion=g.compositor.CreateContainerVisual();root.Children().InsertAtTop(folderMotion);
    panel = g.compositor.CreateSpriteVisual();
    folderMotion.IsVisible(false);
    folderMotion.Opacity(0);
    folderMotion.Children().InsertAtTop(panel);
    InstallDrop(*this);
    WTSRegisterSessionNotification(hwnd,NOTIFY_FOR_THIS_SESSION);
    Layout();
    Draw();
    Register();
}
void Window::Register(bool reset)
{
    if(!hwnd||!app.model.components||app.stopping)return;
    if(reset){registerAttempts=0;frameReported=false;KillTimer(hwnd,104);}
    if(registering){registerAgain=true;return;}
    registering=true;++registerAttempts;
    JsonObject request;Put(request,L"op",std::wstring(L"register"));Put(request,L"board",boardId);
    Put(request,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));Put(request,L"dpi",static_cast<double>(GetDpiForWindow(hwnd)));
    auto state=providerState;
    app.Request(request,[state](JsonObject const &response){
        auto self=Live(state);if(!self||self->app.stopping)return;
        self->registering=false;
        if(self->registerAgain){self->registerAgain=false;self->Register(false);return;}
        if(Bool(response,L"ok")){
            RECT client{};GetClientRect(self->hwnd,&client);float currentDpi=static_cast<float>(GetDpiForWindow(self->hwnd));
            if(currentDpi>0&&(self->dpi!=currentDpi||std::abs(self->width*currentDpi/96-client.right)>1||std::abs(self->height*currentDpi/96-client.bottom)>1))self->Layout();
            self->Draw();self->VerifyDesktopFrame();
        }
        else{
            JsonObject detail;Put(detail,L"board",self->boardId);Put(detail,L"attempt",self->registerAttempts);Put(detail,L"error",Str(response,L"error"));
            self->app.Telemetry(L"desktop-registration-rejected",detail);
            if(self->app.model.desktopRuntime&&self->registerAttempts<3)SetTimer(self->hwnd,104,250*self->registerAttempts,nullptr);
        }
    });
}
void Window::VerifyDesktopFrame()
{
    if(!hwnd||!app.model.desktopRuntime||sessionPaused||app.stopping)return;
    auto board=app.model.Find(boardId);if(!board)return;
    RECT actual{};bool bounds=GetWindowRect(hwnd,&actual)&&std::abs(actual.left-board->x)<=2&&std::abs(actual.top-board->y)<=2&&
        std::abs(actual.right-actual.left-board->width)<=2&&std::abs(actual.bottom-actual.top-board->height)<=2;
    bool shown=IsWindowVisible(hwnd)!=FALSE,projection=ProjectionReady();
    bool submitted=shown&&bounds&&projection&&desktopSurface&&desktopStamp.valid;
    if(submitted&&frameReported)return;
    JsonObject detail;Put(detail,L"board",boardId);Put(detail,L"window",static_cast<double>(reinterpret_cast<UINT_PTR>(hwnd)));
    Put(detail,L"workspaceVersion",static_cast<double>(app.model.workspaceVersion));Put(detail,L"attempt",registerAttempts);
    Put(detail,L"dpi",dpi);Put(detail,L"projectionScale",board->projectionScale);Put(detail,L"tiles",static_cast<double>(board->tiles.size()));
    detail.Insert(L"visible",JsonValue::CreateBooleanValue(shown));detail.Insert(L"boundsMatch",JsonValue::CreateBooleanValue(bounds));detail.Insert(L"projectionReady",JsonValue::CreateBooleanValue(projection));
    Put(detail,L"left",actual.left);Put(detail,L"top",actual.top);Put(detail,L"width",actual.right-actual.left);Put(detail,L"height",actual.bottom-actual.top);
    app.Telemetry(submitted?L"desktop-frame-submitted":L"desktop-frame-pending",detail);
    if(submitted){frameReported=true;KillTimer(hwnd,104);}
    else if(registerAttempts<3)SetTimer(hwnd,104,250*std::max(1,registerAttempts),nullptr);
    // Submission is not proof of DWM presentation or lack of occlusion. A real
    // desktop observation is still required; no resident recovery poll is added.
}
void Window::Layout()
{
    ResetFolderMorph();ResetFolderAvoidance();
    if (!hwnd || !root)
        return;
    RECT r;
    GetClientRect(hwnd, &r);
    dpi = static_cast<float>(GetDpiForWindow(hwnd));
    if (!dpi)
        dpi = 96;
    width = r.right * 96 / dpi;
    height = r.bottom * 96 / dpi;
    float w=std::min(560.0f,std::max(280.0f,width-32));
    D2D1_RECT_F cover{};bool hasCover=false;
    if(!folderId.empty())if(auto board=app.model.Find(boardId))for(auto const &tile:board->tiles)if(tile.folder==folderId){cover=RootBounds(tile.key);hasCover=cover.right>cover.left;break;}
    if(hasCover){float side=std::max(cover.left-32,width-cover.right-32);if(side>=280)w=std::min(w,side);}
    columns=std::max(1,static_cast<int>((w-25)/107));
    float cell=(w-40-(columns-1)*15)/columns,step=cell+15;
    folderLayoutCount=static_cast<int>(Tiles().size());
    int itemRows=std::max(1,(folderLayoutCount+columns-1)/columns);
    float maxHeight=std::min(818.f,std::max(84.f+cell,height-32));
    if(hasCover&&std::max(cover.left-32,width-cover.right-32)<280){
        float vertical=std::max(cover.top-32,height-cover.bottom-32);
        if(vertical>=84+cell)maxHeight=std::min(maxHeight,vertical);
    }
    maxHeight=std::min(maxHeight,NativeGlassHeightLimit(w,dpi/96,NativeMaterial::Panel));
    int shownRows=FolderShownRows(folderLayoutCount,columns,maxHeight,step);
    float h=84+shownRows*step-15;
    panelRect={(width-w)/2,(height-h)/2,(width+w)/2,(height+h)/2};
    if(!folderId.empty())if(auto board=app.model.Find(boardId))for(auto const &tile:board->tiles)if(tile.folder==folderId){
        auto anchor=RootBounds(tile.key);constexpr float gap=16;
        if(anchor.left<panelRect.right&&anchor.right>panelRect.left&&anchor.top<panelRect.bottom&&anchor.bottom>panelRect.top){
            if(anchor.right+gap+w<=width-16){panelRect.left=anchor.right+gap;panelRect.right=panelRect.left+w;}
            else if(anchor.left-gap-w>=16){panelRect.right=anchor.left-gap;panelRect.left=panelRect.right-w;}
            else if(anchor.top-gap-h>=16){panelRect.bottom=anchor.top-gap;panelRect.top=panelRect.bottom-h;}
            else if(anchor.bottom+gap+h<=height-16){panelRect.top=anchor.bottom+gap;panelRect.bottom=panelRect.top+h;}
            else {panelRect.left=Clamp(anchor.right+gap,16,std::max(16.f,width-w-16));panelRect.right=panelRect.left+w;}
        }break;
    }
    contentRect={panelRect.left+20,panelRect.top+64,panelRect.right-20,panelRect.bottom-20};
    scrollRow=std::clamp(scrollRow,0,std::max(0,itemRows-shownRows));
    root.Size({static_cast<float>(r.right), static_cast<float>(r.bottom)});
    desktop.Size(root.Size());if(pageLayer){pageLayer.Size(root.Size());pageLayer.Clip(app.graphics.compositor.CreateInsetClip());}
    scrim.Size(root.Size());
    panel.Size({w * dpi / 96, h * dpi / 96});folderMotion.Size(panel.Size());
    folderMotion.CenterPoint({w * dpi / 192, h * dpi / 192, 0});
    if (!batch)
        folderMotion.Offset({panelRect.left * dpi / 96, panelRect.top * dpi / 96, 0});
    if (edit)
        EndRename();
}
std::vector<Tile> const &Window::Tiles()
{
    if(GridEditing())return gridControls;
    static const std::vector<Tile> empty;
    auto b = app.model.Find(boardId);
    if (!b)
        return empty;
    if (folderId.empty())
        return b->tiles;
    auto i =
        std::find_if(b->tiles.begin(), b->tiles.end(), [&](auto const &t) { return t.folder == folderId; });
    return i == b->tiles.end() ? empty : i->items;
}
Tile const *Window::Find(std::wstring const &key)
{
    auto const &t = Tiles();
    auto i = std::find_if(t.begin(), t.end(), [&](auto const &x) { return x.key == key; });
    return i == t.end() ? nullptr : &*i;
}
D2D1_RECT_F Window::RootBounds(std::wstring const &key)
{
    auto b = gridPreview?&*gridPreview:app.model.Find(boardId);
    if (!b)
        return {};
    auto i = b->positions.find(key);
    if (i == b->positions.end())
        return {};
    auto p = i->second;
    return {b->originX+p.x*b->stepX,b->originY+p.y*b->stepY,b->originX+(p.x+p.w)*b->stepX-8*b->stepX/96,b->originY+(p.y+p.h)*b->stepY-8*b->stepY/96};
}
float Window::RootUnit()const
{
    auto b=gridPreview?&*gridPreview:app.model.Find(boardId);return b?b->stepX/96:1;
}
D2D1_RECT_F Window::Bounds(std::wstring const &key)
{
    if(GridEditing())for(auto const &button:gridButtons)if(button.first==key)return button.second;
    if (folderId.empty())
        return RootBounds(key);
    if(folderPreviewActive){auto at=std::find(folderPreviewOrder.begin(),folderPreviewOrder.end(),key);if(at!=folderPreviewOrder.end())return FolderCellBounds(contentRect,columns,scrollRow,static_cast<int>(at-folderPreviewOrder.begin()));}
    auto const &t = Tiles();
    auto i = std::find_if(t.begin(), t.end(), [&](auto const &x) { return x.key == key; });
    if (i == t.end())
        return {};
    return FolderCellBounds(contentRect, columns, scrollRow, static_cast<int>(std::distance(t.begin(), i)));
}
NativeFolderCover::Layout<D2D1_RECT_F> Window::CoverLayout(Tile const &tile,D2D1_RECT_F rect)
{
    int cols=2,rows=2;
    if(auto board=gridPreview?&*gridPreview:app.model.Find(boardId))if(auto found=board->positions.find(tile.key);found!=board->positions.end()){
        cols=found->second.w;rows=found->second.h;
    }
    return {rect,cols,rows,Bool(app.model.preferences,L"collapsedFolderIconNames")};
}
D2D1_RECT_F Window::NameBounds(Tile const &tile,D2D1_RECT_F r,bool inFolder,float unit)
{
    if(!tile.folder.empty())return CoverLayout(tile,r).Caption();
    float cy=r.top+(inFolder?std::min(34.f,std::max(10.f,(r.bottom-r.top-19)/2)):(r.bottom-r.top-20*unit)/2);
    return {r.left+3*unit,inFolder?std::min(cy+25,r.bottom-17):cy+25*unit,r.right-3*unit,std::min(r.bottom,cy+59*unit)};
}
Tile const *Window::RenameItem()
{
    if(auto tile=Find(renameKey))return tile;
    if(auto board=app.model.Find(boardId))for(auto const &tile:board->tiles)if(tile.key==renameKey&&!tile.folder.empty())return &tile;
    return nullptr;
}
D2D1_RECT_F Window::RenameBounds()
{
    auto tile=RenameItem();if(!tile)return {};
    if(!folderId.empty()&&tile->folder==folderId)return {panelRect.left+24,panelRect.top+22,panelRect.right-64,panelRect.top+50};
    return NameBounds(*tile,Bounds(renameKey),!folderId.empty(),folderId.empty()?RootUnit():1);
}
bool Window::IsOnscreen(std::wstring const &key)
{
    if (!Find(key))
        return false;
    auto r = Bounds(key);
    auto clip = folderId.empty() ? D2D1_RECT_F{0, 0, width, height} : contentRect;
    return r.right > clip.left && r.left < clip.right && r.bottom > clip.top && r.top < clip.bottom;
}
std::wstring Window::Hit(float x, float y)
{
    if(GridEditing()){for(auto const &button:gridButtons)if(Inside(button.second,x,y))return button.first;return L"";}
    if (!folderId.empty())
    {
        auto const &tiles = Tiles();
        int index = FolderHitIndex(contentRect, columns, scrollRow, static_cast<int>(tiles.size()), x, y);
        return index >= 0 ? tiles[index].key : L"";
    }
    for (auto const &tile : Tiles())
        if (IsOnscreen(tile.key) && Inside(Bounds(tile.key), x, y))
            return tile.key;
    return L"";
}
ID2D1SolidColorBrush *Window::ColorBrush(ID2D1DeviceContext *context, D2D1_COLOR_F color)
{
    if (!paintBrush)
        check_hresult(context->CreateSolidColorBrush(color, paintBrush.put()));
    else
        paintBrush->SetColor(color);
    return paintBrush.get();
}
void Window::TextDraw(ID2D1DeviceContext *c, std::wstring const &text, D2D1_RECT_F r, float size,
                      D2D1_COLOR_F color, bool center)
{
    auto brush = ColorBrush(c, color);
    c->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), app.graphics.Format(size, center), r, brush,
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Window::PaintIcon(ID2D1DeviceContext *c,Tile const &item,D2D1_RECT_F box)
{
    app.honor.Scheme(HonorScheme(app.model.preferences));if(app.honor.Draw(app.graphics,c,HonorDesktopFile(item,HonorScheme(app.model.preferences),app.model.recycleFull),box))return;
    auto cached=icons.find(item.key);if(cached!=icons.end()){cached->second.age=++iconAge;c->DrawBitmap(cached->second.bitmap.get(),box,1,D2D1_INTERPOLATION_MODE_LINEAR);}
    else{NativePaint p(c,app.graphics,Preview70Theme::systemDark);p.Icon(item.isFolder?L"storage":L"apps",box,p.theme.Gray(700));}
}
void Window::PaintTile(ID2D1DeviceContext *c, Tile const &tile, D2D1_RECT_F r, int index,bool inFolder,bool detached)
{
    if(pendingDragKeys.contains(tile.key)||!inFolder&&!tile.folder.empty()&&tile.folder==folderId)return;
    struct DragOpacity{ID2D1DeviceContext *context;bool active;~DragOpacity(){if(active)context->PopLayer();}} opacity{c,dragging&&dragKeys.contains(tile.key)};
    if(opacity.active)c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),.35f),nullptr);
    drawnTiles++;
    float unit=inFolder?1:RootUnit();
    // Drag/avoidance surfaces use tile-local coordinates, not the folder's
    // content viewport. Applying that viewport clips their top/left borders.
    auto interaction=NativeRegion(r,detached?r:inFolder?FolderLocalViewport(contentRect,panelRect):D2D1::RectF(0,0,width,height),tile.card||!tile.folder.empty()?NativeMetrics::PanelRadius:NativeMetrics::ControlRadius);
    auto paintSelection=[&]{if(!GridEditing()&&(inFolder||folderId.empty())&&(selected.contains(tile.key)||focusKey==tile.key)){NativePaint selection(c,app.graphics,Preview70Theme::systemDark);selection.Interaction(interaction,selected.contains(tile.key),false,focusKey==tile.key);}};
    if(tile.card){PaintCard(c,tile,r);paintSelection();return;}
    // Selection and the eventual hover treatment share the logical hit geometry.
    NativePaint p(c,app.graphics,Preview70Theme::systemDark);
    auto drawIcon=[&](Tile const &item,D2D1_RECT_F box){PaintIcon(c,item,box);};
    if(inFolder&&folderMorph){p.FileLabel(tile.name,NameBounds(tile,r,true));paintSelection();return;}
    if(!tile.folder.empty()&&!inFolder){
        p.Material(r,NativeMaterial::Card,!NativeLowPower(app.model)&&(cardBackdrops.contains(tile.key)||materialPreview),NativeMetrics::PanelRadius);
        auto scheme=HonorScheme(app.model.preferences);app.honor.Scheme(scheme);
        auto cover=CoverLayout(tile,r);
        auto plate=[&](D2D1_RECT_F box){p.Fill(box,p.theme.Raised(),(box.right-box.left)*.22f);};
        auto glyph=[&](Tile const &item,D2D1_RECT_F box){
            // The white HONOR set already contains its rounded icon plate.
            auto file=HonorDesktopFile(item,scheme,app.model.recycleFull);
            if(scheme==L"honor-white"&&!file.empty()&&app.honor.Draw(app.graphics,c,file,box))return;
            float pad=(box.right-box.left)*.14f;drawIcon(item,{box.left+pad,box.top+pad,box.right-pad,box.bottom-pad});
        };
        for(int i=0;i<cover.MainCount(static_cast<int>(tile.items.size()));i++){
            auto box=cover.Cell(i);glyph(tile.items[i],box);
            if(cover.names)p.FileLabel(tile.items[i].name,cover.Name(i),11*cover.scale);
        }
        int previews=cover.PreviewCount(static_cast<int>(tile.items.size()));
        if(previews){
            for(int i=0;i<previews;i++){auto box=cover.Mini(i);glyph(tile.items[cover.Capacity()-1+i],box);}
        }else{
            auto box=cover.Corner();plate(box);float cx=(box.left+box.right)/2,cy=(box.top+box.bottom)/2,half=cover.tile*.18f;
            p.Icon(L"add",{cx-half,cy-half,cx+half,cy+half},p.theme.Ink());
        }
        if(!renaming||renameKey!=tile.key||folderId==tile.folder)p.Text(tile.name+(tile.showItemCount?L" ("+std::to_wstring(tile.items.size())+L")":L""),NameBounds(tile,r,false,unit),std::max(8.5f,11*unit),p.theme.Ink(),400,DWRITE_TEXT_ALIGNMENT_CENTER);
    }else{
        float cx=(r.left+r.right)/2,cy=r.top+(!inFolder?(r.bottom-r.top-20*unit)/2:std::min(34.f,std::max(10.f,(r.bottom-r.top-19)/2)));
        float size=inFolder?std::min(44.f,std::max(16.f,r.bottom-r.top-22)):44*unit;drawIcon(tile,{cx-size/2,cy-size/2,cx+size/2,cy+size/2});
        if(renaming&&renameKey==tile.key)return;
        auto bounds=NameBounds(tile,r,inFolder,unit);
        if(!inFolder){
            if(Bool(app.model.preferences,L"desktopIconNames",true))PaintDesktopName(c,tile,r,unit);
        }else p.FileLabel(tile.name,bounds);
    }
    paintSelection();
}
void Window::PaintDesktopName(ID2D1DeviceContext *c,Tile const &tile,D2D1_RECT_F r,float unit){
    NativePaint p(c,app.graphics,Preview70Theme::systemDark);auto bounds=NameBounds(tile,r,false,unit);
    auto sample=bounds;auto origin=RootBounds(tile.key);
    if(origin.right>origin.left){sample.left+=origin.left-r.left;sample.right+=origin.left-r.left;sample.top+=origin.top-r.top;sample.bottom+=origin.top-r.top;}
    bool light=labelBackground.LightInk(app.graphics,hwnd,sample,dpi);
    float font=std::max(8.5f,12*unit),line=font*1.2f;
    float labelHeight=std::max(line,std::floor((bounds.bottom-bounds.top)/line)*line);
    com_ptr<IDWriteTextLayout> label;auto format=app.graphics.Format(font,true);
    check_hresult(app.graphics.text->CreateTextLayout(tile.name.c_str(),static_cast<UINT32>(tile.name.size()),format,bounds.right-bounds.left,labelHeight,label.put()));
    label->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    label->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,font*.95f);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};com_ptr<IDWriteInlineObject> ellipsis;
    check_hresult(app.graphics.text->CreateEllipsisTrimmingSign(format,ellipsis.put()));label->SetTrimming(&trim,ellipsis.get());
    // Keep the contrast edge one physical pixel at mixed DPI. Four almost
    // opaque 1-DIP copies previously became a heavy 1.5/2px outline at 150/200%.
    float edge=96/std::max(48.f,dpi);
    for(auto offset:{D2D1_POINT_2F{-edge,0},D2D1_POINT_2F{edge,0},D2D1_POINT_2F{0,-edge},D2D1_POINT_2F{0,edge}})
        c->DrawTextLayout({bounds.left+offset.x,bounds.top+offset.y},label.get(),p.Color(D2D1::ColorF(light?0x000000:0xffffff,.65f)),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    c->DrawTextLayout({bounds.left,bounds.top},label.get(),p.Color(D2D1::ColorF(light?0xffffff:0x000000)),D2D1_DRAW_TEXT_OPTIONS_CLIP);

}
void Window::ClearDesktopName(){
    if(desktopNameVisual&&desktopNameVisual.Parent())desktopNameVisual.Parent().Children().Remove(desktopNameVisual);
    desktopNameVisual=nullptr;desktopNameSurface=nullptr;desktopNameKey.clear();desktopNameStamp.clear();desktopNameBytes=0;
}
void Window::SyncDesktopName(std::wstring const &requested,bool withinDraw){
    NativeRenderScope rendering(app.relay);if(!rendering&&!withinDraw)return;
    std::wstring key=requested;auto tile=Find(key);
    if(!pageLayer||!folderId.empty()||dragging||marquee||renaming||GridEditing()||Bool(app.model.preferences,L"desktopIconNames",true)||!tile||tile->card||!tile->folder.empty()){
        ClearDesktopName();return;
    }
    auto tileRect=RootBounds(key);auto r=NameBounds(*tile,tileRect,false,RootUnit());r.left-=1;r.top-=1;r.right+=1;r.bottom+=1;
    float scale=dpi/96;
    // The cached text surface must land on whole output pixels; otherwise the
    // compositor resamples already antialiased glyphs a second time.
    r.left=std::floor(r.left*scale)/scale;r.top=std::floor(r.top*scale)/scale;
    r.right=std::ceil(r.right*scale)/scale;r.bottom=std::ceil(r.bottom*scale)/scale;
    int w=std::max(1,static_cast<int>(std::lround((r.right-r.left)*scale))),h=std::max(1,static_cast<int>(std::lround((r.bottom-r.top)*scale)));
    size_t bytes=static_cast<size_t>(w)*h*4;
    if(bytes>512*1024){ClearDesktopName();return;}
    auto stamp=key+L"|"+tile->name+L"|"+std::to_wstring(dpi)+L"|"+std::to_wstring(Preview70Theme::revision)+L"|"+std::to_wstring(r.left)+L"|"+std::to_wstring(r.top)+L"|"+std::to_wstring(w)+L"|"+std::to_wstring(h);
    if(desktopNameStamp==stamp)return;
    auto &g=app.graphics;labelBackground.Load(g,hwnd);
    auto surface=g.composition.CreateDrawingSurface({static_cast<float>(w),static_cast<float>(h)},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    auto interop=surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> c;POINT offset{};
    check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),c.put_void(),&offset));
    try{c->SetDpi(dpi,dpi);c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x/scale-r.left,offset.y/scale-r.top));c->Clear(D2D1::ColorF(0,0));PaintDesktopName(c.get(),*tile,tileRect,RootUnit());}
    catch(...){interop->EndDraw();throw;}
    check_hresult(interop->EndDraw());
    if(!desktopNameVisual){desktopNameVisual=g.compositor.CreateSpriteVisual();pageLayer.Children().InsertAtTop(desktopNameVisual);}
    desktopNameVisual.Offset({r.left*scale,r.top*scale,0});desktopNameVisual.Size({static_cast<float>(w),static_cast<float>(h)});desktopNameVisual.Brush(g.compositor.CreateSurfaceBrush(surface));
    desktopNameSurface=surface;desktopNameKey=key;desktopNameStamp=stamp;desktopNameBytes=bytes;++desktopNameDraws;
}
void Window::SyncInteraction(float x,float y,bool leave){
    if(!root)return;auto key=leave||dragging||marquee?std::wstring{}:Hit(x,y);auto tile=Find(key);NativeInteractionRegion region;
    if(tile)region=NativeRegion(Bounds(key),folderId.empty()?D2D1::RectF(0,0,width,height):contentRect,tile->card||!tile->folder.empty()?NativeMetrics::PanelRadius:NativeMetrics::ControlRadius);
    if(!interactionHover&&tile)interactionHover=std::make_shared<NativeHoverVisual>();
    if(!folderId.empty()){
        region.bounds.left-=panelRect.left;region.bounds.right-=panelRect.left;region.bounds.top-=panelRect.top;region.bounds.bottom-=panelRect.top;
        region.viewport.left-=panelRect.left;region.viewport.right-=panelRect.left;region.viewport.top-=panelRect.top;region.viewport.bottom-=panelRect.top;
        x-=panelRect.left;y-=panelRect.top;
    }
    if(interactionHover)interactionHover->Sync(app.graphics.compositor,folderId.empty()?pageLayer:folderMotion,region,{x,y},dpi/96,NativeMotion(app.model),Preview70Theme::systemDark);
    SyncDesktopName(key);
}
int Window::AvailablePageEdge(int direction)const
{
    auto b=app.model.Find(boardId);
    if(!direction||!b||b->pages.size()<2||!folderId.empty())return 0;
    auto at=std::find(b->pages.begin(),b->pages.end(),b->pageId);
    if(at==b->pages.end())return 0;
    auto index=std::distance(b->pages.begin(),at);
    return direction<0?(index>0?-1:0):(index+1<static_cast<ptrdiff_t>(b->pages.size())?1:0);
}
void Window::PaintPages(ID2D1DeviceContext *c)
{
    auto b=app.model.Find(boardId);if(!b||b->pages.size()<2||!folderId.empty())return;
    NativePaint p(c,app.graphics,Preview70Theme::systemDark);int n=std::min(14,static_cast<int>(b->pages.size()));
    float w=20+n*28.f,left=(width-w)/2,top=height-54;if(!app.model.desktopRuntime)p.Fill({left,top,left+w,top+36},p.theme.Folder(),18);
    if(!app.model.desktopRuntime)for(int i=0;i<n;i++){auto ink=p.theme.Ink();ink.a=b->pages[b->PageStart()+i]==b->pageId?1.f:.34f;c->FillEllipse({{left+24+i*28.f,top+18},4,4},p.Color(ink));}
    if(AvailablePageEdge(pageEdge)){bool previous=pageEdge<0;float x=previous?6:width-112,y=height/2-24;
        p.Material({x,y,x+106,y+48},NativeMaterial::Card,!NativeLowPower(app.model)&&(pageBackdrop||materialPreview),24);p.Fill({x+10,y+11,x+36,y+37},p.theme.Gray(100,.35f),13);
        p.Icon(previous?L"previous":L"next",{x+15,y+16,x+31,y+32},p.theme.Ink());
        p.Text(previous?L"上一页":L"下一页",{x+44,y,x+100,y+48},13,p.theme.Ink());}
}
bool Window::ClickPage(float x,float y)
{
    auto b=app.model.Find(boardId);if(!b||b->pages.size()<2||!folderId.empty())return false;
    int n=std::min(14,static_cast<int>(b->pages.size())),index=-1;float w=20+n*28.f,left=(width-w)/2;
    if(!app.model.desktopRuntime&&y>=height-54&&y<=height-18&&x>=left&&x<=left+w)index=b->PageStart()+std::clamp(static_cast<int>((x-left-10)/28),0,n-1);
    if(AvailablePageEdge(pageEdge)&&std::abs(y-height/2)<24&&(pageEdge<0?x<112:x>width-112)){
        auto at=std::find(b->pages.begin(),b->pages.end(),b->pageId);index=static_cast<int>(std::distance(b->pages.begin(),at))+(pageEdge<0?-1:1);
        if(index<0||index>=static_cast<int>(b->pages.size()))return false;}
    if(index<0)return false;JsonObject r;Put(r,L"op",std::wstring(L"page"));Put(r,L"board",boardId);Put(r,L"page",b->pages[index]);app.Request(r);return true;
}
void Window::PaintFolder(ID2D1DeviceContext *c)
{
            float w = panelRect.right - panelRect.left, h = panelRect.bottom - panelRect.top;
            com_ptr<ID2D1SolidColorBrush> brush;
            check_hresult(c->CreateSolidColorBrush(D2D1::ColorF(0xfafcff), brush.put()));
            NativePaint appearance(c,app.graphics,Preview70Theme::systemDark);
            if(!folderMorph)appearance.Material({0,0,w,h},NativeMaterial::Panel,folderBackdrop||materialPreview,NativeMetrics::FloatingRadius);
            auto b = app.model.Find(boardId);
            std::wstring title = L"文件夹";
            if (b)
                for (auto const &t : b->tiles)
                    if (t.folder == folderId)
                        title = t.name;
            if(!renaming||renameKey!=L"folder:"+folderId)appearance.Text(title,{24,22,w-64,50},18,appearance.theme.Ink(),600);
            appearance.Fill({w-54,20,w-24,50},appearance.theme.Gray(200,.22f),6);
            appearance.Icon(L"close",{w-47,27,w-31,43},appearance.theme.Ink());
            auto clip = contentRect;
            clip.left -= panelRect.left;
            clip.right -= panelRect.left;
            clip.top -= panelRect.top;
            clip.bottom -= panelRect.top;
            c->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            auto const &t = Tiles();
            if(t.empty())appearance.Text(L"文件夹为空",{24,76,w-24,h-24},13,appearance.theme.Muted(),400,DWRITE_TEXT_ALIGNMENT_CENTER);
            auto [first, last] = FolderVisibleRange(static_cast<int>(t.size()), columns, scrollRow,
                                                    contentRect.bottom - contentRect.top,FolderRowStep(contentRect,columns));
            for (int i = first; i < last; i++)
            {
                auto r = FolderCellBounds(contentRect, columns, scrollRow, i);
                if (r.bottom <= contentRect.top || r.top >= contentRect.bottom)
                    continue;
                r.left -= panelRect.left;
                r.right -= panelRect.left;
                r.top -= panelRect.top;
                r.bottom -= panelRect.top;
                if(folderAvoidance)continue;
                auto const *item=&t[i];if(folderPreviewActive&&i<static_cast<int>(folderPreviewOrder.size())){auto found=std::find_if(t.begin(),t.end(),[&](auto const &v){return v.key==folderPreviewOrder[i];});if(found!=t.end())item=&*found;}
                if(folderPreviewActive&&folderPreviewKeys.contains(item->key))continue;
                PaintTile(c, *item, r, static_cast<int>(i),true);
            }
            c->PopAxisAlignedClip();
            int rows = (static_cast<int>(t.size()) + columns - 1) / columns;
            float view = contentRect.bottom - contentRect.top;
            if (rows * FolderRowStep(contentRect,columns)-15 > view)
            {
                float thumb = std::max(28.0f, view / (rows * FolderRowStep(contentRect,columns)-15) * view),
                      maximum = std::max(1.0f, rows - std::floor((view+15)/FolderRowStep(contentRect,columns))),
                      top = 64 + (view - thumb) * scrollRow / maximum;
                brush->SetColor(D2D1::ColorF(0x8291a6, .55f));
                c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(w - 9, top, w - 5, top + thumb), 2, 2),
                                        brush.get());
            }
}
void Window::DrawSurface(CompositionDrawingSurface surface, bool folder)
{
    if(!surface){if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);return;}
    auto interop = surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();
    com_ptr<ID2D1DeviceContext> c;
    POINT offset{};
    check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), c.put_void(), &offset));
    bool ended = false;
    try
    {
        c->SetDpi(dpi, dpi);
        c->SetTransform(D2D1::Matrix3x2F::Translation(offset.x * 96 / dpi, offset.y * 96 / dpi));
        c->Clear(D2D1::ColorF(folder ? 0xffffff : 0xf1f4fb, folder||app.model.desktopRuntime ? 0.0f : 1.0f));
        if (folder)
        {
            PaintFolder(c.get());
        }
        else
        {
            auto b = app.model.Find(boardId);
            if(!app.model.desktopRuntime)TextDraw(c.get(), L"Magic · Composition", {22, 15, width - 24, 43}, 23, D2D1::ColorF(0x202938));
            if(!app.model.desktopRuntime)TextDraw(c.get(), b ? b->name : L"正在连接隔离后端…", {24, 47, width - 24, 68}, 12,
                     D2D1::ColorF(0x677387));
            D2D1_MATRIX_3X2_F originalTransform;c->GetTransform(&originalTransform);
            if(gridEdit)PaintGridLines(c.get());
            if (b)
                for (size_t i = 0; i < b->tiles.size(); i++)
                    PaintTile(c.get(), b->tiles[i], RootBounds(b->tiles[i].key), static_cast<int>(i));
            if(gridEdit)c->SetTransform(originalTransform);
            if(!GridEditing())PaintPages(c.get());
            // Grid editor is a separate bounded retained layer.
            if(!app.model.desktopRuntime)TextDraw(c.get(), status, {18, height - 36, width - 18, height - 6}, 11, D2D1::ColorF(0x5d6d84));
        }
        HRESULT hr = interop->EndDraw();
        ended = true;
        check_hresult(hr);
        redraws++;
        if (folder)
            panelRedraws++;
        else
            rootRedraws++;
    }
    catch (...)
    {
        if (!ended)
            interop->EndDraw();
        throw;
    }
}
void Window::DrawIfChanged(CompositionDrawingSurface surface, bool folder)
{
    if(!surface){if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);return;}
    static std::set<std::wstring> const empty;
    auto &stamp = folder ? panelStamp : desktopStamp;
    auto const &selection = folder || folderId.empty() ? selected : empty;
    std::wstring const dir = folder ? boardId + L"/" + folderId : boardId;
    std::wstring const focus = folder || folderId.empty() ? focusKey : L"";
    std::wstring const message = folder ? L"" : status;
    int row = folder ? scrollRow : 0;
    if (stamp.Matches(app.model.revision,iconVersion,row,dir,focus,message,selection))
    {
        surfaceCacheHits++;
        return;
    }
    stamp.valid = false;
    DrawSurface(surface,folder);
    stamp.Capture(app.model.revision,iconVersion,row,dir,focus,message,selection);
}
bool Window::ProjectionReady()
{
    auto board=app.model.Find(boardId);
    return !app.model.desktopRuntime||!board||board->projectionScale==0||std::abs(board->projectionScale-dpi/96)<.001f;
}
SIZE Window::RootSurfacePixels()
{
    auto board=app.model.Find(boardId);
    // Empty desktops still own their input window and transparent visual, but
    // have no pixels to retain. Restore full size as soon as content, page
    // navigation or grid editing needs painting.
    if(app.model.desktopRuntime&&board&&board->tiles.empty()&&board->pages.size()<=1&&!GridEditing())return {1,1};
    float scale=dpi/96;
    return {std::max(1,static_cast<int>(std::ceil(width*scale))),std::max(1,static_cast<int>(std::ceil(height*scale)))};
}
void Window::Draw(bool rootChanged)
{
    NativeRenderScope rendering(app.relay);if(!rendering){if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);return;}
    if(surfaceReleasePending)ReleaseSurfaces(true);
    if(!ProjectionReady()){ReleaseSurfaces(true);return;}
    if (!hwnd || !root || IsIconic(hwnd) || sessionPaused || (everShown&&!IsWindowVisible(hwnd)))
        return;
    SyncMediaTitles();float scale = dpi / 96;
    auto rootPixels=RootSurfacePixels();
    int rw = rootPixels.cx,
        rh = rootPixels.cy,
        pw = std::max(1, static_cast<int>(std::ceil((panelRect.right - panelRect.left) * scale))),
        ph = std::max(1, static_cast<int>(std::ceil((panelRect.bottom - panelRect.top) * scale)));
    size_t rootBytes = static_cast<size_t>(rw) * rh * 4, panelBytes = static_cast<size_t>(pw) * ph * 4;
    if (rootBytes + (folderId.empty()?(panelSurface?static_cast<size_t>(panelPixelWidth)*panelPixelHeight*4:0):panelBytes) + outgoingPageBytes > SurfaceLimit)
        throw std::runtime_error("Per-window 64 MiB surface bound exceeded");
    auto &g = app.graphics;
    if (!desktopSurface || rw != rootPixelWidth || rh != rootPixelHeight)
    {
        desktopSurface = g.composition.CreateDrawingSurface(
            {static_cast<float>(rw), static_cast<float>(rh)},
            Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        desktop.Brush(g.compositor.CreateSurfaceBrush(desktopSurface));
        rootPixelWidth = rw;
        rootPixelHeight = rh;
        rootChanged = true;
        desktopStamp.valid = false;
    }
    if (!folderId.empty() && (!panelSurface || pw != panelPixelWidth || ph != panelPixelHeight))
    {
        panelSurface = g.composition.CreateDrawingSurface(
            {static_cast<float>(pw), static_cast<float>(ph)},
            Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto brush = g.compositor.CreateSurfaceBrush(panelSurface);
        panel.Brush(brush);
        panel.Shadow(nullptr);
        panelStamp.valid = false;
        panelPixelWidth = pw;
        panelPixelHeight = ph;
    }
    estimatedSurfaceBytes =
        rootBytes + (panelSurface ? static_cast<size_t>(panelPixelWidth) * panelPixelHeight * 4 : 0) + outgoingPageBytes+gridSurfaceBytes+gridDockBytes;
    SyncCardBackdrops();
    if(!GridEditing()&&interactionHover&&interactionHover->active)SyncInteraction(interactionHover->lastLight.x,interactionHover->lastLight.y);
    bool folderHadGlass=static_cast<bool>(folderBackdrop);
    auto folderLocal=D2D1::RectF(0,0,panel.Size().x/scale,panel.Size().y/scale);
    bool folderGlass=folderMorph?static_cast<bool>(folderBackdrop):SyncNativeGlass(g,hwnd,folderMotion,folderBackdrop,folderLocal,scale,NativeMetrics::FloatingRadius,NativeMaterial::Panel,!folderId.empty()&&!sessionPaused&&!NativeLowPower(app.model),panel);
    if(folderHadGlass!=folderGlass)panelStamp.valid=false;
    // Wallpaper COM is warmed outside BeginDraw; nested paints are deferred.
    labelBackground.Load(app.graphics,hwnd);
    if (rootChanged)
        DrawIfChanged(desktopSurface, false);
    if (!folderId.empty())
        DrawIfChanged(panelSurface, true);
    SyncDesktopName(desktopNameKey,true);SyncGridScene();RequestIcons();
}
void Window::ReleaseSurfaces(bool force)
{
    HideFileHover(true);ClearMediaTitles();
    if(NativeRenderScope::busy&&!force){surfaceReleasePending=true;if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);return;}
    surfaceReleasePending=false;ClearDesktopName();ReleaseGridScene();
    if(interactionHover){interactionHover->Release(root);interactionHover.reset();}
    ResetFolderMorph();ResetFolderAvoidance();ResetPageMotion();DropPreview();KillTimer(hwnd,98);renameClickKey.clear();
    if(pageLayer)for(auto const &[key,visual]:cardBackdrops)pageLayer.Children().Remove(visual);
    cardBackdrops.clear();cardBackdropBytes=0;cardBlurRadius=0;
    if(pageLayer)ReleaseNativeGlass(pageLayer,pageBackdrop);
    if(root)ReleaseNativeGlass(folderMotion,folderBackdrop);
    paintBrush = nullptr;
    desktopStamp.valid = panelStamp.valid = false;
    if (panel)
    {
        panel.Brush(nullptr);
        panel.Shadow(nullptr);
    }
    if (desktop)
        desktop.Brush(nullptr);
    panelSurface = nullptr;
    desktopSurface = nullptr;
    panelPixelWidth = panelPixelHeight = rootPixelWidth = rootPixelHeight = 0;
    estimatedSurfaceBytes = 0;
}
void Window::ClearIcons()
{
    iconVersion++;
    icons.clear();
    pendingIcons.clear();
    iconBytes = 0;
}
void Window::RequestIcons()
{
    if(GridEditing())return;
    if (app.model.synthetic || !hwnd)
        return;
    JsonArray keys;
    auto const &tiles = Tiles();
    std::set<std::wstring> demand;
    if(folderId.empty()){
        for(auto const &tile:tiles)if(tile.folder.empty()&&!tile.card&&demand.size()<128)demand.insert(tile.key);
        auto board=app.model.Find(boardId);
        for(auto const &tile:tiles){if(tile.folder.empty()||!board||!board->positions.contains(tile.key))continue;
            int n=CoverLayout(tile,RootBounds(tile.key)).VisibleCount(static_cast<int>(tile.items.size()));
            for(int i=0;i<n&&demand.size()<128;i++)demand.insert(tile.items[i].key);}
        // A view's demand also fits the icon-count bound, preventing repeated
        // eviction/re-request cycles when many folder covers are visible.
        for(auto it=icons.begin();it!=icons.end();)if(!demand.contains(it->first)){iconBytes-=it->second.bytes;it=icons.erase(it);}else ++it;
    }
    auto range = folderId.empty() ? std::make_pair(0, static_cast<int>(tiles.size()))
                                  : FolderVisibleRange(static_cast<int>(tiles.size()), columns, scrollRow,
                                                       contentRect.bottom - contentRect.top,FolderRowStep(contentRect,columns));
    for (int i = range.first; i < range.second; i++)
    {
        auto const &t = tiles[i];
        if (HonorDesktopFile(t,HonorScheme(app.model.preferences),app.model.recycleFull).empty() && t.folder.empty() && !t.card && (!folderId.empty()||demand.contains(t.key)) && !icons.count(t.key) && !pendingIcons.count(t.key))
        {
            keys.Append(Text(t.key));
            pendingIcons.insert(t.key);
            if (keys.Size() == 16)
                break;
        }
    }
    std::wstring coverFolder;
    if(!keys.Size()&&folderId.empty())for(auto const &folder:tiles){
        if(folder.folder.empty())continue;auto board=app.model.Find(boardId);if(!board||!board->positions.contains(folder.key))continue;
        int count=CoverLayout(folder,RootBounds(folder.key)).VisibleCount(static_cast<int>(folder.items.size()));
        for(int i=0;i<count;i++){auto const &child=folder.items[i];if(HonorDesktopFile(child,HonorScheme(app.model.preferences),app.model.recycleFull).empty()&&demand.contains(child.key)&&!icons.count(child.key)&&!pendingIcons.count(child.key)){
            keys.Append(Text(child.key));pendingIcons.insert(child.key);coverFolder=folder.folder;if(keys.Size()==16)break;}}
        if(keys.Size())break;
    }
    if (!keys.Size())return;
    JsonObject r;
    Put(r, L"op", std::wstring(L"icons"));
    Put(r, L"board", boardId);
    if (!folderId.empty()||!coverFolder.empty())
        Put(r, L"folder", folderId.empty()?coverFolder:folderId);
    Put(r, L"dpi", dpi / 96);
    r.Insert(L"keys", keys);
    app.Request(r, [this](JsonObject const &response) {
        if (!hwnd)
            return;
        if (Bool(response, L"ok"))
        {
            auto v = response.TryLookup(L"result");
            if (v && v.ValueType() == JsonValueType::Object)
                ReceiveIcons(v.GetObject());
        }
        else
            pendingIcons.clear();
    });
}
void Window::ReceiveIcons(JsonObject const &result)
{
    bool updated = false;
    for (auto const &row : result.GetNamedArray(L"icons"))
    {
        auto value = row.GetObject();
        auto key = Str(value, L"key"), png = Str(value, L"png");
        if (png.empty())
            continue;
        DWORD size = 0;
        if (!CryptStringToBinaryW(png.c_str(), static_cast<DWORD>(png.size()), CRYPT_STRING_BASE64, nullptr,
                                  &size, nullptr, nullptr) ||
            size > 65536)
            continue;
        std::vector<BYTE> bytes(size);
        if (!CryptStringToBinaryW(png.c_str(), static_cast<DWORD>(png.size()), CRYPT_STRING_BASE64,
                                  bytes.data(), &size, nullptr, nullptr))
            continue;
        com_ptr<IStream> stream;
        stream.attach(SHCreateMemStream(bytes.data(), size));
        if (!stream)
            continue;
        com_ptr<IWICBitmapDecoder> decoder;
        check_hresult(app.graphics.wic->CreateDecoderFromStream(stream.get(), nullptr,
                                                                WICDecodeMetadataCacheOnLoad, decoder.put()));
        com_ptr<IWICBitmapFrameDecode> frame;
        check_hresult(decoder->GetFrame(0, frame.put()));
        UINT w, h;
        check_hresult(frame->GetSize(&w, &h));
        if (!w || !h || w > 512 || h > 512)
            continue;
        auto raster=NativeIconRaster(app.graphics.wic.get(),frame.get(),w,h);
        size_t allocation = static_cast<size_t>(w) * h * 4;
        com_ptr<IWICFormatConverter> converter;
        check_hresult(app.graphics.wic->CreateFormatConverter(converter.put()));
        check_hresult(converter->Initialize(raster.get(), GUID_WICPixelFormat32bppPBGRA,
                                            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
        com_ptr<ID2D1DeviceContext> c;
        check_hresult(app.graphics.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, c.put()));
        Icon icon;
        check_hresult(c->CreateBitmapFromWicBitmap(converter.get(), nullptr, icon.bitmap.put()));
        icon.bytes = allocation;
        icon.age = ++iconAge;
        auto previous = icons.find(key);
        if (previous != icons.end()){
            iconBytes -= previous->second.bytes;icons.erase(previous);
        }
        while (!icons.empty() && (icons.size() >= NativeIconEntries || iconBytes + allocation > IconLimit)){
            auto oldest=std::min_element(icons.begin(),icons.end(),[](auto const &a,auto const &b){return a.second.age<b.second.age;});
            iconBytes-=oldest->second.bytes;icons.erase(oldest);
        }
        icons[key] = std::move(icon);
        if(folderMorph)folderMorph->RefreshIcon(*this,key);
        iconBytes += allocation;
        pendingIcons.erase(key);
        updated = true;
    }
    if (updated)
    {
        iconVersion++;
        Draw(folderId.empty());
    }
    RequestIcons();
}
void Window::ResetPageMotion()
{
    if(hwnd)KillTimer(hwnd,108);if(outgoingPage&&root)root.Children().Remove(outgoingPage);outgoingPage=nullptr;outgoingPageBytes=0;estimatedSurfaceBytes=static_cast<size_t>(rootPixelWidth)*rootPixelHeight*4+(panelSurface?static_cast<size_t>(panelPixelWidth)*panelPixelHeight*4:0)+gridSurfaceBytes+gridDockBytes;if(!pageLayer)return;
    pageLayer.StopAnimation(L"Offset");pageLayer.StopAnimation(L"Opacity");pageLayer.Offset({0,0,0});pageLayer.Opacity(1);
}
void Window::SyncPageMotion()
{
    auto b=app.model.Find(boardId);if(!b)return;
    int index=static_cast<int>(std::find(b->pages.begin(),b->pages.end(),b->pageId)-b->pages.begin());
    bool changed=!presentedPage.empty()&&presentedPage!=b->pageId;int direction=index>=presentedPageIndex?1:-1;
    presentedPage=b->pageId;presentedPageIndex=index;
    if(!changed||!pageLayer||!folderId.empty()||sessionPaused||!NativeMotion(app.model)||!IsWindowVisible(hwnd)){if(!NativeMotion(app.model))ResetPageMotion();return;}
    StartPageTransition(direction,dragging||!app.activeDragLease.empty()||GetCapture()==hwnd||!dropStamp.empty());
}
void Window::StartPageTransition(int direction,bool stationary)
{
    ClearMediaTitles();
    ResetPageMotion();
    size_t previousBytes=static_cast<size_t>(rootPixelWidth)*rootPixelHeight*4+cardBackdropBytes;
    if(desktopSurface&&estimatedSurfaceBytes+previousBytes<=SurfaceLimit){
        outgoingPage=pageLayer;outgoingPageBytes=previousBytes;
        pageLayer=app.graphics.compositor.CreateContainerVisual();pageLayer.Size(root.Size());pageLayer.Clip(app.graphics.compositor.CreateInsetClip());root.Children().InsertAbove(pageLayer,outgoingPage);
        desktop=app.graphics.compositor.CreateSpriteVisual();desktop.Size(root.Size());pageLayer.Children().InsertAtTop(desktop);desktopSurface=nullptr;rootPixelWidth=rootPixelHeight=0;desktopStamp.valid=false;
        cardBackdrops.clear();cardBackdropBytes=0;pageBackdrop=nullptr;
        if(interactionHover){interactionHover->Release(root);interactionHover.reset();}
        NativeInteractionMotion::Vector(app.graphics.compositor,outgoingPage,L"Offset",{stationary?0.f:-direction*NativeInteractionMotion::PageTravel*dpi/96,0,0},NativeInteractionMotion::PageMs);
        NativeInteractionMotion::Opacity(app.graphics.compositor,outgoingPage,0,NativeInteractionMotion::PageMs);
    }
    auto pose=NativeInteractionMotion::Page(direction,stationary,0);pageLayer.Offset({pose.offset.x*dpi/96,0,0});pageLayer.Opacity(pose.opacity);
    NativeInteractionMotion::Vector(app.graphics.compositor,pageLayer,L"Offset",{},NativeInteractionMotion::PageMs);
    NativeInteractionMotion::Opacity(app.graphics.compositor,pageLayer,1,NativeInteractionMotion::PageMs);
    if(hwnd)SetTimer(hwnd,108,NativeInteractionMotion::PageMs+16,nullptr);
}
void Window::Apply()
{
    if(GridEditing()&&app.model.workspaceVersion!=gridWorkspace)GridRequest(L"cancel");
    if (!hwnd)
        return;
    if (!app.model.Find(boardId))
    {
        if(app.model.desktopRuntime)return;
        boardId = app.model.boards.front().id;
        folderId.clear();
    }
    if (!folderId.empty())
    {
        auto b = app.model.Find(boardId);
        if (std::none_of(b->tiles.begin(), b->tiles.end(),
                         [&](auto const &t) { return t.folder == folderId; }))
            CloseFolder(true);
    }
    if(!folderId.empty()&&!closing&&folderLayoutCount!=static_cast<int>(Tiles().size()))Layout();
    for (auto i = selected.begin(); i != selected.end();)
        if (!Find(*i))
            i = selected.erase(i);
        else
            ++i;
    if (!Find(focusKey))
        focusKey.clear();
    if(!folderPreviewCommitted||folderPreviewRevision!=app.model.revision)ResetFolderAvoidance();
    auto board=app.model.Find(boardId);
    if(edit){if(RenameItem())ResizeRename();else if(!(GetWindowLongPtrW(edit,GWL_STYLE)&ES_READONLY))EndRename();}
    if(!pendingCreatedKeys.empty()&&board&&(pendingCreatedPage.empty()||board->pageId==pendingCreatedPage)&&std::all_of(pendingCreatedKeys.begin(),pendingCreatedKeys.end(),[&](auto const &key){return Find(key)!=nullptr;})){selected=std::move(pendingCreatedKeys);pendingCreatedKeys.clear();focusKey=*selected.begin();NotifySelection();NotifyFocus();}
    if(!GridEditing())SyncPageMotion();
    Draw();
    UpdateCardClock();
    if (UiaClientsAreListening())
    {
        auto p = MakeProvider(providerState);
        UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, nullptr, 0);
        p->Release();
    }
}
void Window::Select(std::wstring const &key, bool control, bool extend)
{
    if(GridEditing()){focusKey=key;desktopStamp.valid=false;Draw();NotifyFocus();return;}
    auto const &t = Tiles();
    if (extend && !focusKey.empty())
    {
        auto a = std::find_if(t.begin(), t.end(), [&](auto const &v) { return v.key == focusKey; }),
             b = std::find_if(t.begin(), t.end(), [&](auto const &v) { return v.key == key; });
        if (a != t.end() && b != t.end())
        {
            if (!control)
                selected.clear();
            if (a > b)
                std::swap(a, b);
            for (; a <= b; ++a)
                selected.insert(a->key);
        }
    }
    else
    {
        if (!control)
            selected.clear();
        if (control && selected.count(key))
            selected.erase(key);
        else
            selected.insert(key);
    }
    Focus(key);
    NotifySelection();
}
void Window::Focus(std::wstring const &key)
{
    if (!Find(key))
        return;
    focusKey = key;
    if (!folderId.empty())
    {
        auto const &t = Tiles();
        int index = static_cast<int>(std::distance(
            t.begin(), std::find_if(t.begin(), t.end(), [&](auto const &v) { return v.key == key; })));
        int row = index / columns,
            visible = std::max(1, static_cast<int>((contentRect.bottom - contentRect.top+15) / FolderRowStep(contentRect,columns)));
        if (row < scrollRow)
            scrollRow = row;
        else if (row >= scrollRow + visible)
            scrollRow = row - visible + 1;
    }
    Draw(folderId.empty());
    NotifyFocus();
}
void Window::Activate(std::wstring const &key)
{
    if(GridEditing()){GridAction(key);return;}
    auto t = Find(key);
    if (!t)
        return;
    if(t->card){if(Str(t->card,L"type")==L"system.handoff"){CardAction(key,L"handoff-copy");return;}if(Str(t->card,L"type")==L"system.todo"||Str(t->card,L"type")==L"system.week"){CardAction(key,L"todo-edit",-1);return;}if(Str(t->card,L"type")==L"system.clipboard-image"){CardAction(key,L"image-copy");return;}if(Str(t->card,L"type")==L"system.storage"){CardAction(key,L"cleanup");return;}auto custom=t->card.TryLookup(L"custom");if(custom&&custom.ValueType()==JsonValueType::Object&&Str(custom.GetObject(),L"kind")==L"shortcut")CardAction(key,L"shortcut",0);return;}
    if (!t->folder.empty())
    {
        OpenFolder(key);
        return;
    }
    JsonObject r;
    Put(r, L"op", std::wstring(L"open"));
    Put(r, L"board", boardId);
    Put(r, L"key", key);
    if (!folderId.empty())
        Put(r, L"folder", folderId);
    if(app.model.desktopRuntime){
        // Input belongs to this renderer, while ShellExecute runs in the
        // adapter. Transfer that input right before opening an existing app.
        bool granted=AllowSetForegroundWindow(app.parent)!=FALSE;
        bool lowered=SetWindowPos(hwnd,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)!=FALSE;
        JsonObject detail;detail.Insert(L"foregroundGranted",JsonValue::CreateBooleanValue(granted));detail.Insert(L"desktopLowered",JsonValue::CreateBooleanValue(lowered));detail.Insert(L"folderOpen",JsonValue::CreateBooleanValue(!folderId.empty()));app.Telemetry(L"external-open-handoff",detail);
    }
    app.Request(r);
}
void Window::OpenFolder(std::wstring const &key)
{
    auto b = app.model.Find(boardId);
    if (!b)
        return;
    auto t = std::find_if(b->tiles.begin(), b->tiles.end(), [&](auto const &v) { return v.key == key; });
    if (t == b->tiles.end() || t->folder.empty())
        return;
    long long start = Clock();
    ResetPageMotion();freshOpen = folderId.empty();
    if(interactionHover){interactionHover->Release(root);interactionHover.reset();}
    EndRename();
    folderId = t->folder;
    selected.clear();
    scrollRow = 0;
    focusKey = t->items.empty() ? L"" : t->items.front().key;
    Layout();
    folderMotion.IsVisible(true);
    scrim.IsVisible(true);
    Draw();
    Animate(false);
    JsonObject detail;
    Put(detail, L"items", static_cast<double>(t->items.size()));
    Put(detail, L"inputToSurfaceCommitMs", Millis(start));
    Put(detail, L"scope", std::wstring(L"CPU command/EndDraw only; not input-to-present latency"));
    app.Telemetry(L"folder-open", detail);
}
void Window::Animate(bool close)
{
    ++animationGeneration;closing=close;if(batch){batch.Completed(batchToken);batch=nullptr;}
    if(!NativeMotion(app.model,true)){ResetFolderMorph();closing=close;if(close)AnimationDone(animationGeneration);else{folderMotion.Scale({1,1,1});folderMotion.Offset({panelRect.left*dpi/96,panelRect.top*dpi/96,0});folderMotion.Opacity(1);scrim.Opacity(1);}return;}
    if(!folderMorph){auto motion=std::make_shared<NativeFolderMorph>();if(!motion->Prepare(*this,close)){closing=close;AnimationDone(animationGeneration);return;}folderMorph=motion;panelStamp.valid=false;Draw(false);}
    closing=close;auto &c=app.graphics.compositor;batch=c.CreateScopedBatch(CompositionBatchTypes::Animation);folderMorph->Animate(*this,close);NativeInteractionMotion::Opacity(c,scrim,close?0.f:1.f,NativeFolderMorph::Duration);
    int generation=animationGeneration;auto token=providerState;batchToken=batch.Completed([token,generation](auto const&,auto const&){if(token->alive)PostMessageW(token->hwnd,AnimationMessage,generation,0);});batch.End();
}
void Window::AnimationDone(int generation)
{
    if (generation != animationGeneration || !hwnd)
        return;
    if (batch)
    {
        batch.Completed(batchToken);
        batch = nullptr;
    }
    bool finishClosing=closing;ResetFolderMorph();closing=finishClosing;
    folderMotion.StopAnimation(L"Scale");
    folderMotion.StopAnimation(L"Offset");
    folderMotion.StopAnimation(L"Opacity");
    scrim.StopAnimation(L"Opacity");
    if (closing)
    {
        ResetFolderAvoidance();
        if(interactionHover){interactionHover->Release(root);interactionHover.reset();}
        folderId.clear();
        selected.clear();
        focusKey.clear();
        folderMotion.IsVisible(false);
        ReleaseNativeGlass(folderMotion,folderBackdrop);
        scrim.IsVisible(false);
        folderMotion.Opacity(0);
        pendingIcons.clear();
        SetTimer(app.relay,75,15000,nullptr);
        closing = false;
        desktopStamp.valid = panelStamp.valid = false;
        Draw(); NotifySelection(); NotifyFocus();
    }
    else
    {
        folderMotion.Scale({1, 1, 1});
        folderMotion.Offset({panelRect.left * dpi / 96, panelRect.top * dpi / 96, 0});
        folderMotion.Opacity(1);
        scrim.Opacity(1);panelStamp.valid=false;Draw(false);
    }
}
void Window::CloseFolder(bool immediate)
{
    if (folderId.empty())
        return;
    EndRename();
    if (immediate)
    {
        animationGeneration++;
        closing = true;
        AnimationDone(animationGeneration);
    }
    else
        Animate(true);
}
void Window::Scroll(int delta)
{
    ResetFolderMorph();ResetFolderAvoidance();
    if (folderId.empty())
        return;
    EndRename();
    int rows = (static_cast<int>(Tiles().size()) + columns - 1) / columns,
        visible = std::max(1, static_cast<int>((contentRect.bottom - contentRect.top+15) / FolderRowStep(contentRect,columns)));
    scrollRow = std::clamp(scrollRow + delta, 0, std::max(0, rows - visible));
    Draw(false);
}
void Window::Key(UINT key)
{
    if((key==VK_RETURN||key==VK_SPACE)&&!(GetKeyState(VK_CONTROL)&0x8000))if(auto tile=Find(focusKey);tile&&tile->card&&Str(tile->card,L"type")==L"system.handoff"){CardAction(tile->key,L"handoff-copy");return;}
    if(folderMorph&&key!=VK_ESCAPE){ResetFolderMorph();Draw(false);}
    if(GridEditing()){GridKey(key);return;}
    if(marquee&&key==VK_ESCAPE){marquee=false;MarqueePreview();selected=marqueeBase;marqueeBase.clear();if(GetCapture()==hwnd)ReleaseCapture();Draw();NotifySelection();return;}
    KillTimer(hwnd,98);renameClickKey.clear();
    if(edit){if(key==VK_ESCAPE||key==VK_F5){EndRename();if(key==VK_ESCAPE)return;}else return;}
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if(app.model.components&&ctrl&&!shift&&key=='Z')if(auto tile=Find(focusKey);tile&&tile->card&&(Str(tile->card,L"type")==L"system.todo"||Str(tile->card,L"type")==L"system.week")){CardAction(tile->key,L"todo-undo");return;}
    if(app.model.components&&key==VK_F5){JsonObject r;Put(r,L"op",std::wstring(L"desktop-action"));Put(r,L"board",boardId);Put(r,L"kind",std::wstring(L"refresh"));app.Request(r);return;}
    if(app.model.components&&key==VK_DELETE&&!selected.empty()){
        if(selected.size()>1&&std::any_of(selected.begin(),selected.end(),[](auto const &key){return key.rfind(L"item:",0)!=0;})){status=L"批量删除请选择普通文件；卡片和大文件夹请使用各自菜单。";Draw();return;}
        POINT p{};GetCursorPos(&p);NativeDesktopMenu::Open(*this,p,L"delete");return;
    }
    if(app.model.components&&ctrl&&!shift&&(key=='C'||key=='X'||key=='V'||key=='Z')){
        JsonObject request;Put(request,L"op",std::wstring(L"clipboard"));Put(request,L"board",boardId);Put(request,L"action",std::wstring(key=='C'?L"copy":key=='X'?L"cut":key=='Z'?L"undo":L"paste"));JsonArray keys;for(auto const &k:selected)keys.Append(Text(k));request.Insert(L"keys",keys);app.Request(request);return;
    }
    if(app.model.components&&folderId.empty()&&!ctrl&&!shift&&(key==VK_PRIOR||key==VK_NEXT||key==VK_HOME||key==VK_END||key==VK_LEFT||key==VK_RIGHT)&&app.model.Find(boardId)&&app.model.Find(boardId)->pages.size()>1){
        auto board=app.model.Find(boardId);if(board&&!board->pages.empty()){auto found=std::find(board->pages.begin(),board->pages.end(),board->pageId);int index=static_cast<int>(std::distance(board->pages.begin(),found));index=key==VK_HOME?0:key==VK_END?static_cast<int>(board->pages.size())-1:std::clamp(index+(key==VK_NEXT||key==VK_RIGHT?1:-1),0,static_cast<int>(board->pages.size())-1);JsonObject request;Put(request,L"op",std::wstring(L"page"));Put(request,L"board",boardId);Put(request,L"page",board->pages[index]);app.Request(request);}return;
    }
    if (key == VK_ESCAPE)
    {
        if (!folderId.empty())
            CloseFolder();
        else
        {
            selected.clear();focusKey.clear();
            Draw();NotifySelection();
        }
        return;
    }
    if (key == VK_F8)
    {
        Snapshot(L"manual-snapshot");
        return;
    }
    if (key == VK_F9)
    {
        ClearIcons();
        if (folderId.empty() && panel)
        {
            panel.Brush(nullptr);
            panel.Shadow(nullptr);
            panelSurface = nullptr;
        }
        app.graphics.Trim();
        Draw();
        return;
    }
    if (key == VK_F6 && !app.model.desktopRuntime)
    {
        CloseFolder(true);
        EndRename();
        auto b = std::find_if(app.model.boards.begin(), app.model.boards.end(),
                              [&](auto const &board) { return board.id == boardId; });
        if (b != app.model.boards.end())
            boardId =
                app.model.boards[(std::distance(app.model.boards.begin(), b) + 1) % app.model.boards.size()]
                    .id;
        selected.clear();
        focusKey.clear();
        ClearIcons();
        Draw();
        return;
    }
    if(app.model.components&&key==VK_RETURN&&(GetKeyState(VK_MENU)&0x8000)&&!selected.empty()){POINT p{};GetCursorPos(&p);NativeDesktopMenu::Open(*this,p,L"properties");return;}
    if (key == VK_RETURN)
    {
        if (!focusKey.empty())
            Activate(focusKey);
        return;
    }
    if (key == VK_F2)
    {
        Rename();
        return;
    }
    if (key == VK_APPS || (key == VK_F10 && shift))
    {
        POINT p{0, 0};
        if (!focusKey.empty())
        {
            auto r = Bounds(focusKey);
            p = {static_cast<LONG>(r.left * dpi / 96), static_cast<LONG>(r.bottom * dpi / 96)};
        }
        ClientToScreen(hwnd, &p);
        if(app.model.components&&shift)NativeDesktopMenu::Open(*this,p,L"native");else Menu(p);
        return;
    }
    auto const &t = Tiles();
    if (t.empty())
        return;
    if (folderId.empty() && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN))
    {
        if (!Find(focusKey))
        {
            Select(t.front().key, ctrl, shift);
            return;
        }
        auto origin = RootBounds(focusKey);
        float ox = (origin.left + origin.right) / 2, oy = (origin.top + origin.bottom) / 2, best = 1e30f;
        std::wstring next;
        for (auto const &tile : t)
        {
            if (tile.key == focusKey)
                continue;
            auto r = RootBounds(tile.key);
            float x = (r.left + r.right) / 2 - ox, y = (r.top + r.bottom) / 2 - oy;
            bool eligible = key == VK_LEFT ? x < 0 : key == VK_RIGHT ? x > 0 : key == VK_UP ? y < 0 : y > 0;
            float score = (key == VK_LEFT || key == VK_RIGHT) ? std::abs(y) * 1000 + std::abs(x)
                                                              : std::abs(x) * 1000 + std::abs(y);
            if (eligible && score < best)
            {
                best = score;
                next = tile.key;
            }
        }
        if (!next.empty())
            Select(next, ctrl, shift);
        return;
    }
    if (ctrl && key == 'A')
    {
        selected.clear();
        for (auto const &tile : t)
            if (tile.folder.empty()&&!tile.card)
                selected.insert(tile.key);
        Draw();
        NotifySelection();
        return;
    }
    int index = -1;
    auto i = std::find_if(t.begin(), t.end(), [&](auto const &tile) { return tile.key == focusKey; });
    if (i != t.end())
        index = static_cast<int>(std::distance(t.begin(), i));
    int step = key == VK_LEFT    ? -1
               : key == VK_RIGHT ? 1
               : key == VK_UP    ? -columns
               : key == VK_DOWN  ? columns
               : key == VK_PRIOR ? -columns * 4
               : key == VK_NEXT  ? columns * 4
               : key == VK_TAB   ? (shift ? -1 : 1)
                                 : 0;
    if (step || key == VK_HOME || key == VK_END)
    {
        int n = key == VK_HOME  ? 0
                : key == VK_END ? static_cast<int>(t.size()) - 1
                                : std::clamp(index + step, 0, static_cast<int>(t.size()) - 1);
        Select(t[n].key, ctrl, shift);
    }
}
void Window::Menu(POINT p)
{
    if(GridEditing())return;
    if(app.model.components){NativeDesktopMenu::Open(*this,p);return;}
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开");
    AppendMenuW(menu, MF_STRING, 2, L"重命名");
    if (!folderId.empty())AppendMenuW(menu, MF_STRING, 3, L"移出到桌面");
    int id = 100;
    std::map<int, std::wstring> targets;
    for (auto const &b : app.model.boards)
        if (b.id != boardId)
        {
            targets[id] = b.id;
            AppendMenuW(menu, MF_STRING, id++, (L"移到 " + b.name).c_str());
        }
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (command == 1)
        Activate(focusKey);
    else if (command == 2)
        Rename();
    else if (command == 3)
        MoveTo(boardId, true);
    else if(targets.count(command))MoveTo(targets[command]);
}
void Window::MarqueePreview(float x,float y)
{
    if(!marquee){if(marqueeVisual){root.Children().Remove(marqueeVisual);marqueeVisual=nullptr;}return;}
    float sx=pressedPoint.x*96/dpi,sy=pressedPoint.y*96/dpi;
    auto clip=folderId.empty()?D2D1::RectF(0,0,width,height):contentRect;
    D2D1_RECT_F r{Clamp(std::min(x,sx),clip.left,clip.right),Clamp(std::min(y,sy),clip.top,clip.bottom),Clamp(std::max(x,sx),clip.left,clip.right),Clamp(std::max(y,sy),clip.top,clip.bottom)};
    auto next=marqueeControl?marqueeBase:std::set<std::wstring>{};
    if(r.right-r.left>2||r.bottom-r.top>2)for(auto const &tile:Tiles()){
        auto b=Bounds(tile.key);if(IsOnscreen(tile.key)&&b.left<r.right&&b.right>r.left&&b.top<r.bottom&&b.bottom>r.top){if(marqueeControl&&marqueeBase.contains(tile.key))next.erase(tile.key);else next.insert(tile.key);}}
    if(next!=selected){selected=std::move(next);Draw();NotifySelection();}
    if(!marqueeVisual){marqueeVisual=app.graphics.compositor.CreateShapeVisual();root.Children().InsertAtTop(marqueeVisual);}
    marqueeVisual.Size({width,height});marqueeVisual.Scale({dpi/96,dpi/96,1});marqueeVisual.Shapes().Clear();
    auto geometry=app.graphics.compositor.CreateRectangleGeometry();geometry.Offset({r.left,r.top});geometry.Size({std::max(0.f,r.right-r.left),std::max(0.f,r.bottom-r.top)});
    auto shape=app.graphics.compositor.CreateSpriteShape(geometry);auto accent=Preview70Theme{Preview70Theme::systemDark}.Accent();
    auto color=Windows::UI::Color{255,static_cast<BYTE>(accent.r*255),static_cast<BYTE>(accent.g*255),static_cast<BYTE>(accent.b*255)};
    shape.StrokeBrush(app.graphics.compositor.CreateColorBrush(color));shape.StrokeThickness(1);color.A=36;shape.FillBrush(app.graphics.compositor.CreateColorBrush(color));marqueeVisual.Shapes().Append(shape);
}
LRESULT CALLBACK Window::RenameHostProc(HWND window,UINT message,WPARAM w,LPARAM l)
{
    auto self=reinterpret_cast<Window*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(self){
        if(message==WM_COMMAND&&self->edit==reinterpret_cast<HWND>(l)&&HIWORD(w)==EN_CHANGE){self->ResizeRename();return 0;}
        if((message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC)&&self->edit==reinterpret_cast<HWND>(l)){SetTextColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOWTEXT));SetBkColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOW));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));}
        if(message==WM_ACTIVATE){if(LOWORD(w)==WA_INACTIVE&&self->edit&&!(GetWindowLongPtrW(self->edit,GWL_STYLE)&ES_READONLY))PostMessageW(self->hwnd,WM_APP+47,self->renameGeneration,0);else if(self->edit)SetFocus(self->edit);return 0;}
        if(message==WM_CLOSE){self->EndRename();return 0;}
    }
    return DefWindowProcW(window,message,w,l);
}
void Window::Rename(std::wstring key)
{
    if(key.empty())key=focusKey;
    auto t = Find(key);
    if(!t)if(auto board=app.model.Find(boardId))for(auto const &tile:board->tiles)if(tile.key==key&&!tile.folder.empty()){t=&tile;break;}
    if (!t||t->card||t->isVirtual)
        return;
    // Draw/activation can pump COM messages and replace the model snapshot.
    // Keep only immutable label data across those calls, never a Tile pointer.
    auto editKey=t->key,itemFolder=t->folder,targetPath=t->target,displayName=t->name;bool isFolder=t->isFolder;
    EndRename();
    renameKey = editKey;
    renaming = true;
    selected.clear(); focusKey.clear(); NotifySelection();
    desktopStamp.valid = panelStamp.valid = false; Draw();
    auto r = RenameBounds();
    auto initial=itemFolder.empty()&&!targetPath.empty()?std::wstring(PathFindFileNameW(targetPath.c_str())):displayName;
    renameOriginal=initial;
    bool title=!folderId.empty()&&itemFolder==folderId;
    // Create hidden at the final label anchor. Painting a full-cell EDIT first
    // leaves stale GDI/non-client borders over the Composition surface.
    static bool registered=[](){WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=RenameHostProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MagicCompositionRenameHost";cls.hbrBackground=GetSysColorBrush(COLOR_WINDOW);if(!RegisterClassExW(&cls))throw_last_error();return true;}();
    (void)registered;
    renameHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"MagicCompositionRenameHost",L"重命名",WS_POPUP|WS_CLIPCHILDREN,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),this);
    if(!renameHost)throw_last_error();
    edit=CreateWindowExW(0,L"EDIT",initial.c_str(),WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|(title?ES_LEFT:ES_CENTER),0,0,1,1,renameHost,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!edit){SetWindowLongPtrW(renameHost,GWLP_USERDATA,0);DestroyWindow(renameHost);renameHost=nullptr;throw_last_error();}
    float font=title?18.f:folderId.empty()?std::max(8.5f,(itemFolder.empty()?12.f:11.f)*RootUnit()):12.f;
    editFont = CreateFontW(-static_cast<int>(font * dpi / 96), 0, 0, 0, title?FW_SEMIBOLD:FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, NativeUiFont::Family().c_str());
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(editFont), TRUE);
    SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    ResizeRename();ShowWindow(edit,SW_SHOW);ShowWindow(renameHost,SW_SHOWNORMAL);SetForegroundWindow(renameHost);SetFocus(edit);
    JsonObject opened;opened.Insert(L"folderTitle",JsonValue::CreateBooleanValue(title));opened.Insert(L"focusAcquired",JsonValue::CreateBooleanValue(GetFocus()==edit));app.Telemetry(L"rename-editor-opened",opened);
    int dot = targetPath.empty()||isFolder ? -1 : static_cast<int>(initial.find_last_of(L'.'));
    SendMessageW(edit, EM_SETSEL, 0, dot > 0 ? dot : -1);
}
void Window::SaveRename()
{
    if(!edit||(GetWindowLongPtrW(edit,GWL_STYLE)&ES_READONLY))return;
    // A focus-loss commit can be queued before IME end-composition. Defer it
    // until that notification instead of silently discarding the only commit.
    if(composing){renameCommitPending=true;return;}
    HIMC context = ImmGetContext(edit);
    bool composition = context && ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
    if (context)
        ImmReleaseContext(edit, context);
    if(composition){renameCommitPending=true;return;}
    int size = GetWindowTextLengthW(edit);
    if (size < 1 || size > 200)
    {
        status = L"名称长度应为 1–200 个字符";
        SetForegroundWindow(renameHost);SetFocus(edit);Draw();
        return;
    }
    std::wstring text(size + 1, L'\0');
    GetWindowTextW(edit, text.data(), size + 1);
    text.resize(size);
    if(text==renameOriginal){EndRename();return;}
    JsonObject r;
    Put(r, L"op", std::wstring(L"rename"));
    Put(r, L"board", boardId);
    Put(r, L"key", renameKey);
    Put(r, L"name", text);
    if (!folderId.empty())
        Put(r, L"folder", folderId);
    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    int generation = renameGeneration;
    JsonObject submitted;Put(submitted,L"nameLength",static_cast<double>(size));app.Telemetry(L"rename-submitted",submitted);
    app.Request(r, [this, generation](JsonObject const &response) {
        if (!hwnd || generation != renameGeneration)
            return;
        JsonObject receipt;receipt.Insert(L"ok",JsonValue::CreateBooleanValue(Bool(response,L"ok")));app.Telemetry(L"rename-replied",receipt);
        if (Bool(response, L"ok"))
        {
            auto result = response.GetNamedObject(L"result");
            auto key = Str(result, L"key");
            bool title=!folderId.empty()&&renameKey==L"folder:"+folderId;
            EndRename();
            if(title){Draw();return;}
            selected.clear();
            selected.insert(key);
            Focus(key);
        }
        else if (edit)
        {
            SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
            SetForegroundWindow(renameHost);SetFocus(edit);
        }
    });
}
void Window::ResizeRename()
{
    if(!edit)return; auto r=RenameBounds();if(r.right<=r.left||r.bottom<=r.top)return;
    int n=GetWindowTextLengthW(edit);std::wstring value(n+1,L'\0');GetWindowTextW(edit,value.data(),n+1);
    HDC dc=GetDC(edit);auto old=SelectObject(dc,editFont);SIZE size{};GetTextExtentPoint32W(dc,value.c_str(),n,&size);SelectObject(dc,old);ReleaseDC(edit,dc);
    int cap=std::max(24,static_cast<int>(std::min(240.f,r.right-r.left)*dpi/96)),w=std::clamp(static_cast<int>(size.cx)+static_cast<int>(12*dpi/96),std::min(cap,static_cast<int>(40*dpi/96)),cap);
    auto tile=RenameItem();bool title=tile&&!folderId.empty()&&tile->folder==folderId;
    POINT anchor{title?static_cast<int>(r.left*dpi/96):static_cast<int>((r.left+r.right)*dpi/192)-w/2,static_cast<int>((r.top-2)*dpi/96)};
    if(!ClientToScreen(hwnd,&anchor))return;
    int h=static_cast<int>(std::max(24.f,r.bottom-r.top+4)*dpi/96);RECT current{};GetWindowRect(renameHost,&current);
    if(current.left!=anchor.x||current.top!=anchor.y||current.right-current.left!=w||current.bottom-current.top!=h)
    {
        SetWindowPos(renameHost,nullptr,anchor.x,anchor.y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
        SetWindowPos(edit,nullptr,0,0,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
    }
}
void Window::EndRename()
{
    bool hadEdit=edit!=nullptr,ownedFocus=edit&&GetFocus()==edit;
    renameGeneration++;
    if (edit)
    {
        auto old=std::exchange(edit,nullptr);ShowWindow(old,SW_HIDE);
        RemoveWindowSubclass(old, EditProc, 1);
        DestroyWindow(old);
    }
    if(renameHost){auto host=std::exchange(renameHost,nullptr);SetWindowLongPtrW(host,GWLP_USERDATA,0);ShowWindow(host,SW_HIDE);DestroyWindow(host);}
    if (editFont)
    {
        DeleteObject(editFont);
        editFont = nullptr;
    }
    renaming = false;
    composing = false;renameCommitPending=false;
    renameKey.clear();renameOriginal.clear();
    if (hwnd&&hadEdit){if(ownedFocus)SetFocus(hwnd);desktopStamp.valid=panelStamp.valid=false;InvalidateRect(hwnd,nullptr,FALSE);Draw();}
}
LRESULT CALLBACK Window::EditProc(HWND edit, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
{
    auto self = reinterpret_cast<Window *>(data);
    if(m==WM_ERASEBKGND){RECT rect{};GetClientRect(edit,&rect);FillRect(reinterpret_cast<HDC>(w),&rect,GetSysColorBrush(COLOR_WINDOW));return 1;}
    if (m == WM_IME_STARTCOMPOSITION)
        self->composing = true;
    else if (m == WM_IME_ENDCOMPOSITION)
    {self->composing=false;if(self->renameCommitPending||GetFocus()!=edit){self->renameCommitPending=false;PostMessageW(self->hwnd,WM_APP+47,self->renameGeneration,0);}}
    else if (m == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;
    else if(m==WM_KILLFOCUS&&!(GetWindowLongPtrW(edit,GWL_STYLE)&ES_READONLY))PostMessageW(self->hwnd,WM_APP+47,self->renameGeneration,0);
    else if (m == WM_KEYDOWN && !self->composing)
    {
        if (w == VK_RETURN)
        {
            self->SaveRename();
            return 0;
        }
        if (w == VK_ESCAPE){self->EndRename();return 0;}
        if(w==VK_F5){self->EndRename();self->Key(VK_F5);return 0;}
    }
    auto result=DefSubclassProc(edit,m,w,l);
    if(self->edit==edit&&(m==WM_CHAR||m==WM_PASTE||m==WM_CUT||m==WM_UNDO||m==WM_SETTEXT))self->ResizeRename();
    return result;
}
JsonObject Window::DragPayload(std::wstring const &key)
{
    JsonObject out;
    Put(out, L"session", app.model.session);
    Put(out, L"revision", static_cast<double>(app.model.revision));
    Put(out, L"board", boardId);
    Put(out, L"anchor", key);
    if (!folderId.empty())
        Put(out, L"folder", folderId);
    else
        out.Insert(L"folder", JsonValue::CreateNullValue());
    JsonArray keys;
    auto t = Find(key);
    if (t && (!t->folder.empty()||t->card))
        keys.Append(Text(key));
    else
        for (auto const &value : selected)
            if (auto item = Find(value); item && item->folder.empty()&&!item->card)
                keys.Append(Text(value));
    out.Insert(L"keys", keys);
    JsonArray places;if(auto board=app.model.Find(boardId))for(auto const &key:keys){auto k=std::wstring(key.GetString());if(auto found=board->positions.find(k);found!=board->positions.end()){auto p=found->second;JsonObject row;Put(row,L"key",k);Put(row,L"x",static_cast<double>(p.x));Put(row,L"y",static_cast<double>(p.y));Put(row,L"w",static_cast<double>(p.w));Put(row,L"h",static_cast<double>(p.h));places.Append(row);}}out.Insert(L"placements",places);
    return out;
}
void Window::BeginDrag(std::wstring const &key)
{
    ResetPageMotion();
    if (dragging || pendingDragKeys.contains(key) || !Find(key))
        return;
    if (!selected.count(key))
        Select(key);
    auto payload = DragPayload(key);auto anchorRect=Bounds(key);
    Put(payload,L"grabX",static_cast<double>(pressedPoint.x*96/dpi-anchorRect.left));Put(payload,L"grabY",static_cast<double>(pressedPoint.y*96/dpi-anchorRect.top));
    std::vector<std::wstring> files;
    for (auto const &v : payload.GetNamedArray(L"keys"))
    {
        auto t = Find(std::wstring(v.GetString()));
        if (t && !t->isVirtual && !t->target.empty())
            files.push_back(t->target);
    }
    JsonObject r;Put(r,L"op",std::wstring(L"prepareOutbound"));r.Insert(L"drag",payload);
    auto owner=providerState;
    app.Request(r,[owner,payload,files](JsonObject const &response){
        auto window=Live(owner);if(!window||!Bool(response,L"ok"))return;
        auto result=response.GetNamedObject(L"result");auto intents=result.GetNamedArray(L"intents");
        auto lease=Str(result,L"lease");Put(payload,L"lease",lease);Put(payload,L"page",Str(result,L"page"));
        window->app.activeDragLease=lease;
        try{OleDrag(*window,payload,files);}catch(...){window->app.Telemetry(L"drag-error");JsonObject notice;Put(notice,L"type",std::wstring(L"card-notice"));Put(notice,L"title",std::wstring(L"文件拖放"));Put(notice,L"text",std::wstring(L"Windows 未完成此次拖放，正在核对实际文件状态；未移动的项目保留原位。"));window->app.Packet(to_string(notice.Stringify()));}
        if(auto current=Live(owner)){
            current->app.activeDragLease.clear();JsonObject finish;Put(finish,L"op",std::wstring(L"finishOutbound"));Put(finish,L"board",Str(payload,L"board"));Put(finish,L"lease",lease);finish.Insert(L"intents",intents);current->app.Request(finish);
        }
    });
}
bool Window::AcceptDrag(JsonObject const &drag)
{
    if(GridEditing())return false;
    if(app.activeDragLease.empty())ResetPageMotion();
    return Str(drag, L"session") == app.model.session &&
           (static_cast<long long>(Num(drag,L"revision"))==app.model.revision||(!app.activeDragLease.empty()&&Str(drag,L"lease")==app.activeDragLease));
}
Tile const *Window::DropTarget(float x,float y)
{
    // A closing folder remains alive only for rendering. Drop hit testing must
    // already use desktop covers, even before its compositor batch completes.
    if(FolderDropOpen())return Find(Hit(x,y));
    if(auto board=app.model.Find(boardId))for(auto const &tile:board->tiles)if(Inside(RootBounds(tile.key),x,y))return &tile;
    return nullptr;
}
void Window::DragPreview(bool outside)
{
    if(dragOutPreview==outside)return;
    dragOutPreview=outside;
    if(outside&&!folderId.empty()&&!closing){ResetFolderAvoidance();CloseFolder();}
    // DragLeave / Drop / cancellation clear only the gesture flag. They never
    // reopen the panel or interrupt a shrink already in flight.
}
std::wstring Window::DropFolder(JsonObject const &drag,float x,float y)
{
    if(FolderDropOpen()&&Inside(panelRect,x,y))return folderId;
    if(Str(drag,L"anchor").rfind(L"item:",0)!=0)return L"";
    auto tile=DropTarget(x,y);return tile&&tile->key!=Str(drag,L"anchor")?tile->folder:L"";
}
bool Window::CanDrop(JsonObject const &drag, float x, float y)
{
    if(GridEditing())return false;
    if (DropScope(FolderDropOpen(), panelRect, x, y) == FolderDropScope::Desktop){
        auto board=app.model.Find(boardId);if(!board)return false;
        auto hit=DropTarget(x,y);auto anchor=Str(drag,L"anchor");bool files=anchor.rfind(L"item:",0)==0;
        if(files&&hit&&hit->key!=anchor&&!hit->card&&(!hit->folder.empty()||hit->isFolder||hit->target.find(L"645FF040")!=std::wstring::npos))return true;
        auto places=DropPlaces(drag,x,y);if(places.empty())return false;
        std::set<std::wstring> ignored;if(Str(drag,L"board")==boardId&&Str(drag,L"folder").empty()&&(Str(drag,L"page").empty()||Str(drag,L"page")==board->pageId))for(auto const &k:drag.GetNamedArray(L"keys"))ignored.insert(std::wstring(k.GetString()));
        for(auto const &p:places){if(p.x<0||p.y<0||p.x+p.w>board->gridColumns||p.y+p.h>board->gridRows)return false;
            for(auto const &[key,q]:board->positions)if(!ignored.contains(key)&&p.x<q.x+q.w&&p.x+p.w>q.x&&p.y<q.y+q.h&&p.y+p.h>q.y)return false;}
        return true;
    }
    if (!Inside(contentRect, x, y))return false;
    if (Str(drag,L"board")==boardId&&Str(drag,L"folder")==folderId)return !Tiles().empty();
    return Str(drag,L"anchor").rfind(L"item:",0)==0;
}
std::vector<Place> Window::DropPlaces(JsonObject const &drag,float x,float y,bool *adjusted)
{
    auto destination=app.model.Find(boardId),source=app.model.Find(Str(drag,L"board"));
    if(!destination||!source)return {};
    auto keys=drag.GetNamedArray(L"keys");
    if(keys.Size()<1||keys.Size()>256)return {};
    std::vector<Place> places;
    auto anchor=Str(drag,L"anchor");
    Place origin{};
    bool fromFolder=!Str(drag,L"folder").empty();
    int cols=std::max(1,std::min({4,destination->gridColumns,static_cast<int>(std::ceil(std::sqrt(keys.Size())))}));
    for(UINT i=0;i<keys.Size();++i){
        auto key=std::wstring(keys.GetAt(i).GetString());
        Place p{};
        if(fromFolder){p.x=i%cols;p.y=i/cols;}
        else {
            // The source page may have changed during dwell navigation. Keep
            // the geometry captured at drag start, just like the adapter lease.
            bool found=false;
            if(auto captured=drag.TryLookup(L"placements");captured&&captured.ValueType()==JsonValueType::Array){
                for(auto const &v:captured.GetArray()){
                    auto row=v.GetObject();
                    if(Str(row,L"key")!=key)continue;
                    p={static_cast<int>(Num(row,L"x")),static_cast<int>(Num(row,L"y")),
                       static_cast<int>(Num(row,L"w",1)),static_cast<int>(Num(row,L"h",1))};
                    found=true;break;
                }
            }
            if(!found){
                auto it=source->positions.find(key);
                if(it==source->positions.end())return {};
                p=it->second;
            }
        }
        places.push_back(p);
        if(key==anchor)origin=p;
    }
    int cx=std::max(0,static_cast<int>(std::round((x-static_cast<float>(Num(drag,L"grabX"))-destination->originX)/destination->stepX)));
    int cy=std::max(0,static_cast<int>(std::round((y-static_cast<float>(Num(drag,L"grabY"))-destination->originY)/destination->stepY)));
    // Reuse one resolved plan for preview, eligibility and commit within this grid cell.
    auto stamp=std::to_wstring(app.model.revision)+L":"+boardId+L":"+destination->pageId+L":"+std::to_wstring(destination->gridColumns)+L":"+std::to_wstring(destination->gridRows)+L":"+std::to_wstring(cx)+L":"+std::to_wstring(cy)+L":"+std::wstring(drag.Stringify());
    if(stamp==dropPlanStamp){if(adjusted)*adjusted=dropPlanAdjusted;return dropPlanPlaces;}
    dropPlanStamp=stamp;dropPlanPlaces.clear();dropPlanAdjusted=false;
    int columns=std::clamp(destination->gridColumns,1,128),rows=std::clamp(destination->gridRows,1,128);
    std::set<std::wstring> ignored;
    if(Str(drag,L"board")==boardId&&!fromFolder&&(Str(drag,L"page").empty()||Str(drag,L"page")==destination->pageId))
        for(auto const &key:keys)ignored.insert(std::wstring(key.GetString()));
    std::vector<unsigned char> occupied(columns*rows,0);
    for(auto const &[key,q]:destination->positions)if(!ignored.contains(key))
        for(int yy=std::max(0,q.y);yy<std::min(rows,q.y+q.h);++yy)
            for(int xx=std::max(0,q.x);xx<std::min(columns,q.x+q.w);++xx)occupied[yy*columns+xx]=1;
    auto fits=[&](int ax,int ay){for(auto p:places){p.x+=ax-origin.x;p.y+=ay-origin.y;
        if(p.x<0||p.y<0||p.x+p.w>columns||p.y+p.h>rows)return false;
        for(int yy=p.y;yy<p.y+p.h;++yy)for(int xx=p.x;xx<p.x+p.w;++xx)if(occupied[yy*columns+xx])return false;
    }return true;};
    int ax=cx,ay=cy;
    if(!fits(ax,ay)){
        dropPlanAdjusted=true;long long nearest=LLONG_MAX;bool found=false;
        for(int yy=0;yy<rows;++yy)for(int xx=0;xx<columns;++xx){
            long long dx=xx-cx,dy=yy-cy,distance=dx*dx+dy*dy;
            if(distance<nearest&&fits(xx,yy)){nearest=distance;ax=xx;ay=yy;found=true;}
        }
        if(!found){if(adjusted)*adjusted=true;return {};}
    }
    for(auto &p:places){p.x+=ax-origin.x;p.y+=ay-origin.y;}
    dropPlanPlaces=places;if(adjusted)*adjusted=dropPlanAdjusted;
    return places;
}
void Window::DropPreview(JsonObject const *drag,float x,float y,bool valid)
{
    if(!drag){if(!folderPreviewCommitted)ResetFolderAvoidance();if(dropVisual){if(root)root.Children().Remove(dropVisual);dropVisual=nullptr;}dropStamp.clear();return;}
    auto board=app.model.Find(boardId);if(!board)return;
    bool sameFolder=FolderDropOpen()&&Str(*drag,L"board")==boardId&&Str(*drag,L"folder")==folderId;
    if(sameFolder&&Inside(contentRect,x,y)){
        if(dropVisual){root.Children().Remove(dropVisual);dropVisual=nullptr;}dropStamp.clear();if(valid)PreviewFolderDrag(*drag,x,y);else if(folderPreviewActive){ResetFolderAvoidance();Draw(false);}return;
    }
    if(folderPreviewActive){ResetFolderAvoidance();Draw(false);}
    if(sameFolder)return; // OLE cursor still represents genuinely invalid destinations.
    std::vector<D2D1_RECT_F> boxes;
    auto hit=DropTarget(x,y);bool inside=DropScope(FolderDropOpen(),panelRect,x,y)==FolderDropScope::Folder;
    bool adjusted=false;
    bool fileTarget=Str(*drag,L"anchor").rfind(L"item:",0)==0&&hit&&hit->key!=Str(*drag,L"anchor")&&!hit->card&&(!hit->folder.empty()||hit->isFolder||hit->target.find(L"645FF040")!=std::wstring::npos);
    if(hit&&(inside||fileTarget))boxes.push_back(inside?Bounds(hit->key):RootBounds(hit->key));
    else for(auto const &p:DropPlaces(*drag,x,y,&adjusted))boxes.push_back({board->originX+p.x*board->stepX,board->originY+p.y*board->stepY,board->originX+(p.x+p.w)*board->stepX-8*board->stepX/96,board->originY+(p.y+p.h)*board->stepY-8*board->stepY/96});
    if(boxes.empty()&&!inside){
        float left=board->originX+std::max(0.f,std::round((x-static_cast<float>(Num(*drag,L"grabX"))-board->originX)/board->stepX))*board->stepX;
        float top=board->originY+std::max(0.f,std::round((y-static_cast<float>(Num(*drag,L"grabY"))-board->originY)/board->stepY))*board->stepY;
        boxes.push_back({left,top,left+board->stepX-8*board->stepX/96,top+board->stepY-8*board->stepY/96});valid=false;
    }
    valid=valid&&!adjusted;
    std::wstring stamp=std::to_wstring(dpi)+std::to_wstring(Preview70Theme::systemDark)+std::to_wstring(NativeLowPower(app.model));for(auto r:boxes)stamp+=L":"+std::to_wstring(r.left)+L":"+std::to_wstring(r.top)+L":"+std::to_wstring(r.right)+L":"+std::to_wstring(r.bottom);if(dropVisual&&stamp==dropStamp)return;dropStamp=stamp;
    if(!dropVisual){dropVisual=app.graphics.compositor.CreateShapeVisual();root.Children().InsertAtTop(dropVisual);}
    dropVisual.Size({width,height});dropVisual.Scale({dpi/96,dpi/96,1});dropVisual.Shapes().Clear();
    auto color=NativeDropColor(app.model,Preview70Theme::systemDark);
    constexpr float radius=NativeMetrics::PanelRadius;
    for(auto r:boxes){
        auto geometry=app.graphics.compositor.CreateRoundedRectangleGeometry();
        geometry.Offset({r.left+1,r.top+1});
        geometry.Size({std::max(1.f,r.right-r.left-2),std::max(1.f,r.bottom-r.top-2)});
        geometry.CornerRadius({std::max(0.f,radius-1),std::max(0.f,radius-1)});
        auto shape=app.graphics.compositor.CreateSpriteShape(geometry);
        shape.StrokeBrush(app.graphics.compositor.CreateColorBrush(color));
        shape.StrokeThickness(2);
        dropVisual.Shapes().Append(shape);
    }
}
void Window::Drop(JsonObject const &drag, POINTL point, DWORD *effect)
{
    POINT p{point.x, point.y};
    ScreenToClient(hwnd, &p);
    float x = p.x * 96 / dpi, y = p.y * 96 / dpi;
    // Drop may carry a newer pointer position than the last DragOver.
    if(FolderDropOpen()&&DropScope(true,panelRect,x,y)==FolderDropScope::Desktop)DragPreview(true);
    if (!CanDrop(drag, x, y))
        return;
    bool inside = DropScope(FolderDropOpen(), panelRect, x, y) == FolderDropScope::Folder;
    bool sameFolder = inside && Str(drag, L"board") == boardId && Str(drag, L"folder") == folderId;
    JsonObject r;
    r.Insert(L"drag", drag);
    if(app.model.desktopRuntime&&!FolderDropOpen()){
        auto target=DropTarget(x,y);
        if(target&&target->folder.empty()&&!target->card&&(target->isFolder||target->target.find(L"645FF040")!=std::wstring::npos)&&target->key!=Str(drag,L"anchor")){
            Put(r,L"op",std::wstring(L"physical-drop"));Put(r,L"board",boardId);Put(r,L"target",target->key);
            r.Insert(L"control",JsonValue::CreateBooleanValue((GetKeyState(VK_CONTROL)&0x8000)!=0));r.Insert(L"shift",JsonValue::CreateBooleanValue((GetKeyState(VK_SHIFT)&0x8000)!=0));CommitDrop(r,drag,false);*effect=DROPEFFECT_MOVE;return;
        }
    }
    if (sameFolder)
    {
        Put(r, L"op", std::wstring(L"reorder"));
        std::set<std::wstring> keys;for(auto const &key:drag.GetNamedArray(L"keys"))keys.insert(std::wstring(key.GetString()));auto plan=NativeFolderPlan::Build(Tiles(),keys,Str(drag,L"anchor"),FolderDropIndex(drag,x,y));if(plan.insertion<0)return;
        Put(r,L"target",Tiles()[plan.nearest].key);Put(r,L"insertionIndex",static_cast<double>(plan.insertion));folderPreviewCommitted=folderPreviewActive;
    }
    else
    {
        Put(r, L"op", std::wstring(L"move"));
        Put(r, L"destination", boardId);
        std::wstring destination = DropFolder(drag,x,y);
        if (!destination.empty())
            Put(r, L"destinationFolder", destination);
        if(destination.empty()){
            auto plan=DropPlaces(drag,x,y);auto keys=drag.GetNamedArray(L"keys");
            for(UINT i=0;i<keys.Size()&&i<plan.size();++i)if(std::wstring(keys.GetAt(i).GetString())==Str(drag,L"anchor")){
                Put(r,L"x",static_cast<double>(plan[i].x));Put(r,L"y",static_cast<double>(plan[i].y));break;
            }
        }
    }
    bool leaving = !folderId.empty() && !inside;
    CommitDrop(r,drag,leaving);
    *effect = DROPEFFECT_MOVE;
}
void Window::CommitDrop(JsonObject const &r,JsonObject const &drag,bool leaving)
{
    auto owner = providerState;
    std::shared_ptr<ProviderState> sourceState;
    std::set<std::wstring> pending;for(auto const &k:drag.GetNamedArray(L"keys"))pending.insert(std::wstring(k.GetString()));
    long long feedbackBegin=Clock();double feedbackMs=0;
    for(auto const &source:app.windows)if(source->boardId==Str(drag,L"board")){sourceState=source->providerState;source->pendingDragKeys.insert(pending.begin(),pending.end());source->desktopStamp.valid=source->panelStamp.valid=false;source->Draw();feedbackMs=Millis(feedbackBegin);break;}
    app.Request(r, [owner, leaving,sourceState,pending,feedbackMs,feedbackBegin](JsonObject const &response) {
        if(auto source=Live(sourceState)){if(!Bool(response,L"ok")||source->folderPreviewRevision!=source->app.model.revision)source->ResetFolderAvoidance();for(auto const &key:pending)source->pendingDragKeys.erase(key);source->desktopStamp.valid=source->panelStamp.valid=false;source->Draw();JsonObject timing;Put(timing,L"feedbackDrawMs",feedbackMs);Put(timing,L"replyAfterFeedbackMs",Millis(feedbackBegin)-feedbackMs);timing.Insert(L"accepted",JsonValue::CreateBooleanValue(Bool(response,L"ok")));source->app.Telemetry(L"drop-feedback-completed",timing);}
        if (auto window = Live(owner); window && leaving && Bool(response, L"ok"))
        {
            if(!window->closing)window->CloseFolder();
            window->Draw();
        }
    });
}
void Window::MoveTo(std::wstring const &destination, bool)
{
    if (focusKey.empty())
        return;
    if (!selected.count(focusKey))
        Select(focusKey);
    JsonObject r;
    Put(r, L"op", std::wstring(L"moveAuto"));
    r.Insert(L"drag", DragPayload(focusKey));
    Put(r, L"destination", destination);
    app.Request(r);
}
void Window::Snapshot(std::wstring const &phase)
{
    auto d = app.graphics.Metrics();
    Put(d, L"board", boardId);
    Put(d, L"folder", folderId);
    Put(d, L"items", static_cast<double>(Tiles().size()));
    if(auto board=app.model.Find(boardId))Put(d,L"cardCount",static_cast<double>(std::count_if(board->tiles.begin(),board->tiles.end(),[](auto const &tile){return static_cast<bool>(tile.card);})));
    Put(d, L"dpi", dpi);
    Put(d, L"clientWidthDip", width);
    Put(d, L"clientHeightDip", height);
    Put(d, L"redraws", static_cast<double>(redraws));
    Put(d, L"drawnTiles", static_cast<double>(drawnTiles));
    Put(d, L"rootRedraws", static_cast<double>(rootRedraws));
    Put(d, L"panelRedraws", static_cast<double>(panelRedraws));
    Put(d,L"cardRedraws",static_cast<double>(cardRedraws));Put(d,L"cardUpdatePixels",static_cast<double>(cardUpdatePixels));
    Put(d, L"textFormatCount", static_cast<double>(app.graphics.formats.size()));
    Put(d, L"surfaceCacheHits", static_cast<double>(surfaceCacheHits));
    Put(d, L"iconCount", static_cast<double>(icons.size()));
    Put(d, L"decodedIconBytes", static_cast<double>(iconBytes));
    Put(d, L"surfacePixelBytesEstimate", static_cast<double>(estimatedSurfaceBytes+AdditionalSurfaceBytes()+(folderAvoidance?folderAvoidance->bytes:0)+(folderMorph?folderMorph->bytes:0)));Put(d,L"gridSurfaceBytes",static_cast<double>(gridSurfaceBytes));Put(d,L"gridDockBytes",static_cast<double>(gridDockBytes));Put(d,L"folderMorphBytes",static_cast<double>(folderMorph?folderMorph->bytes:0));Put(d,L"folderAvoidanceBytes",static_cast<double>(folderAvoidance?folderAvoidance->bytes:0));Put(d,L"outgoingPageBytes",static_cast<double>(outgoingPageBytes));
    app.Telemetry(phase, d);
}
void Window::CancelBenchmark()
{
    if (benchmark)
    {
        benchmark = false;
        KillTimer(app.relay, 71);
        app.Telemetry(L"benchmark-cancelled");
        status = L"检测到交互，自动对照已取消";
    }
}
void Window::QueueDisplayChange(RECT const *dpiRect)
{
    if(!hwnd||app.stopping)return;
    if(dpiRect){pendingDpiRect=*dpiRect;dpiChangePending=true;}
    bool queued=displayChangePending;displayChangePending=true;
    if(!queued)app.QueueRenderWork();
}
void Window::RefreshDisplay()
{
    if(!hwnd||app.stopping||!displayChangePending)return;
    if(NativeRenderScope::busy){app.QueueRenderWork();return;}
    bool dpiChanged=dpiChangePending;auto bounds=pendingDpiRect;
    displayChangePending=dpiChangePending=false;
    JsonObject detail;Put(detail,L"board",boardId);detail.Insert(L"dpiChanged",JsonValue::CreateBooleanValue(dpiChanged));app.Telemetry(L"display-refresh-start",detail);
    // Do not call wallpaper COM, rebuild visuals or cancel a grid draft from
    // inside WM_DISPLAYCHANGE / WM_DPICHANGED (both may be sent reentrantly).
    {
        NativeRenderScope updating(app.relay);
        if(GridEditing())GridRequest(L"cancel");
        labelBackground.Invalidate();ClearIcons();
        if(dpiChanged){EndRename();CloseFolder(true);}
        ReleaseSurfaces(true);
        if(dpiChanged){
            applyingDisplayChange=true;
            SetWindowPos(hwnd,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
            applyingDisplayChange=false;
        }
        Layout();
    }
    Draw();if(dpiChanged)Register();
    Snapshot(dpiChanged?L"dpi-changed":L"display-changed");
}
LRESULT Window::Message(UINT m, WPARAM w, LPARAM l)
{
    if(m==WM_DISPLAYCHANGE){QueueDisplayChange();return 0;}
    if(m==WM_DPICHANGED){QueueDisplayChange(reinterpret_cast<RECT const*>(l));return 0;}
    if(m==WM_SIZE&&(applyingDisplayChange||displayChangePending||NativeRenderScope::busy)){if(!applyingDisplayChange)QueueDisplayChange();return 0;}
    if(gridClosing&&(m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN||m==WM_KEYDOWN))FinishGridMotion();
    if(GridPointer(m,GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi))return 0;
    // Geometry changes invalidate only this draft; saved layout remains intact.
    if(GridEditing()&&m==WM_WTSSESSION_CHANGE)GridRequest(L"cancel");
    if(m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN||m==WM_MBUTTONDOWN||m==WM_KEYDOWN||m==WM_MOUSEWHEEL||m==WM_SIZE||m==WM_DPICHANGED||m==WM_SETTINGCHANGE)ResetPageMotion();
    switch (m)
    {
    case WM_APP+47:
        if(edit&&static_cast<int>(w)==renameGeneration)SaveRename();return 0;
    case WM_COMMAND:
        if(edit&&reinterpret_cast<HWND>(l)==edit&&HIWORD(w)==EN_CHANGE){ResizeRename();return 0;}break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if(reinterpret_cast<HWND>(l)==edit){SetTextColor(reinterpret_cast<HDC>(w),RGB(18,18,18));SetBkColor(reinterpret_cast<HDC>(w),RGB(255,255,255));return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));}break;
    case WM_CREATE:
        Initialize();
        return 0;
    case WM_TIMER:
        if(w==111){ShowFileHover();return 0;}
        if(w==109){FinishGridMotion();return 0;}
        if(w==108){ResetPageMotion();return 0;}
        if(w==104){KillTimer(hwnd,104);if(!sessionPaused&&!app.stopping)Register(false);return 0;}
        if(w==98){KillTimer(hwnd,98);if(!renameClickKey.empty()&&selected.size()==1&&selected.contains(renameClickKey)&&focusKey==renameClickKey&&!dragging)Rename();renameClickKey.clear();return 0;}
        if(w==97){if(hwnd)KillTimer(hwnd,97);auto fire=std::move(dragPageDwell);dragPageDwell={};if(fire)fire();return 0;}
        if(w==73){if(app.model.todoData&&!Str(app.model.todoData,L"todayDate").empty()&&Str(app.model.todoData,L"todayDate")!=NativeTodoToday()){JsonObject r;Put(r,L"op",std::wstring(L"snapshot"));app.Request(r);}DrawCards(false);UpdateCardClock();return 0;}break;
    case WM_WTSSESSION_CHANGE:
        if(w==WTS_SESSION_LOCK||w==WTS_SESSION_UNLOCK){
            sessionPaused=w==WTS_SESSION_LOCK;UpdateCardClock();
            if(app.model.components){JsonObject request;Put(request,L"op",std::wstring(L"visibility"));Put(request,L"board",boardId);request.Insert(L"visible",JsonValue::CreateBooleanValue(!sessionPaused&&IsWindowVisible(hwnd)));app.Request(request);}
            if(sessionPaused)ReleaseSurfaces();else DrawCards(false);
        }return 0;
    case WM_SHOWWINDOW:
        HideFileHover();
        if(w)everShown=true;
        if(!w)ReleaseSurfaces();else PostMessageW(hwnd,WM_PAINT,0,0);
        UpdateCardClock();
        if(app.model.components){JsonObject request;Put(request,L"op",std::wstring(L"visibility"));Put(request,L"board",boardId);request.Insert(L"visible",JsonValue::CreateBooleanValue(w!=0&&!sessionPaused));app.Request(request);}
        break;
    case WM_SIZE:
        HideFileHover();
        if (w == SIZE_MINIMIZED)
        {
            ClearIcons();
            ReleaseSurfaces();
            app.graphics.Trim();
        }
        else if (root)
        {
            Layout();
            Draw();
        }
        return 0;
    case WM_POWERBROADCAST:
        desktopStamp.valid=false;panelStamp.valid=false;ReleaseSurfaces();Draw();return TRUE;
    case WM_SETTINGCHANGE:
        labelBackground.Invalidate();Preview70Theme::Refresh();desktopStamp.valid=false;panelStamp.valid=false;InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_PAINT: {
        PAINTSTRUCT p;
        BeginPaint(hwnd, &p);
        EndPaint(hwnd, &p);
        Draw();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_GETOBJECT:
        if (static_cast<LONG>(l) == UiaRootObjectId)
        {
            auto p = MakeProvider(providerState);
            LRESULT result = UiaReturnRawElementProvider(hwnd, w, l, p);
            p->Release();
            return result;
        }
        break;
    case WM_SETFOCUS:
        // The event-driven desktop guard owns depth policy. Unconditionally
        // lowering on focus undoes Windows Show Desktop for borderless games.
        // Explicit external-app launch handoffs still lower the owned group.
        NotifyFocus();return 0;
    case WM_SYSKEYDOWN:
        if(w==VK_RETURN&&(GetKeyState(VK_MENU)&0x8000)){app.inputEpoch++;Key(VK_RETURN);return 0;}
        break;
    case WM_KEYDOWN:
        HideFileHover();if(MediaKey(static_cast<UINT>(w))||HonorKey(static_cast<UINT>(w)))return 0;
        CancelBenchmark();
        Key(static_cast<UINT>(w));
        return 0;
    case WM_MOUSEWHEEL:
        HideFileHover();
        {POINT cursor{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(hwnd,&cursor);if(auto tile=Find(Hit(cursor.x*96/dpi,cursor.y*96/dpi));tile&&tile->card&&Str(tile->card,L"type")==L"system.honor-service"){HonorPage(*tile,GET_WHEEL_DELTA_WPARAM(w)>0?-1:1);return 0;}}

        if(folderId.empty()&&!pressed){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(hwnd,&point);auto tile=Find(Hit(point.x*96/dpi,point.y*96/dpi));
            if(tile&&tile->card&&(Str(tile->card,L"type")==L"system.todo"||Str(tile->card,L"type")==L"system.week")){
                float tw=static_cast<float>(Num(tile->card,L"columns",4))*96-8,th=static_cast<float>(Num(tile->card,L"rows",2))*96-8;bool week=Str(tile->card,L"type")==L"system.week";
                auto body=week?NativeWeekBody(tw,th):D2D1::RectF(14,52,tw-14,th-16);auto data=NativeTodoData(*this,*tile);float maximum=NativeTodoScrollMax(data,body,week?31.f:32.f,week?NativeWeekSelected(*this,*tile):L"");
                todoScrolls[tile->key]=std::clamp(todoScrolls[tile->key]-GET_WHEEL_DELTA_WPARAM(w)/120.f*32,0.f,maximum);DrawCards(true,Str(tile->card,L"type"));return 0;}}
        CancelBenchmark();
        Scroll(-GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 2);
        return 0;
    case WM_LBUTTONDOWN: {
        HideFileHover();if(HandoffPointer(WM_LBUTTONDOWN,GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi))return 0;if(MediaPointer(WM_LBUTTONDOWN,GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi))return 0;
        app.inputEpoch++;if(app.menus)app.menus->Close();
        KillTimer(hwnd,98);renameClickKey.clear();renameClick=false;
        CancelBenchmark();
        if(edit){SaveRename();return 0;}
        SetFocus(hwnd);
        float x = GET_X_LPARAM(l) * 96 / dpi, y = GET_Y_LPARAM(l) * 96 / dpi;
        if (!folderId.empty() &&
            (!Inside(panelRect, x, y) || (x > panelRect.right - 56 && y < panelRect.top + 55)))
        {
            CloseFolder();
            return 0;
        }
        if(!folderId.empty()&&Inside({panelRect.left+24,panelRect.top+22,panelRect.right-64,panelRect.top+50},x,y)){Rename(L"folder:"+folderId);return 0;}
        if(folderMorph){ResetFolderMorph();Draw(false);}
        if(ClickPage(x,y))return 0;
        auto key = Hit(x, y);
        if (key.empty())
        {
            marqueeBase=selected;marqueeControl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
            if(!marqueeControl)selected.clear();focusKey.clear();NotifySelection();
            marquee=true;pressedPoint={GET_X_LPARAM(l),GET_Y_LPARAM(l)};SetCapture(hwnd);
            Draw();
            return 0;
        }
        auto clickedFolder=Find(key);
        if(clickedFolder&&!clickedFolder->folder.empty()&&folderId.empty()){
            auto r=RootBounds(key);
            if(Inside(NameBounds(*clickedFolder,r,false,RootUnit()),x,y)){Rename(key);return 0;}
            if(Inside(CoverLayout(*clickedFolder,r).Corner(),x,y)){OpenFolder(key);return 0;}
        }
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        auto clicked=Find(key);
        renameClick=!ctrl&&!shift&&selected.size()==1&&selected.contains(key)&&focusKey==key&&(!selectedClickAt||GetTickCount64()-selectedClickAt>GetDoubleClickTime())&&clicked&&!clicked->card&&clicked->folder.empty()&&!clicked->isVirtual;
        selectedClickAt=GetTickCount64();
        if (!selected.count(key) || ctrl || shift)
            Select(key, ctrl, shift);
        else
            Focus(key);
        pressed = true;
        pressedPoint = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        pressedKey = key;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if(HandoffPointer(WM_LBUTTONUP,GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi))return 0;
        if(MediaPointer(WM_LBUTTONUP,GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi))return 0;
        if(marquee){marquee=false;MarqueePreview();marqueeBase.clear();if(GetCapture()==hwnd)ReleaseCapture();NotifySelection();return 0;}
        if(pressed&&!dragging&&renameClick&&Hit(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi)==pressedKey){renameClickKey=pressedKey;SetTimer(hwnd,98,GetDoubleClickTime(),nullptr);}renameClick=false;
        if(pressed&&!dragging&&app.model.components){
            auto tile=Find(pressedKey);
            if(tile&&tile->card){auto rect=RootBounds(tile->key);float scale=std::min((rect.right-rect.left)/184,(rect.bottom-rect.top)/184);float x=(GET_X_LPARAM(l)*96/dpi-rect.left)/scale,y=(GET_Y_LPARAM(l)*96/dpi-rect.top)/scale;
                if(Str(tile->card,L"type")==L"system.handoff"){
                    float tw=static_cast<float>(Num(tile->card,L"columns",2))*96-8,th=static_cast<float>(Num(tile->card,L"rows",2))*96-8,zoom=(rect.right-rect.left)/tw;
                    CardAction(tile->key,NativeHandoffAction(*this,*tile,(GET_X_LPARAM(l)*96/dpi-rect.left)/zoom,(GET_Y_LPARAM(l)*96/dpi-rect.top)/zoom,tw,th));
                }
                else if(Str(tile->card,L"type")==L"system.media"){
                    float tw=static_cast<float>(Num(tile->card,L"columns",2))*96-8,th=static_cast<float>(Num(tile->card,L"rows",2))*96-8,zoom=(rect.right-rect.left)/tw;
                    int action=NativeMediaActionAt((GET_X_LPARAM(l)*96/dpi-rect.left)/zoom,(GET_Y_LPARAM(l)*96/dpi-rect.top)/zoom,tw,th);MediaSend(tile->key,action==1?L"media-previous":action==2?L"media-toggle":action==3?L"media-next":L"media-open");
                }
                else if(Str(tile->card,L"type")==L"system.honor-service"){
                    float tw=static_cast<float>(Num(tile->card,L"columns",4))*96-8,zoom=(rect.right-rect.left)/tw;int index=NativeServiceHit(*this,*tile,(GET_X_LPARAM(l)*96/dpi-rect.left)/zoom,(GET_Y_LPARAM(l)*96/dpi-rect.top)/zoom,tw);
                    if(index==-2||index==-3)HonorPage(*tile,index==-2?-1:1);else if(index>=0)CardAction(tile->key,L"honor-open",index);
                }
                else if(Str(tile->card,L"type")==L"system.weather"&&Hit(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi)==pressedKey){
                    float tw=static_cast<float>(Num(tile->card,L"columns",2))*96-8,th=static_cast<float>(Num(tile->card,L"rows",2))*96-8;
                    float zoom=std::min((rect.right-rect.left)/tw,(rect.bottom-rect.top)/th);
                    NativeWeatherTurn(*this,*tile,NativeWeatherHours::Hit((GET_X_LPARAM(l)*96/dpi-rect.left)/zoom,(GET_Y_LPARAM(l)*96/dpi-rect.top)/zoom,tw,th),tw,th);
                }
                else if(Str(tile->card,L"type")==L"system.clipboard-image")CardAction(tile->key,L"image-copy");
                else if(Str(tile->card,L"type")==L"system.storage"&&app.model.honorSignedIn&&x>=14&&x<=128&&y>=153&&y<=176)CardAction(tile->key,L"cleanup");
                else if(Str(tile->card,L"type")==L"system.todo"||Str(tile->card,L"type")==L"system.week"){
                    float tw=static_cast<float>(Num(tile->card,L"columns",4))*96-8,th=static_cast<float>(Num(tile->card,L"rows",2))*96-8;
                    float zoom=std::min((rect.right-rect.left)/tw,(rect.bottom-rect.top)/th);
                    float tx=(GET_X_LPARAM(l)*96/dpi-rect.left)/zoom,ty=(GET_Y_LPARAM(l)*96/dpi-rect.top)/zoom;
                    auto state=NativeTodoData(*this,*tile);bool week=Str(tile->card,L"type")==L"system.week";
                    auto action=week?NativeWeekHit(state,tx,ty,tw,th,todoScrolls[tile->key],NativeWeekSelected(*this,*tile)):NativeTodoHit(state,tx,ty,tw,th,todoScrolls[tile->key]);
                    if(action.first==L"todo-select-day"){auto dates=NativeWeekDates(state);todoSelectedDays[tile->key]=dates[action.second];todoScrolls[tile->key]=0;DrawCards(true,L"system.week");}
                    else if(!action.first.empty())CardAction(tile->key,action.first,action.second);
                }
                else if(Str(tile->card,L"type")==L"system.encodeck"){
                    float ew=static_cast<float>(Num(tile->card,L"columns",4))*96-8,eh=static_cast<float>(Num(tile->card,L"rows",2))*96-8;
                    float zoom=std::min((rect.right-rect.left)/ew,(rect.bottom-rect.top)/eh);
                    auto action=EncoAction(app.model.encodeckData,(GET_X_LPARAM(l)*96/dpi-rect.left)/zoom,(GET_Y_LPARAM(l)*96/dpi-rect.top)/zoom);
                    if(!action.first.empty())CardAction(tile->key,action.first,action.second);
                }
                else if(auto custom=tile->card.TryLookup(L"custom");custom&&custom.ValueType()==JsonValueType::Object&&Str(custom.GetObject(),L"kind")==L"shortcut"&&y>=39)CardAction(tile->key,L"shortcut",std::clamp(static_cast<int>((y-39)/22),0,5));
            }
        }
        pressed = false;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if(HandoffPointer(WM_CAPTURECHANGED,0,0))return 0;
        if(MediaPointer(WM_CAPTURECHANGED,0,0))return 0;
        if(marquee){marquee=false;MarqueePreview();marqueeBase.clear();}
        pressed = false;
        return 0;
    case WM_MOUSELEAVE:
        HideFileHover(true);SyncInteraction(0,0,true);if(pageEdge){pageEdge=0;desktopStamp.valid=false;Draw();}return 0;
    case WM_MOUSEMOVE: {
        if(!handoffPressKey.empty())return 0;
        if(!mediaSeekKey.empty()){MediaPointer(WM_MOUSEMOVE,GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi);return 0;}
        FileHover(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi);SyncInteraction(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi);TRACKMOUSEEVENT hoverTrack{sizeof(hoverTrack),TME_LEAVE,hwnd,0};TrackMouseEvent(&hoverTrack);
        if(marquee){MarqueePreview(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi);return 0;}
        if(folderId.empty()&&!pressed&&!dragging){float x=GET_X_LPARAM(l)*96/dpi,y=GET_Y_LPARAM(l)*96/dpi;
            int edge=std::abs(y-height/2)<80?(x<112?-1:x>width-112?1:0):0;
            edge=AvailablePageEdge(edge);
            if(edge!=pageEdge){pageEdge=edge;desktopStamp.valid=false;Draw();}
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);}
        if (pressed && !dragging &&
            ((std::abs(GET_X_LPARAM(l) - pressedPoint.x) > GetSystemMetrics(SM_CXDRAG)) ||
             (std::abs(GET_Y_LPARAM(l) - pressedPoint.y) > GetSystemMetrics(SM_CYDRAG))))
        {
            pressed = false;
            ReleaseCapture();
            BeginDrag(pressedKey);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        HideFileHover();
        KillTimer(hwnd,98);renameClickKey.clear();renameClick=false;
        CancelBenchmark();
        auto key=Hit(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi);auto cover=Find(key);
        if(folderId.empty()&&cover&&!cover->folder.empty()){
            int child=CoverLayout(*cover,RootBounds(key)).HitMain(GET_X_LPARAM(l)*96/dpi,GET_Y_LPARAM(l)*96/dpi,static_cast<int>(cover->items.size()));
            if(child>=0){JsonObject request;Put(request,L"op",std::wstring(L"open"));Put(request,L"board",boardId);Put(request,L"folder",cover->folder);Put(request,L"key",cover->items[child].key);
                pressed=false;if(GetCapture()==hwnd)ReleaseCapture();
                if(app.model.desktopRuntime){AllowSetForegroundWindow(app.parent);SetWindowPos(hwnd,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}app.Request(request);return 0;
            }
        }
        Activate(key);
        return 0;
    }
    case WM_CONTEXTMENU: {
        HideFileHover();
        KillTimer(hwnd,98);renameClickKey.clear();renameClick=false;
        CancelBenchmark();
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}, client = p;
        ScreenToClient(hwnd, &client);
        auto key = Hit(client.x * 96 / dpi, client.y * 96 / dpi);
        if(key.empty()&&folderId.empty()){selected.clear();focusKey.clear();}
        if (!key.empty())
        {
            if (!selected.count(key))
                Select(key);
            else
                Focus(key);
        }
        if(!HonorContext(p))Menu(p);
        return 0;
    }
    case AnimationMessage:
        AnimationDone(static_cast<int>(w));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        displayChangePending=dpiChangePending=false;
        HideFileHover();fileTooltip.reset();KillTimer(hwnd,104);
        WTSUnRegisterSessionNotification(hwnd);
        CancelBenchmark();
        EndRename();
        providerState->alive = false;
        KillTimer(hwnd,97);dragPageDwell={};
        RemoveDrop(*this);
        if (batch)
        {
            batch.Completed(batchToken);
            batch = nullptr;
        }
        ClearIcons();
        ReleaseSurfaces();
        if (target)
            target.Root(nullptr);
        target = nullptr;
        root = nullptr;
        desktop = nullptr;
        panel = nullptr;
        scrim = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd = nullptr;
        if (std::none_of(app.windows.begin(), app.windows.end(),
                         [](auto const &x) { return x->hwnd != nullptr; }))
            PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    auto self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->hwnd = hwnd;
        self->providerState->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(hwnd, m, w, l);
    try
    {
        return self->Message(m, w, l);
    }
    catch (hresult_error const &error)
    {
        self->status = L"图形错误：" + std::wstring(error.message());
        try
        {
            if (error.code() == DXGI_ERROR_DEVICE_REMOVED || error.code() == DXGI_ERROR_DEVICE_RESET ||
                error.code() == D2DERR_RECREATE_TARGET)
                self->app.Recover();
            else {
                JsonObject detail;Put(detail,L"error",std::wstring(error.message()));Put(detail,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));Put(detail,L"message",static_cast<double>(m));self->app.Telemetry(L"render-failure",detail);
                self->Snapshot(L"render-error");
            }
        }
        catch (...)
        {
            PostQuitMessage(2);
        }
        return m == WM_CREATE ? -1 : 0;
    }
    catch (std::exception const &error)
    {
        self->status = std::wstring(to_hstring(error.what()));
        try
        {
            self->Snapshot(L"render-error");
        }
        catch (...)
        {
        }
        return m == WM_CREATE ? -1 : 0;
    }
}
void App::Recover()
{
    if(NativeRenderScope::busy){deviceRecoveryPending=true;QueueRenderWork();return;}
    deviceRecoveryPending=false;
    if(shell&&shell->captionClose)shell->captionClose->Reset();
    if(recovery)recovery->Close();recovery.reset();
    honor.Clear();
    if(menus)menus->Close();
    ClearRetiringPopups();if(popup)popup->Release();
    if(shell){shell->ClearIcons();for(auto const &[id,bar]:shell->bars)bar->Release();if(shell->toast)shell->toast->Release();}
    clipboardBitmap=nullptr;
    graphics.Recover();
    if(!clipboardPng.empty()){JsonObject data;Put(data,L"png",clipboardPng);data.Insert(L"available",JsonValue::CreateBooleanValue(true));LoadClipboardTexture(*this,data);}
    if(shell){for(auto const &[id,bar]:shell->bars)bar->Draw();if(shell->toast)shell->toast->Draw();shell->RequestIcons();}
    if(popup&&popup->visible)popup->Draw();
    for (auto const &w : windows)
        if (w->hwnd)
        {
            w->ClearIcons();
            w->ReleaseSurfaces();
            w->Draw();
            w->Snapshot(L"device-recreated");
        }
}
void App::StartWindows()
{
    if (started || model.boards.empty())
        return;
    started = true;
    graphics.Start();graphicsReady=true;
    if(!clipboardPng.empty()){JsonObject data;Put(data,L"png",clipboardPng);data.Insert(L"available",JsonValue::CreateBooleanValue(true));LoadClipboardTexture(*this,data);}
    int count = model.desktopRuntime?std::min(64,static_cast<int>(model.boards.size())):allDisplays?std::min(4,static_cast<int>(model.boards.size())):1;
    for (int i = 0; i < count; i++)
    {
        windows.push_back(std::make_unique<Window>(*this, model.boards[i].id));
        windows.back()->Create(i);
    }
    if(popupCycles&&!model.desktopRuntime){
        RECT r{};GetWindowRect(windows[0]->hwnd,&r);JsonObject snapshot,screen,rect,toolbar,dock,tbSettings,dockSettings,focused,flyouts;JsonArray monitors;
        Put(rect,L"left",static_cast<double>(r.left));Put(rect,L"top",static_cast<double>(r.top));Put(rect,L"right",static_cast<double>(r.right));Put(rect,L"bottom",static_cast<double>(r.bottom));Put(screen,L"id",std::wstring(L"fixture"));Put(screen,L"scale",GetDpiForWindow(windows[0]->hwnd)/96.0);screen.Insert(L"rect",rect);screen.Insert(L"toolbar",JsonValue::CreateBooleanValue(true));screen.Insert(L"dock",JsonValue::CreateBooleanValue(true));monitors.Append(screen);snapshot.Insert(L"monitors",monitors);snapshot.Insert(L"preview",JsonValue::CreateBooleanValue(true));
        for(auto section:{L"left",L"center",L"right"}){JsonArray tbItems,dockItems;if(std::wstring(section)==L"center"){JsonObject clock;Put(clock,L"id",std::wstring(L"fixture-clock"));Put(clock,L"label",std::wstring(L"clock"));clock.Insert(L"clock",JsonValue::CreateBooleanValue(true));tbItems.Append(clock);for(int i=0;i<4;i++){JsonObject item;Put(item,L"key",L"fixture-dock-"+std::to_wstring(i));Put(item,L"type",std::wstring(L"AppOrFile"));Put(item,L"label",L"应用 "+std::to_wstring(i+1));item.Insert(L"windows",JsonArray{});dockItems.Append(item);}}toolbar.Insert(section,tbItems);dock.Insert(section,dockItems);}
        Put(tbSettings,L"itemSize",16.0);Put(tbSettings,L"padding",4.0);Put(tbSettings,L"hideMode",std::wstring(L"Never"));Put(dockSettings,L"size",28.0);Put(dockSettings,L"padding",6.0);Put(dockSettings,L"hideMode",std::wstring(L"Never"));snapshot.Insert(L"toolbar",toolbar);snapshot.Insert(L"dock",dock);snapshot.Insert(L"toolbarSettings",tbSettings);snapshot.Insert(L"dockSettings",dockSettings);snapshot.Insert(L"focused",focused);snapshot.Insert(L"flyouts",flyouts);snapshot.Insert(L"dark",JsonValue::CreateBooleanValue(false));shell=std::make_unique<NativeShell>(*this);shell->Update(snapshot);
    }
    if (cycles || popupCycles)
    {
        windows.front()->benchmark = true;
        if(popupCycles)PopupSnapshot(L"benchmark-popup-idle-before");else windows.front()->Snapshot(L"benchmark-idle-before");
        SetTimer(relay, 71, model.components?15000:5000, nullptr);
    }
    Telemetry(L"ready", graphics.Metrics());
}
void App::Packet(std::string const &bytes)
{
    auto p = JsonObject::Parse(to_hstring(bytes));
    if(Str(p,L"type")==L"recovery-view"){if(recovery&&recovery->hwnd)recovery->Set(p.GetNamedObject(L"data"));return;}
    if(Str(p,L"type")==L"resource-probe"){
        JsonObject data;JsonObject packets;for(auto const &[kind,count]:packetCounts)Put(packets,kind.c_str(),static_cast<double>(count));data.Insert(L"packetCounts",packets);Put(data,L"waitWakes",static_cast<double>(waitWakes));JsonObject counts;for(auto const &[id,count]:messageCounts)Put(counts,std::to_wstring(id).c_str(),static_cast<double>(count));data.Insert(L"messageCounts",counts);auto epoch=p.TryLookup(L"expectedInputEpoch");bool interrupted=epoch&&epoch.ValueType()==JsonValueType::Number&&static_cast<long long>(epoch.GetNumber())!=inputEpoch;
        Put(data,L"popupTrayReorderBytes",static_cast<double>(popup&&popup->trayDrag?popup->trayDrag->bytes:0));
        if(!interrupted){auto action=Str(p,L"action");if(action==L"open-folder"||action==L"close-folder")for(auto const &window:windows)if(window->boardId==Str(p,L"board")){if(action==L"open-folder"){auto board=model.Find(window->boardId);if(board)for(auto const &tile:board->tiles)if(tile.folder==Str(p,L"folder")){window->OpenFolder(tile.key);break;}}else window->CloseFolder();break;}}
        if(!interrupted&&shell){auto action=Str(p,L"action");if(action==L"hide-dock-preview")shell->HidePreview();else if(action==L"show-dock-preview"){bool shown=false;for(auto const &[id,bar]:shell->bars){if(!bar->dock)continue;for(int i=0;i<static_cast<int>(bar->entries.size());i++)if(bar->entries[i].HasKey(L"windows")&&bar->entries[i].GetNamedArray(L"windows").Size()){bar->Reveal();shell->ShowPreview(*bar,i);shown=true;break;}if(shown)break;}}else if(action==L"show-flyout"&&!shell->bars.empty()){auto snapshot=shell->bars.begin()->second->data;shell->Toast(snapshot,L"音量",Num(snapshot,L"volume",0));}}
        Put(data,L"previewThumbnails",shell&&shell->preview?static_cast<double>(shell->preview->taskThumbnails.size()):0);Put(data,L"previewNominalBytes",shell&&shell->preview?static_cast<double>(shell->preview->surfaceBytes):0);data.Insert(L"flyoutVisible",JsonValue::CreateBooleanValue(shell&&shell->toast&&shell->toast->visible));Put(data,L"flyoutResizeBytes",shell&&shell->toast?static_cast<double>(shell->toast->FlyoutResizeBytes()):0);Put(data,L"flyoutSurfaceBytes",shell&&shell->toast?static_cast<double>(shell->toast->surfaceBytes):0);
        data.Insert(L"interrupted",JsonValue::CreateBooleanValue(interrupted));Put(data,L"inputEpoch",static_cast<double>(inputEpoch));Put(data,L"popupSurfaceBytes",popup?static_cast<double>(popup->surfaceBytes):0);Put(data,L"popupResizeBytes",popup?static_cast<double>(popup->FlyoutResizeBytes()+popup->ResizeBytes()):0);Put(data,L"popupGlassSheetBytes",popup?static_cast<double>(popup->glassSheet.bytes):0);Put(data,L"popupMediaMarqueeBytes",popup?static_cast<double>(popup->marqueeBytes):0);Put(data,L"popupRefreshFeedbackBytes",popup?static_cast<double>(popup->refreshBytes):0);data.Insert(L"popupVisible",JsonValue::CreateBooleanValue(popup&&popup->visible));Put(data,L"popupKind",popup&&popup->visible?popup->kind:L"");Put(data,L"popupRedraws",popup?static_cast<double>(popup->redraws):0);Put(data,L"retiringPopupCount",static_cast<double>(retiringPopups.size()));size_t retiringBytes=0;for(auto const &old:retiringPopups){retiringBytes+=old->surfaceBytes+old->glassSheet.bytes+old->marqueeBytes+old->refreshBytes;if(auto glass=NativeGlassStates().find(old->hwnd);glass!=NativeGlassStates().end())retiringBytes+=glass->second.bytes;}Put(data,L"retiringPopupBytes",static_cast<double>(retiringBytes));Put(data,L"nativeEditCount",popup?static_cast<double>(popup->edits.size()):0);Put(data,L"popupIconBytes",popup?static_cast<double>(popup->appearanceIcons.bytes):0);Put(data,L"taskPreviewCount",popup?static_cast<double>(popup->taskThumbnails.size()):0);Put(data,L"taskPreviewNominalBytes",popup?static_cast<double>(popup->taskPreviewBytes):0);Put(data,L"textFormats",static_cast<double>(graphics.formats.size()));
        size_t menuBytes=0,menuIconBytes=0; if(menus)for(auto const &level:menus->levels){menuBytes+=level.popup->surfaceBytes;menuIconBytes+=level.popup->appearanceIcons.bytes;}Put(data,L"menuSurfaceBytes",static_cast<double>(menuBytes));Put(data,L"menuIconBytes",static_cast<double>(menuIconBytes));Put(data,L"menuLevels",menus?static_cast<double>(menus->visibleLevels):0);
        Put(data,L"recoverySurfaceBytes",recovery?static_cast<double>(recovery->surfaceBytes):0);Put(data,L"recoveryIconBytes",recovery?static_cast<double>(recovery->icons.bytes):0);data.Insert(L"recoveryVisible",JsonValue::CreateBooleanValue(recovery&&recovery->hwnd));
        JsonArray desktops;size_t surfaces=menuBytes+retiringBytes+(recovery?recovery->surfaceBytes:0),iconBytes=menuIconBytes+(recovery?recovery->icons.bytes:0);for(auto const &window:windows){JsonObject item;Put(item,L"board",window->boardId);Put(item,L"surfaceBytes",static_cast<double>(window->estimatedSurfaceBytes+window->AdditionalSurfaceBytes()));Put(item,L"panelSurfaceBytes",window->panelSurface?static_cast<double>(window->panelPixelWidth)*window->panelPixelHeight*4:0);Put(item,L"iconBytes",static_cast<double>(window->iconBytes));Put(item,L"openFolder",window->folderId);Put(item,L"rootRedraws",static_cast<double>(window->rootRedraws));Put(item,L"panelRedraws",static_cast<double>(window->panelRedraws));Put(item,L"cardRedraws",static_cast<double>(window->cardRedraws));Put(item,L"cardBackdrops",static_cast<double>(window->cardBackdrops.size()));Put(item,L"cardBackdropBytes",static_cast<double>(window->cardBackdropBytes));Put(item,L"cardBlurRadius",window->cardBlurRadius);Put(item,L"gridSurfaceBytes",static_cast<double>(window->gridSurfaceBytes));Put(item,L"gridDockBytes",static_cast<double>(window->gridDockBytes));Put(item,L"folderMorphBytes",static_cast<double>(window->folderMorph?window->folderMorph->bytes:0));Put(item,L"folderAvoidanceBytes",static_cast<double>(window->folderAvoidance?window->folderAvoidance->bytes:0));Put(item,L"outgoingPageBytes",static_cast<double>(window->outgoingPageBytes));surfaces+=window->estimatedSurfaceBytes+window->AdditionalSurfaceBytes()+(window->folderAvoidance?window->folderAvoidance->bytes:0)+(window->folderMorph?window->folderMorph->bytes:0);iconBytes+=window->iconBytes+(window->fileTooltip?window->fileTooltip->appearanceIcons.bytes:0);desktops.Append(item);}data.Insert(L"desktops",desktops);
        JsonArray barRows;if(shell){for(auto const &[key,bar]:shell->bars){JsonObject item;Put(item,L"kind",bar->kind);Put(item,L"monitor",bar->monitor);Put(item,L"surfaceBytes",static_cast<double>(bar->SurfaceBytes()));Put(item,L"dissolveMaskBytes",static_cast<double>(bar->dockDissolve.maskBytes));Put(item,L"dissolveLayerBytes",static_cast<double>(bar->dockDissolve.layerBytes));Put(item,L"dissolveError",static_cast<double>(bar->dockDissolve.error));Put(item,L"iconScheme",HonorScheme(model.preferences));Put(item,L"honorCacheBytes",static_cast<double>(honor.bytes));Put(item,L"honorCacheEntries",static_cast<double>(honor.entries.size()));Put(item,L"itemRedraws",static_cast<double>(bar->itemRedraws));Put(item,L"redraws",static_cast<double>(bar->redraws));item.Insert(L"hidden",JsonValue::CreateBooleanValue(bar->edge||!bar->visible));surfaces+=bar->SurfaceBytes();barRows.Append(item);}iconBytes+=shell->iconBytes;}iconBytes+=honor.bytes;for(auto const &[_,icon]:handoffImages)iconBytes+=icon.bytes;for(auto const &[_,icon]:honorServiceIcons)iconBytes+=icon.bytes;data.Insert(L"bars",barRows);if(shell&&!shell->bars.empty()){auto snapshot=shell->bars.begin()->second->data;if(snapshot.HasKey(L"webviews"))data.Insert(L"webviews",snapshot.GetNamedArray(L"webviews"));}Put(data,L"surfaceBytes",static_cast<double>(surfaces));Put(data,L"iconBytes",static_cast<double>(iconBytes+(popup?popup->appearanceIcons.bytes:0)));data.Insert(L"graphics",graphics.Metrics());data.Insert(L"lowPowerRendering",JsonValue::CreateBooleanValue(NativeLowPower(model)));data.Insert(L"batterySaverKnown",JsonValue::CreateBooleanValue(model.saverKnown));data.Insert(L"batterySaverActive",JsonValue::CreateBooleanValue(model.saverActive));Put(data,L"powerNotificationSubscriptions",static_cast<double>(powerNotifications.size()));JsonObject request;Put(request,L"op",std::wstring(L"resource-report"));Put(request,L"nonce",Str(p,L"nonce"));request.Insert(L"data",data);Request(request);return;
    }
    lastPacketKind=Str(p,L"type");if(auto found=packetCounts.find(lastPacketKind);found!=packetCounts.end())++found->second;else if(packetCounts.size()<32)packetCounts.emplace(lastPacketKind,1);
    if(stopping)return;
    if(Str(p,L"type")==L"shell-data"&&!graphicsReady){pendingShellData=p;return;}
    if(Str(p,L"type")==L"shell-data"){if(!shell)shell=std::make_unique<NativeShell>(*this);shell->Update(p.GetNamedObject(L"data"));return;}
    if(Str(p,L"type")==L"popup-update"){if(popup)popup->Update(p);return;}
    if(Str(p,L"type")==L"popup"){Put(p,L"kind",CanonicalNativePopupId(Str(p,L"kind")));
        try{if(menus)menus->Close();RetirePopup(Str(p,L"kind"));if(!popup)popup=std::make_unique<NativePopup>(*this);popup->Show(p);}
        catch(...){if(popup)popup->Hide(false);JsonObject ready;Put(ready,L"op",std::wstring(L"popup-shown"));Put(ready,L"token",Str(p,L"token"));Put(ready,L"window",0.0);ready.Insert(L"shown",JsonValue::CreateBooleanValue(false));Request(ready);Telemetry(L"popup-fallback");}return;
    }
    if(Str(p,L"type")==L"popup-hide"){
        if(popup&&popup->token==Str(p,L"token"))popup->Hide(false);return;
    }
    if(Str(p,L"type")==L"images-invalidated"){
        auto path=Str(p,L"path");
        auto matches=[&](std::wstring const &source){return source.size()>=path.size()&&CompareStringOrdinal(source.c_str(),static_cast<int>(path.size()),path.c_str(),static_cast<int>(path.size()),TRUE)==CSTR_EQUAL;};
        if(popup&&popup->visible){for(auto const &v:popup->Data().GetNamedArray(L"rows",JsonArray{})){auto item=v.GetObject();if(!matches(Str(item,L"image")))continue;auto key=Str(item,L"id");auto found=popup->appearanceIcons.values.find(key);if(found!=popup->appearanceIcons.values.end()){popup->appearanceIcons.bytes-=found->second.bytes;popup->appearanceIcons.values.erase(found);popup->appearanceIcons.glyphs.erase(key);}popup->iconRequests.erase(key);}popup->RequestAppearanceIcons();popup->Draw();}
        if(shell){for(auto const &[key,source]:shell->iconSources)if(matches(source)){auto found=shell->icons.find(key);if(found!=shell->icons.end()){shell->iconBytes-=found->second.bytes;shell->icons.erase(found);}shell->pending.erase(key);}shell->RequestIcons();for(auto const &[id,bar]:shell->bars)bar->Draw();}
        return;
    }
    if(Str(p,L"type")==L"icons-invalidated"){
        for(auto const &window:windows)if(window->hwnd){
            for(auto const &value:p.GetNamedArray(L"keys")){auto key=std::wstring(value.GetString());auto found=window->icons.find(key);if(found!=window->icons.end()){window->iconBytes-=found->second.bytes;window->icons.erase(found);}window->pendingIcons.erase(key);}
            window->iconVersion++;window->Draw();window->RequestIcons();
        }return;
    }
    if (Str(p, L"type") == L"icons-ready")
    {
        if(recovery&&recovery->hwnd)recovery->IconsReady();
        if(shell){shell->pending.clear();shell->RequestIcons();}
        if(popup&&popup->visible){popup->iconRequests.clear();popup->RequestAppearanceIcons();}
        for (auto const &w : windows)
            if (w->hwnd)
            {
                w->pendingIcons.clear();
                w->RequestIcons();
            }
        return;
    }
    if(Str(p,L"type")==L"media-data"){
        model.mediaData=p.GetNamedObject(L"data");for(auto const &window:windows)if(window->hwnd){if(!window->mediaSeekKey.empty()&&window->mediaSeekTrack!=Str(model.mediaData,L"trackId")){window->mediaSeekKey.clear();if(GetCapture()==window->hwnd)ReleaseCapture();}window->SyncMediaTitles();window->DrawCards(true,L"system.media");window->UpdateCardClock();}return;
    }
    if(Str(p,L"type")==L"card-notice"){if(shell&&!shell->bars.empty()){shell->Info(shell->bars.begin()->second->data,Str(p,L"title"),Str(p,L"text"),L"");if(shell->toast&&shell->toast->hwnd)SetTimer(shell->toast->hwnd,84,Str(p,L"title")==L"天气"?2000:1400,nullptr);}return;}
    if(Str(p,L"type")==L"weather-data"||Str(p,L"type")==L"clipboard-image"){
        if(Str(p,L"type")==L"weather-data")model.weatherData=p.GetNamedObject(L"data");else LoadClipboardTexture(*this,p.GetNamedObject(L"data"));
        if(recovery&&recovery->hwnd)recovery->Draw();
        for(auto const &window:windows)if(window->hwnd){try{window->DrawCards(true,Str(p,L"type")==L"weather-data"?L"system.weather":L"system.clipboard-image");}catch(hresult_error const&error){JsonObject d;Put(d,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));Put(d,L"source",Str(p,L"type"));Telemetry(L"card-update-failed",d);}}return;
    }
    if(Str(p,L"type")==L"encodeck-data"){
        auto state=p.TryLookup(L"data");if(!state||state.ValueType()!=JsonValueType::Object)return;
        model.encodeckData=state.GetObject();
        if(recovery&&recovery->hwnd)recovery->Draw();
        for(auto const &w:windows)if(w->hwnd&&IsWindowVisible(w->hwnd)){try{w->DrawCards(true,L"system.encodeck");}catch(hresult_error const&error){JsonObject d;Put(d,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));Put(d,L"source",Str(p,L"type"));Telemetry(L"card-update-failed",d);}}
        return;
    }
    if(Str(p,L"type")==L"card-data"){
        // The packet owns one immutable current resource snapshot.
        model.cardData=p.GetNamedObject(L"data");
        if(recovery&&recovery->hwnd)recovery->Draw();
        for(auto const &w:windows)if(w->hwnd){try{w->DrawCards(true);}catch(hresult_error const&error){JsonObject d;Put(d,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));Telemetry(L"card-update-failed",d);}}
        return;
    }
    auto snapshot = p.TryLookup(L"snapshot");
    if (snapshot && snapshot.ValueType() == JsonValueType::Object && model.AcceptsSnapshot(snapshot.GetObject()))
    {
        auto previousHonor=model.honorData?std::wstring(model.honorData.Stringify()):L"";auto previousHandoff=std::wstring(model.handoffData.Stringify());
        auto previousScheme=HonorScheme(model.preferences);bool previousRecycleFull=model.recycleFull,previousHonorSignedIn=model.honorSignedIn;auto previousIconIdentity=model.iconIdentityVersion;
        auto previousPreferences=model.preferences?std::wstring(model.preferences.Stringify()):L"";
        auto previousWorkspace=model.workspaceVersion,previousCatalog=model.cardCatalogVersion;int previousRecovery=model.recoveryCount;
        model.Read(snapshot.GetObject());
        if(previousRecovery!=model.recoveryCount&&shell&&shell->authoritative)shell->Update(shell->authoritative);
        if(model.recoveryCount==0&&recovery)recovery->Close();
        if(previousScheme!=HonorScheme(model.preferences)){honor.Scheme(HonorScheme(model.preferences));if(shell)for(auto const &[id,bar]:shell->bars){bar->toolbarSvg.documents.clear();for(auto &[key,v]:bar->toolbarVisuals)v.rasterKey.clear();}}
        if(previousPreferences!=(model.preferences?std::wstring(model.preferences.Stringify()):L"")){
            RefreshRenderPolicy();
        }
        if(previousCatalog!=model.cardCatalogVersion||previousIconIdentity!=model.iconIdentityVersion||previousHonorSignedIn!=model.honorSignedIn)for(auto const &window:windows){window->desktopStamp.valid=false;window->panelStamp.valid=false;}
        if(previousScheme!=HonorScheme(model.preferences)&&popup&&popup->visible)popup->Draw();
        bool wasStarted=started;
        StartWindows();
        if(started&&pendingShellData){auto pending=pendingShellData;pendingShellData=nullptr;Packet(to_string(pending.Stringify()));}
        if(wasStarted&&model.desktopRuntime&&previousWorkspace!=model.workspaceVersion){
            for(auto const &board:model.boards){
                auto found=std::find_if(windows.begin(),windows.end(),[&](auto const &window){return window->hwnd&&window->boardId==board.id;});
                if(found==windows.end()){windows.push_back(std::make_unique<Window>(*this,board.id));windows.back()->Create(0);}
                else{auto &window=**found;SetWindowPos(window.hwnd,nullptr,board.x,board.y,board.width,board.height,SWP_NOZORDER|SWP_NOACTIVATE);window.Register();}
            }
            for(auto const &window:windows)if(window->hwnd&&!model.Find(window->boardId))DestroyWindow(window->hwnd);
        }
        for (auto const &w : windows){
            w->Apply();
            if(previousHonorSignedIn!=model.honorSignedIn){w->DrawCards(true,L"system.storage");w->DrawCards(true,L"system.honor-service");}
            if(previousHonor!=(model.honorData?std::wstring(model.honorData.Stringify()):L""))w->DrawCards(true,L"system.honor-service");
            if(previousHandoff!=std::wstring(model.handoffData.Stringify()))w->DrawCards(true,L"system.handoff");
        }
        TrimNativeHandoffImages(*this);
        if(!model.honorData||!model.honorData.GetNamedArray(L"items",JsonArray{}).Size()){honorServiceIcons.clear();honorServicePng.clear();}
        for(auto const &window:windows){std::erase_if(window->honorServicePages,[&](auto const &v){return !window->Find(v.first);});std::erase_if(window->honorServiceFocus,[&](auto const &v){return !window->Find(v.first);});std::erase_if(window->todoScrolls,[&](auto const &v){return !window->Find(v.first);});std::erase_if(window->todoSelectedDays,[&](auto const &v){return !window->Find(v.first);});std::erase_if(window->weatherHourPages,[&](auto const &v){return !window->Find(v.first);});}
        if(shell)for(auto const &[id,bar]:shell->bars)if(!bar->dock||previousScheme!=HonorScheme(model.preferences)||previousRecycleFull!=model.recycleFull){bar->Layout();bar->Draw();}
    }
    if (p.HasKey(L"ok") && !Bool(p, L"ok") && !Bool(p,L"background"))
    {
        auto error = Str(p, L"error");
        HWND owner=nullptr;
        for (auto const &w : windows)
            if (w->hwnd)
            {
                w->status = error;
                w->Draw();
                if(w->renameHost)owner=w->renameHost;else if(!owner)owner=w->hwnd;
            }
        // Errors must not enter a modal nested message loop on the render/IPC
        // thread. A stale Shell menu can fail while other panels are active.
        if(model.desktopRuntime&&owner&&shell&&shell->authoritative)
            shell->Info(shell->authoritative,L"桌面操作",error,L"");
    }
    long long id = static_cast<long long>(Num(p, L"id"));
    auto callback = callbacks.find(id);
    if (callback != callbacks.end())
    {
        auto action = std::move(callback->second);
        callbacks.erase(callback);
        action(p);
    }
}
void App::BenchmarkTick()
{
    if(popupCycles){PopupBenchmarkTick();return;}
    if (windows.empty() || !windows[0]->hwnd || !windows[0]->benchmark)
    {
        KillTimer(relay, 71);
        return;
    }
    auto &w = *windows[0];
    if (!tick)
    {
        SetTimer(relay, 71, 450, nullptr);
        w.Snapshot(L"benchmark-cycles");
    }
    if (tick < cycles * 2)
    {
        if (tick % 2 == 0)
        {
            auto b = model.Find(w.boardId);
            auto folder = std::find_if(b->tiles.begin(), b->tiles.end(),
                                       [](auto const &t) { return t.items.size() == 240; });
            if (folder != b->tiles.end())
                w.OpenFolder(folder->key);
        }
        else
            w.CloseFolder();
        tick++;
        return;
    }
    if (tick == cycles * 2)
    {
        w.Snapshot(L"benchmark-idle-after");
        tick++;
        SetTimer(relay, 71, model.components?30000:5000, nullptr);
        return;
    }
    KillTimer(relay, 71);
    w.benchmark = false;
    w.Snapshot(L"benchmark-complete");
    SetTimer(relay, 72, 500, nullptr);
}
void App::PopupSnapshot(std::wstring const &phase)
{
    auto value=graphics.Metrics();Put(value,L"shows",popup?static_cast<double>(popup->shows):0);Put(value,L"hides",popup?static_cast<double>(popup->hides):0);Put(value,L"popupRedraws",popup?static_cast<double>(popup->redraws):0);Put(value,L"popupSurfaceBytes",popup?static_cast<double>(popup->surfaceBytes):0);Put(value,L"textFormatCount",static_cast<double>(graphics.formats.size()));Put(value,L"shellBars",shell?static_cast<double>(shell->bars.size()):0);Put(value,L"nativeEdits",popup?static_cast<double>(popup->edits.size()):0);Telemetry(phase,value);
}
void App::PopupBenchmarkTick()
{
    if(windows.empty()||!windows[0]->hwnd||!windows[0]->benchmark){KillTimer(relay,71);if(popup)popup->Hide(false);return;}
    if(tick==0){SetTimer(relay,71,700,nullptr);PopupSnapshot(L"benchmark-popup-cycles");}
    if(tick<popupCycles*2){
        if(tick%2==0){
            if(!popup)popup=std::make_unique<NativePopup>(*this);JsonObject packet,payload,position,data,account;
            std::wstring kinds[]={L"@magic/calendar-popup",L"@magic/keyboard-selector",L"@magic/user-menu",L"@magic/quick-settings",L"@magic/media-popup",L"@magic/network-popup",L"@magic/bluetooth-popup",L"@magic/notifications",L"@magic/system-tray",L"@magic/apps-menu",L"@magic/task-switcher",L"@magic/power-menu"};Put(packet,L"kind",kinds[(tick/2)%12]);Put(packet,L"token",L"fixture-"+std::to_wstring(tick));
            RECT window{};GetWindowRect(windows[0]->hwnd,&window);Put(position,L"x",static_cast<double>(window.right-60));Put(position,L"y",static_cast<double>(window.top+60));payload.Insert(L"desiredPosition",position);Put(payload,L"alignX",std::wstring(L"End"));Put(payload,L"alignY",std::wstring(L"Start"));packet.Insert(L"payload",payload);
            Put(data,L"language",std::wstring(L"zh-CN"));Put(data,L"startOfWeek",std::wstring(L"Monday"));data.Insert(L"preview",JsonValue::CreateBooleanValue(true));JsonArray layouts;
            for(int i=0;i<2;i++){JsonObject row;Put(row,L"language",std::wstring(i?L"英语（美国）":L"简体中文（中国大陆）"));Put(row,L"name",std::wstring(i?L"美式键盘":L"微软拼音"));Put(row,L"id",i?std::wstring(L"00000409"):std::wstring(L"00000804"));Put(row,L"handle",i?std::wstring(L"0409:00000409"):std::wstring(L"0804:00000804"));row.Insert(L"active",JsonValue::CreateBooleanValue(i==0));layouts.Append(row);}data.Insert(L"layouts",layouts);if((tick/2)%12>=3){JsonArray entries;for(int i=0;i<14;i++){JsonObject row;Put(row,L"id",L"fixture-row-"+std::to_wstring(i));Put(row,L"label",L"原生控件 "+std::to_wstring(i+1));Put(row,L"detail",std::wstring(L"隔离样本，不执行系统操作"));Put(row,L"control",std::wstring(i==0?L"range":i==1?L"text":L"button"));Put(row,L"value",0.6);row.Insert(L"enabled",JsonValue::CreateBooleanValue(i!=1));entries.Append(row);}data.Insert(L"rows",entries);Put(data,L"title",kinds[(tick/2)%12]);data.Insert(L"taskArgs",JsonObject{});}packet.Insert(L"data",data);
            Put(account,L"version",model.frontendVersion);Put(account,L"name",std::wstring(L"隔离样本"));account.Insert(L"accountAvailable",JsonValue::CreateBooleanValue(false));packet.Insert(L"account",account);popup->Show(packet);
        }else if(popup)popup->Hide();
        ++tick;return;
    }
    if(tick==popupCycles*2){++tick;PopupSnapshot(L"benchmark-popup-idle-after");SetTimer(relay,71,30000,nullptr);return;}
    KillTimer(relay,71);windows[0]->benchmark=false;PopupSnapshot(L"benchmark-popup-complete");SetTimer(relay,72,500,nullptr);
}
void App::Shutdown()
{
    honor.Clear();handoffImages.clear();handoffImageKeys.clear();honorServiceIcons.clear();honorServicePng.clear();
    if(stopping)return;
    stopping=true;
    recovery.reset();
    for(auto token:powerNotifications)UnregisterPowerSettingNotification(token);powerNotifications.clear();
    // Stop producers first. No packet/callback may re-enter a partly destroyed
    // popup, bar or desktop window during synchronous User32 destruction.
    pipe.Stop();callbacks.clear();
    menus.reset();ClearRetiringPopups();if(popup)popup->Hide(false);popup.reset();shell.reset();
    KillTimer(relay, 71);
    KillTimer(relay, 72);
    for (auto const &w : windows)
        if (w->hwnd)
            DestroyWindow(w->hwnd);
    windows.clear();clipboardBitmap=nullptr;pendingShellData=nullptr;
}
LRESULT CALLBACK App::RelayProc(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    auto self = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(hwnd, m, w, l);
    try
    {
        if (m == PacketMessage)
        {
            if (w)
            {
                self->shutdownRequested=true;self->QueueRenderWork();
                return 0;
            }
            std::unique_ptr<std::string> bytes(reinterpret_cast<std::string *>(l));
            if(NativeRenderScope::busy||!self->deferredRenderPackets.empty()){
                if(self->deferredRenderPackets.size()>=128||self->deferredRenderBytes+bytes->size()>8*1024*1024)throw std::runtime_error("Deferred render packet bound");
                self->deferredRenderBytes+=bytes->size();self->deferredRenderPackets.push_back(std::move(bytes));
                if(NativeRenderScope::busy)NativeRenderScope::needsDrain=true;else PostMessageW(hwnd,DeferredRenderPackets,0,0);
                return 0;
            }
            self->Packet(*bytes);
            return 0;
        }
        if(m==DeferredRenderPackets){
            if(NativeRenderScope::busy){NativeRenderScope::needsDrain=true;return 0;}
            if(self->shutdownRequested){self->Shutdown();PostQuitMessage(1);return 0;}
            if(self->stopping)return 0;
            if(self->deviceRecoveryPending)self->Recover();
            if(self->recovery&&self->recovery->closePending)self->recovery->FlushClose();
            if(self->shell){
                std::vector<std::wstring> dragBars;
                for(auto const &[key,bar]:self->shell->bars){
                    if(bar->dpiRefreshPending)bar->RefreshDpi();
                    if(!bar->deferredDragInput.empty())dragBars.push_back(key);
                }
                for(auto const &key:dragBars){
                    auto bar=self->shell->bars.find(key);
                    if(bar!=self->shell->bars.end())bar->second->FlushDragInput();
                }
                if(self->shell->geometryDeferred)self->shell->RefreshGeometry();
            }
            for(auto const &window:self->windows)if(window->displayChangePending)window->RefreshDisplay();
            if(!self->deferredRenderPackets.empty()){auto bytes=std::move(self->deferredRenderPackets.front());self->deferredRenderPackets.pop_front();self->deferredRenderBytes-=bytes->size();self->Packet(*bytes);if(!self->deferredRenderPackets.empty())PostMessageW(hwnd,DeferredRenderPackets,0,0);}
            return 0;
        }
        if(m==WM_POWERBROADCAST){
            if(w==PBT_POWERSETTINGCHANGE&&l){if(NativePowerPolicy::Accept(self->model,*reinterpret_cast<POWERBROADCAST_SETTING const*>(l)))self->RefreshRenderPolicy();}
            else if(w==PBT_APMPOWERSTATUSCHANGE&&!self->model.saverModern){SYSTEM_POWER_STATUS power{};if(GetSystemPowerStatus(&power)){self->model.saverKnown=true;self->model.saverActive=power.SystemStatusFlag==1;self->RefreshRenderPolicy();}}
            return TRUE;
        }
        if(m==NativeClockPreferenceMessage){if(self->shell)self->shell->ClockPreferenceChanged();return 0;}
        if(m==WM_APP+22){SetTimer(hwnd,76,32,nullptr);return 0;}
        if(m==WM_TIMER&&w==76){KillTimer(hwnd,76);if(self->shell)self->shell->RefreshGeometry();return 0;}
        if(m==WM_TIMER&&w==75){KillTimer(hwnd,75);for(auto const &window:self->windows)if(window->folderId.empty()&&!window->closing&&window->panelSurface){window->panel.Brush(nullptr);window->panel.Shadow(nullptr);window->panelSurface=nullptr;window->panelStamp.valid=false;window->estimatedSurfaceBytes=static_cast<size_t>(window->rootPixelWidth)*window->rootPixelHeight*4;}self->graphics.Trim();return 0;}
        if(m==WM_TIMER&&w==RetiringPopupTimer){self->ClearRetiringPopups();return 0;}
        if(m==WM_TIMER&&w==74){KillTimer(hwnd,74);if(!self->popup||!self->popup->visible)self->graphics.Trim();return 0;}
        if (m == WM_TIMER && w == 71)
        {
            self->BenchmarkTick();
            return 0;
        }
        if (m == WM_TIMER && w == 72)
        {
            self->Shutdown();
            return 0;
        }
    }
    catch (hresult_error const &error)
    {
        try
        {
            JsonObject d;
            Put(d, L"error", std::wstring(error.message()));Put(d,L"hresult",static_cast<double>(static_cast<int32_t>(error.code())));Put(d,L"packetKind",self->lastPacketKind);Put(d,L"rendererPid",static_cast<double>(GetCurrentProcessId()));
            self->Telemetry(L"fatal", d);
        }
        catch (...)
        {
        }
        self->Shutdown();
        PostQuitMessage(2);
    }
    catch (std::exception const &error)
    {
        try
        {
            JsonObject detail;
            Put(detail, L"error", std::wstring(to_hstring(error.what())));
            self->Telemetry(L"fatal-std", detail);
        }
        catch (...)
        {
        }
        self->Shutdown();
        PostQuitMessage(2);
    }
    return DefWindowProcW(hwnd, m, w, l);
}
// Pure geometry checks: no windows, file transactions, devices, or simulated input.
int CheckFolderGeometry()
{
    int checked = 0, failed = 0;
    auto check = [&](bool pass) {
        checked++;
        if (!pass)
            failed++;
    };
    D2D1_RECT_F panel{100, 100, 660, 918}, content{120, 164, 640, 898};
    for(int columns:{2,4})for(int rows:{2,3}){
        NativeFolderCover::Layout<D2D1_RECT_F> cover({0,0,400,300},columns,rows);
        for(int index=0;index<cover.MainCount(50);++index){auto r=cover.Cell(index);check(cover.HitMain((r.left+r.right)/2,(r.top+r.bottom)/2,50)==index);}
        auto corner=cover.Corner();check(cover.HitMain((corner.left+corner.right)/2,(corner.top+corner.bottom)/2,50)==-1);
        auto caption=cover.Caption();check(cover.HitMain((caption.left+caption.right)/2,(caption.top+caption.bottom)/2,50)==-1);
        auto first=cover.Cell(0);check(cover.HitMain((first.left+first.right)/2,(first.top+first.bottom)/2,0)==-1);
    }
    check(DropScope(true, panel, 100, 100) == FolderDropScope::Folder);
    check(DropScope(true, panel, 659.9f, 917.9f) == FolderDropScope::Folder);
    check(DropScope(true, panel, 660, 500) == FolderDropScope::Desktop);
    check(DropScope(true, panel, 99.9f, 500) == FolderDropScope::Desktop);
    check(DropScope(true, panel, 500, 99.9f) == FolderDropScope::Desktop);
    check(DropScope(true, panel, 500, 918) == FolderDropScope::Desktop);
    check(DropScope(false, panel, 300, 300) == FolderDropScope::Desktop);
    check(DropScope(true, panel, 110, 110) == FolderDropScope::Folder); // header is not outside
    check(FolderHitIndex(content, 5, 0, 240, 130, 170) == 0);
    check(FolderHitIndex(content, 5, 0, 240, 220, 170) == -1); // column gap
    check(FolderHitIndex(content, 5, 0, 240, 130, content.top+FolderRowStep(content,5)-7) == -1); // row gap
    check(FolderHitIndex(content, 5, 0, 240, 130, 150) == -1); // header
    auto cell = FolderCellBounds(content, 5, 44, 239);
    check(FolderHitIndex(content, 5, 44, 240, (cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2) ==
          239);
    check(FolderVisibleRange(240, 5, 44, 734,FolderRowStep(content,5)) == std::make_pair(220, 240));
    check(FolderVisibleRange(0, 5, 0, 734,FolderRowStep(content,5)) == std::make_pair(0, 0));
    check(FolderVisibleRange(24000, 5, 10, 734,FolderRowStep(content,5)) == std::make_pair(50, 85)); // independent of total count
    check(FolderHitIndex(content, 5, 0, 0, 130, 170) == -1);
    SurfaceStamp stamp; std::set<std::wstring> selected{L"a"};
    check(!stamp.Matches(1,1,0,L"folder",L"a",L"",selected));
    stamp.Capture(1,1,0,L"folder",L"a",L"",selected);
    check(stamp.Matches(1,1,0,L"folder",L"a",L"",selected));
    check(!stamp.Matches(2,1,0,L"folder",L"a",L"",selected));
    check(!stamp.Matches(1,2,0,L"folder",L"a",L"",selected));
    check(!stamp.Matches(1,1,1,L"folder",L"a",L"",selected));
    check(!stamp.Matches(1,1,0,L"other",L"a",L"",selected));
    check(!stamp.Matches(1,1,0,L"folder",L"b",L"",selected));
    check(!stamp.Matches(1,1,0,L"folder",L"a",L"error",selected));
    check(!stamp.Matches(1,1,0,L"folder",L"a",L"",{}));
    stamp.valid=false;
    check(!stamp.Matches(1,1,0,L"folder",L"a",L"",selected));
    for (float dpi : {96.0f, 144.0f, 192.0f})
        check(DropScope(true, panel, 660 * dpi / 96 * 96 / dpi, 500) == FolderDropScope::Desktop);
    for(float scale:{1.f,1.5f,2.f})for(float grab:{0.f,45.25f,184.f})for(int distance:{100,300}){
        auto first=DragImageOrigin(900,grab,scale),last=DragImageOrigin(900+static_cast<long>(distance*scale),grab,scale);
        check(std::abs((last-first)/scale-distance)<=1.f);check(std::abs(first+grab*scale-900)<=.5f);
    }
    std::printf("{\"allPass\":%s,\"checks\":%d,\"failed\":%d}\n", failed ? "false" : "true", checked, failed);
    return failed ? 1 : 0;
}
#include "native-appearance-gallery.h"
#include "../../tools/review/native-grid-settings-checks.h"
#include "../../tools/review/native-flyout-checks.h"
#include "../../tools/review/native-nov04-feature-checks.h"
#include "../../tools/review/native-v040-acceptance-checks.h"
#include "../../tools/review/native-toolbar-extension-checks.h"
#include "../../tools/review/native-nov04-resource-checks.h"
#include "../../tools/review/native-empty-desktop-checks.h"
#include "../../tools/review/native-contour-motion-checks.h"
#include "../../tools/review/native-folder-return-checks.h"
#include "../../tools/review/native-clock-notify-check.h"
#include "../../tools/review/native-nov07-toolbar-checks.h"
#include "../../tools/review/native-music-names-checks.h"
#include "../../tools/review/native-nov08-feature-checks.h"
#include "../../tools/review/native-nov09-details-checks.h"
#include "../../tools/review/native-nov08-bug-checks.h"
#include "../../tools/review/native-music-sources-checks.h"
#include "../../tools/review/native-settings-ui-checks.h"
#include "../../tools/review/native-ole-feedback-checks.h"
#include "../../tools/review/native-context-motion-checks.h"
#include "../../tools/review/native-desktop-more-checks.h"
#include "../../tools/review/native-dock-menu-checks.h"
#include "../../tools/review/native-card-dependency-checks.h"
#include "../../tools/review/native-grid-motion-checks.h"
#include "../../tools/review/native-center-panel-checks.h"
#include "../../tools/review/native-topbar-compact-checks.h"
#include "../../tools/review/native-preview17-ui-checks.h"
#include "../../tools/review/native-preview18-feature-checks.h"
#include "../../tools/review/native-display-recovery-checks.h"
#include "../../tools/review/native-display-lifecycle-checks.h"
#include "../../tools/review/native-ui-batch-checks.h"
#include "../../tools/review/native-dock-followup-checks.h"
#include "../../tools/review/native-oct29-followup-checks.h"
#include "../../tools/review/native-nov10-ui-checks.h"
#include "../../tools/review/native-close-fixture-checks.h"
#include "../../tools/review/native-tray-motion-checks.h"
#include "native-transition.h"
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};InitCommonControlsEx(&controls);
    HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized))
        return 3;
    // Locale JSON objects hold Windows.Web COM implementations. Empty these
    // static maps while OLE is live; CRT onexit runs after OleUninitialize and
    // otherwise releases vtables from the already unloaded Windows.Web.dll.
    struct ApartmentScope {~ApartmentScope(){NativeShellLocaleCache().clear();NativeClockLocaleCache().clear();winrt::clear_factory_cache();OleUninitialize();}} apartmentScope;
    int result = 0;
    Preview70Theme::Refresh();
    try
    {
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if(count==2&&std::wstring(args[1])==L"--nov10-close-fixture"){LocalFree(args);return Nov10CloseFixture::Target();}
        if(count==3&&std::wstring(args[1])==L"--nov10-close-checks"){std::wstring output=args[2];LocalFree(args);try{return Nov10CloseFixture::Check(output);}catch(std::exception const &e){std::ofstream file(std::filesystem::path(output)/L"error.txt");file<<e.what();throw;}catch(hresult_error const &e){std::ofstream file(std::filesystem::path(output)/L"error.txt");file<<std::hex<<static_cast<unsigned>(e.code())<<" "<<to_string(e.message());throw;}}
        if(count==3&&std::wstring(args[1])==L"--nov10-ui-checks"){std::wstring output=args[2];LocalFree(args);try{return CheckNov10Ui(output);}catch(hresult_error const &e){std::ofstream file(std::filesystem::path(output)/L"error.txt");file<<std::hex<<static_cast<unsigned>(e.code())<<" "<<to_string(e.message());throw;}}
        if (count == 2 && std::wstring(args[1]) == L"--check-geometry")
        {
            LocalFree(args);
            int checked = CheckFolderGeometry();

            return checked;
        }
        if(count==3&&std::wstring(args[1])==L"--oct29-followup-checks"){std::wstring output=args[2];LocalFree(args);return CheckOct29Followup(output);}
        if(count==3&&std::wstring(args[1])==L"--display-recovery-checks"){std::wstring output=args[2];LocalFree(args);return CheckDisplayRecovery(output);}
        if(count==3&&std::wstring(args[1])==L"--display-lifecycle-checks"){std::wstring output=args[2];LocalFree(args);
            try{return CheckDisplayLifecycle(output);}catch(hresult_error const &error){std::ofstream log{std::filesystem::path(output+L"/failure.txt")};log<<std::hex<<static_cast<uint32_t>(error.code())<<" "<<to_string(error.message());throw;}catch(std::exception const &error){std::ofstream log{std::filesystem::path(output+L"/failure.txt")};log<<error.what();throw;}}
        if(count==3&&std::wstring(args[1])==L"--ui-batch-checks"){std::wstring output=args[2];LocalFree(args);return CheckUiBatch(output);}
        if(count==3&&std::wstring(args[1])==L"--dock-followup-checks"){std::wstring output=args[2];LocalFree(args);return CheckDockFollowup(output);}
        if(count==3&&std::wstring(args[1])==L"--preview18-feature-checks"){std::wstring output=args[2];LocalFree(args);return CheckPreview18Features(output);}
        if(count==3&&std::wstring(args[1])==L"--context-motion-checks"){std::wstring output=args[2];LocalFree(args);return CheckContextMotion(output);}
        if(count==3&&std::wstring(args[1])==L"--apps-input-checks"){std::wstring output=args[2];LocalFree(args);return CheckAppsInputFixture(output);}
        if(count==4&&std::wstring(args[1])==L"--desktop-more-checks"){std::wstring replies=args[2],output=args[3];LocalFree(args);return CheckDesktopMore(replies,output);}
        if(count==3&&std::wstring(args[1])==L"--dock-menu-checks"){std::wstring output=args[2];LocalFree(args);return CheckDockMenus(output);}
        if(count==4&&std::wstring(args[1])==L"--card-dependency-checks"){std::wstring catalog=args[2],output=args[3];LocalFree(args);
            try{return CheckCardDependencies(catalog,output);}catch(winrt::hresult_error const &e){std::ofstream log{std::filesystem::path(output+L"/failure.txt")};log<<winrt::to_string(e.message());throw;}catch(std::exception const &e){std::ofstream log{std::filesystem::path(output+L"/failure.txt")};log<<e.what();throw;}}
        if(count==3&&std::wstring(args[1])==L"--contour-motion-checks"){std::wstring output=args[2];LocalFree(args);return CheckContourMotion(output);}
        if(count==3&&std::wstring(args[1])==L"--folder-return-checks"){std::wstring output=args[2];LocalFree(args);return CheckFolderDragReturn(output);}
        if(count==3&&std::wstring(args[1])==L"--nov04-resource-platform-cache"){std::wstring output=args[2];LocalFree(args);return CheckNov04Resources(output,true);}
        if(count==3&&std::wstring(args[1])==L"--nov04-resource-checks"){std::wstring output=args[2];LocalFree(args);return CheckNov04Resources(output);}
        if(count==3&&std::wstring(args[1])==L"--empty-desktop-checks"){std::wstring output=args[2];LocalFree(args);return CheckEmptyDesktopResources(output,false);}
        if(count==3&&std::wstring(args[1])==L"--empty-desktop-baseline"){std::wstring output=args[2];LocalFree(args);return CheckEmptyDesktopResources(output,true);}
        if(count==3&&std::wstring(args[1])==L"--toolbar-extension-checks"){std::wstring output=args[2];LocalFree(args);return CheckToolbarExtensions(output);}
        if(count==3&&std::wstring(args[1])==L"--v040-acceptance-checks"){std::wstring output=args[2];LocalFree(args);return CheckV040Acceptance(output);}
        if(count==3&&std::wstring(args[1])==L"--nov04-feature-checks"){std::wstring output=args[2];LocalFree(args);return CheckNov04Features(output);}
        if(count==3&&std::wstring(args[1])==L"--clock-notify-check"){std::wstring output=args[2];LocalFree(args);return CheckNativeClockNotify(output);}
        if(count==3&&std::wstring(args[1])==L"--tray-motion-checks"){std::wstring output=args[2];LocalFree(args);try{return CheckTrayMotion(output);}catch(winrt::hresult_error const&e){std::ofstream f(std::filesystem::path(output)/L"error.txt");f<<std::hex<<e.code().value<<" "<<to_string(e.message());return 1;}catch(std::exception const&e){std::ofstream f(std::filesystem::path(output)/L"error.txt");f<<e.what();return 1;}}
        if(count==3&&std::wstring(args[1])==L"--settings-ui-checks"){std::wstring output=args[2];LocalFree(args);try{return CheckSettingsUi(output);}catch(winrt::hresult_error const&e){std::ofstream f(std::filesystem::path(output)/L"error.txt");f<<std::hex<<e.code().value<<" "<<to_string(e.message());return 1;}catch(std::exception const&e){std::ofstream f(std::filesystem::path(output)/L"error.txt");f<<e.what();return 1;}}
        if(count==3&&(std::wstring(args[1])==L"--music-sources-checks"||std::wstring(args[1])==L"--toolbar-contrast-sampling")){std::wstring command=args[1],output=args[2];LocalFree(args);
            try{return command==L"--music-sources-checks"?CheckMusicSources(output):CheckToolbarContrastSampling(output);}catch(winrt::hresult_error const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<std::hex<<static_cast<unsigned long>(e.code().value)<<" "<<to_string(e.message());return 1;}
            catch(std::exception const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<e.what();return 1;}}
        if(count==3&&std::wstring(args[1])==L"--music-names-checks"){std::wstring output=args[2];LocalFree(args);
            try{return CheckMusicNames(output);}catch(winrt::hresult_error const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<std::hex<<static_cast<unsigned long>(e.code().value)<<" "<<to_string(e.message());throw;}
            catch(std::exception const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<e.what();throw;}}
        if(count==3&&std::wstring(args[1])==L"--nov09-details-checks"){std::wstring output=args[2];LocalFree(args);
            try{return CheckNov09Details(output);}catch(winrt::hresult_error const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<std::hex<<static_cast<unsigned long>(e.code().value)<<" "<<to_string(e.message());return 2;}
            catch(std::exception const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<e.what();return 2;}}
        if(count==3&&std::wstring(args[1])==L"--nov08-feature-checks"){std::wstring output=args[2];LocalFree(args);return CheckNov08Features(output);}
        if(count==3&&std::wstring(args[1])==L"--nov08-bug-checks"){std::wstring output=args[2];LocalFree(args);
            try{return CheckNov08Bugs(output);}catch(winrt::hresult_error const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<std::hex<<static_cast<unsigned long>(e.code().value)<<" "<<to_string(e.message());throw;}
            catch(std::exception const &e){std::ofstream file(std::filesystem::path(output)/L"native-error.txt");file<<e.what();throw;}}
        if(count==3&&std::wstring(args[1])==L"--nov07-toolbar-checks"){std::wstring output=args[2];LocalFree(args);return CheckNov07Toolbar(output);}
        if(count==3&&std::wstring(args[1])==L"--ole-feedback-checks"){std::wstring output=args[2];LocalFree(args);return CheckOleReleaseFeedback(output);}
        if(count==3&&std::wstring(args[1])==L"--flyout-checks"){std::wstring output=args[2];LocalFree(args);return CheckFlyouts(output);}
        if(count==3&&std::wstring(args[1])==L"--grid-motion-checks"){std::wstring output=args[2];LocalFree(args);return CheckGridMotion(output);}
        if(count==3&&std::wstring(args[1])==L"--center-panel-checks"){std::wstring output=args[2];LocalFree(args);return CheckCenterPanels(output);}
        if(count==3&&std::wstring(args[1])==L"--topbar-compact-checks"){std::wstring output=args[2];LocalFree(args);return CheckTopbarCompact(output);}
        if(count==3&&std::wstring(args[1])==L"--preview17-ui-checks"){std::wstring output=args[2];LocalFree(args);return CheckPreview17Ui(output);}
        if(count==3&&std::wstring(args[1])==L"--grid-settings-checks"){std::wstring output=args[2];LocalFree(args);return CheckGridSettings(output);}
        if(count==4&&(std::wstring(args[1])==L"--appearance-gallery"||std::wstring(args[1])==L"--appearance-gallery-checks"||std::wstring(args[1])==L"--desktop-selection-checks"||std::wstring(args[1])==L"--ui-correction-checks"||std::wstring(args[1])==L"--honor-icon-checks")){
            bool honorOnly=std::wstring(args[1])==L"--honor-icon-checks";bool correctionsOnly=std::wstring(args[1])==L"--ui-correction-checks";bool selectionOnly=std::wstring(args[1])==L"--desktop-selection-checks";bool checks=honorOnly||correctionsOnly||selectionOnly||std::wstring(args[1])==L"--appearance-gallery-checks";std::wstring fixture=args[2],output=args[3];LocalFree(args);
            try{return ExportAppearanceGallery(fixture,output,checks,selectionOnly,correctionsOnly,honorOnly);}
            catch(winrt::hresult_error const &error){std::fprintf(stderr,"Gallery HRESULT %08lx: %s\n",static_cast<unsigned long>(error.code().value),winrt::to_string(error.message()).c_str());throw;}
            catch(std::exception const &error){std::fprintf(stderr,"Gallery: %s\n",error.what());throw;}
        }
        if(count==8&&std::wstring(args[1])==L"--desktop-transition"){
            bool entrance=std::wstring(args[2])==L"entrance";std::wstring marker=args[3],nonce=args[4],media=args[5];DWORD owner=wcstoul(args[6],nullptr,10);bool reduce=std::wstring(args[7])!=L"0";LocalFree(args);int transitionResult=0;{NativeTransition transition;transitionResult=transition.Run(entrance,marker,nonce,media,owner,reduce);}return transitionResult;
        }
        std::wstring pipe;
        DWORD parent = 0;
        long long birth = 0;
        App app;
        for (int i = 1; i < count; i++)
        {
            std::wstring arg = args[i];
            if (arg == L"--all-displays")
                app.allDisplays = true;
            else if (i + 1 < count)
            {
                auto value = args[++i];
                if (arg == L"--pipe")
                    pipe = value;
                else if (arg == L"--parent")
                    parent = wcstoul(value, nullptr, 10);
                else if (arg == L"--parent-created")
                    birth = _wcstoi64(value, nullptr, 10);
                else if (arg == L"--cycles")
                    app.cycles = _wtoi(value);
                else if(arg==L"--popup-cycles")app.popupCycles=_wtoi(value);
            }
        }
        LocalFree(args);
        if (pipe.rfind(L"MagicCompositionTrial-", 0) != 0 || !parent || !birth || app.cycles < 0 ||
            app.cycles > 100 || app.popupCycles<0 || app.popupCycles>100 || (app.cycles&&app.popupCycles))
            throw std::runtime_error("Invalid isolated launch");
        app.parentHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
        if (!app.parentHandle)
            throw_last_error();
        FILETIME created{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(app.parentHandle, &created, &exit, &kernel, &user) ||
            static_cast<long long>((static_cast<unsigned long long>(created.dwHighDateTime) << 32) |
                                   created.dwLowDateTime) != birth)
            throw std::runtime_error("Parent identity changed");
        WNDCLASSEXW type{sizeof(type)};
        type.hInstance = instance;
        type.lpfnWndProc = App::RelayProc;
        type.lpszClassName = L"MagicCompositionTrialRelay";
        RegisterClassExW(&type);
        app.relay =
            CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &app);
        if (!app.relay)
            throw_last_error();
        app.SubscribePower();
        type.style = CS_DBLCLKS;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.lpfnWndProc = Window::WndProc;
        type.lpszClassName = L"MagicCompositionTrialWindow";
        RegisterClassExW(&type);
        app.pipe.Start(pipe, parent, app.relay);
        MSG message;
        bool quit = false;
        while (!quit)
        {
            HANDLE handles[2] = {app.parentHandle, app.graphics.budgetEvent};
            DWORD n = app.graphics.budgetEvent ? 2 : 1,
                  wait = MsgWaitForMultipleObjectsEx(n, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            ++app.waitWakes;
            if (wait == WAIT_OBJECT_0)
            {
                app.Shutdown();
                break;
            }
            if (n == 2 && wait == WAIT_OBJECT_0 + 1 && app.graphics.MaintainBudget())
            {
                for (auto const &w : app.windows)
                    if (w->hwnd)
                    {
                        w->ClearIcons();
                        if (w->folderId.empty())
                        {
                            w->panel.Brush(nullptr);
                            w->panel.Shadow(nullptr);
                            w->panelSurface = nullptr;
                        }
                    }
                app.graphics.Trim();
            }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                app.CountMessage(message.message);
                if (message.message == WM_QUIT)
                {
                    result = static_cast<int>(message.wParam);
                    quit = true;
                    break;
                }
                if(app.popup&&app.popup->visible&&!app.popup->edits.empty()&&IsDialogMessageW(app.popup->hwnd,&message))continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        app.Shutdown();
        if(app.relay){MSG pending{};while(PeekMessageW(&pending,app.relay,PacketMessage,PacketMessage,PM_REMOVE))if(!pending.wParam)delete reinterpret_cast<std::string*>(pending.lParam);
            SetWindowLongPtrW(app.relay,GWLP_USERDATA,0);DestroyWindow(app.relay);app.relay=nullptr;}
        if (app.parentHandle)
            CloseHandle(app.parentHandle);
    }
    catch (...)
    {
        result = 3;
    }

    return result;
}
