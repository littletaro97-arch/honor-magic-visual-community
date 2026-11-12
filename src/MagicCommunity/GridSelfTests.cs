using System.IO;
using System.Text.Json;

namespace MagicCommunity;
public static class GridSelfTests
{
    public static void Run(string folder,Action<string,Action> check,Action<bool,string> expect)
    {
        var item=new DesktopItem{Name="Grid app",Target=Path.Combine(folder,"grid-app.lnk")};
        LayoutState Fixture()=>new(){Boards=[new(){Id="one",Boxes=[new(){Id="folder",Name="Folder",Items=[item]}],RootItems=[new(){Name="Other",Target=Path.Combine(folder,"other.txt")}],Grid=new(){Columns=10,Rows=12}},new(){Id="two",Grid=new(){Columns=6,Rows=8}}]};
        LayoutState Edit(LayoutState source,Action<LayoutState> edit){var draft=StateStore.Copy(source);edit(draft);StateStore.Validate(draft);return draft;}
        check("folder-count-default-persistence-copy-and-screen-move",()=>
        {
            expect(JsonSerializer.Deserialize<Box>("{\"Name\":\"Legacy\"}")!.ShowItemCount,"legacy folders must retain count");
            var source=Fixture();DesktopGridActions.ReconcileBoard(source.Boards[0]);
            var edited=Edit(source,draft=>DesktopGridActions.SetFolderCount(draft.Boards[0],"folder",false));
            expect(source.Boards[0].Boxes[0].ShowItemCount&&!edited.Boards[0].Boxes[0].ShowItemCount,"toggle must not mutate old revision");
            var restored=JsonSerializer.Deserialize<LayoutState>(JsonSerializer.Serialize(edited))!;
            DesktopGridActions.MoveScreen(restored.Boards[0],restored.Boards[1],"folder:folder");
            expect(!restored.Boards[1].Boxes[0].ShowItemCount,"screen move lost count preference");
            DesktopGridActions.SetFolderCount(restored.Boards[1],"folder",true);expect(restored.Boards[1].Boxes[0].ShowItemCount,"count could not be restored");
            try{DesktopGridActions.SetFolderCount(restored.Boards[0],"missing",false);throw new Exception("stale folder accepted");}catch(LayoutEditException){}
        });
        check("grid-integer-collision-overflow-and-failure-atomicity",()=>
        {
            var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);var before=JsonSerializer.Serialize(state);
            foreach(var position in new[]{(2,0),(-1,2),(int.MaxValue,2),(1,int.MaxValue)})
            {
                try{Edit(state,draft=>DesktopGridActions.Place(draft.Boards[0],"folder:folder",position.Item1,position.Item2));throw new Exception("invalid grid placement accepted");}catch(LayoutEditException){}
                expect(JsonSerializer.Serialize(state)==before,"failed transaction changed original");
            }
        });
        check("grid-four-columns-two-rows-and-square-footprints",()=>
        {
            var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);DesktopGridActions.Place(state.Boards[0],"folder:folder",0,5);
            foreach(var size in new[]{(2,2),(4,2),(4,4)})
            {
                var next=Edit(state,draft=>DesktopGridActions.Resize(draft.Boards[0],"folder",size.Item1,size.Item2));var rect=next.Boards[0].Grid!.Placements["folder:folder"];
                expect(rect.X==0&&rect.Y==5&&rect.W==size.Item1&&rect.H==size.Item2,"size or anchor changed");
                if(size.Item1==size.Item2)expect(rect.W*88+(rect.W-1)*18==rect.H*88+(rect.H-1)*18,"square not square");
            }
        });
        check("grid-legacy-tall-folder-corrected-without-overlap-or-neighbour-move",()=>
        {
            var state=Fixture();var board=state.Boards[0];DesktopGridActions.ReconcileBoard(board);
            var neighbour=board.Grid!.Placements[DesktopGridActions.ItemKey(board.RootItems[0])];
            board.Boxes[0].Columns=2;board.Boxes[0].Rows=4;board.Grid.Placements["folder:folder"]=new(0,5,2,4);
            DesktopGridActions.ReconcileBoard(board);expect(board.Grid.Placements["folder:folder"]==new GridRect(0,5,4,2),"wide orientation not restored at free anchor");
            expect(board.Grid.Placements[DesktopGridActions.ItemKey(board.RootItems[0])]==neighbour,"neighbour shifted");StateStore.Validate(state);
            // If widening collides, only the corrected folder gets a new free integer footprint.
            board.Boxes[0].Columns=2;board.Boxes[0].Rows=4;board.Grid.Placements["folder:folder"]=new(0,5,2,4);
            DesktopGridActions.Place(board,DesktopGridActions.ItemKey(board.RootItems[0]),3,5);neighbour=board.Grid.Placements[DesktopGridActions.ItemKey(board.RootItems[0])];
            DesktopGridActions.ReconcileBoard(board);StateStore.Validate(state);expect(board.Grid.Placements[DesktopGridActions.ItemKey(board.RootItems[0])]==neighbour,"collision migration shifted neighbour");
        });
        check("grid-dissolve-and-cross-screen-keep-reference",()=>
        {
            var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);
            state=Edit(state,draft=>DesktopGridActions.MoveScreen(draft.Boards[0],draft.Boards[1],"folder:folder"));
            expect(state.Boards[0].Boxes.Count==0&&state.Boards[1].Boxes[0].Items[0].Target==item.Target,"cross-screen lost folder");
            state=Edit(state,draft=>DesktopGridActions.Dissolve(draft.Boards[1],"folder"));
            expect(state.Boards[1].Boxes.Count==0&&state.Boards[1].RootItems.Single().Target==item.Target,"dissolve lost reference");
            expect(!File.Exists(item.Target),"test unexpectedly touched reference target");
        });
        check("grid-schema1-migration-keeps-immutable-source",()=>
        {
            var store=new StateStore(Path.Combine(folder,"grid-migration"));Directory.CreateDirectory(store.DirectoryPath);
            string source="{\"SchemaVersion\":1,\"SoftwareRendering\":true,\"Boards\":[{\"Id\":\"one\",\"Name\":\"Existing\",\"Boxes\":[{\"Id\":\"keep-id\",\"Name\":\"Existing folder\",\"Collapsed\":true,\"Items\":[]}]}]}";
            File.WriteAllText(store.FilePath,source);var state=store.Load();expect(store.NeedsMigration&&!store.ReadOnly&&state.SchemaVersion==4,"legacy load failed");expect(File.ReadAllText(store.FilePath)==source,"read performed migration write");
            DesktopGridActions.Initialize(state.Boards[0],1200,900);store.Save(state);
            var copy=Directory.GetFiles(store.DirectoryPath,"layout.json.before-unified-v4-*.json");expect(copy.Length==1&&File.ReadAllText(copy[0])==source,"migration did not preserve exact v1 bytes");
            expect(store.Load().Boards[0].Boxes[0].Id=="keep-id","folder identity changed");store.Save(state);expect(Directory.GetFiles(store.DirectoryPath,"layout.json.before-unified-v4-*.json").Length==1,"backup replaced or copied repeatedly");
        });
        check("grid-reconcile-preserves-disconnected-and-dpi-coordinates",()=>
        {
            var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);DesktopGridActions.Place(state.Boards[0],"folder:folder",3,5);var before=state.Boards[0].Grid!.Placements["folder:folder"];
            DesktopGridActions.Initialize(state.Boards[0],500,400);expect(state.Boards[0].Grid!.Placements["folder:folder"]==before&&state.Boards[0].Grid!.Columns==10,"viewport/DPI reflowed logical grid");
            DesktopGridActions.Reconcile(state,[],[]);expect(state.Boards[0].Grid!.Placements["folder:folder"]==before,"disconnected board moved");
        });
        check("grid-reference-membership-moves-and-collision-failure",()=>
        {
            var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);var other=state.Boards[0].RootItems[0];
            state=Edit(state,draft=>DesktopGridActions.MoveReference(draft,draft.Boards[0],other,null,"folder"));expect(state.Boards[0].RootItems.Count==0&&state.Boards[0].Boxes[0].Items.Count==2,"root to folder failed");
            var before=JsonSerializer.Serialize(state);try{Edit(state,draft=>DesktopGridActions.MoveReference(draft,draft.Boards[0],other,"folder",null,0,0));throw new Exception("occupied drop accepted");}catch(LayoutEditException){}
            expect(JsonSerializer.Serialize(state)==before,"failed drop changed source");
            state=Edit(state,draft=>DesktopGridActions.MoveReference(draft,draft.Boards[0],other,"folder",null,3,4));expect(state.Boards[0].Grid!.Placements[DesktopGridActions.ItemKey(other)]==new GridRect(3,4,1,1),"drop did not snap to requested cell");
        });
        check("grid-downgrade-preserves-latest-references-without-writing-source",()=>
        {
            var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);var before=JsonSerializer.Serialize(state);
            using var document=JsonDocument.Parse(LayoutCompatibility.ExportV1(state));var legacy=document.RootElement;
            expect(legacy.GetProperty("SchemaVersion").GetInt32()==1,"wrong downgrade schema");
            var board=legacy.GetProperty("Boards")[0];expect(!board.TryGetProperty("Grid",out _)&&!board.TryGetProperty("RootItems",out _),"v2 data leaked to legacy");
            var targets=board.GetProperty("Boxes").EnumerateArray().SelectMany(box=>box.GetProperty("Items").EnumerateArray()).Select(row=>row.GetProperty("Target").GetString()).ToArray();
            expect(targets.Length==2&&targets.Contains(item.Target)&&targets.Contains(state.Boards[0].RootItems[0].Target),"downgrade lost reference");
            expect(JsonSerializer.Serialize(state)==before,"export changed source");
        });
        check("grid-locked-layout-reports-path-and-retains-data",()=>
        {
            var store=new StateStore(Path.Combine(folder,"locked-grid"));var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);store.Save(state);var bytes=File.ReadAllBytes(store.FilePath);
            using(var held=new FileStream(store.FilePath,FileMode.Open,FileAccess.Read,FileShare.None))
            {
                try{store.Save(state);throw new Exception("locked file overwritten");}catch(IOException ex){expect(ex.Message.Contains(store.FilePath),"lock error did not name target file");}
            }
            expect(File.ReadAllBytes(store.FilePath).SequenceEqual(bytes),"lock failure changed file");store.Save(state);expect(!store.ReadOnly,"released lock did not recover");
        });
        check("accessory-schema2-keeps-references-without-projecting-native-icons",()=>
        {
            var store=new StateStore(Path.Combine(folder,"accessory-migration"));Directory.CreateDirectory(store.DirectoryPath);var state=Fixture();DesktopGridActions.ReconcileBoard(state.Boards[0]);state.SchemaVersion=2;
            string source=JsonSerializer.Serialize(state,StateStore.Json);File.WriteAllText(store.FilePath,source);var loaded=store.Load();expect(!store.ReadOnly&&loaded.SchemaVersion==4&&loaded.Boards[0].RootItems.Count==1,"migration lost reference");
            expect(loaded.Boards[0].Grid!.FoldersOnly&&loaded.Boards[0].Grid!.Placements.Keys.All(key=>key.StartsWith("folder:")),"native icon duplicate still projected");store.Save(loaded);
            expect(Directory.GetFiles(store.DirectoryPath,"layout.json.before-unified-v4-*.json").Any(path=>File.ReadAllText(path)==source),"schema2 snapshot not preserved");
        });
        check("accessory-native-icon-avoidance-and-square-frame",()=>
        {
            var display=new Display("one","One","test",100,50,1000,800,true);var native=new NativeDesktopSnapshot(true,null,0,100,100,[new(100,50,200,150)]);var projection=new AccessoryProjection(0,0,100,100,1,10,8);
            var board=new Board{Id="one",Boxes=[new(){Id="a",Name="a"},new(){Id="b",Name="b"}],Grid=new(){Columns=10,Rows=8,FoldersOnly=true}};DesktopGridActions.ReconcileBoard(board);var neighbour=board.Grid.Placements["folder:b"];
            AccessoryPlacement.Ensure(board,display,projection,native,true);expect(board.Grid.Placements["folder:b"]==neighbour,"native avoidance moved an unrelated folder");expect(board.Boxes.All(box=>AccessoryPlacement.NativeFree(board.Grid.Placements[DesktopGridActions.FolderKey(box)],display,projection,native)),"folder overlaps Windows icon");
            var squareProjection=AccessoryProjection.Create(display,1,native);var square=squareProjection.Frame(new(0,0,2,2),display);expect(square.Right-square.Left==square.Bottom-square.Top,"square projection is not square");
            var before=JsonSerializer.Serialize(board);try{var draft=JsonSerializer.Deserialize<Board>(before)!;DesktopGridActions.Place(draft,"folder:a",0,0);AccessoryPlacement.Ensure(draft,display,projection,native,false);throw new Exception("native overlap accepted");}catch(LayoutEditException){}expect(JsonSerializer.Serialize(board)==before,"failed move changed folder");
        });
        check("unified-projection-footprints-dpi-and-negative-origin",()=>
        {
            var display=new Display("one","One","test",-1600,-586,1600,2400,false);
            foreach(double scale in new[]{.75,1,1.25,1.5,2})
            {
                var projection=AccessoryProjection.Create(display,scale,new(false,"not needed",0,76,112,[]));
                expect(projection.StepX==projection.StepY&&projection.StepX==UnifiedDesktopGrid.Cell*scale,"pitch depends on non-square native spacing");
                foreach(var size in new[]{(2,2),(4,2),(4,4)})
                {
                    var r=new GridRect(1,2,size.Item1,size.Item2);var f=projection.Frame(r,display);
                    expect(f==projection.Frame(r,display),"projection is not deterministic");
                    expect(Math.Abs(f.Right-f.Left-(size.Item1*UnifiedDesktopGrid.Cell-UnifiedDesktopGrid.Inset)*scale)<=1,"visible width differs from footprint inset");
                    expect(Math.Abs(f.Bottom-f.Top-(size.Item2*UnifiedDesktopGrid.Cell-UnifiedDesktopGrid.Inset)*scale)<=1,"visible height differs from footprint inset");
                    if(size.Item1==size.Item2)expect(f.Right-f.Left==f.Bottom-f.Top,"square dimensions differ");
                }
            }
        });
        check("unified-smaller-viewport-bounds-pages-and-keeps-references",()=>
        {
            var state=Fixture();var board=state.Boards[0];DesktopGridActions.ReconcileBoard(board);
            DesktopGridActions.Place(board,"folder:folder",7,5);var original=board.Grid!.Placements["folder:folder"];
            board.Grid.NativeInitialized=true;board.Grid.FoldersOnly=false;
            DesktopPages.Initialize(state,board,new(14,8,96,96,1,6,8),true);
            expect(board.Grid!.Columns==6&&board.Grid.Rows==8&&DesktopPages.Position(board,"folder:folder") is not null&&board.Pages!.All(page=>page.Grid.Placements.Values.All(rect=>rect.X+rect.W<=6&&rect.Y+rect.H<=8)),"DPI/viewport shrink did not bound pages or lost references");
        });
        check("unified-grouping-locked-files-never-moves-or-deletes",()=>
        {
            var state=Fixture();var board=state.Boards[0];DesktopGridActions.ReconcileBoard(board);
            string path=board.RootItems[0].Target;File.WriteAllText(path,"unchanged original");
            var original=board.RootItems[0];
            using(var held=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.None))
            {
                state=Edit(state,draft=>UnifiedDesktopGrid.Collect(draft,draft.Boards[0],"folder",[original]));
                expect(state.Boards[0].RootItems.Count==0&&state.Boards[0].Boxes[0].Items.Any(item=>item.Target==path),"virtual collection failed");
                state=Edit(state,draft=>DesktopGridActions.Dissolve(draft.Boards[0],"folder"));
                expect(state.Boards[0].RootItems.Any(item=>item.Target==path),"dissolve lost original reference");
            }
            expect(File.ReadAllText(path)=="unchanged original","virtual grouping changed original file");
        });
        check("unified-visible-view-filter-keeps-explicit-folder-references",()=>
        {
            var hidden=new DesktopItem{Name="Videos",Target=@"C:\Users\test\Videos"};var visible=new DesktopItem{Name="Keep",Target=@"C:\Desktop\keep.lnk"};
            var board=new Board{Id="one",RootItems=[hidden,visible],Boxes=[new(){Id="keep-group",Items=[hidden]}],Grid=new(){Columns=10,Rows=8}};
            var state=new LayoutState{Boards=[board]};UnifiedDesktopGrid.PruneUnshownRoots(state,new HashSet<string>(StringComparer.OrdinalIgnoreCase){hidden.Target});
            expect(board.RootItems.Count==1&&board.RootItems[0]==visible,"hidden namespace root remains");
            expect(board.Boxes[0].Items.Single()==hidden,"explicit folder content was lost");
        });
        check("unified-cross-display-mixed-dpi-scroll-offset",()=>
        {
            var source=new Display("one","One","test",0,64,3072,1760,true);var target=new Display("two","Two","test",3072,-586,1600,2400,false);
            var point=UnifiedDesktopGrid.ProjectDrop(source,1.5,2228,-252.5,target,1.25,96,192,10,17);
            expect(point.Column==2&&point.Row==4,"negative-origin mixed-DPI drop missed the centered projection");
        });
        check("unified-cross-display-transfer-and-collision-rollback",()=>
        {
            var state=Fixture();var source=state.Boards[0];DesktopGridActions.ReconcileBoard(source);
            var destination=state.Boards[1];DesktopGridActions.ReconcileBoard(destination);
            var item=source.RootItems[0];string key=DesktopGridActions.ItemKey(item);
            state=Edit(state,draft=>UnifiedDesktopGrid.Transfer(draft,draft.Boards[0],draft.Boards[1],key,null,3,3));
            expect(state.Boards[0].RootItems.Count==0&&state.Boards[1].Grid!.Placements[key]==new GridRect(3,3,1,1),"cross-screen item anchor lost");
            state=Edit(state,draft=>UnifiedDesktopGrid.Transfer(draft,draft.Boards[0],draft.Boards[1],"folder:folder",null,3,4));
            state=Edit(state,draft=>UnifiedDesktopGrid.Transfer(draft,draft.Boards[1],draft.Boards[0],"folder:folder",null,0,0));
            string before=JsonSerializer.Serialize(state);
            try{Edit(state,draft=>UnifiedDesktopGrid.Transfer(draft,draft.Boards[1],draft.Boards[0],key,null,0,0));throw new Exception("overlap accepted");}catch(LayoutEditException){}
            expect(JsonSerializer.Serialize(state)==before,"failed cross-screen move changed state");
        });
        check("pages-v6-shrink-migration-preserves-identities-settings-and-immutable-backup",()=>
        {
            var store=new StateStore(Path.Combine(folder,"pages-v6"));
            var board=new Board{Id="paged",Name="Main",Grid=new(){Columns=21,Rows=12,NativeInitialized=true},RootItems=Enumerable.Range(0,190).Select(i=>new DesktopItem{Name="Item "+i,Target=Path.Combine(folder,"page-item-"+i+".txt")}).ToList(),Cards=[new(){Type="fixture.basic",Columns=4,Rows=2,Settings=JsonSerializer.SerializeToElement(new{text="retain settings"})}]};
            var state=new LayoutState{SchemaVersion=5,Boards=[board]};DesktopGridActions.ReconcileBoard(board);store.Save(state);var bytes=File.ReadAllBytes(store.FilePath);
            var anchors=board.Grid!.Placements.Where(row=>row.Value.X+row.Value.W<=14&&row.Value.Y+row.Value.H<=9).ToDictionary(row=>row.Key,row=>row.Value);
            var projection=AccessoryProjection.Create(new("paged","Main","test",0,0,3072,1824,true),2);
            expect(projection.Columns==14&&projection.Rows==9&&projection.OriginX==192&&projection.OriginY==48,"expected centered 14x9 working area");
            DesktopPages.Initialize(state,board,projection,true);
            expect(board.Pages!.Count>1&&board.Unplaced.Count==0&&board.Pages.Sum(page=>page.Grid.Placements.Count)==191,"overflow lost or duplicated objects");
            expect(anchors.All(row=>board.Pages[0].Grid.Placements.GetValueOrDefault(row.Key)==row.Value),"valid anchor moved");
            store.Save(state);var restored=store.Load();expect(!store.ReadOnly&&restored.SchemaVersion==6&&restored.Boards[0].Cards!.Single().Settings.GetProperty("text").GetString()=="retain settings","v6 restart lost settings");
            var backups=Directory.GetFiles(store.DirectoryPath,"layout.json.before-pages-v6-*.json");expect(backups.Length==1&&File.ReadAllBytes(backups[0]).SequenceEqual(bytes),"v5 source was not preserved exactly");
            DesktopPages.Initialize(state,board,projection,false);store.Save(state);expect(board.Paged&&Directory.GetFiles(store.DirectoryPath,"layout.json.before-pages-v6-*.json").Length==1,"primary change altered page policy or repeated backup");
            StateStore.Validate(restored);
        });
        check("pages-v6-cross-page-group-drop-and-empty-page-deletion",()=>
        {
            var state=Fixture();var board=state.Boards[0];board.RootItems.Add(new(){Name="Group peer",Target=Path.Combine(folder,"page-group-peer.txt")});DesktopPages.Initialize(state,board,new(0,0,96,96,1,6,8),true);
            string sourcePage=board.ActivePageId!,key=DesktopGridActions.ItemKey(board.RootItems[0]),peer=DesktopGridActions.ItemKey(board.RootItems[1]);DesktopPages.Add(board,false);string targetPage=board.ActivePageId!;
            DesktopPages.Drop(state,board,sourcePage,targetPage,[key,peer],key,null,null,100,100);
            expect(!board.Pages![0].Grid.Placements.ContainsKey(key)&&board.Grid!.Placements[key]==new GridRect(4,7,1,1)&&board.Grid.Placements[peer]==new GridRect(5,7,1,1),"nearest valid target or source membership incorrect");
            string before=JsonSerializer.Serialize(state);try{DesktopPages.Delete(board,targetPage);throw new Exception("nonempty page deleted");}catch(LayoutEditException){}expect(JsonSerializer.Serialize(state)==before,"rejected delete changed layout");
            DesktopPages.Add(board,false);string empty=board.ActivePageId!;DesktopPages.Delete(board,empty);expect(board.ActivePageId==targetPage&&board.Pages.Count==2,"empty deletion did not retain active neighbour");
            DesktopPages.Select(board,sourcePage);DesktopPages.Drop(state,board,targetPage,sourcePage,[key,peer],key,null,"folder",0,0);
            expect(board.RootItems.Count==0&&board.Boxes.Single().Items.Count==3&&DesktopPages.Position(board,key) is null,"cross-page collection lost membership");StateStore.Validate(state);
            var duplicate=new Box{Name="Duplicate",Items=[board.Boxes[0].Items.First(item=>DesktopGridActions.ItemKey(item)==key)]};board.Boxes.Add(duplicate);DesktopGridActions.ReconcileBoard(board);DesktopPages.Select(board,targetPage);before=JsonSerializer.Serialize(state);
            try{DesktopPages.Drop(state,board,sourcePage,targetPage,[key],key,"folder",null,0,0);throw new Exception("duplicate folder reference exposed as root");}catch(LayoutEditException){}expect(JsonSerializer.Serialize(state)==before,"rejected cross-page folder-out mutated references");
        });
        check("pages-v6-secondary-capacity-recovery-and-role-stability",()=>
        {
            var board=new Board{Id="secondary",Name="Secondary",RootItems=Enumerable.Range(0,80).Select(i=>new DesktopItem{Name="S "+i,Target=Path.Combine(folder,"secondary-"+i+".lnk")}).ToList()};
            var state=new LayoutState{Boards=[board]};DesktopPages.Initialize(state,board,new(0,0,96,96,1,8,6),false);
            expect(board.Pages!.Count==1&&!board.Paged&&board.Grid!.Placements.Count==48&&board.Unplaced.Count==32,"secondary silently grew or lost overflow");
            DesktopPages.Initialize(state,board,new(0,0,96,96,1,8,12),true);
            expect(!board.Paged&&board.Pages.Count==1&&board.Unplaced.Count==0&&board.Grid!.Placements.Count==80,"growth did not restore retained items or primary switch changed policy");StateStore.Validate(state);
        });
        check("paste-batch-reserves-pointer-anchor-before-provisional-neighbours",()=>
        {
            var board=new Board{Id="paste"};var state=new LayoutState{Boards=[board]};var projection=new AccessoryProjection(0,0,96,96,1,8,8);
            DesktopPages.Initialize(state,board,projection,true);
            var incoming=Enumerable.Range(0,3).Select(i=>new DesktopItem{Name="Paste "+i,Target=Path.Combine(folder,"paste-"+i+".txt")}).ToArray();
            DesktopCreation.PlaceItems(state,board,incoming,projection,(4,3));
            for(int i=0;i<3;i++)expect(board.Grid!.Placements[DesktopGridActions.ItemKey(incoming[i])]==new GridRect(4+i,3,1,1),"batch displaced pointer anchor or source order");
            StateStore.Validate(state);
        });
        check("paste-occupied-cell-is-nearby-and-origin-page-is-retained",()=>
        {
            var fixedItem=new DesktopItem{Name="Keep",Target=Path.Combine(folder,"keep-paste.txt")};var board=new Board{Id="paste",RootItems=[fixedItem]};var state=new LayoutState{Boards=[board]};var projection=new AccessoryProjection(0,0,96,96,1,8,8);
            DesktopPages.Initialize(state,board,projection,true);DesktopGridActions.Place(board,DesktopGridActions.ItemKey(fixedItem),4,3);string origin=board.ActivePageId!;
            DesktopPages.Add(board,false);string active=board.ActivePageId!;var incoming=new DesktopItem{Name="Pasted",Target=Path.Combine(folder,"near-paste.txt")};
            DesktopPages.OnSourcePage(board,origin,()=>DesktopCreation.PlaceItems(state,board,[incoming],projection,(4,3)));
            var page=board.Pages!.Single(p=>p.Id==origin);var rect=page.Grid.Placements[DesktopGridActions.ItemKey(incoming)];
            expect(Math.Abs(rect.X-4)+Math.Abs(rect.Y-3)==1,"occupied target jumped to first cell");expect(board.ActivePageId==active,"paste switched visible page");
            expect(page.Grid.Placements[DesktopGridActions.ItemKey(fixedItem)]==new GridRect(4,3,1,1),"paste moved existing neighbour");StateStore.Validate(state);
        });
        check("pages-projection-wide-and-narrow-working-areas-stay-bounded",()=>
        {
            foreach(var sample in new[]{(3840,2112,1.0),(3440,1392,1.0),(1600,2464,2.0),(300,640,1.0),(1920,1020,1.25)})
            {
                var projection=AccessoryProjection.Create(new("test","Test","test",-100,50,sample.Item1,sample.Item2,false),sample.Item3);
                expect(projection.OriginX>=0&&projection.OriginY>=0&&projection.OriginX+projection.Columns*projection.StepX<=sample.Item1&&projection.OriginY+projection.Rows*projection.StepY<=sample.Item2,"projection exceeded working area");
                if(sample.Item1==3840)expect(projection.Columns>32,"wide screen retained old 32-column ceiling");
                if(sample.Item1==300)expect(projection.StepX==80,"narrow working area did not use smaller cells");
            }
        });
    }
}
