using System.IO;
using System.Text.Json;

namespace MagicCommunity;

public sealed class DesktopDisplayRecord
{
    public string Id { get; set; } = "";
    public string Hardware { get; set; } = "";
    public string Label { get; set; } = "";
    public bool Primary { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
    public uint Dpi { get; set; }
    public DateTimeOffset? DisconnectedAt { get; set; }
    public bool Reordered { get; set; }
    public List<string> Order { get; set; } = [];
}

// Layout references only: this subsystem never moves, renames or deletes a file.
internal static class DesktopDisplayRecovery
{
    internal static IEnumerable<Board> Offline(LayoutState state, IReadOnlyList<Display> screens) =>
        state.Boards.Where(b => !screens.Any(s => s.Id == b.Id) && DesktopGridActions.Expected(b).Count > 0);

    internal static IReadOnlyList<string> Order(Board board, DesktopDisplayRecord? memory)
    {
        var expected = DesktopGridActions.Expected(board);
        var keys = new List<string>();
        if (memory is not null) keys.AddRange(memory.Order.Where(expected.ContainsKey));
        keys.AddRange(DesktopPages.Grids(board).SelectMany(g => g.Placements.OrderBy(p => p.Value.Y).ThenBy(p => p.Value.X).Select(p => p.Key)));
        keys.AddRange(board.Unplaced.Keys); keys.AddRange(expected.Keys);
        return keys.Where(expected.ContainsKey).Distinct(StringComparer.Ordinal).ToArray();
    }
    private static bool IsUnidentifiedEmptyBoard(Board board, DesktopDisplayRecord memory) =>
        memory.Hardware.Length == 0 && !memory.Reordered && memory.Order.Count == 0 && board.Name == memory.Label &&
        DesktopGridActions.Expected(board).Count == 0 && board.Unplaced.Count == 0 &&
        !board.Paged && board.Pages is not { Count: > 1 } && board.IconGrid is null && board.SortSettings is null &&
        DesktopPages.Grids(board).All(g => g.Placements.Count == 0);
    internal static void Validate(LayoutState state)
    {
        if (state.Displays is null) return;
        if (state.SchemaVersion < 8 || state.Displays.Count > 64 || state.Displays.Select(d => d.Id).Distinct(StringComparer.Ordinal).Count() != state.Displays.Count)
            throw new InvalidDataException("显示器恢复记录无效。");
        foreach (var d in state.Displays)
            if (string.IsNullOrWhiteSpace(d.Id) || d.Id.Length > 512 || d.Hardware is null || d.Hardware.Length > 80 || d.Label is null || d.Label.Length > 160 ||
                d.Width is < 0 or > 131072 || d.Height is < 0 or > 131072 || d.Dpi > 768 || d.Order is null || d.Order.Count > 8192 ||
                d.Order.Any(k => k is null || k.Length > 160) || d.Order.Distinct(StringComparer.Ordinal).Count() != d.Order.Count || !state.Boards.Any(b => b.Id == d.Id))
                throw new InvalidDataException("显示器恢复记录超出范围。");
    }
    internal static List<string> Reconcile(LayoutState state, IReadOnlyList<Display> screens)
    {
        var notices = new List<string>();
        var memories = state.Displays ??= [];
        state.SchemaVersion = Math.Max(8, state.SchemaVersion);
        foreach (var board in state.Boards)
            if (!memories.Any(m => m.Id == board.Id)) memories.Add(new() { Id = board.Id, Label = board.Name, Primary = board.Name.StartsWith("主屏", StringComparison.Ordinal) });
        // Windows device interface paths are case-insensitive; their casing is
        // not a disconnect or a new physical display.
        foreach (var screen in screens.Where(s => !state.Boards.Any(b => b.Id == s.Id)))
        {
            var matches = state.Boards.Where(b => string.Equals(b.Id, screen.Id, StringComparison.OrdinalIgnoreCase)).ToArray();
            if (matches.Length != 1) continue;
            memories.Single(m => m.Id == matches[0].Id).Id = screen.Id;
            matches[0].Id = screen.Id;
        }
        foreach (var m in memories.Where(m => !screens.Any(s => s.Id == m.Id)))
        {
            if (m.DisconnectedAt is null) { m.DisconnectedAt = DateTimeOffset.UtcNow; m.Order = Order(state.Boards.Single(b => b.Id == m.Id), m).ToList(); }
        }
        var newcomers = screens.Where(s => !state.Boards.Any(b => b.Id == s.Id)).ToList();
        // A stable, serialized EDID identity may survive a connector/device-ID change.
        foreach (var screen in screens)
        {
            var placeholder = state.Boards.SingleOrDefault(b => b.Id == screen.Id);
            var placeholderMemory = placeholder is null ? null : memories.Single(m => m.Id == placeholder.Id);
            // The interface may arrive before EDID. Once hardware becomes known,
            // rejoin its unique saved board only if the temporary active layout
            // is still empty and unmodified. Never merge over user edits.
            if (placeholder is not null && !IsUnidentifiedEmptyBoard(placeholder, placeholderMemory!)) continue;
            var matches = memories.Where(m => m.DisconnectedAt is not null && screen.HardwareIdentity.Length > 0 && m.Hardware == screen.HardwareIdentity).ToArray();
            if (matches.Length != 1 || screens.Count(s => s.HardwareIdentity == screen.HardwareIdentity) != 1) continue;
            if (placeholder is not null) { state.Boards.Remove(placeholder); memories.Remove(placeholderMemory!); }
            var m = matches[0]; state.Boards.Single(b => b.Id == m.Id).Id = screen.Id; m.Id = screen.Id; newcomers.Remove(screen);
        }
        var missing = Offline(state, screens).Where(b => !memories.Single(m => m.Id == b.Id).Primary).ToArray();
        var newSecondary = newcomers.Where(s => !s.Primary).ToArray();
        // Ambiguous N:M replacement stays in the recovery panel for explicit mapping.
        if (missing.Length == 1 && newSecondary.Length == 1)
        {
            var board = missing[0]; var m = memories.Single(d => d.Id == board.Id); board.Id = newSecondary[0].Id; m.Id = board.Id;
            // This is a replacement, not an exact identity match. Never carry
            // the previous monitor's hardware identity into an unknown one.
            m.Hardware = newSecondary[0].HardwareIdentity;
            m.Reordered = true; newcomers.Remove(newSecondary[0]);
        }
        foreach (var screen in newcomers)
        {
            state.Boards.Add(new() { Id = screen.Id, Name = screen.Label });
            memories.Add(new() { Id = screen.Id, Label = screen.Label });
        }
        foreach (var screen in screens)
        {
            var m = memories.Single(d => d.Id == screen.Id); var board = state.Boards.Single(b => b.Id == screen.Id);
            var size = DesktopGridLayout.Physical(screen);
            // Some docks reuse their connector ID without an intermediate empty topology.
            bool hardwareChanged = m.Hardware.Length > 0 && screen.HardwareIdentity.Length > 0 && m.Hardware != screen.HardwareIdentity;
            if (m.Width > 0 && hardwareChanged) m.DisconnectedAt ??= DateTimeOffset.UtcNow;
            if (m.DisconnectedAt is not null)
            {
                // A resume can report a temporary DPI/resolution even for the
                // same monitor. Geometry is projected separately and must not
                // destroy page IDs, card anchors or custom grid settings.
                // Missing EDID during resume is unknown, not evidence of a
                // replacement. The interface ID still matches this board;
                // primary/secondary roles and DISPLAY1/2 names are not identity.
                bool same = !m.Reordered && !hardwareChanged &&
                    (screen.HardwareIdentity.Length == 0 || m.Hardware.Length == 0 ||
                     screens.Count(s => s.HardwareIdentity == m.Hardware) == 1);
                if (!same) Pack(board, screen, Order(board, m));
                notices.Add(same ? (screen.Primary?"主屏内容已保留原页面和位置。":"副屏内容已按原布局恢复。") : "副屏身份或顺序已变化，内容已按默认顺序重排。");
                m.DisconnectedAt = null; m.Reordered = false;
            }
            // Keep the last verified identity across temporary registry/driver
            // gaps, including a persisted state followed by an application restart.
            if (screen.HardwareIdentity.Length > 0) m.Hardware = screen.HardwareIdentity;
            m.Width = size.Width; m.Height = size.Height; m.Dpi = screen.Dpi; m.Primary = screen.Primary; m.Label = screen.Label;
            m.Order = Order(board, m).ToList();
        }
        return notices;
    }
    internal static void Pack(Board board, Display target, IEnumerable<string> order)
    {
        var expected = DesktopGridActions.Expected(board);
        var p = AccessoryProjection.Create(target, (target.Dpi == 0 ? 96 : target.Dpi) / 96.0);
        int columns = Math.Max(p.Columns, expected.Values.Select(v => v.W).DefaultIfEmpty(1).Max());
        int rows = Math.Max(p.Rows, expected.Values.Select(v => v.H).DefaultIfEmpty(1).Max());
        var grid = new DesktopGrid { Columns = columns, Rows = rows, NativeInitialized = true, Bounded = true };
        board.Pages = [new() { Grid = grid }]; board.ActivePageId = board.Pages[0].Id; board.Unplaced.Clear(); board.IconGrid = null; board.SortSettings = null;
        foreach (var key in order.Concat(expected.Keys).Distinct(StringComparer.Ordinal).Where(expected.ContainsKey))
        {
            var size = expected[key]; GridRect? rect = null;
            foreach (var page in board.Pages) if ((rect = DesktopGridActions.FindFree(page.Grid, size.W, size.H)) is not null) { grid = page.Grid; break; }
            if (rect is null && board.Pages.Count < DesktopPages.MaximumPages)
            {
                grid = new() { Columns = columns, Rows = rows, NativeInitialized = true, Bounded = true }; board.Pages.Add(new() { Grid = grid }); rect = DesktopGridActions.FindFree(grid, size.W, size.H);
            }
            if (rect is null) board.Unplaced[key] = new(0, 0, size.W, size.H); else grid.Placements[key] = rect;
        }
        if (board.Pages.Count > 1) board.Paged = true;
    }
    internal static GridRect Slot(Board destination, int w, int h, int? column, int? row)
    {
        var grid = destination.Grid ?? throw new LayoutEditException("目标桌面尚未就绪。");
        int start = Math.Clamp(row ?? 0, 0, grid.Rows - 1) * grid.Columns + Math.Clamp(column ?? 0, 0, grid.Columns - 1);
        for (int offset = 0; offset < grid.Columns * grid.Rows; offset++)
        {
            int cell = (start + offset) % (grid.Columns * grid.Rows); var rect = new GridRect(cell % grid.Columns, cell / grid.Columns, w, h);
            if (DesktopGridActions.CanFit(grid, rect)) return rect;
        }
        throw new LayoutEditException("目标当前页放不下，原内容已保留。请先切换页面或腾出空间。");
    }
    internal static void Extract(LayoutState state, string sourceId, string key, string? folder, Board destination, int? column, int? row)
    {
        var source = state.Boards.Single(b => b.Id == sourceId);
        if (source.Id == destination.Id) throw new LayoutEditException("目标必须是已连接的另一块屏幕。");
        var size = folder is not null ? (W: 1, H: 1) : DesktopGridActions.Expected(source).GetValueOrDefault(key);
        if (size.W == 0 || size.H == 0) throw new LayoutEditException("来源项目已变化。");
        var slot = Slot(destination, size.W, size.H, column, row);
        if (key.StartsWith("card:", StringComparison.Ordinal)) DesktopCards.Transfer(source, destination, key[5..], slot.X, slot.Y);
        else if (key.StartsWith("folder:", StringComparison.Ordinal))
        {
            var box = source.Boxes.Single(b => DesktopGridActions.FolderKey(b) == key); source.Boxes.Remove(box); destination.Boxes.Add(box);
            DesktopPages.Forget(source, key); destination.Grid!.Placements.Add(key, slot);
        }
        else
        {
            var list = folder is null ? source.RootItems : source.Boxes.Single(b => b.Id == folder).Items;
            var item = list.Single(i => DesktopGridActions.ItemKey(i) == key); list.Remove(item); DesktopPages.Forget(source, key);
            destination.RootItems.Add(item); destination.Grid!.Placements.Add(key, slot);
        }
        DesktopGridActions.ReconcileBoard(source); DesktopSort.Manual(destination);
    }
    internal static void RestoreTo(LayoutState state, string sourceId, Display target)
    {
        var source = state.Boards.Single(b => b.Id == sourceId); var destination = state.Boards.Single(b => b.Id == target.Id);
        if (source == destination) throw new LayoutEditException("此屏幕已经接入。");
        var cards = DesktopCards.Items(source).ToArray();
        if (destination.Boxes.Count + source.Boxes.Count > 100 || destination.RootItems.Count + source.RootItems.Count > 4096 || DesktopCards.Items(destination).Count() + cards.Length > 64)
            throw new LayoutEditException("目标内容超过单屏上限，原内容已保留。");
        var expected = DesktopGridActions.Expected(source);
        var existing = DesktopGridActions.Expected(destination);
        if (expected.Keys.Any(existing.ContainsKey)) throw new LayoutEditException("来源与目标包含重复项目，原内容已保留。");
        var template = destination.Grid ?? throw new LayoutEditException("目标桌面尚未就绪，原内容已保留。");
        int existingPages = destination.Pages?.Count ?? 1;
        var appended = new List<DesktopPage>();
        // Plan against new pages only. Never fill gaps, reconcile or repack the
        // existing pages; even an empty last page belongs to the user's layout.
        foreach (var key in Order(source, state.Displays?.SingleOrDefault(m => m.Id == sourceId)))
        {
            var size = expected[key];
            if (size.W > template.Columns || size.H > template.Rows)
                throw new LayoutEditException("目标页面放不下副屏的大文件夹或卡片，原内容已保留。请先调整其尺寸。");
            GridRect? slot = null; DesktopPage? page = null;
            foreach (var candidate in appended)
                if ((slot = DesktopGridActions.FindFree(candidate.Grid, size.W, size.H)) is not null) { page = candidate; break; }
            if (page is null)
            {
                if (existingPages + appended.Count >= DesktopPages.MaximumPages)
                    throw new LayoutEditException("目标页面已达上限，原内容已保留。请先整理空页。");
                page = new() { Grid = new() { Columns = template.Columns, Rows = template.Rows, NativeInitialized = true, Bounded = true } };
                appended.Add(page); slot = new(0, 0, size.W, size.H);
            }
            page.Grid.Placements.Add(key, slot!);
        }
        // All limits and placements succeeded before changing either board.
        if (appended.Count > 0)
        {
            if (destination.Pages is null)
            {
                var first = new DesktopPage { Grid = template };
                destination.Pages = [first]; destination.ActivePageId = first.Id;
            }
            destination.Pages.AddRange(appended); destination.Paged = true;
        }
        destination.RootItems.AddRange(source.RootItems); destination.Boxes.AddRange(source.Boxes);
        if (cards.Length > 0) (destination.Cards ??= []).AddRange(cards);
        state.Boards.Remove(source); state.Displays?.RemoveAll(m => m.Id == sourceId);
    }
}
