#pragma once
#include "../../tools/review/native-snapshot-order-checks.h"
#include <filesystem>
#include "../../tools/review/native-folder-cover-checks.h"
#include "../../tools/review/native-preview15-bug-checks.h"
#include "../../tools/review/native-oct28-material-checks.h"
#include "../../tools/review/native-user-confirmation-checks.h"
#include "../../tools/review/native-folder-viewport-checks.h"
#include "../../tools/review/native-apps-input-checks.h"
#include "../../tools/review/native-popup-navigation-checks.h"
#include "../../tools/review/native-dock-reorder-checks.h"
#include "../../tools/review/native-toolbar-release-checks.h"
#include "../../tools/review/native-gallery-variadic-checks.h"
#include "../../tools/review/native-gallery-remaining-checks.h"
// Render the live D2D painters to a bounded bitmap. This developer entry never
// creates HWNDs, connects to the bridge, reads layout/settings or takes a lease.
inline void AppearancePng(Graphics &g,std::wstring const &file,float width,float height,float dpi,
                          std::function<void(ID2D1DeviceContext *)> const &draw)
{
    UINT w=static_cast<UINT>(std::ceil(width*dpi/96)),h=static_cast<UINT>(std::ceil(height*dpi/96));
    if(!w||!h||static_cast<size_t>(w)*h*4>16*1024*1024)throw std::runtime_error("Gallery bitmap bound");
    com_ptr<ID2D1DeviceContext> c;check_hresult(g.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,c.put()));
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
    com_ptr<ID2D1Bitmap1> bitmap;check_hresult(c->CreateBitmap({w,h},nullptr,0,&props,bitmap.put()));c->SetTarget(bitmap.get());c->SetDpi(dpi,dpi);
    c->BeginDraw();c->Clear(D2D1::ColorF(0,0));draw(c.get());check_hresult(c->EndDraw());
    auto cpuProps=props;cpuProps.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    com_ptr<ID2D1Bitmap1> copy;check_hresult(c->CreateBitmap({w,h},nullptr,0,&cpuProps,copy.put()));check_hresult(copy->CopyFromBitmap(nullptr,bitmap.get(),nullptr));
    D2D1_MAPPED_RECT mapped{};check_hresult(copy->Map(D2D1_MAP_OPTIONS_READ,&mapped));std::vector<BYTE> bytes(static_cast<size_t>(w)*h*4);
    for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++){auto a=mapped.bits+y*mapped.pitch+x*4;auto b=bytes.data()+(static_cast<size_t>(y)*w+x)*4;
        b[3]=a[3];for(int k=0;k<3;k++)b[k]=a[3]?static_cast<BYTE>(std::min(255u,static_cast<unsigned>(a[k])*255/a[3])):0;}
    copy->Unmap();com_ptr<IWICStream> stream;check_hresult(g.wic->CreateStream(stream.put()));check_hresult(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE));
    com_ptr<IWICBitmapEncoder> encoder;check_hresult(g.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));check_hresult(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;check_hresult(encoder->CreateNewFrame(frame.put(),nullptr));check_hresult(frame->Initialize(nullptr));check_hresult(frame->SetSize(w,h));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;check_hresult(frame->SetPixelFormat(&format));check_hresult(frame->WritePixels(h,w*4,static_cast<UINT>(bytes.size()),bytes.data()));check_hresult(frame->Commit());check_hresult(encoder->Commit());
}
// Offline image of the product noise/transfer/composite graph, not a HWND capture.
inline void GalleryDissolve(ID2D1DeviceContext *c,ID2D1Image *content,float width,float height,float x,float y,float time){
    auto noise=NativeDissolve::MakeNoise(width,height);com_ptr<ID2D1Bitmap1> bitmap;
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    check_hresult(c->CreateBitmap({noise.width,noise.height},noise.pixels.data(),noise.width*4,&props,bitmap.put()));
    com_ptr<ID2D1Effect> transform,gamma,composite;
    check_hresult(c->CreateEffect(CLSID_D2D12DAffineTransform,transform.put()));check_hresult(D2D1::SetDpiCompensatedEffectInput(c,transform.get(),0,bitmap.get()));
    check_hresult(transform->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,D2D1::Matrix3x2F::Scale(width/noise.width,height/noise.height)*D2D1::Matrix3x2F::Translation(x,y)));
    check_hresult(transform->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_NEAREST_NEIGHBOR));
    check_hresult(c->CreateEffect(CLSID_D2D1GammaTransfer,gamma.put()));gamma->SetInputEffect(0,transform.get());
    for(auto key:{D2D1_GAMMATRANSFER_PROP_RED_DISABLE,D2D1_GAMMATRANSFER_PROP_GREEN_DISABLE,D2D1_GAMMATRANSFER_PROP_BLUE_DISABLE})check_hresult(gamma->SetValue(key,TRUE));
    check_hresult(gamma->SetValue(D2D1_GAMMATRANSFER_PROP_ALPHA_AMPLITUDE,NativeDissolve::Slope));check_hresult(gamma->SetValue(D2D1_GAMMATRANSFER_PROP_ALPHA_OFFSET,NativeDissolve::Offset(time)));check_hresult(gamma->SetValue(D2D1_GAMMATRANSFER_PROP_CLAMP_OUTPUT,TRUE));
    check_hresult(c->CreateEffect(CLSID_D2D1Composite,composite.put()));composite->SetInput(0,content);composite->SetInputEffect(1,gamma.get());
    check_hresult(composite->SetValue(D2D1_COMPOSITE_PROP_MODE,D2D1_COMPOSITE_MODE_DESTINATION_IN));c->DrawImage(composite.get());
}
inline Icon GalleryStockIcon(Graphics &g,int id)
{
    SHSTOCKICONINFO stock{sizeof(stock)};Icon icon;
    if(FAILED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(id),SHGSI_ICON|SHGSI_LARGEICON,&stock)))return icon;
    com_ptr<IWICBitmap> bitmap;HRESULT hr=g.wic->CreateBitmapFromHICON(stock.hIcon,bitmap.put());DestroyIcon(stock.hIcon);if(FAILED(hr))return icon;
    com_ptr<IWICFormatConverter> converter;check_hresult(g.wic->CreateFormatConverter(converter.put()));check_hresult(converter->Initialize(bitmap.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    com_ptr<ID2D1DeviceContext> c;check_hresult(g.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,c.put()));check_hresult(c->CreateBitmapFromWicBitmap(converter.get(),nullptr,icon.bitmap.put()));
    auto size=icon.bitmap->GetPixelSize();icon.bytes=size.width*size.height*4;return icon;
}
inline int CheckDesktopSelection(App &app,std::wstring const &output){
    int count=0;auto expect=[&](bool ok,char const *message){if(!ok){std::fprintf(stderr,"Desktop selection check failed: %s\n",message);throw std::runtime_error(message);}++count;};
    // No HWND, desktop lease, mouse injection or bridge: exercise the same
    // selection/marquee routines and the right-click snapshot on owned data.
    auto previousModel=app.model;
    app.model.Read(JsonObject::Parse(LR"({"revision":1,"components":true,"syntheticIcons":true,"boards":[{"id":"selection-fixture","pageId":"p1","projection":{"originX":0,"originY":0,"stepX":96,"stepY":96},"placements":[{"key":"item:a","x":0,"y":1},{"key":"card:c","x":1,"y":1},{"key":"folder:f","x":2,"y":1}],"tiles":[{"key":"item:a","name":"Owned file"},{"key":"card:c","name":"Owned card","card":{"type":"system.clock"}},{"key":"folder:f","name":"Owned folder","folderId":"f","items":[{"key":"item:child","name":"Owned child"}]}]}]})"));
    Window selection(app,L"selection-fixture");selection.width=600;selection.height=400;selection.dpi=96;selection.root=app.graphics.compositor.CreateContainerVisual();
    selection.Select(L"item:a");expect(selection.selected==std::set<std::wstring>{L"item:a"}&&selection.focusKey==L"item:a","Single selection lost");
    selection.Select(L"card:c",true);expect(selection.selected.size()==2,"Ctrl multi-selection lost");
    selection.Select(L"card:c",true);expect(selection.selected.size()==1&&!selection.selected.contains(L"card:c"),"Ctrl deselection lost");
    selection.marquee=true;selection.pressedPoint={0,64};selection.MarqueePreview(290,190);
    expect(selection.selected.size()==3&&selection.selected.contains(L"item:a")&&selection.selected.contains(L"card:c")&&selection.selected.contains(L"folder:f"),"Marquee must select icons cards and folders");
    NativeDesktopMenu selectedMenu(selection,{});expect(selectedMenu.keys.Size()==3,"Context menu lost the multi-selection snapshot");
    selection.marqueeControl=true;selection.marqueeBase={L"item:a",L"card:c"};selection.MarqueePreview(90,190);
    expect(selection.selected==std::set<std::wstring>{L"card:c"},"Ctrl marquee toggled unrelated targets");
    selection.marquee=false;selection.MarqueePreview();expect(!selection.marqueeVisual,"Marquee visual leaked after release");
    selection.folderId=L"f";selection.columns=2;selection.contentRect={0,64,200,220};selection.Select(L"item:child");
    expect(selection.selected==std::set<std::wstring>{L"item:child"},"Folder child selection lost");
    for(float scale:{1.f,1.5f,2.f}){
        selection.dpi=96*scale;selection.panelRect={200,100,500,350};selection.contentRect={224,164,476,326};
        auto cell=FolderCellBounds(selection.contentRect,2,0,0);
        auto local=FolderLocalViewport(cell,selection.panelRect),viewport=FolderLocalViewport(selection.contentRect,selection.panelRect);
        auto region=NativeRegion(local,viewport);
        expect(region.Valid()&&region.Contains((local.left+local.right)/2,(local.top+local.bottom)/2),"Folder selection clipped in desktop coordinates");
        auto invalidBefore=NativeRegion(local,selection.contentRect);
        expect(!invalidBefore.Valid(),"Offset fixture does not reproduce the old invisible selection");
        selection.marquee=true;selection.marqueeControl=false;selection.pressedPoint={static_cast<LONG>(224*scale),static_cast<LONG>(164*scale)};
        selection.MarqueePreview(cell.right,cell.bottom);
        expect(selection.selected==std::set<std::wstring>{L"item:child"},"Offset folder marquee changed its hit set");
        selection.marquee=false;selection.MarqueePreview();
        expect(selection.selected.contains(L"item:child"),"Release cleared folder selection");
        NativeDesktopMenu folderMenu(selection,{});expect(folderMenu.keys.Size()==1&&folderMenu.keys.GetAt(0).GetString()==L"item:child","Folder action snapshot differs from the visible selection");
        selection.marquee=true;selection.marqueeControl=true;selection.marqueeBase=selection.selected;selection.MarqueePreview(cell.right,cell.bottom);
        expect(selection.selected.empty(),"Ctrl folder marquee cannot deselect");selection.marquee=false;selection.MarqueePreview();
    }
    {
        NativeMenus menus(app);int dispatches=0;menus.style=L"desktop";menus.choose=[&](size_t,std::wstring const &){++dispatches;};
        NativeMenus::Level level;level.popup=std::make_unique<NativePopup>(app);JsonObject row;
        Put(row,L"id",std::wstring(L"owned-submenu"));Put(row,L"control",std::wstring(L"submenu"));level.values.Append(row);
        menus.levels.push_back(std::move(level));menus.visibleLevels=1;
        menus.Hover(0,L"owned-submenu");expect(dispatches==0,"Desktop hover must not open or query a submenu");
        menus.Dispatch(0,L"owned-submenu");menus.Dispatch(0,L"owned-submenu");expect(dispatches==1,"Duplicate click repeated a pending submenu query");
        NativeMenus::Level child;child.popup=std::make_unique<NativePopup>(app);menus.levels.push_back(std::move(child));menus.visibleLevels=2;menus.levels[0].opening=false;
        menus.Dispatch(0,L"owned-submenu");expect(dispatches==1,"Opened submenu was rebuilt on click");
        menus.HideFrom(1);menus.Dispatch(0,L"owned-submenu");expect(dispatches==2,"Closed submenu could not be reopened");
    }
    {
        auto top=NativeTrayMenuAnchor::Position({100,1800,400,1900},{0,64,3072,1800},{200,64},false,true);
        expect(top.x==50&&top.y==64,"Top tray menu retained the native bottom-taskbar anchor");
        auto bottom=NativeTrayMenuAnchor::Position({0,0,300,900},{-1600,-600,0,1800},{-10,1800},true,true);
        expect(bottom.x==-300&&bottom.y==900,"Tray menu escaped a negative-coordinate monitor");
        auto child=NativeTrayMenuAnchor::Position({2900,1700,3200,1900},{0,64,3072,1800},{200,64},false,false);
        expect(child.x==2772&&child.y==1600,"Tray child menu was not clamped independently");
    }
    app.model=std::move(previousModel);
    {
        RECT body{500,1000,1300,1080},screen{0,0,1920,1200};
        expect(NativeDockPointerHold(body,screen,{800,1199},L"Bottom"),"Bottom reveal strip must retain the Dock");
        expect(NativeDockPointerHold(body,screen,{800,1100},L"Bottom"),"Floating margin must remain traversable");
        expect(NativeDockPointerHold(body,screen,{0,1199},L"Bottom"),"Full-width reveal strip must not re-hide immediately");
        expect(NativeDockPointerHold(body,screen,{800,1040},L"Bottom"),"Dock body lost hover retention");
        expect(!NativeDockPointerHold(body,screen,{800,900},L"Bottom"),"Pointer leaving the Dock must permit hide");
        expect(!NativeDockPointerHold(body,screen,{800,1200},L"Bottom"),"Another monitor must not retain the Dock");
        expect(NativeDockPointerHold({-1500,0,-1440,800},{-1600,-100,0,1000},{-1599,500},L"Left"),"Negative-coordinate side Dock lost edge retention");
        auto owner=NativeShell::eventOwner;
        if(owner){auto previous=owner->movingWindow;auto dragged=reinterpret_cast<HWND>(123);
            NativeShell::WindowEvent(nullptr,EVENT_SYSTEM_MOVESIZESTART,dragged,0,0,0,0);
            NativeShell::WindowEvent(nullptr,EVENT_SYSTEM_FOREGROUND,reinterpret_cast<HWND>(456),0,0,0,0);
            expect(owner->movingWindow==dragged,"Snap foreground changes must retain the drag owner");
            NativeShell::WindowEvent(nullptr,EVENT_SYSTEM_MOVESIZEEND,dragged,0,0,0,0);
            expect(!owner->movingWindow,"Completed move must release the toolbar yield");owner->movingWindow=previous;
        }
    }
    {
        auto left=output+L"/layer-renderer-a.fixture",right=output+L"/layer-renderer-b.fixture";
        {std::ofstream a{std::filesystem::path(left),std::ios::binary},b{std::filesystem::path(right),std::ios::binary};a<<"owned renderer copy";b<<"owned renderer copy";}
        expect(NativeRendererFilesMatch(left.c_str(),right.c_str()),"Installed and version copies of the same renderer must share cover protection");
        {std::ofstream b{std::filesystem::path(right),std::ios::binary};b<<"other renderer copy";}
        expect(!NativeRendererFilesMatch(left.c_str(),right.c_str()),"Same-size different renderer bytes must not acquire cover protection");
        expect(!NativeRendererFilesMatch(left.c_str(),(output+L"/missing-renderer.fixture").c_str()),"Missing transition image must not acquire cover protection");
    }
    return count;
}
// Draw a fixed model delta with the same product item painters and Dock spring.
// No user click handler, application launch, or runtime animation clock is used.
inline void GalleryDockMutation(App &app,NativeShell &shell,ID2D1DeviceContext *c,JsonObject const &fixture,bool dark){
    auto afterData=fixture.GetNamedObject(L"data"),beforeData=fixture.GetNamedObject(L"fromData");float oldW=static_cast<float>(Num(fixture,L"fromWidth")),newW=static_cast<float>(Num(fixture,L"width")),h=static_cast<float>(Num(fixture,L"height",64));
    float x=static_cast<float>(Num(fixture,L"canvasX")),y=static_cast<float>(Num(fixture,L"canvasY")),k=NativeInteractionMotion::DockSpring(static_cast<float>(Num(fixture,L"motionTime")));
    NativeBar before(app,shell,L"fixture",true),after(app,shell,L"fixture",true);
    auto prepare=[&](NativeBar &bar,JsonObject const &data,float w){bar.width=w;bar.height=h;bar.dark=dark;bar.materialPreview=true;bar.data=data;bar.settings=data.GetNamedObject(L"dockSettings");bar.padding=8;bar.iconSize=40;bar.gap=8;bar.Layout();int i=0;for(auto const &row:bar.rows)if(!shell.icons.contains(row.key)){auto icon=GalleryStockIcon(app.graphics,i++%2?SIID_APPLICATION:SIID_FOLDER);shell.iconBytes+=icon.bytes;shell.icons[row.key]=std::move(icon);}};
    prepare(before,beforeData,oldW);prepare(after,afterData,newW);float body=oldW+(newW-oldW)*k;
    D2D1_MATRIX_3X2_F original;c->GetTransform(&original);after.width=body;c->SetTransform(D2D1::Matrix3x2F::Translation(x+(newW-body)/2,y)*original);after.PaintPreview70(c);after.width=newW;
    auto paint=[&](NativeBar &bar,int index,float px,float py,float scale,float opacity){if(opacity<=0)return;auto r=bar.rows[index].rect;float w=r.right-r.left,rh=r.bottom-r.top;
        c->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,{w/2,rh/2})*D2D1::Matrix3x2F::Translation(px,py)*original);
        c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),opacity),nullptr);bar.PaintDockItem(c,index);c->PopLayer();};
    for(int i=0;i<static_cast<int>(after.rows.size());i++){auto const &row=after.rows[i];auto old=std::find_if(before.rows.begin(),before.rows.end(),[&](auto const &v){return v.id==row.id;});bool fresh=old==before.rows.end();
        float from=fresh?row.rect.left:old->rect.left+(newW-oldW)/2;float px=x+from+(row.rect.left-from)*k;
        paint(after,i,px,y+row.rect.top,fresh?k:1,fresh?std::clamp(k,0.f,1.f):1);
    }
    for(int i=0;i<static_cast<int>(before.rows.size());i++){auto const &row=before.rows[i];if(std::any_of(after.rows.begin(),after.rows.end(),[&](auto const &v){return v.id==row.id;}))continue;
        paint(before,i,x+row.rect.left+(newW-oldW)/2,y+row.rect.top,1-.85f*k,1-std::clamp(k,0.f,1.f));}
    c->SetTransform(original);
}
// Offline sampling of the actual page painters and insertion plan. The live
// implementation uses finite Composition animations, never these frame loops.
inline void GalleryPagePair(App &app,ID2D1DeviceContext *c,JsonObject const &v,bool nominal){
    float width=static_cast<float>(Num(v,L"width")),height=static_cast<float>(Num(v,L"height")),p=NativeInteractionMotion::Progress(static_cast<float>(Num(v,L"motionTime"))),direction=static_cast<float>(Num(v,L"direction",1));
    D2D1_MATRIX_3X2_F old;c->GetTransform(&old);
    auto paint=[&](JsonObject const &data,float offset,float opacity){
        app.model.Read(data);app.model.cardData=data.GetNamedObject(L"cardData",JsonObject{});Window page(app,L"fixture");page.width=width;page.height=height;page.materialPreview=nominal;
        for(auto const &tile:app.model.boards[0].tiles)if(!tile.folder.empty()){int i=0;for(auto const &child:tile.items)page.icons[child.key]=GalleryStockIcon(app.graphics,i++%2?SIID_DOCNOASSOC:SIID_FOLDER);}
        c->SetTransform(D2D1::Matrix3x2F::Translation(offset,0)*old);c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),opacity),nullptr);
        for(auto const &tile:app.model.boards[0].tiles)page.PaintTile(c,tile,page.RootBounds(tile.key),0);c->PopLayer();
    };
    paint(v.GetNamedObject(L"fromData"),-direction*NativeInteractionMotion::PageTravel*p,1-p);paint(v.GetNamedObject(L"data"),direction*NativeInteractionMotion::PageTravel*(1-p),.55f+.45f*p);c->SetTransform(old);
}
inline float GalleryFolderProgress(float time){
    auto value=[](float t,float a,float b){float u=1-t;return 3*u*u*t*a+3*u*t*t*b+t*t*t;};float lo=0,hi=1;for(int i=0;i<20;i++){float m=(lo+hi)/2;if(value(m,.22f,.28f)<time)lo=m;else hi=m;}return value((lo+hi)/2,1,1);
}
inline int CheckNativeParity(App &app,NativeShell &shell){
    int count=0;auto expect=[&](bool ok,char const *message){if(!ok){std::fprintf(stderr,"Native parity check failed: %s\n",message);throw std::runtime_error(message);}++count;};
    Board page;for(int i=0;i<128;i++)page.pages.push_back(std::to_wstring(i));page.pageId=L"73";auto start=page.PageStart();
    expect(start<=73&&start+14>73,"Active page outside visible dots");page.pageId=L"127";expect(page.PageStart()==114,"Last page unreachable");
    NativeBar bar(app,shell,L"fixture",true);bar.width=600;bar.height=56;bar.padding=8;bar.gap=8;bar.iconSize=40;
    bar.data=JsonObject::Parse(LR"({"dock":{"left":[{"key":"a","type":"Plugin"}],"center":[{"key":"b","type":"Plugin"}],"right":[{"key":"c","type":"Plugin"}]}})");
    auto contentRows=[&](){std::vector<PopupRow> result;for(auto const &row:bar.rows)if(!row.id.starts_with(L"@boundary:"))result.push_back(row);return result;};
    bar.settings=JsonObject::Parse(LR"({"mode":"FullWidth"})");bar.Layout();auto groups=contentRows();expect(groups.size()==3,"Dock groups lost");expect(groups.front().rect.left==8&&groups.back().rect.right==592,"Full-width groups not anchored");
    expect(groups[0].rect.right<groups[1].rect.left&&groups[1].rect.right<groups[2].rect.left,"Dock groups overlap");
    Put(bar.settings,L"position",std::wstring(L"Left"));bar.width=56;bar.height=600;bar.Layout();expect(bar.Vertical()&&contentRows().back().rect.bottom==592,"Vertical dock did not transpose layout");
    bar.data=JsonObject::Parse(LR"({"dock":{"left":[],"center":[{"key":"app","type":"AppOrFile","pinned":true,"windows":[{"hwnd":1,"monitor":"fixture"},{"hwnd":2,"monitor":"other"}]}],"right":[]}})");
    bar.settings=JsonObject::Parse(LR"({"splitWindows":true})");bar.Layout();expect(contentRows().size()==2,"Split windows ignored");
    Put(bar.settings,L"temporalItemsVisibility",std::wstring(L"OnMonitor"));bar.Layout();expect(contentRows().size()==1&&std::any_of(bar.entries.begin(),bar.entries.end(),[](auto const &e){return Str(e,L"type")==L"AppOrFile"&&e.GetNamedArray(L"windows").Size()==1&&Num(e.GetNamedArray(L"windows").GetAt(0).GetObject(),L"hwnd")==1;}),"Monitor filtering lost target");
    bar.primary=false;Put(bar.settings,L"pinnedItemsVisibility",std::wstring(L"WhenPrimary"));bar.monitor=L"none";bar.Layout();expect(bar.rows.empty(),"Primary-only pinned item leaked");
    NativePopup menu(app);menu.kind=L"@magic/context-menu";menu.width=320;menu.height=220;menu.packet=JsonObject::Parse(LR"({"data":{"rows":[{"id":"a","label":"打开"},{"id":"s","control":"separator"},{"id":"d","label":"禁用","enabled":false},{"id":"sub","label":"子菜单","control":"submenu"}]}})");menu.BuildRows();
    expect(menu.rows.size()==4&&!menu.rows[1].enabled&&!menu.rows[2].enabled&&menu.rows[3].control==L"submenu","Menu state lost");
    Window desktop(app,L"fixture");NativeDesktopMenu state(desktop,{});auto values=JsonArray::Parse(LR"([{"index":0,"key":"open","label":"打开"}])");auto root=state.Compact(values);auto child=state.Compact(values);
    expect(Str(root.GetAt(0).GetObject(),L"id")!=Str(child.GetAt(0).GetObject(),L"id")&&state.shellEntries.size()==2,"Submenu command replaced parent");
    NativePopup apps(app);apps.kind=L"@magic/apps-menu";apps.width=760;apps.height=520;apps.packet=JsonObject::Parse(LR"({"data":{"rows":[{"id":"search","label":"搜索","control":"text"},{"id":"apps-view","label":"收藏"},{"id":"apps-display","label":"全屏"},{"id":"apps-power","label":"电源"}]}})");apps.BuildRows();expect(apps.rows[1].rect.left>=150&&!apps.rows[1].offscreen&&apps.rows[1].rect.top<48&&apps.rows[0].offscreen,"Apps header or favorites search visibility wrong");
    auto data=apps.Data();auto list=data.GetNamedArray(L"rows");for(int i=0;i<36;i++){JsonObject row;Put(row,L"id",L"app:"+std::to_wstring(i));Put(row,L"label",std::wstring(L"App"));list.Append(row);}apps.scroll=1;apps.BuildRows();expect(apps.rows[1].rect.top==10&&apps.rows[4].offscreen,"Apps scroll moved header or left first band visible");
    NativeBar toolbar(app,shell,L"fixture",false);toolbar.width=100;toolbar.height=32;toolbar.padding=8;toolbar.iconSize=16;toolbar.data=JsonObject::Parse(LR"({"toolbar":{"left":[{"id":"toolbar:a","label":"A"},{"id":"toolbar:b","label":"B"}],"center":[{"id":"toolbar:c","label":"C"}],"right":[{"id":"toolbar:d","label":"D"},{"id":"toolbar:e","label":"E"}]}})");toolbar.settings=JsonObject{};toolbar.Layout();expect(toolbar.toolbarSequential,"Toolbar overlap did not switch to sequential flow");for(size_t i=1;i<toolbar.rows.size();i++)if(toolbar.rows[i-1].control!=L"corner"&&toolbar.rows[i].control!=L"corner")expect(toolbar.rows[i-1].rect.right<=toolbar.rows[i].rect.left,"Sequential toolbar items overlap");
    NativePopup calendar(app);calendar.width=320;calendar.year=2026;calendar.month=2;calendar.firstWeekday=0;float four=calendar.CalendarHeight();calendar.month=8;expect(calendar.CalendarHeight()>four,"Calendar height ignores visible week count");
    int fired=0;desktop.dragPageDwell=[&]{++fired;};desktop.Message(WM_TIMER,97,0);desktop.Message(WM_TIMER,97,0);expect(fired==1&&!desktop.dragPageDwell,"Page dwell callback was retained or repeated");
    return count;
}
inline int CheckNativeUiCorrections(App &app){
    int count=0;auto expect=[&](bool ok,char const *message){if(!ok)throw std::runtime_error(message);++count;};
    count+=CheckPreview15Bugs(app);
    count+=CheckOct28Materials(app);
    count+=CheckFolderViewport(app);
    count+=CheckDockReorder(app);
    count+=CheckToolbarRelease(app);
    count+=CheckPopupNavigationCases(app);
    count+=CheckAppsInputs(app);
    count+=CheckSnapshotOrder(app);
    expect(NativeMoveYieldsBar(true,true,true,false),"Real move event was denied without keyboard/mouse state access");
    expect(NativeMoveYieldsBar(true,true,false,false),"Snap foreground transition discarded the real move owner");
    expect(!NativeMoveYieldsBar(false,true,true,true),"Cached backend move overrode the local end event");
    expect(NativeMoveYieldsBar(false,false,true,true),"No-hook geometry fallback lost its move state");
    expect(!NativeMoveYieldsBar(false,false,false,true),"Other-monitor fallback hid this bar");
    count+=CheckUserConfirmation(app);
    {
        // Real offscreen HWNDs, product focus handler, no foreground activation.
        // An owner-preserving HWND_BOTTOM reproduces the original blank desktop.
        struct OwnedDepthWindows{std::vector<HWND> values;~OwnedDepthWindows(){for(auto it=values.rbegin();it!=values.rend();++it)DestroyWindow(*it);}} owned;
        auto make=[&](HWND owner){auto hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Magic focus depth fixture",WS_POPUP,-30000,-30000,40,40,owner,nullptr,GetModuleHandleW(nullptr),nullptr);if(!hwnd)throw_last_error();owned.values.push_back(hwnd);return hwnd;};
        auto above=[](HWND a,HWND b){for(auto next=GetWindow(a,GW_HWNDNEXT);next;next=GetWindow(next,GW_HWNDNEXT))if(next==b)return true;return false;};
        auto owner=make(nullptr),other=make(nullptr);Window focus(app,L"owned-depth-fixture");focus.hwnd=make(owner);focus.providerState->hwnd=focus.hwnd;
        auto foreground=GetForegroundWindow();bool live=app.model.desktopRuntime;app.model.desktopRuntime=true;
        auto flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE;
        for(int cycle=0;cycle<6;cycle++){
            SetWindowPos(owner,HWND_BOTTOM,0,0,0,0,flags|SWP_SHOWWINDOW);
            SetWindowPos(other,HWND_TOP,0,0,0,0,flags|SWP_SHOWWINDOW);
            SetWindowPos(focus.hwnd,HWND_TOP,0,0,0,0,flags|SWP_SHOWWINDOW|SWP_NOOWNERZORDER);
            focus.Message(WM_SETFOCUS,0,0);
            expect(above(focus.hwnd,owner),"Desktop focus placed the surface behind its Explorer owner");
            expect(above(other,focus.hwnd),"Desktop focus left the surface above an application");
        }
        expect(GetForegroundWindow()==foreground,"Offscreen desktop depth check changed real foreground");app.model.desktopRuntime=live;
    }
    {
        expect(NativeTrayMenuAnchor::MenuStyle(L"Chrome_WidgetWin_1",0x14c20000,0x108),"QQ custom menu class/style was not recognized");
        expect(!NativeTrayMenuAnchor::MenuStyle(L"Chrome_WidgetWin_1",WS_OVERLAPPEDWINDOW,WS_EX_TOPMOST),"QQ main window must not be moved as a tray menu");
        expect(!NativeTrayMenuAnchor::MenuStyle(L"Chrome_WidgetWin_1",0x14c20000,0x100),"Ordinary Electron window must not be moved as a tray menu");
        auto qq=NativeTrayMenuAnchor::Position({2304,-958,2596,34},{0,64,3072,1920},{2400,64},false,true);
        expect(qq.y==64&&qq.x>=0&&qq.x+292<=3072,"Observed QQ menu remained above the top bar");
    }
    {
        NativePopup apps(app);apps.kind=L"@magic/apps-menu";apps.generic=true;apps.visible=true;apps.iconReady=true;
        apps.hwnd=CreateWindowExW(0,L"STATIC",L"Magic owned popup fixture",WS_POPUP,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        expect(apps.hwnd!=nullptr,"Icon request fixture HWND failed");
        apps.popupRoot=app.graphics.compositor.CreateContainerVisual();apps.life=std::make_shared<PopupLife>();apps.life->owner=&apps;
        apps.packet=JsonObject::Parse(LR"({"payload":{"alignY":"Center"},"data":{"rows":[{"id":"app:umid-only","image":"","iconUmid":"Owned.App!App"}]}})");
        PopupRow row;row.id=L"app:umid-only";apps.rows.push_back(row);auto previous=app.nextId;
        apps.RequestAppearanceIcons();expect(apps.iconRequests.contains(row.id),"AUMID-only application icon was never requested");
        for(auto it=app.callbacks.begin();it!=app.callbacks.end();)if(it->first>previous)it=app.callbacks.erase(it);else ++it;
        apps.edits[L"search"]=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE,0,0,100,24,apps.hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        expect((GetWindowLongPtrW(apps.edits[L"search"],GWL_STYLE)&WS_VISIBLE)!=0,"Search fixture was not visible before dismissal");
        auto search=apps.edits[L"search"];apps.Hide(false);expect(apps.dismissing&&!IsWindow(search)&&apps.edits.empty(),"Search HWND lingered during Composition close");
        NativePopup tray(app);tray.kind=L"@magic/system-tray";tray.width=320;tray.height=220;tray.popupRoot=app.graphics.compositor.CreateContainerVisual();
        PopupRow icon;icon.id=L"tray:owned";icon.control=L"tray";icon.rect={10,10,58,58};icon.active=true;tray.rows.push_back(icon);tray.SyncInteraction(0);
        expect(!tray.interactionVisual.active,"Tray promotion/hover recreated a rounded selection frame");
    }
    for(auto const &alias:NativePopupAliases)expect(CanonicalNativePopupId(alias.legacy)==alias.canonical,"Legacy popup alias changed its native destination");
    expect(CanonicalNativePopupId(L"@seelen/third-party-custom")==L"@seelen/third-party-custom","External plugin ID was rewritten");
    expect(CanonicalNativePopupId(L"@magic/context-menu")==L"@magic/context-menu","Local native ID was rewritten");
    {NativePopup old(app),current(app);old.kind=L"@seelen/power-menu";current.kind=L"@magic/power-menu";old.width=current.width=380;old.height=current.height=410;auto packet=JsonObject::Parse(LR"({"data":{"rows":[{"id":"restart","label":"Restart","control":"button"}]}})");old.packet=packet;current.packet=packet;old.BuildRows();current.BuildRows();expect(old.kind==current.kind&&old.rows.size()==current.rows.size()&&old.rows[0].action==current.rows[0].action&&old.rows[0].rect.left==current.rows[0].rect.left,"Old popup identity changed layout or confirmation action");}
    {
        auto previousOwner=NativeShell::eventOwner;NativeShell fixtureShell(app);NativeBar dock(app,fixtureShell,L"fixture",true);
        dock.settings=JsonObject::Parse(LR"({"position":"Bottom"})");dock.iconSize=40;dock.width=120;dock.height=56;
        dock.toolbarLayer=app.graphics.compositor.CreateContainerVisual();dock.toolbarRoot=app.graphics.compositor.CreateContainerVisual();
        PopupRow row;row.id=L"fixture-icon";row.rect={40,8,80,48};dock.rows.push_back(row);
        auto &v=dock.toolbarVisuals[row.id];v.group=app.graphics.compositor.CreateContainerVisual();v.hoverLayer=app.graphics.compositor.CreateContainerVisual();v.group.Children().InsertAtTop(v.hoverLayer);dock.toolbarLayer.Children().InsertAtTop(v.group);
        for(int hz:{30,60,165}){
            bool immediate=true;
            for(int n=0;n<=hz;n++){
                dock.DockPointer(12+96.f*n/hz,20);auto desired=dock.DockPose(0);
                if(desired.scale>1)immediate&=std::abs(v.hoverLayer.Scale().x-desired.scale)<.0001f&&!v.hoverLayer.TryGetAnimationController(L"Scale")&&!v.hoverLayer.TryGetAnimationController(L"Offset");
            }
            expect(immediate,"Pointer pose waits for a restarted compositor animation");
        }
        dock.LockDockReorderHover();
        expect(dock.dockReorderHoverBlocked&&v.hoverLayer.Scale().x==1&&!v.hoverRetireAt&&!v.hoverLayer.TryGetAnimationController(L"Scale"),"Dock grab did not immediately restore original icon size");
        dock.barDrag=true;dock.DockPointerLeft(true);dock.DockPointer(60,20);
        expect(dock.dockReorderHoverBlocked&&v.hoverLayer.Scale().x==1,"Leaving during capture rearmed Dock magnification");
        dock.barDrag=false;dock.DockPointer(60,20);dock.SyncDockHover();
        expect(dock.dockReorderHoverBlocked&&v.hoverLayer.Scale().x==1&&!v.hoverLayer.TryGetAnimationController(L"Offset"),"Drop or model refresh restarted Dock magnification");
        dock.DockPointerLeft(false);dock.DockPointer(60,20);
        expect(dock.dockReorderHoverBlocked&&v.hoverLayer.Scale().x==1,"Spurious leave over Dock cleared reorder intent");
        dock.AnimateToolbarOffset(v.group,{120,8,0});
        expect(v.group.TryGetAnimationController(L"Offset")&&v.hoverLayer.Scale().x==1,"Reorder lock disabled position exchange animation");
        dock.DockPointerLeft(true);expect(!dock.dockReorderHoverBlocked&&dock.dockHoverAxis==-FLT_MAX,"Real leave did not rearm Dock hover");
        dock.DockPointer(60,20);expect(v.hoverLayer.Scale().x>1,"Reentering Dock did not restore magnification");
        dock.ClearDockHover();expect(v.hoverRetireAt>0&&v.hoverLayer.TryGetAnimationController(L"Scale"),"Leaving Dock lost finite return animation");
        v.hoverRetireAt=GetTickCount64()-1;dock.SyncDockHover();expect(!v.hoverRetireAt&&v.hoverLayer.Scale().x==1&&!v.hoverLayer.TryGetAnimationController(L"Scale"),"Completed Dock return retained its transform/controller");
        dock.edge=true;dock.hwnd=CreateWindowExW(0,L"STATIC",L"Magic owned Dock timer fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        expect(dock.hwnd!=nullptr,"Owned hidden timer window failed");SetWindowLongPtrW(dock.hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&dock));
        DWORD timerForeground=0;GetWindowThreadProcessId(GetForegroundWindow(),&timerForeground);dock.data=JsonObject{};Put(dock.data,L"hostPid",static_cast<double>(timerForeground));
        dock.dockEnterAt=dock.dockResizeAt=1;dock.dockBodyPending=true;dock.toolbarRoot.Scale({.8f,.8f,1});
        NativeBar::Proc(dock.hwnd,WM_TIMER,NativeBar::DockPreviewTimer,0);
        expect(dock.dockEnterAt==1&&dock.dockResizeAt==1,"Preview cleanup consumed Dock body state");
        NativeBar::Proc(dock.hwnd,WM_TIMER,NativeBar::DockBodyTimer,0);
        expect(!dock.dockEnterAt&&!dock.dockResizeAt&&!dock.dockBodyPending&&dock.toolbarRoot.Scale().x==1,"Body timer did not finalize backdrop geometry");
        fixtureShell.bars[L"fixture"]=std::make_unique<NativeBar>(app,fixtureShell,L"fixture",true);
        NativeShell::WindowEvent(nullptr,EVENT_SYSTEM_MINIMIZEEND,dock.hwnd,0,0,0,0);
        expect(fixtureShell.bars.at(L"fixture")->dockBackdropDirty,"Minimize end did not invalidate Dock backdrop");
        fixtureShell.bars.at(L"fixture")->dockBackdropDirty=false;NativeShell::WindowEvent(nullptr,EVENT_SYSTEM_FOREGROUND,dock.hwnd,0,0,0,0);
        expect(fixtureShell.bars.at(L"fixture")->dockBackdropDirty,"Foreground change did not invalidate Dock backdrop");
        NativeShell::eventOwner=previousOwner;
    }
    {PipeClient stalled;JsonObject telemetry;Put(telemetry,L"op",std::wstring(L"telemetry"));for(int i=0;i<1000;i++)stalled.Send(telemetry);
     JsonObject command;Put(command,L"op",std::wstring(L"shell-command"));bool accepted=true;try{for(int i=0;i<32;i++)stalled.Send(command);}catch(...){accepted=false;}
     expect(accepted,"Telemetry crowded out bounded user actions");bool bounded=false;try{stalled.Send(command);}catch(std::runtime_error const&){bounded=true;}expect(bounded,"Action queue lost its resource bound");}
    {NativeRenderScope first(nullptr);expect(static_cast<bool>(first)&&NativeRenderScope::busy,"Outer render did not acquire the scene");{NativeRenderScope nested(nullptr);expect(!static_cast<bool>(nested),"Reentrant drawing was allowed");}expect(NativeRenderScope::busy,"Nested scope released the outer draw");}
    expect(!NativeRenderScope::busy,"Render scene was not released");
    {Window released(app,L"fixture");released.DrawSurface(nullptr,false);released.DrawIfChanged(nullptr,false);expect(true,"Released drawing surface was not ignored");}
    {Window card(app,L"fixture");Tile malformed;malformed.card=JsonObject::Parse(LR"({"type":"system.weather","columns":2,"rows":2,"settings":null})");
     auto surface=app.graphics.composition.CreateDrawingSurface({184,184},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);auto interop=surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> context;POINT offset{};check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),context.put_void(),&offset));context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));card.PaintCard(context.get(),malformed,{0,0,184,184});expect(SUCCEEDED(interop->EndDraw()),"Malformed weather card corrupted its drawing surface");}
    {auto saved=app.model.encodeckData;
     Window card(app,L"fixture");Tile tile;tile.card=JsonObject::Parse(LR"({"type":"system.encodeck","columns":4,"rows":2,"settings":{}})");
     for(auto state:{LR"({"available":false,"connecting":false})",LR"({"available":false,"connecting":true})",LR"({"available":true,"deviceName":"Owned fixture","deviceConnected":true,"controlReady":true,"canSetNoise":true,"noiseMode":"NoiseCancellation","noiseStale":false,"left":{"level":80,"stale":false},"right":{"level":null,"stale":true},"case":{"level":50,"charging":true,"stale":false},"modes":[{"mode":"NoiseCancellation","label":"Noise"},{"mode":"Transparency","label":"Ambient"}],"levels":[{"index":3,"label":"Comfort","selected":true}]})"}){
        app.model.encodeckData=JsonObject::Parse(state);
        for(float dpi:{96.f,144.f,192.f,288.f}){
            auto surface=app.graphics.composition.CreateDrawingSurface({376*dpi/96,184*dpi/96},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            auto interop=surface.as<abiComposition::ICompositionDrawingSurfaceInterop>();com_ptr<ID2D1DeviceContext> context;POINT offset{};check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),context.put_void(),&offset));
            context->SetDpi(dpi,dpi);context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x)*96/dpi,static_cast<float>(offset.y)*96/dpi));
            card.PaintCard(context.get(),tile,{0,0,376,184});
            expect(SUCCEEDED(interop->EndDraw()),"EncoDeck card failed bounded native drawing");
        }
     }app.model.encodeckData=saved;}
    auto host=app.graphics.compositor.CreateContainerVisual();NativeHoverVisual feedback;
    auto row=[](float top){return NativeRegion({10,top,310,top+38},{0,0,320,220},10);};
    feedback.Sync(app.graphics.compositor,host,row(10),{100,28},1,true,false);
    feedback.Sync(app.graphics.compositor,host,row(60),{100,78},1,true,false);
    feedback.Sync(app.graphics.compositor,host,{}, {100,104},1,true,false);
    feedback.Sync(app.graphics.compositor,host,{}, {100,106},1,true,false);
    expect(static_cast<bool>(feedback.root.TryGetAnimationController(L"Opacity")),"Repeated row gaps interrupted hover fade");
    feedback.Sync(app.graphics.compositor,host,row(120),{100,138},1,true,false);
    expect(static_cast<bool>(feedback.root.TryGetAnimationController(L"Offset")),"Hover re-entry snapped instead of retargeting");
    feedback.Sync(app.graphics.compositor,host,row(60),{100,78},1,false,false);
    expect(!feedback.root.TryGetAnimationController(L"Offset"),"Reduced motion retained hover translation");
    feedback.Release(host);expect(host.Children().Count()==0,"Hover visual leaked after release");
    if(NativeGlassAvailable()){
        struct ProbeWindow{HWND hwnd;~ProbeWindow(){if(hwnd)DestroyWindow(hwnd);}} probe{CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,L"STATIC",L"Magic hidden material check",WS_POPUP,0,0,1600,900,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)};
        expect(probe.hwnd!=nullptr,"Hidden material check window failed");
        auto content=app.graphics.compositor.CreateSpriteVisual();host.Children().InsertAtTop(content);NativeGlassSheet sheet;
        expect(sheet.Sync(app.graphics,probe.hwnd,host,{10,10,1590,890},1,16,NativeMaterial::Panel,true,content,32*1024*1024),"Continuous large material graph failed");
        expect(sheet.root.Children().Count()==1&&sheet.bytes<=32*1024*1024,"Material split into independent sampling blocks or exceeded budget");
        sheet.Release(host);expect(sheet.bytes==0&&host.Children().Count()==1,"Material graph leaked after release");host.Children().Remove(content);
    }
    Model policy;policy.preferences=JsonObject::Parse(LR"({"reduceOnBatterySaver":true})");
    struct Setting{GUID id;DWORD length,value;};Setting modern{NativePowerPolicy::Energy,4,1};
    expect(NativePowerPolicy::Accept(policy,*reinterpret_cast<POWERBROADCAST_SETTING*>(&modern))&&NativeLowPower(policy),"Energy saver not applied");
    auto legacy=Setting{NativePowerPolicy::Legacy,4,0};expect(!NativePowerPolicy::Accept(policy,*reinterpret_cast<POWERBROADCAST_SETTING*>(&legacy))&&NativeLowPower(policy),"Legacy notification overrode modern saver");
    policy.preferences=JsonObject::Parse(LR"({"reduceOnBatterySaver":false})");expect(NativeLowPower(policy),"Legacy preference bypassed Windows energy-saver rendering");
    policy.preferences=JsonObject::Parse(LR"({"reduceOnBatterySaver":true})");modern.value=0;expect(NativePowerPolicy::Accept(policy,*reinterpret_cast<POWERBROADCAST_SETTING*>(&modern))&&!NativeLowPower(policy),"Saver exit did not restore rendering");
    NativePopup single(app);single.kind=L"@magic/context-menu";single.width=260;single.height=48;single.packet=JsonObject::Parse(LR"({"data":{"menuStyle":"toolbar","rows":[{"id":"remove","label":"移除模块"}]}})");single.BuildRows();expect(!single.ContextOverflows(),"Single toolbar row produced a scrollbar");single.height=30;expect(single.ContextOverflows(),"Actual context overflow was hidden");
    NativePopup power(app);power.kind=L"@magic/power-menu";power.packet=JsonObject::Parse(LR"({"data":{"rows":[{"id":"restart","label":"Restart","control":"button"},{"id":"settings","label":"Settings","control":"button"}]}})");power.BuildRows();expect(power.generic&&power.rows.size()==2&&power.rows[0].action==L"restart"&&power.rows[1].action==L"settings","Independent power rows lost native confirmation actions");
    NativePopup tiny(app);tiny.kind=L"@magic/apps-menu";tiny.width=760;tiny.height=520;tiny.packet=JsonObject::Parse(LR"({"data":{"rows":[{"id":"apps-view","label":"全部应用"},{"id":"apps-display","label":"全屏"},{"id":"apps-power","label":"电源"},{"id":"app:0","label":"应用"}]}})");tiny.BuildRows();expect(tiny.appearanceOverflow==0,"Intentionally hidden controls produced a generic scrollbar");
    NativePopup start(app);start.kind=L"@magic/apps-menu";start.packet=JsonObject::Parse(LR"({"payload":{"alignY":"Center"}})");auto pose=NativeInteractionMotion::Panel(800,500,false,start.PanelAbove(),1);expect(pose.origin.y==500&&pose.offset.y>0,"Start menu did not expand from bottom center");
    std::vector<Tile> tiles;for(int i=0;i<6;i++){Tile t;t.key=std::to_wstring(i);t.name=t.key;tiles.push_back(t);}
    auto right=NativeFolderPlan::Build(tiles,{L"0"},L"0",3);expect(right.order==std::vector<std::wstring>{L"1",L"2",L"3",L"0",L"4",L"5"},"Rightward folder insertion swapped instead of shifting");
    auto left=NativeFolderPlan::Build(tiles,{L"4"},L"4",1);expect(left.order==std::vector<std::wstring>{L"0",L"4",L"1",L"2",L"3",L"5"},"Leftward folder insertion incorrect");
    auto multi=NativeFolderPlan::Build(tiles,{L"1",L"3"},L"3",5);expect(multi.order==std::vector<std::wstring>{L"0",L"2",L"4",L"5",L"1",L"3"}&&multi.insertion==4,"Multi-selection insertion lost order or anchor");
    expect(NativeFolderPlan::Build(tiles,{L"missing"},L"missing",2).insertion<0,"Stale drag member accepted");
    Window page(app,L"fixture");page.root=app.graphics.compositor.CreateContainerVisual();page.root.Size({640,480});page.pageLayer=app.graphics.compositor.CreateContainerVisual();page.root.Children().InsertAtBottom(page.pageLayer);page.desktop=app.graphics.compositor.CreateSpriteVisual();page.pageLayer.Children().InsertAtTop(page.desktop);
    auto surface=[&]{return app.graphics.composition.CreateDrawingSurface({64,64},Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);};
    page.desktopSurface=surface();page.desktop.Brush(app.graphics.compositor.CreateSurfaceBrush(page.desktopSurface));page.rootPixelWidth=page.rootPixelHeight=64;page.estimatedSurfaceBytes=64*64*4;auto previous=page.pageLayer;page.StartPageTransition(1,false);
    expect(page.outgoingPage==previous&&page.pageLayer!=previous&&page.root.Children().Count()==2,"Old page was not retained alongside incoming page");
    expect(page.outgoingPage.TryGetAnimationController(L"Offset")&&page.outgoingPage.TryGetAnimationController(L"Opacity")&&page.pageLayer.TryGetAnimationController(L"Offset"),"Page pair animations not bound in parallel");
    page.desktopSurface=surface();page.desktop.Brush(app.graphics.compositor.CreateSurfaceBrush(page.desktopSurface));page.rootPixelWidth=page.rootPixelHeight=64;page.StartPageTransition(-1,true);expect(page.root.Children().Count()==2,"Repeated paging accumulated outgoing layers");page.ResetPageMotion();expect(page.root.Children().Count()==1&&!page.outgoingPage&&page.outgoingPageBytes==0,"Outgoing page resources not released");
    Board board;board.id=L"drag-fixture";Tile folder;folder.key=L"folder:f";folder.folder=L"f";folder.items=tiles;board.tiles.push_back(folder);app.model.boards.push_back(board);Window drag(app,board.id);drag.folderId=L"f";drag.folderMotion=app.graphics.compositor.CreateContainerVisual();drag.panel=app.graphics.compositor.CreateSpriteVisual();drag.panel.Size({560,520});drag.folderMotion.Children().InsertAtBottom(drag.panel);drag.panelRect={0,0,560,520};drag.contentRect={24,64,536,496};drag.columns=5;drag.folderPreviewKeys={L"0"};NativeFolderAvoidance avoid;
    expect(avoid.Sync(drag,right)&&avoid.bytes>0&&avoid.bytes<=8*1024*1024,"Folder avoidance graph failed or exceeded budget");avoid.Release(drag.folderMotion);expect(avoid.bytes==0&&drag.folderMotion.Children().Count()==1,"Folder avoidance surfaces leaked");drag.estimatedSurfaceBytes=SurfaceLimit;expect(!avoid.Sync(drag,right)&&!avoid.layer,"Folder avoidance ignored total surface budget");drag.folderPreviewActive=true;drag.folderPreviewOrder=right.order;auto moved=drag.Bounds(L"1"),first=FolderCellBounds(drag.contentRect,drag.columns,0,0);expect(moved.left==first.left&&moved.top==first.top,"Preview hit geometry did not follow insertion layout");drag.folderPreviewCommitted=true;drag.ResetFolderAvoidance();expect(!drag.folderPreviewActive&&!drag.folderPreviewCommitted,"Folder preview state survived cancellation");app.model.boards.pop_back();
    count+=CheckNativeFolderCovers(app);
    return count;
}
#include "../../tools/review/native-honor-checks.h"
inline int ExportAppearanceGallery(std::wstring const &fixture,std::wstring const &output,bool runChecks=false,bool selectionOnly=false,bool correctionsOnly=false,bool honorOnly=false)
{
    std::ifstream input{std::filesystem::path(fixture)};if(!input)throw std::runtime_error("Missing gallery fixture");
    std::string bytes{std::istreambuf_iterator<char>(input),{}};if(bytes.size()>512*1024)throw std::runtime_error("Gallery fixture bound");
    auto cases=JsonArray::Parse(to_hstring(bytes));std::filesystem::create_directories(output);
    App app;app.graphics.Start();app.model.saverKnown=true;app.model.saverActive=false;app.model.cardData=JsonObject{};app.model.weatherData=JsonObject{};auto glass=Preview70Glass(app.graphics);NativeShell shell(app);
    if(std::any_of(cases.begin(),cases.end(),[](auto const &v){return Str(v.GetObject(),L"motion")==L"dissolve";})){
        // Compile and attach the exact live effect to an unattached visual tree.
        // There is no desktop target or visible window, so this is not playback acceptance.
        auto host=app.graphics.compositor.CreateContainerVisual(),content=app.graphics.compositor.CreateContainerVisual();host.Children().InsertAtTop(content);
        auto tile=app.graphics.compositor.CreateSpriteVisual();tile.Size({640,56});tile.Brush(app.graphics.compositor.CreateColorBrush({255,255,255,255}));content.Children().InsertAtTop(tile);
        NativeDissolve::Session session;bool prepared=session.Start(app.graphics,host,content,640,56,2);auto maskBytes=session.maskBytes,layerBytes=session.layerBytes;auto error=session.error;
        session.Reset(host,content);
        std::ofstream receipt{std::filesystem::path(output+L"/native-dissolve-graph.json")};receipt<<"{\"prepared\":"<<(prepared?"true":"false")<<",\"error\":"<<error<<",\"maskBytes\":"<<maskBytes<<",\"nominalLayerBytes\":"<<layerBytes<<",\"releasedMaskBytes\":"<<session.maskBytes<<",\"releasedLayerBytes\":"<<session.layerBytes<<",\"visibleWindowsCreated\":false,\"actualPlaybackVerified\":false}";
        if(!prepared)throw std::runtime_error("Dissolve Composition graph could not be prepared; see native-dissolve-graph.json");
    }
    if(std::any_of(cases.begin(),cases.end(),[](auto const &v){return Str(v.GetObject(),L"kind")==L"@review/parallel";})){
    auto host=app.graphics.compositor.CreateContainerVisual();NativeHoverVisual feedback;
        feedback.Sync(app.graphics.compositor,host,NativeRegion({10,10,310,48},{0,0,320,160},10),{120,28},1,true,false);
        feedback.Sync(app.graphics.compositor,host,NativeRegion({10,60,310,98},{0,0,320,160},10),{150,78},1,true,false);
        feedback.Release(host);
        NativeBar motionBar(app,shell,L"fixture",true);motionBar.dockShiftX=24;auto moving=app.graphics.compositor.CreateContainerVisual();
        bool bound=NativeMotion(app.model);motionBar.AnimateToolbarOffset(moving,{160,12,0});moving.StopAnimation(L"Offset");
        auto spring=NativeInteractionMotion::DockSpringVector(app.graphics.compositor,{0,0,1},{1,1,1});moving.StartAnimation(L"Scale",spring);moving.StopAnimation(L"Scale");
        std::ofstream receipt{std::filesystem::path(output+L"/native-interaction-graph.json")};receipt<<"{\"hoverBrushPrepared\":true,\"animatedGeometryPrepared\":true,\"dockOffsetExpressionBound\":"<<(bound?"true":"false")<<",\"springBound\":true,\"visibleWindowsCreated\":false,\"actualPlaybackVerified\":false}";
    }
    if(honorOnly)app.model.fixtureRoot=output;
    if(runChecks){try{int checks=honorOnly?CheckHonorIcons(app,shell):correctionsOnly?CheckNativeUiCorrections(app):selectionOnly?CheckDesktopSelection(app,output):CheckNativeParity(app,shell);std::ofstream receipt{std::filesystem::path(output+L"/native-model-checks.json")};receipt<<"{\"allPass\":true,\"checks\":"<<checks<<"}";}catch(winrt::hresult_error const &e){std::ofstream failure{std::filesystem::path(output+L"/native-check-failure.txt")};failure<<std::hex<<e.code().value<<" "<<to_string(e.message());throw;}catch(std::exception const &e){std::ofstream failure{std::filesystem::path(output+L"/native-check-failure.txt")};failure<<e.what();throw;}}
    for(bool dark:{false,true})for(float dpi:{96.f,192.f})for(auto const &value:cases){
        auto v=value.GetObject();auto id=Str(v,L"id"),kind=CanonicalNativePopupId(Str(v,L"kind"));float w=static_cast<float>(Num(v,L"width",320)),h=static_cast<float>(Num(v,L"height",450));
        auto file=output+L"/"+id+(dark?L"-dark":L"-light")+(dpi==192?L"-200.png":L"-100.png");
        Preview70Theme::systemDark=dark;
        app.model.preferences=JsonObject{};Put(app.model.preferences,L"iconScheme",Str(v,L"iconScheme"));app.honor.Scheme(HonorScheme(app.model.preferences));
        bool nominal=Bool(v,L"materialPreview");app.model.saverKnown=true;app.model.saverActive=Bool(v,L"lowPower");
        float cw=static_cast<float>(Num(v,L"canvasWidth",w)),ch=static_cast<float>(Num(v,L"canvasHeight",h));
        AppearancePng(app.graphics,file,cw,ch,dpi,[&](ID2D1DeviceContext *c){
            // Synthetic smooth color field previews tint only; this is not a HostBackdrop capture.
            if(nominal){NativePaint p(c,app.graphics,dark);for(float y=0;y<ch;y+=2){float t=y/std::max(1.f,ch);p.Fill({0,y,cw,y+2},D2D1::ColorF(.72f+.15f*t,.48f+.12f*t,.35f+.08f*t),0);}}

            auto backdrop=Str(v,L"backdrop");if(backdrop==L"white"||backdrop==L"dark"){NativePaint field(c,app.graphics,dark);field.Fill({0,0,cw,ch},D2D1::ColorF(backdrop==L"white"?0xffffff:0x17171c),0);}
            auto motion=Str(v,L"motion");
            com_ptr<ID2D1Image> originalTarget;com_ptr<ID2D1CommandList> dissolveContent;
            if(motion==L"dissolve"){c->GetTarget(originalTarget.put());check_hresult(c->CreateCommandList(dissolveContent.put()));c->SetTarget(dissolveContent.get());}
            float progress=NativeInteractionMotion::Progress(static_cast<float>(Num(v,L"motionTime",1)),kind==L"dock");
            NativeInteractionMotion::DockPose pose;bool panel=motion==L"open"||motion==L"close";
            if(panel)pose=NativeInteractionMotion::Panel(w,h,kind==L"@magic/card-picker",kind==L"@magic/apps-menu"||Bool(v,L"above"),motion==L"close"?progress:1-progress,kind==L"@magic/power-menu"||kind==L"@magic/task-switcher");
            if(motion==L"dock-enter"){float t=static_cast<float>(Num(v,L"motionTime",1)),k=NativeInteractionMotion::DockSpring(t);pose.scale=k;pose.origin={w/2,h,0};pose.offset={0,10*(1-k),0};pose.opacity=NativeInteractionMotion::Progress(std::min(1.f,t*NativeInteractionMotion::DockBodyMs/230));}
            if(motion.starts_with(L"page"))pose=NativeInteractionMotion::Page(motion==L"page-prev"?-1:1,motion==L"page-drag",progress);
            auto transform=D2D1::Matrix3x2F::Scale(pose.scale,pose.scale,{pose.origin.x,pose.origin.y})*D2D1::Matrix3x2F::Translation(static_cast<float>(Num(v,L"canvasX"))+pose.offset.x,static_cast<float>(Num(v,L"canvasY"))+pose.offset.y);
            c->SetTransform(transform);bool layer=pose.opacity<1;if(layer)c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),pose.opacity),nullptr);
            if(kind==L"@review/notification-theme"){
                c->Clear(D2D1::ColorF(.5f,.5f,.5f));NativeToolbarSvg svg;
                for(int i=0;i<4;i++){float x=16+i*72.f;auto ink=Preview70Theme{i==2?!dark:dark}.Ink();
                    if(!svg.Draw(c,i==0?L"Honor:nonotify":L"Honor:havenotify",{x,16,x+48,64},ink))throw std::runtime_error("Notification SVG unavailable");}
            }
            else if(kind==L"@review/material"){NativePaint p(c,app.graphics,dark);p.Material({0,0,w,h},static_cast<NativeMaterial>(static_cast<int>(Num(v,L"tier",4))),true,16);}
            else if(kind==L"@review/page-pair"){GalleryPagePair(app,c,v,nominal);}
            else if(kind==L"@review/parallel"){
                for(auto const &entry:v.GetNamedArray(L"layers")){auto panelCase=entry.GetObject();NativePopup popup(app);popup.kind=Str(panelCase,L"kind");popup.width=static_cast<float>(Num(panelCase,L"width"));popup.height=static_cast<float>(Num(panelCase,L"height"));popup.dark=dark;popup.materialPreview=true;popup.packet=panelCase;popup.year=2026;popup.month=10;popup.selectedDay=24;popup.BuildRows();
                    float t=static_cast<float>(Num(v,L"motionTime")),duration=Bool(panelCase,L"closing")?140.f:180.f,amount=NativeInteractionMotion::Progress(std::min(1.f,t*180/duration));if(!Bool(panelCase,L"closing"))amount=1-amount;
                    auto p=NativeInteractionMotion::Panel(popup.width,popup.height,false,false,amount);c->SetTransform(D2D1::Matrix3x2F::Scale(p.scale,p.scale,{p.origin.x,p.origin.y})*D2D1::Matrix3x2F::Translation(static_cast<float>(Num(panelCase,L"x"))+p.offset.x,static_cast<float>(Num(panelCase,L"y"))+p.offset.y));
                    c->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),p.opacity),nullptr);popup.PaintPreview70(c);c->PopLayer();
                }c->SetTransform(D2D1::Matrix3x2F::Identity());
            }
            else if(kind==L"dock"&&motion==L"dock-mutate"){c->SetTransform(D2D1::Matrix3x2F::Identity());GalleryDockMutation(app,shell,c,v,dark);}
            else if(kind==L"toolbar"||kind==L"dock"){
                NativeBar bar(app,shell,L"fixture",kind==L"dock");bar.width=w;bar.height=h;bar.dark=dark;bar.materialPreview=nominal;bar.data=v.GetNamedObject(L"data");
                bar.settings=bar.data.GetNamedObject(kind==L"dock"?L"dockSettings":L"toolbarSettings");
                bar.padding=static_cast<float>(Num(bar.settings,L"padding",8));bar.iconSize=static_cast<float>(Num(bar.settings,kind==L"dock"?L"size":L"itemSize",kind==L"dock"?40:16));bar.gap=8;bar.Layout();bar.focus=static_cast<int>(Num(v,L"focus",-1));bar.hover=static_cast<int>(Num(v,L"hover",-1));
                if(kind==L"dock"&&!Bool(v,L"officialIcons")){int i=0;for(auto const &r:bar.rows)if(r.enabled){auto icon=GalleryStockIcon(app.graphics,i++%2?SIID_APPLICATION:SIID_FOLDER);shell.iconBytes+=icon.bytes;shell.icons[r.key]=std::move(icon);}}
                bar.PaintPreview70(c);
                // h8r2 item rasters are individual Composition visuals in live bars.
                // Reuse those local-coordinate painters on the WIC export target.
                for(int index=0;index<static_cast<int>(bar.rows.size());++index){
                    auto rect=bar.rows[index].rect;D2D1_MATRIX_3X2_F old;c->GetTransform(&old);NativeInteractionMotion::DockPose itemPose;
                    if(kind==L"dock"&&motion==L"hover"&&bar.rows[index].control.empty()){
                        float axis=static_cast<float>(Num(v,L"hoverAxis")),center=(rect.left+rect.right)/2;
                        itemPose=NativeInteractionMotion::Dock(rect.right-rect.left,rect.bottom-rect.top,axis,center,bar.iconSize,L"Bottom",true);
                        float amount=Bool(v,L"leaving")?1-progress:progress;itemPose.scale=1+(itemPose.scale-1)*amount;itemPose.offset.x*=amount;itemPose.offset.y*=amount;
                    }
                    c->SetTransform(D2D1::Matrix3x2F::Scale(itemPose.scale,itemPose.scale,{itemPose.origin.x,itemPose.origin.y})*D2D1::Matrix3x2F::Translation(rect.left+itemPose.offset.x,rect.top+itemPose.offset.y)*old);
                    if(kind==L"dock")bar.PaintDockItem(c,index);else bar.PaintToolbarItem(c,index);c->SetTransform(old);
                }
            }else if(kind==L"desktop"||kind==L"folder"){
                app.model.Read(v.GetNamedObject(L"data"));app.model.cardData=v.GetNamedObject(L"data").GetNamedObject(L"cardData");
                app.model.weatherData=v.GetNamedObject(L"data").GetNamedObject(L"weatherData",JsonObject{});app.model.encodeckData=v.GetNamedObject(L"data").GetNamedObject(L"encodeckData",JsonObject{});
                if(auto clip=v.GetNamedObject(L"data").TryLookup(L"clipboard");clip&&clip.ValueType()==JsonValueType::Object)LoadClipboardTexture(app,clip.GetObject());else app.clipboardBitmap=nullptr;Window desktop(app,L"fixture");desktop.width=w;desktop.height=h;desktop.pageEdge=static_cast<int>(Num(v,L"pageEdge"));desktop.materialPreview=nominal;if(auto keys=v.TryLookup(L"selected");keys&&keys.ValueType()==JsonValueType::Array)for(auto const &key:keys.GetArray())desktop.selected.insert(std::wstring(key.GetString()));desktop.focusKey=Str(v,L"focusKey");
                for(auto const &tile:app.model.boards[0].tiles)if(!tile.folder.empty()){
                    int i=0;for(auto const &child:tile.items){auto icon=GalleryStockIcon(app.graphics,i++%2?SIID_DOCNOASSOC:SIID_FOLDER);desktop.iconBytes+=icon.bytes;desktop.icons[child.key]=std::move(icon);}
                }
                if(kind==L"folder"){
                    desktop.folderId=L"f1";float ox=static_cast<float>(Num(v,L"panelOriginX")),oy=static_cast<float>(Num(v,L"panelOriginY"));desktop.panelRect={ox,oy,ox+w,oy+h};desktop.contentRect={ox+20,oy+64,ox+w-20,oy+h-20};desktop.columns=5;desktop.scrollRow=static_cast<int>(Num(v,L"scrollRow"));
                    if(motion==L"folder-reflow")desktop.folderAvoidance=std::make_shared<NativeFolderAvoidance>();desktop.PaintFolder(c);
                    if(motion==L"folder-reflow"){
                        auto const &tiles=desktop.Tiles();int from=std::clamp(static_cast<int>(Num(v,L"dragIndex")),0,static_cast<int>(tiles.size())-1),target=static_cast<int>(Num(v,L"targetIndex",3));auto plan=NativeFolderPlan::Build(tiles,{tiles[from].key},tiles[from].key,target);float k=GalleryFolderProgress(static_cast<float>(Num(v,L"motionTime")));
                        c->PushAxisAlignedClip(desktop.contentRect,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        for(int i=0;i<static_cast<int>(plan.order.size());i++){auto found=std::find_if(tiles.begin(),tiles.end(),[&](auto const &t){return t.key==plan.order[i];});int j=static_cast<int>(found-tiles.begin());auto a=FolderCellBounds(desktop.contentRect,desktop.columns,0,j),b=FolderCellBounds(desktop.contentRect,desktop.columns,0,i);D2D1_RECT_F r{a.left+(b.left-a.left)*k,a.top+(b.top-a.top)*k,a.right+(b.right-a.right)*k,a.bottom+(b.bottom-a.bottom)*k};desktop.PaintTile(c,*found,r,i,true);}
                        c->PopAxisAlignedClip();desktop.folderAvoidance.reset();
                    }
                }else{for(auto const &tile:app.model.boards[0].tiles)desktop.PaintTile(c,tile,desktop.RootBounds(tile.key),0);desktop.PaintPages(c);}
            }else{
                if(auto defs=v.GetNamedObject(L"data").TryLookup(L"cardDefinitions");defs&&defs.ValueType()==JsonValueType::Array)app.model.cardDefinitions=defs.GetArray();
                NativePopup popup(app);popup.kind=kind;popup.width=w;popup.height=h;popup.dark=dark;popup.materialPreview=nominal;popup.packet=v;popup.visibleRows=static_cast<int>((h-20)/32);
                popup.caption=Str(v.GetNamedObject(L"data"),L"title");popup.year=2026;popup.month=10;popup.selectedYear=2026;popup.selectedMonth=10;popup.selectedDay=21;
                popup.quickDevices=Bool(v,L"quickDevices");popup.scroll=static_cast<int>(Num(v,L"scroll"));popup.BuildRows();popup.focus=static_cast<int>(Num(v,L"focus",-1));popup.hover=static_cast<int>(Num(v,L"hover",-1));
                popup.armed=Str(v,L"armed");if(!popup.armed.empty())popup.armedUntil=GetTickCount64()+static_cast<ULONGLONG>(Num(v,L"confirmationSeconds",3))*1000;
                int i=0;for(auto const &r:popup.rows)if(r.id.rfind(L"app:",0)==0||r.control==L"tray"||r.id.rfind(L"window:",0)==0){
                    auto icon=GalleryStockIcon(app.graphics,i++%3==0?SIID_FOLDER:i%2?SIID_APPLICATION:SIID_DOCNOASSOC);popup.appearanceIcons.bytes+=icon.bytes;popup.appearanceIcons.values[r.id]=std::move(icon);
                }
                popup.PaintPreview70(c);
            }
            if(layer)c->PopLayer();c->SetTransform(D2D1::Matrix3x2F::Identity());
            if(dissolveContent){c->SetTarget(originalTarget.get());check_hresult(dissolveContent->Close());GalleryDissolve(c,dissolveContent.get(),w,h,static_cast<float>(Num(v,L"canvasX")),static_cast<float>(Num(v,L"canvasY")),static_cast<float>(Num(v,L"motionTime",1)));}
        });
    }
    return 0;
}
