using System.IO;
using System.Text.Json;

namespace MagicCommunity;

// Owned state and files only. Never reads/writes the installed desktop layout.
internal static class DesktopDisplayRecoveryTests
{
    internal static object Run(string root)
    {
        string folder = Path.Combine(root, "artifacts", "display-recovery-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        var results = new List<object>();
        void Expect(bool value, string reason) { if (!value) throw new InvalidOperationException(reason); }
        void Check(string name, Action test) { test(); results.Add(new { name, pass = true }); }
        Display Screen(string id, bool primary = false, string hardware = "hardware-b", uint dpi = 96, int width = 1920) =>
            new(id, primary ? "主屏" : "副屏", id, 0, 0, width, 1080, primary) { PixelWidth = width, PixelHeight = 1080, Dpi = dpi, HardwareIdentity = hardware };
        var primary = Screen("a", true, "hardware-a"); var secondary = Screen("b");
        LayoutState Fixture()
        {
            var state = new LayoutState { SchemaVersion = 7, Boards = [new() { Id = "a", Name = "主屏" }, new() { Id = "b", Name = "副屏" }] };
            var b = state.Boards[1];
            b.RootItems.Add(new() { Name = "source.txt", Target = Path.Combine(folder, "source.txt"), FromDesktop = true });
            b.Boxes.Add(new() { Name = "工作", Items = [new() { Name = "nested.txt", Target = Path.Combine(folder, "nested.txt") }] });
            b.Cards = [new() { Type = "system.todo", Columns = 4, Rows = 2, Settings = JsonSerializer.SerializeToElement(new { text = "retain setting" }) }];
            foreach (var board in state.Boards) DesktopDisplayRecovery.Pack(board, board.Id == "a" ? primary : secondary, []);
            b.Grid!.Placements[DesktopGridActions.ItemKey(b.RootItems[0])] = new(7, 5, 1, 1);
            DesktopDisplayRecovery.Reconcile(state, [primary, secondary]); StateStore.Validate(state); return state;
        }
        LayoutState Disconnected() { var state = Fixture(); DesktopDisplayRecovery.Reconcile(state, [primary]); return state; }
        string Anchors(Board b) => JsonSerializer.Serialize(b.Pages, StateStore.Json);
        void Valid(LayoutState state) { StateStore.Validate(state); foreach (var b in state.Boards.Where(b => b.Grid is not null)) Expect(DesktopGridActions.Expected(b).Count == DesktopPages.Grids(b).Sum(g => g.Placements.Count) + b.Unplaced.Count, "Content lost"); }
        File.WriteAllText(Path.Combine(folder, "source.txt"), "original bytes"); File.WriteAllText(Path.Combine(folder, "nested.txt"), "nested bytes");

        foreach (bool restart in new[] { false, true })
            Check("external-only-primary-resume-retains-content-after-missing-edid-and-connector-change-restart-" + restart, () => {
                var state = Fixture(); var board = state.Boards[1];
                board.IconGrid = new() { Columns = board.Grid!.Columns, Rows = board.Grid.Rows, RefWidth = 1920, RefHeight = 1080 };
                board.SortSettings = new() { Mode = "custom" };
                board.Paged = true; DesktopPages.Add(board, false);
                var expected = StateStore.Copy(state).Boards[1];
                var external = secondary with { Primary = true, Label = "主屏", Device = "DISPLAY2" };
                DesktopDisplayRecovery.Reconcile(state, [external]); // Windows: show only on display 2.
                for (int cycle = 0; cycle < 3; cycle++) {
                    DesktopDisplayRecovery.Reconcile(state, []);
                    DesktopDisplayRecovery.Reconcile(state, [external with { HardwareIdentity = "", Dpi = 0, Device = "DISPLAY1" }]);
                    if (restart) {
                        var store = new StateStore(Path.Combine(folder, "external-resume-" + cycle));
                        store.Save(state); state = store.Load();
                    }
                    external = external with { Id = "external-resumed-" + cycle, Device = "DISPLAY1", Dpi = 144 };
                    DesktopDisplayRecovery.Reconcile(state, [external]);
                    expected.Id = external.Id;
                    Expect(state.Boards.Count == 2 && !DesktopDisplayRecovery.Offline(state, [external]).Any(), "External primary content was stranded in offline recovery");
                    Expect(JsonSerializer.Serialize(state.Boards.Single(b => b.Id == external.Id), StateStore.Json) == JsonSerializer.Serialize(expected, StateStore.Json), "External primary pages, files, cards or custom layout changed");
                    Valid(state);
                }
                Expect(File.ReadAllText(Path.Combine(folder, "source.txt")) == "original bytes" && File.ReadAllText(Path.Combine(folder, "nested.txt")) == "nested bytes", "Resume changed file bytes");
            });

        Check("late-edid-rejoins-unmodified-empty-primary-after-save-and-restart", () => {
            var state = Fixture(); var expected = StateStore.Copy(state).Boards[1];
            var pending = secondary with { Id = "external-pending", Primary = true, HardwareIdentity = "" };
            DesktopDisplayRecovery.Reconcile(state, [pending]);
            var empty = state.Boards.Single(b => b.Id == pending.Id);
            DesktopDisplayRecovery.Pack(empty, pending, []); // The renderer has already initialized an empty page.
            var store = new StateStore(Path.Combine(folder, "late-edid")); store.Save(state); state = store.Load();
            var verified = pending with { HardwareIdentity = secondary.HardwareIdentity };
            DesktopDisplayRecovery.Reconcile(state, [verified]); expected.Id = verified.Id;
            Expect(state.Boards.Count == 2 && !DesktopDisplayRecovery.Offline(state, [verified]).Any(), "Late EDID left the external primary content in recovery");
            Expect(JsonSerializer.Serialize(state.Boards.Single(b => b.Id == verified.Id), StateStore.Json) == JsonSerializer.Serialize(expected, StateStore.Json), "Late EDID changed saved primary layout");
            Valid(state);
        });
        foreach (string edit in new[] { "file", "empty-folder", "card", "page", "grid", "sort", "name", "duplicate-identity" })
            Check("late-edid-never-overwrites-active-layout-" + edit, () => {
                var state = Fixture(); var pending = secondary with { Id = "external-pending", Primary = true, HardwareIdentity = "" };
                DesktopDisplayRecovery.Reconcile(state, [pending]); var board = state.Boards.Single(b => b.Id == pending.Id);
                DesktopDisplayRecovery.Pack(board, pending, []);
                switch (edit) {
                    case "file": board.RootItems.Add(new() { Name = "new-owned", Target = Path.Combine(folder, "new-owned") }); break;
                    case "empty-folder": board.Boxes.Add(new() { Name = "keep-empty" }); break;
                    case "card": board.Cards = [new() { Type = "system.clock", Columns = 2, Rows = 2 }]; break;
                    case "page": board.Paged = true; DesktopPages.Add(board, false); break;
                    case "grid": board.IconGrid = new() { Columns = board.Grid!.Columns, Rows = board.Grid.Rows, RefWidth = 1920, RefHeight = 1080 }; break;
                    case "sort": board.SortSettings = new() { Mode = "custom" }; break;
                    case "name": board.Name = "user name"; break;
                    case "duplicate-identity": state.Displays!.Single(m => m.Id == "a").Hardware = secondary.HardwareIdentity; break;
                }
                DesktopGridActions.ReconcileBoard(board); string before = JsonSerializer.Serialize(board, StateStore.Json);
                DesktopDisplayRecovery.Reconcile(state, [pending with { HardwareIdentity = secondary.HardwareIdentity }]);
                Expect(state.Boards.Count == 3 && JsonSerializer.Serialize(state.Boards.Single(b => b.Id == pending.Id), StateStore.Json) == before && state.Boards.Any(b => b.Id == "b"), "Late identity overwrote an edited or ambiguous board");
                Valid(state);
            });

        Check("disabled-internal-panel-does-not-supply-active-monitor-identity", () => {
            var devices = new (string Id, uint Flags)[] { ("internal-disabled", 0), ("external-active", 1) };
            int reads = 0;
            var id = Displays.ReadActiveIdentity(i => { reads++; return i < devices.Length ? devices[i] : null; });
            Expect(id == "external-active" && reads == 2, "Disabled index zero was mistaken for the external screen");
            Array.Reverse(devices);
            Expect(Displays.ReadActiveIdentity(i => i < devices.Length ? devices[i] : null) == id, "Enumeration order changed active identity");
        });
        Check("missing-active-identity-defers-instead-of-using-disabled-panel", () => {
            Expect(Displays.ReadActiveIdentity(i => i == 0 ? ("internal-disabled", 0U) : null) is null, "Inactive panel used as fallback");
            Expect(Displays.ReadActiveIdentity(i => i == 0 ? ("", 1U) : ("internal-disabled", 0U)) is null, "Blank active identity used a different monitor");
            int reads = 0;
            Expect(Displays.ReadActiveIdentity(i => { reads++; return ("disabled", 0U); }) is null && reads == 64, "Malformed enumeration was unbounded");
            Expect(Displays.ReadActiveIdentity(i => i == 0 ? ("clone-first-active", 1U) : ("clone-second-active", 1U)) == "clone-first-active", "Clone view changed the Windows monitor order");
        });
        Check("incomplete-resume-snapshots-retain-previous-topology-and-layout", () => {
            var state = Fixture(); string before = JsonSerializer.Serialize(state, StateStore.Json);
            IReadOnlyList<Display> accepted = [primary, secondary];
            IReadOnlyList<Display>[] incomplete = [[], [primary, secondary with { Id = "temporary:DISPLAY2" }],
                [primary, secondary with { Id = "A" }], [primary, secondary with { Primary = true }],
                [secondary], [primary with { Width = 0 }]];
            foreach (var snapshot in incomplete) {
                if (Displays.CanReconcile(snapshot)) { DesktopDisplayRecovery.Reconcile(state, snapshot); accepted = snapshot; }
                Expect(JsonSerializer.Serialize(state, StateStore.Json) == before && accepted.Count == 2 && accepted[0] == primary, "Partial wake topology changed the accepted layout");
            }
            Expect(Displays.CanReconcile([secondary with { Primary = true }]), "External-only topology rejected");
            Expect(Displays.CanReconcile([secondary with { Primary = true }, primary with { Primary = false }]), "Primary role switch rejected");
        });
        Check("external-primary-recovery-does-not-adopt-disabled-internal-content", () => {
            var state = Fixture(); var internalBoard = state.Boards[0];
            internalBoard.RootItems.Add(new() { Name = "laptop-only", Target = Path.Combine(folder, "laptop-only") });
            DesktopGridActions.ReconcileBoard(internalBoard);
            string internalBefore = JsonSerializer.Serialize(internalBoard, StateStore.Json), externalPages = Anchors(state.Boards[1]);
            var external = secondary with { Primary = true };
            DesktopDisplayRecovery.Reconcile(state, [external]);
            DesktopDisplayRecovery.Reconcile(state, []);
            DesktopDisplayRecovery.Reconcile(state, [external with { HardwareIdentity = "" }]);
            external = external with { Id = "external-reconnected" };
            DesktopDisplayRecovery.Reconcile(state, [external]);
            Expect(DesktopDisplayRecovery.Offline(state, [external]).Single().Id == primary.Id, "Current external primary was treated as the offline panel");
            Expect(JsonSerializer.Serialize(internalBoard, StateStore.Json) == internalBefore && Anchors(state.Boards.Single(b => b.Id == external.Id)) == externalPages, "Primary role swap transferred content between monitors");
            Valid(state);
        });
        Check("same-interface-missing-edid-preserves-both-primary-role-directions", () => {
            foreach (bool wasPrimary in new[] { false, true }) {
                var state = Fixture(); var board = state.Boards[1]; string before = JsonSerializer.Serialize(board, StateStore.Json);
                DesktopDisplayRecovery.Reconcile(state, [primary with { Primary = !wasPrimary }, secondary with { Primary = wasPrimary }]);
                DesktopDisplayRecovery.Reconcile(state, []);
                DesktopDisplayRecovery.Reconcile(state, [primary with { Primary = wasPrimary }, secondary with { Primary = !wasPrimary, HardwareIdentity = "" }]);
                Expect(JsonSerializer.Serialize(board, StateStore.Json) == before && state.Displays!.Single(m => m.Id == "b").Hardware == "hardware-b", "Role switch or missing EDID reset the saved layout/identity");
                Valid(state);
            }
        });
        Check("same-interface-without-serial-from-first-use-retains-external-primary-layout", () => {
            var state = Fixture(); var board = state.Boards[1]; string before = JsonSerializer.Serialize(board, StateStore.Json);
            state.Displays!.Single(m => m.Id == "b").Hardware = "";
            DesktopDisplayRecovery.Reconcile(state, []);
            DesktopDisplayRecovery.Reconcile(state, [secondary with { Primary = true, HardwareIdentity = "" }]);
            Expect(JsonSerializer.Serialize(board, StateStore.Json) == before && !DesktopDisplayRecovery.Offline(state, [secondary]).Any(), "Monitor without a serial lost its stable-interface layout");
            Valid(state);
        });
        Check("interface-case-change-without-edid-does-not-create-empty-primary", () => {
            var state = Fixture(); var expected = StateStore.Copy(state).Boards[1]; expected.Id = "B";
            var external = secondary with { Id = "B", Primary = true, HardwareIdentity = "" };
            DesktopDisplayRecovery.Reconcile(state, [external]);
            Expect(state.Boards.Count == 2 && !DesktopDisplayRecovery.Offline(state, [external]).Any() &&
                JsonSerializer.Serialize(state.Boards.Single(b => b.Id == "B"), StateStore.Json) == JsonSerializer.Serialize(expected, StateStore.Json), "Interface casing stranded a layout");
            Valid(state);
        });
        Check("unidentified-replacement-does-not-inherit-old-edid", () => {
            var state = Disconnected();
            DesktopDisplayRecovery.Reconcile(state, [primary, Screen("c", hardware: "")]);
            Expect(state.Displays!.Single(m => m.Id == "c").Hardware == "", "Replacement inherited the previous physical monitor's hash");
            DesktopDisplayRecovery.Reconcile(state, [primary with { Primary = false }, Screen("other", true)]);
            Expect(state.Boards.Any(b => b.Id == "c") && state.Boards.Single(b => b.Id == "other").RootItems.Count == 0, "Unknown replacement was rematched using stale EDID");
            Valid(state);
        });

        Check("new-displays-have-no-generated-default-folders-after-five-cycles", () => {
            var state = new LayoutState();
            for (int i = 0; i < 5; i++) {
                DesktopDisplayRecovery.Reconcile(state, [primary, secondary]);
                Expect(state.Boards.Count == 2 && state.Boards.All(b => b.Boxes.Count == 0), "Default folders regenerated");
                DesktopDisplayRecovery.Reconcile(state, [primary]);
            }
            Valid(state);
        });
        Check("user-created-folder-with-former-default-name-is-not-hidden-or-removed", () => {
            var state = Fixture(); var board = state.Boards[1]; var box = new Box { Name = "常用" }; board.Boxes.Add(box);
            DesktopGridActions.ReconcileBoard(board);
            DesktopDisplayRecovery.Reconcile(state, [primary]); DesktopDisplayRecovery.Reconcile(state, [primary, secondary]);
            Expect(board.Boxes.Contains(box) && DesktopPages.Position(board, DesktopGridActions.FolderKey(box)) is not null, "User folder removed by name"); Valid(state);
        });

        Check("primary-session-return-without-edid-retains-pages-and-active-page",()=>{
            var state=Fixture();var board=state.Boards[0];
            board.Pages!.Add(new(){Grid=new(){Columns=board.Grid!.Columns,Rows=board.Grid.Rows,Bounded=true,NativeInitialized=true}});
            board.ActivePageId=board.Pages[1].Id;board.Paged=true;
            string before=JsonSerializer.Serialize(board,StateStore.Json);
            DesktopDisplayRecovery.Reconcile(state,[]);
            DesktopDisplayRecovery.Reconcile(state,[primary with{HardwareIdentity=""},secondary]);
            Expect(JsonSerializer.Serialize(board,StateStore.Json)==before,"Session return repacked primary pages");Valid(state);
        });
        Check("disconnect-retains-all-pages-and-reference-bytes", () => { var s = Fixture(); string before = Anchors(s.Boards[1]); DesktopDisplayRecovery.Reconcile(s, [primary]); Expect(Anchors(s.Boards[1]) == before && DesktopDisplayRecovery.Offline(s, [primary]).Count() == 1, "Disconnect moved content"); Valid(s); });
        Check("same-hardware-resolution-dpi-restores-exact-anchors", () => { var s = Disconnected(); string before = Anchors(s.Boards[1]); DesktopDisplayRecovery.Reconcile(s, [primary, secondary]); Expect(Anchors(s.Boards[1]) == before && !DesktopDisplayRecovery.Offline(s, [primary, secondary]).Any(), "Exact restore changed anchors"); Valid(s); });
        Check("confirmed-hardware-replacement-still-uses-default-order", () => { var s = Disconnected(); var key = DesktopGridActions.ItemKey(s.Boards[1].RootItems[0]); DesktopDisplayRecovery.Reconcile(s, [primary, secondary with { HardwareIdentity = "new-monitor" }]); Expect(DesktopPages.Position(s.Boards[1], key) != new GridRect(7, 5, 1, 1), "Changed display restored stale anchor"); Valid(s); });
        Check("confirmed-primary-hardware-replacement-does-not-use-primary-role-as-identity", () => {
            var state = Fixture(); var board = state.Boards[1]; var key = DesktopGridActions.ItemKey(board.RootItems[0]);
            DesktopDisplayRecovery.Reconcile(state, [secondary with { Primary = true }]);
            DesktopDisplayRecovery.Reconcile(state, [secondary with { Primary = true, HardwareIdentity = "confirmed-replacement" }]);
            Expect(DesktopPages.Position(board, key) != new GridRect(7, 5, 1, 1) && state.Displays!.Single(m => m.Id == "b").Hardware == "confirmed-replacement", "Primary role concealed a known hardware replacement");
            Valid(state);
        });
        foreach(var target in new[]{secondary with{Dpi=144},secondary with{Dpi=0},secondary with{PixelWidth=2560}})
            Check("same-hardware-projection-change-preserves-pages-"+target.Dpi+"-"+target.PixelWidth,()=>{var state=Disconnected();string before=JsonSerializer.Serialize(state.Boards[1],StateStore.Json);DesktopDisplayRecovery.Reconcile(state,[primary,target]);Expect(JsonSerializer.Serialize(state.Boards[1],StateStore.Json)==before,"Same monitor geometry repacked layout");Valid(state);});
        Check("three-primary-resume-cycles-retain-pages-icons-cards-and-custom-grid",()=>{
            var state=Fixture();var board=state.Boards[0];board.Paged=true;
            board.IconGrid=new(){Columns=board.Grid!.Columns,Rows=board.Grid.Rows,RefWidth=1920,RefHeight=1080};board.SortSettings=new(){Mode="custom"};
            board.RootItems.Add(new(){Name="primary-owned",Target=Path.Combine(folder,"primary-owned")});
            board.Grid.Placements[DesktopGridActions.ItemKey(board.RootItems[0])]=new(7,5,1,1);
            board.Cards=[new(){Type="system.clock",Columns=2,Rows=2}];
            DesktopPages.Add(board,false);board.Grid!.Placements[DesktopCards.Key(board.Cards[0])]=new(3,4,2,2);
            string before=JsonSerializer.Serialize(board,StateStore.Json);
            for(int cycle=0;cycle<3;cycle++){
                DesktopDisplayRecovery.Reconcile(state,[]);
                var transient=primary with{HardwareIdentity="",Dpi=0,PixelWidth=1024,PixelHeight=768};
                DesktopDisplayRecovery.Reconcile(state,[transient,secondary]);
                DesktopGridLayout.ResizeWithoutMoving(board,DesktopGridLayout.Project(board,transient,1));
                DesktopDisplayRecovery.Reconcile(state,[primary,secondary]);
                DesktopGridLayout.ResizeWithoutMoving(board,DesktopGridLayout.Project(board,primary,1));
                Expect(JsonSerializer.Serialize(board,StateStore.Json)==before,"Resume geometry overwrote a primary page/card/grid anchor");Valid(state);
            }
        });
        Check("connector-reuse-without-intermediate-disconnect-does-not-trust-old-monitor", () => { var s = Fixture(); var b = s.Boards[1]; var key = DesktopGridActions.ItemKey(b.RootItems[0]); DesktopDisplayRecovery.Reconcile(s, [primary, secondary with { HardwareIdentity = "swapped-in-dock" }]); Expect(DesktopPages.Position(b, key) != new GridRect(7, 5, 1, 1), "Reused connector trusted old display"); Valid(s); });
        Check("connector-change-with-unique-edid-preserves-board-and-anchors", () => { var s = Disconnected(); var old = s.Boards[1]; string before = Anchors(old); DesktopDisplayRecovery.Reconcile(s, [primary, secondary with { Id = "new-connector" }]); Expect(old.Id == "new-connector" && Anchors(old) == before && s.Boards.Count == 2, "Connector change duplicated board"); Valid(s); });
        Check("one-new-secondary-adopts-offline-content-with-default-order", () => { var s = Disconnected(); var b = s.Boards[1]; var key = DesktopGridActions.ItemKey(b.RootItems[0]); DesktopDisplayRecovery.Reconcile(s, [primary, Screen("c", hardware: "different")]); Expect(b.Id == "c" && DesktopPages.Position(b, key) != new GridRect(7, 5, 1, 1), "Replacement failed"); Valid(s); });
        Check("ambiguous-new-monitors-leave-explicit-mapping", () => { var s = Disconnected(); DesktopDisplayRecovery.Reconcile(s, [primary, Screen("c", hardware: "same"), Screen("d", hardware: "same")]); Expect(DesktopDisplayRecovery.Offline(s, [primary, Screen("c"), Screen("d")]).Single().Id == "b" && s.Boards.Count == 4, "Ambiguous mapping guessed"); Valid(s); });
        Check("duplicate-edid-is-not-an-exact-identity", () => { var s = Disconnected(); DesktopDisplayRecovery.Reconcile(s, [primary, secondary, Screen("c")]); var key = DesktopGridActions.ItemKey(s.Boards[1].RootItems[0]); Expect(DesktopPages.Position(s.Boards[1], key) != new GridRect(7, 5, 1, 1), "Duplicate serial trusted"); Valid(s); });
        Check("restart-offline-serialization-and-v8-migration-backup", () => { var store = new StateStore(Path.Combine(folder, "migration")); var s = Fixture(); s.SchemaVersion = 7; s.Displays = null; store.Save(s); string previous = File.ReadAllText(store.FilePath); s = store.Load(); DesktopDisplayRecovery.Reconcile(s, [primary, secondary]); DesktopDisplayRecovery.Reconcile(s, [primary]); store.Save(s); var loaded = new StateStore(store.DirectoryPath).Load(); Expect(loaded.SchemaVersion == 8 && DesktopDisplayRecovery.Offline(loaded, [primary]).Count() == 1, "Restart lost offline references"); var copies = Directory.GetFiles(store.DirectoryPath, "*.before-display-v8-*.json"); Expect(copies.Length == 1 && File.ReadAllText(copies[0]) == previous, "Original schema not preserved"); Valid(loaded); });
        foreach (string type in new[] { "root", "folder", "card", "nested" })
            Check("extract-" + type + "-moves-reference-only-and-preserves-identity", () => { var s = Disconnected(); var source = s.Boards[1]; var dest = s.Boards[0]; string? parent = type == "nested" ? source.Boxes[0].Id : null; string key = type switch { "root" => DesktopGridActions.ItemKey(source.RootItems[0]), "folder" => DesktopGridActions.FolderKey(source.Boxes[0]), "card" => DesktopCards.Key(source.Cards![0]), _ => DesktopGridActions.ItemKey(source.Boxes[0].Items[0]) }; string? settings = type == "card" ? source.Cards![0].Settings.GetRawText() : null; DesktopDisplayRecovery.Extract(s, source.Id, key, parent, dest, 6, 4); Expect(DesktopPages.Position(dest, key)?.X == 6, "Requested available destination lost"); if (settings is not null) Expect(dest.Cards![0].Settings.GetRawText() == settings, "Card settings lost"); Expect(File.ReadAllText(Path.Combine(folder, "source.txt")) == "original bytes" && File.ReadAllText(Path.Combine(folder, "nested.txt")) == "nested bytes", "File altered"); Valid(s); });
        Check("occupied-drop-finds-next-free-slot", () => { var s = Disconnected(); var item = new DesktopItem { Name = "occupied", Target = Path.Combine(folder, "occupied") }; var dest = s.Boards[0]; dest.RootItems.Add(item); dest.Grid!.Placements.Add(DesktopGridActions.ItemKey(item), new(0, 0, 1, 1)); var key = DesktopGridActions.ItemKey(s.Boards[1].RootItems[0]); DesktopDisplayRecovery.Extract(s, "b", key, null, dest, 0, 0); Expect(DesktopPages.Position(dest, key) == new GridRect(1, 0, 1, 1), "Occupied cell overwritten"); Valid(s); });
        Check("full-page-failure-leaves-original-state", () => { var s = Disconnected(); var dest = s.Boards[0]; dest.Grid!.Columns = dest.Grid.Rows = 1; dest.RootItems.Add(new() { Name = "occupied", Target = Path.Combine(folder, "full") }); DesktopGridActions.ReconcileBoard(dest); string before = JsonSerializer.Serialize(s); var copy = StateStore.Copy(s); bool failed = false; try { DesktopDisplayRecovery.Extract(copy, "b", DesktopCards.Key(copy.Boards[1].Cards![0]), null, copy.Boards[0], 0, 0); } catch (LayoutEditException) { failed = true; } Expect(failed && before == JsonSerializer.Serialize(s), "Failed drop changed original"); Valid(s); });
        Check("explicit-restore-merges-without-losing-content", () => { var s = Disconnected(); int count = DesktopGridActions.Expected(s.Boards[1]).Count; DesktopDisplayRecovery.RestoreTo(s, "b", primary); Expect(s.Boards.Count == 1 && DesktopGridActions.Expected(s.Boards[0]).Count == count && s.Displays!.All(d => d.Id != "b"), "Restore lost board contents"); Valid(s); });
        Check("restore-appends-only-and-preserves-existing-pages-active-page-and-settings", () => {
            var state = Disconnected(); var board = state.Boards[0];
            var original = new DesktopItem { Name = "original", Target = Path.Combine(folder, "original") }; board.RootItems.Add(original);
            board.Grid!.Placements.Add(DesktopGridActions.ItemKey(original), new(8, 6, 1, 1));
            board.Paged = true; DesktopPages.Add(board, false); // An intentionally empty last page must also remain untouched.
            string oldPages = Anchors(board), active = board.ActivePageId!;
            var unplaced = board.Unplaced; var iconGrid = board.IconGrid; var sort = board.SortSettings;
            var incoming = DesktopGridActions.Expected(state.Boards[1]).Keys.ToHashSet();
            DesktopDisplayRecovery.RestoreTo(state, "b", primary);
            Expect(board.Pages!.Count == 3 && JsonSerializer.Serialize(board.Pages.Take(2), StateStore.Json) == oldPages, "Old pages changed or empty page reused");
            Expect(board.ActivePageId == active && ReferenceEquals(board.Unplaced, unplaced) && board.IconGrid == iconGrid && board.SortSettings == sort, "Existing presentation changed");
            Expect(incoming.SetEquals(board.Pages[2].Grid.Placements.Keys), "Incoming items filled existing gaps");
            DesktopPages.Select(board, board.Pages[2].Id); Valid(state);
            // A normal appended page follows the existing empty-page deletion rule.
            string appended = board.Pages[2].Id;
            board.RootItems.RemoveAll(i => incoming.Contains(DesktopGridActions.ItemKey(i)));
            board.Boxes.RemoveAll(b => incoming.Contains(DesktopGridActions.FolderKey(b)));
            board.Cards?.RemoveAll(c => incoming.Contains(DesktopCards.Key(c)));
            foreach (string key in incoming) DesktopPages.Forget(board, key);
            DesktopPages.Delete(board, appended);
            Expect(Anchors(board) == oldPages, "Deleting emptied appended page changed old pages"); Valid(state);
        });
        Check("three-restores-append-one-populated-page-per-source-with-no-empty-extras", () => {
            var state = Fixture(); state.Boards.RemoveAt(1); state.Displays!.RemoveAll(m => m.Id == "b"); var board = state.Boards[0];
            string original = Anchors(board);
            for (int i = 0; i < 3; i++) {
                var source = new Board { Id = "offline-" + i, Name = "副屏", RootItems = [new() { Name = "incoming-" + i, Target = Path.Combine(folder, "incoming-" + i) }] };
                DesktopDisplayRecovery.Pack(source, secondary, []); state.Boards.Add(source);
                DesktopDisplayRecovery.RestoreTo(state, source.Id, primary);
                Expect(board.Pages!.Count == i + 2 && board.Pages.Skip(1).All(p => p.Grid.Placements.Count == 1), "Restore duplicated or created empty pages"); Valid(state);
            }
            Expect(JsonSerializer.Serialize(board.Pages!.Take(1), StateStore.Json) == original, "First page changed");
        });
        foreach (string failure in new[] { "page-limit", "oversize", "card-limit" })
            Check("restore-preflight-" + failure + "-leaves-both-boards-unchanged", () => {
                var state = Disconnected(); var board = state.Boards[0];
                if (failure == "page-limit") { board.Paged = true; while (board.Pages!.Count < DesktopPages.MaximumPages) DesktopPages.Add(board, false); }
                if (failure == "oversize") { board.Grid!.Columns = 1; board.Grid.Rows = 1; }
                if (failure == "card-limit") { board.Cards = Enumerable.Range(0, 64).Select(_ => new DesktopCard { Type = "system.clock", Columns = 1, Rows = 1 }).ToList(); DesktopDisplayRecovery.Pack(board, primary, []); }
                string before = JsonSerializer.Serialize(state, StateStore.Json); bool failed = false;
                try { DesktopDisplayRecovery.RestoreTo(state, "b", primary); } catch (LayoutEditException) { failed = true; }
                Expect(failed && JsonSerializer.Serialize(state, StateStore.Json) == before, "Preflight failure partially changed state"); Valid(state);
            });
        Check("empty-source-does-not-append-an-empty-page", () => {
            var state = Fixture(); var source = state.Boards[1]; source.RootItems.Clear(); source.Boxes.Clear(); source.Cards = null; DesktopGridActions.ReconcileBoard(source);
            var board = state.Boards[0]; string before = JsonSerializer.Serialize(board, StateStore.Json);
            DesktopDisplayRecovery.RestoreTo(state, "b", primary);
            Expect(state.Boards.Count == 1 && JsonSerializer.Serialize(board, StateStore.Json) == before, "Empty source added a page"); Valid(state);
        });
        Check("restore-rejects-card-limit-on-transaction-copy", () => { var s = Disconnected(); var dest = s.Boards[0]; dest.Cards = Enumerable.Range(0, 64).Select(_ => new DesktopCard { Type = "system.clock", Columns = 1, Rows = 1 }).ToList(); DesktopDisplayRecovery.Pack(dest, primary, []); var copy = StateStore.Copy(s); bool failed = false; try { DesktopDisplayRecovery.RestoreTo(copy, "b", primary); } catch (LayoutEditException) { failed = true; } Expect(failed && s.Boards.Count == 2 && s.Boards[1].Cards!.Count == 1, "Limit failure mutated original"); Valid(s); });
        Check("many-page-overflow-retains-every-key", () => { var s = Fixture(); var b = s.Boards[1]; b.RootItems = Enumerable.Range(0, 4096).Select(i => new DesktopItem { Name = "item" + i, Target = Path.Combine(folder, "bulk" + i) }).ToList(); var tiny = Screen("b", dpi: 384, width: 320) with { Height = 240, PixelHeight = 240 }; DesktopDisplayRecovery.Pack(b, tiny, []); Expect(b.Pages!.Count <= DesktopPages.MaximumPages && b.Unplaced.Count > 0, "Overflow path not exercised"); Valid(s); });
        Check("custom-offline-order-triggers-repack-on-reconnect", () => { var s = Disconnected(); var b = s.Boards[1]; var memory = s.Displays!.Single(d => d.Id == "b"); memory.Order = DesktopDisplayRecovery.Order(b, memory).Reverse().ToList(); memory.Reordered = true; var first = memory.Order[0]; DesktopDisplayRecovery.Reconcile(s, [primary, secondary]); Expect(DesktopPages.Position(b, first) is { X: 0, Y: 0 }, "Offline custom order ignored"); Valid(s); });
        Check("malformed-display-record-rejected", () => { var s = Fixture(); s.Displays!.Add(s.Displays[0]); bool failed = false; try { StateStore.Validate(s); } catch (InvalidDataException) { failed = true; } Expect(failed, "Duplicate record accepted"); });
        Check("future-schema-remains-read-only", () => { var store = new StateStore(Path.Combine(folder, "future")); Directory.CreateDirectory(store.DirectoryPath); File.WriteAllText(store.FilePath, "{\"SchemaVersion\":10,\"Secret\":\"retain\"}"); string before = File.ReadAllText(store.FilePath); store.Load(); Expect(store.ReadOnly && File.ReadAllText(store.FilePath) == before, "Future version overwritten"); });
        return new { allPass = true, tests = results, output = folder, scope = "Owned files and layout fixtures only; no real hotplug, desktop activation or user acceptance." };
    }
}
