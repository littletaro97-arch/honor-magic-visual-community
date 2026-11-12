using System.IO;
using System.Text.Json;

namespace MagicCommunity;

public sealed class LayoutState
{
    public int SchemaVersion { get; set; } = 4;
    public long Revision { get; set; }
    public bool SoftwareRendering { get; set; } = true;
    public List<Board> Boards { get; set; } = [];
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public JsonElement? SharedTodos { get; set; }
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public List<DesktopDisplayRecord>? Displays { get; set; }
}
public sealed class Board
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public List<Box> Boxes { get; set; } = [];
    public List<DesktopItem> RootItems { get; set; } = [];
    private DesktopGrid? legacyGrid;
    // Existing callers always operate on the active page. Only legacy layouts
    // serialize Grid; v6 has one authoritative copy of each page's placements.
    [System.Text.Json.Serialization.JsonIgnore]
    public DesktopGrid? Grid
    {
        get=>Pages is {Count:>0}?Pages.FirstOrDefault(page=>page.Id==ActivePageId)?.Grid:legacyGrid;
        set {if(Pages is {Count:>0})Pages.Single(page=>page.Id==ActivePageId).Grid=value??new();else legacyGrid=value;}
    }
    [System.Text.Json.Serialization.JsonPropertyName("Grid")]
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public DesktopGrid? LegacyGrid { get=>Pages is null?legacyGrid:null;set=>legacyGrid=value; }
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public List<DesktopPage>? Pages { get; set; }
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public DesktopIconGridSettings? IconGrid { get; set; }
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public DesktopSortSettings? SortSettings { get; set; }
    public bool Paged { get; set; }
    public string? ActivePageId { get; set; }
    public Dictionary<string,GridRect> Unplaced { get; set; } = new(StringComparer.Ordinal);
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public List<DesktopCard>? Cards { get; set; }
}
public sealed class Box
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Name { get; set; } = "新收纳盒";
    public bool Collapsed { get; set; }
    public bool ShowItemCount { get; set; } = true;
    public List<DesktopItem> Items { get; set; } = [];
    public int Columns { get; set; } = 2;
    public int Rows { get; set; } = 2;
}
public sealed class DesktopItem
{
    public string Name { get; set; } = "";
    public string Target { get; set; } = "";
    public bool IsFolder { get; set; }
    public bool IsVirtual { get; set; }
    public bool FromDesktop { get; set; }
}
public sealed class StateStore(string directory)
{
    public static readonly JsonSerializerOptions Json = new() { WriteIndented = true, PropertyNameCaseInsensitive = true };
    public string DirectoryPath { get; } = Path.GetFullPath(directory);
    public string FilePath => Path.Combine(DirectoryPath, "layout.json");
    public string BackupPath => FilePath + ".bak";
    public string? Problem { get; private set; }
    public bool ReadOnly { get; private set; }
    public bool BackupAvailable { get; private set; }
    public bool NeedsMigration { get; private set; }
    private int loadedSchema=4;
    public LayoutState Load()
    {
        if (!File.Exists(FilePath)) return new();
        try { var state=ReadValidated(FilePath,out int sourceVersion);loadedSchema=state.SchemaVersion;NeedsMigration=sourceVersion<4;return state; }
        catch (FutureSchemaException ex) { Problem = ex.Message; ReadOnly = true; return new(); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
        {
            ReadOnly = true;
            Problem = "布局文件读取失败，已保护原文件。" + ex.Message;
            try { var backup = ReadValidated(BackupPath); BackupAvailable = true; return backup; }
            catch { return new(); }
        }
    }
    public LayoutState RestoreBackup()
    {
        if (!BackupAvailable) throw new InvalidOperationException("没有可恢复的布局备份。");
        var recovered = ReadValidated(BackupPath);
        var preserved = FilePath + ".corrupt-" + DateTime.UtcNow.ToString("yyyyMMddHHmmssfff");
        File.Copy(FilePath, preserved, false);
        // Keep the known-good backup during explicit recovery; failed writes remain read-only.
        AtomicWrite(FilePath, JsonSerializer.Serialize(recovered, Json), null);
        ReadOnly = false; BackupAvailable = false; Problem = null;
        loadedSchema=recovered.SchemaVersion;
        return recovered;
    }
    public void Save(LayoutState state)
    {
        if (ReadOnly) throw new InvalidOperationException(Problem ?? "布局当前为只读。");
        Validate(state);
        Directory.CreateDirectory(DirectoryPath);
        if(NeedsMigration && File.Exists(FilePath))
        {
            // Keep a separate immutable v1 copy; normal .bak rotation must not erase it.
            string copy=FilePath+".before-unified-v4-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+"-"+Guid.NewGuid().ToString("N")[..8]+".json";
            File.Copy(FilePath,copy,false);
        }
        if(state.SchemaVersion==5&&loadedSchema<5&&File.Exists(FilePath))File.Copy(FilePath,FilePath+".before-cards-v5-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+"-"+Guid.NewGuid().ToString("N")[..8]+".json",false);
        if(state.SchemaVersion==6&&loadedSchema<6&&File.Exists(FilePath))File.Copy(FilePath,FilePath+".before-pages-v6-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+"-"+Guid.NewGuid().ToString("N")[..8]+".json",false);
        if(state.SchemaVersion>=7&&loadedSchema<7&&File.Exists(FilePath))File.Copy(FilePath,FilePath+".before-grid-v7-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+"-"+Guid.NewGuid().ToString("N")[..8]+".json",false);
        if(state.SchemaVersion>=8&&loadedSchema<8&&File.Exists(FilePath))File.Copy(FilePath,FilePath+".before-display-v8-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+"-"+Guid.NewGuid().ToString("N")[..8]+".json",false);
        if(state.SchemaVersion>=9&&loadedSchema<9&&File.Exists(FilePath))File.Copy(FilePath,FilePath+".before-shared-todo-v9-"+DateTime.UtcNow.ToString("yyyyMMddHHmmssfff")+"-"+Guid.NewGuid().ToString("N")[..8]+".json",false);
        var text = JsonSerializer.Serialize(state, Json);
        if (System.Text.Encoding.UTF8.GetByteCount(text) > 8 * 1024 * 1024) throw new InvalidDataException("布局超过 8 MiB；本次修改未保存。");
        AtomicWrite(FilePath, text, BackupPath);
        NeedsMigration=false;
        loadedSchema=state.SchemaVersion;
    }
    public static LayoutState Copy(LayoutState state) => JsonSerializer.Deserialize<LayoutState>(JsonSerializer.Serialize(state, Json), Json)!;
    private static LayoutState ReadValidated(string path)=>ReadValidated(path,out _);
    private static LayoutState ReadValidated(string path,out int sourceVersion)
    {
        sourceVersion=0;
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length > 8 * 1024 * 1024) throw new InvalidDataException("布局文件超过 8 MiB。");
        using var doc = JsonDocument.Parse(bytes);
        if (!doc.RootElement.TryGetProperty("SchemaVersion", out var schema) || !schema.TryGetInt32(out var version))
            throw new InvalidDataException("布局缺少有效版本。");
        if (version > 9) throw new FutureSchemaException("布局来自较新版本，当前预览版以只读模式打开。");
        if (version is not (1 or 2 or 3 or 4 or 5 or 6 or 7 or 8 or 9)) throw new InvalidDataException("不支持的布局版本。");
        sourceVersion=version;
        var state = JsonSerializer.Deserialize<LayoutState>(bytes, Json) ?? throw new InvalidDataException("布局为空。");
        state.SchemaVersion=version<4?4:version;
        if(version<3)foreach(var board in state.Boards)if(board.Grid is { } grid){grid.FoldersOnly=true;grid.NativeInitialized=false;foreach(var key in grid.Placements.Keys.Where(key=>key.StartsWith("item:",StringComparison.Ordinal)).ToArray())grid.Placements.Remove(key);}
        Validate(state); return state;
    }
    public static void Validate(LayoutState state)
    {
        if (state.SchemaVersion is not (4 or 5 or 6 or 7 or 8 or 9) || state.Revision<0 || state.Boards is null || state.Boards.Count > 64) throw new InvalidDataException("布局结构无效。");
        if(state.SharedTodos is {} tasks){if(state.SchemaVersion<9)throw new InvalidDataException("共享待办需要版本9。");DesktopCards.ValidateSettings(tasks);if(System.Text.Encoding.UTF8.GetByteCount(tasks.GetRawText())>15000)throw new InvalidDataException("共享待办超过存储上限。");}
        DesktopDisplayRecovery.Validate(state);
        var rootTargets=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var boardIds = new HashSet<string>(StringComparer.Ordinal);
        var boxIds = new HashSet<string>(StringComparer.Ordinal);
        var cardIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var board in state.Boards)
        {
            if (board is null || string.IsNullOrWhiteSpace(board.Id) || !boardIds.Add(board.Id) || board.Boxes is null || board.Boxes.Count > 100)
                throw new InvalidDataException("屏幕或收纳盒结构无效。");
            if (board.Name is null || board.Name.Length > 160) throw new InvalidDataException("屏幕名称无效。");
            if(board.RootItems is null || board.RootItems.Count>4096)throw new InvalidDataException("独立图标结构无效。");
            foreach(var item in board.RootItems){ValidateItem(item);if(!rootTargets.Add(item.Target))throw new InvalidDataException("跨屏独立图标重复。");}
            foreach (var box in board.Boxes)
            {
                if (box is null || string.IsNullOrWhiteSpace(box.Id) || !boxIds.Add(box.Id) || string.IsNullOrWhiteSpace(box.Name) || box.Name.Length > 80 || box.Items is null || box.Items.Count > 2000 || !DesktopGridActions.StoredSize(box.Columns,box.Rows))
                    throw new InvalidDataException("收纳盒结构无效。");
                var targets = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var item in box.Items)
                {
                    ValidateItem(item);
                    if (!targets.Add(item.Target))
                        throw new InvalidDataException("项目引用无效或重复。");
                    if (item.IsVirtual ? !item.Target.StartsWith("::", StringComparison.Ordinal) : !Path.IsPathFullyQualified(item.Target))
                        throw new InvalidDataException("只接受绝对文件路径或 Shell 虚拟项目。");
                }
            }
            board.IconGrid?.Validate();board.SortSettings?.Validate();
            if((board.IconGrid is not null||board.SortSettings is not null)&&state.SchemaVersion<7)throw new InvalidDataException("桌面图标设置需要版本7。");
            DesktopCards.Validate(board,cardIds);
            if(state.SchemaVersion==4&&DesktopCards.Items(board).Any())throw new InvalidDataException("卡片布局需要版本5。");
            if(board.Pages is not null&&state.SchemaVersion<6)throw new InvalidDataException("分页布局需要版本6。");
            DesktopGridActions.Validate(board);
        }
    }
    private static void ValidateItem(DesktopItem item)
    {
        if(item is null || string.IsNullOrWhiteSpace(item.Name)||item.Name.Length>512||string.IsNullOrWhiteSpace(item.Target)||item.Target.Length>32768)throw new InvalidDataException("项目引用无效。");
        if(item.IsVirtual?!item.Target.StartsWith("::",StringComparison.Ordinal):!Path.IsPathFullyQualified(item.Target))throw new InvalidDataException("只接受绝对路径或 Shell 项目。");
    }
    public static void AtomicWrite(string destination, string text, string? backup)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        var temp = destination + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var fs = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
            using (var writer = new StreamWriter(fs, new System.Text.UTF8Encoding(false)))
            { writer.Write(text); writer.Flush(); fs.Flush(true); }
            if (File.Exists(destination)) File.Replace(temp, destination, backup, true);
            else File.Move(temp, destination);
        }
        catch(IOException ex){throw new IOException("文件保存未完成："+destination+"。原文件未被本次保存覆盖。",ex);}
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
    private sealed class FutureSchemaException(string message) : Exception(message);
}
public static class LayoutActions
{
    public static int Add(Box box, IEnumerable<DesktopItem> items)
    {
        var targets = box.Items.Select(x => x.Target).ToHashSet(StringComparer.OrdinalIgnoreCase);
        int count = 0;
        foreach (var item in items)
            if (targets.Add(item.Target)) { box.Items.Add(new() { Name = item.Name, Target = item.Target, IsFolder = item.IsFolder, IsVirtual = item.IsVirtual }); count++; }
        return count;
    }
    public static void Move(LayoutState state, string sourceBoxId, string target, string destinationBoxId)
    {
        var source = state.Boards.SelectMany(b => b.Boxes).Single(b => b.Id == sourceBoxId);
        var destination = state.Boards.SelectMany(b => b.Boxes).Single(b => b.Id == destinationBoxId);
        if (source.Id == destination.Id) return;
        var item = source.Items.Single(x => StringComparer.OrdinalIgnoreCase.Equals(x.Target, target));
        Add(destination, [item]); source.Items.Remove(item);
    }
}
