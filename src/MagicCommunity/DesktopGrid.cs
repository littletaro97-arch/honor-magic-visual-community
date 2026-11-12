using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace MagicCommunity;

public sealed class DesktopGrid
{
    public int Columns { get; set; } = 10;
    public int Rows { get; set; } = 8;
    public bool FoldersOnly { get; set; }
    public bool NativeInitialized { get; set; }
    public bool Bounded { get; set; }
    public Dictionary<string, GridRect> Placements { get; set; } = new(StringComparer.Ordinal);
}
public sealed record GridRect(int X,int Y,int W,int H);

public static class DesktopGridActions
{
    public static string ItemId(DesktopItem item)=>Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(item.Target.ToUpperInvariant())));
    public static string ItemKey(DesktopItem item)=>"item:"+ItemId(item);
    public static string FolderKey(Box box)=>"folder:"+box.Id;
    public static void SetFolderCount(Board board,string id,bool show)
        =>(board.Boxes.SingleOrDefault(box=>box.Id==id)??throw new LayoutEditException("大文件夹已变化，请重新打开菜单。")).ShowItemCount=show;
    public static bool ValidSize(int columns,int rows)=>(columns,rows) is (2,2) or (4,2) or (4,4);
    public static bool StoredSize(int columns,int rows)=>ValidSize(columns,rows)||(columns,rows)==(2,4);
    private static bool Overlaps(GridRect a,GridRect b)=>a.X<b.X+b.W&&a.X+a.W>b.X&&a.Y<b.Y+b.H&&a.Y+a.H>b.Y;
    private static bool Fits(DesktopGrid grid,GridRect rect,string? ignore=null)=>rect.X>=0&&rect.Y>=0&&rect.W>0&&rect.H>0&&rect.X<=grid.Columns-rect.W&&rect.Y<=grid.Rows-rect.H&&grid.Placements.All(row=>row.Key==ignore||!Overlaps(rect,row.Value));
    private static GridRect Free(DesktopGrid grid,int w,int h,bool grow)
    {
        if(w>grid.Columns||h>128)throw new LayoutEditException("文件夹尺寸超出网格。");
        while(true)
        {
            for(int y=0;y<=grid.Rows-h;y++)for(int x=0;x<=grid.Columns-w;x++)
            {var rect=new GridRect(x,y,w,h);if(Fits(grid,rect))return rect;}
            if(!grow||grid.Bounded||grid.Rows>=128)throw new LayoutEditException("当前页面已满，请先整理、切换页面或移到其它屏幕。");
            grid.Rows=Math.Min(128,grid.Rows+4);
        }
    }
    public static void Initialize(Board board,double width,double height)
    {
        board.Grid??=new(){Columns=Math.Clamp((int)((width-UnifiedDesktopGrid.MarginX)/UnifiedDesktopGrid.Cell),4,32),Rows=Math.Clamp((int)((height-UnifiedDesktopGrid.MarginY)/UnifiedDesktopGrid.Cell),4,128)};
        ReconcileBoard(board);
    }
    public static void Reconcile(LayoutState state,IReadOnlyList<DesktopItem> desktop,IReadOnlyList<Display> screens)
    {
        var assigned=state.Boards.SelectMany(b=>b.Boxes).SelectMany(b=>b.Items).Select(i=>i.Target).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var live=desktop.ToDictionary(i=>i.Target,StringComparer.OrdinalIgnoreCase);
        var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach(var board in state.Boards)
        {
            board.RootItems.RemoveAll(item=>assigned.Contains(item.Target)||(item.FromDesktop&&!live.ContainsKey(item.Target))||!seen.Add(item.Target));
            foreach(var item in board.RootItems)if(live.TryGetValue(item.Target,out var current)){item.Name=current.Name;item.IsFolder=current.IsFolder;item.IsVirtual=current.IsVirtual;}
        }
        var primary=state.Boards.FirstOrDefault(b=>b.Id==screens.FirstOrDefault(s=>s.Primary)?.Id)??state.Boards.FirstOrDefault();
        if(primary is not null)foreach(var item in desktop)if(!assigned.Contains(item.Target)&&seen.Add(item.Target))primary.RootItems.Add(item);
        foreach(var board in state.Boards)ReconcileBoard(board);
    }
    internal static Dictionary<string,(int W,int H)> Expected(Board board)=>board.Boxes.Select(box=>(Key:FolderKey(box),Size:(box.Columns,box.Rows))).Concat(board.Grid?.FoldersOnly==true?[]:board.RootItems.Select(item=>(Key:ItemKey(item),Size:(1,1)))).Concat(DesktopCards.Items(board).Select(card=>(Key:DesktopCards.Key(card),Size:(card.Columns,card.Rows)))).ToDictionary(row=>row.Key,row=>row.Size,StringComparer.Ordinal);
    internal static GridRect? FindFree(DesktopGrid grid,int w,int h)
    {for(int y=0;y<=grid.Rows-h;y++)for(int x=0;x<=grid.Columns-w;x++){var rect=new GridRect(x,y,w,h);if(Fits(grid,rect))return rect;}return null;}
    internal static GridRect ReserveSpace(Board board,int w,int h)=>FindFree(board.Grid??throw new LayoutEditException("目标屏幕尚未就绪。"),w,h)??throw new LayoutEditException(board.Paged?"目标当前页已满，请先切换到有空位的页面，或新建页面。":"副屏已满，可先移走一些项目，或移到主屏。");
    internal static bool CanFit(DesktopGrid grid,GridRect rect,string? ignore=null)=>Fits(grid,rect,ignore);
    internal static GridRect ReserveRect(Board board,int w,int h,int? x=null,int? y=null)
    {
        var grid=board.Grid??throw new LayoutEditException("桌面网格尚未就绪。");if(!DesktopCards.ValidSize(w,h))throw new LayoutEditException("卡片尺寸无效。");
        if(w>grid.Columns)throw new LayoutEditException("卡片宽度超出当前网格。");
        if(x.HasValue!=y.HasValue)throw new LayoutEditException("卡片位置不完整。");
        if(!x.HasValue)return ReserveSpace(board,w,h);var rect=new GridRect(x.Value,y!.Value,w,h);if(!Fits(grid,rect))throw new LayoutEditException("卡片目标已被占用或越界，原布局保留。");return rect;
    }
    public static void ReconcileBoard(Board board)
    {
        if(board.Pages is not null){DesktopPages.Reconcile(board);return;}
        if(board.Grid is not { } grid)return;
        // preview.5 used the opposite orientation. Correct that folder only; never shift neighbours.
        foreach(var box in board.Boxes.Where(box=>box.Columns==2&&box.Rows==4))
        {
            string key=FolderKey(box);var old=grid.Placements.GetValueOrDefault(key);grid.Placements.Remove(key);
            var next=old is null?null:new GridRect(old.X,old.Y,4,2);
            if(next is null||!Fits(grid,next))next=Free(grid,4,2,true);
            box.Columns=4;box.Rows=2;grid.Placements[key]=next;
        }
        var expected=Expected(board);
        foreach(var key in grid.Placements.Keys.Where(key=>!expected.ContainsKey(key)).ToArray())grid.Placements.Remove(key);
        foreach(var row in expected)if(!grid.Placements.ContainsKey(row.Key))grid.Placements[row.Key]=Free(grid,row.Value.W,row.Value.H,true);
        Validate(board);
    }
    public static void Validate(Board board)
    {
        if(board.Pages is not null){DesktopPages.Validate(board);return;}
        if(board.Grid is not { } grid)return;
        if(grid.Columns is <1 or >128||grid.Rows is <1 or >128||grid.Placements is null)throw new InvalidDataException("网格尺寸无效。");
        var expected=Expected(board);
        if(grid.Placements.Count!=expected.Count)throw new InvalidDataException("网格占位与项目不一致。");
        foreach(var row in grid.Placements)
        {
            if(row.Value is null||!expected.TryGetValue(row.Key,out var size)||row.Value.W!=size.W||row.Value.H!=size.H||!Fits(grid,row.Value,row.Key))throw new InvalidDataException("网格越界或重叠，修改未保存。");
        }
    }
    public static void Place(Board board,string key,int x,int y)
    {
        var grid=board.Grid??throw new LayoutEditException("桌面网格尚未就绪。");
        var old=grid.Placements.GetValueOrDefault(key)??throw new LayoutEditException("该项目已变化，请刷新。");
        var next=old with{X=x,Y=y};
        if(!Fits(grid,next,key))throw new LayoutEditException("目标网格已被占用或越界，原位置已保留。");
        grid.Placements[key]=next;
    }
    public static void Resize(Board board,string id,int columns,int rows)
    {
        if(!ValidSize(columns,rows))throw new LayoutEditException("请选择 2排×2列、2排×4列或4排×4列。");
        var box=board.Boxes.Single(box=>box.Id==id);var grid=board.Grid!;string key=FolderKey(box);
        var next=grid.Placements[key] with{W=columns,H=rows};
        if(!Fits(grid,next,key))throw new LayoutEditException("周围空间不足，请先移动文件夹再调整布局。");
        box.Columns=columns;box.Rows=rows;grid.Placements[key]=next;
    }
    public static void Dissolve(Board board,string id)
    {
        var box=board.Boxes.Single(box=>box.Id==id);board.Boxes.Remove(box);DesktopPages.Forget(board,FolderKey(box));
        foreach(var item in box.Items)if(!board.RootItems.Any(root=>StringComparer.OrdinalIgnoreCase.Equals(root.Target,item.Target)))board.RootItems.Add(item);
        ReconcileBoard(board);
    }
    public static void MoveReference(LayoutState state,Board board,DesktopItem item,string? sourceFolder,string? destinationFolder,int? x=null,int? y=null)
    {
        var source=sourceFolder is null?board.RootItems:board.Boxes.Single(box=>box.Id==sourceFolder).Items;
        if(!source.Any(row=>StringComparer.OrdinalIgnoreCase.Equals(row.Target,item.Target)))throw new LayoutEditException("来源已变化，请刷新。");
        if(sourceFolder==destinationFolder){if(sourceFolder is null&&x.HasValue&&y.HasValue)Place(board,ItemKey(item),x.Value,y.Value);return;}
        var target=destinationFolder is null?board.RootItems:board.Boxes.Single(box=>box.Id==destinationFolder).Items;
        GridRect? reserved=null;
        if(board.Pages is not null&&destinationFolder is null)
        {
            reserved=x.HasValue&&y.HasValue?new(x.Value,y.Value,1,1):ReserveSpace(board,1,1);
            if(!Fits(board.Grid!,reserved))throw new LayoutEditException("当前页目标位置已被占用或越界，原位置已保留。");
        }
        if(destinationFolder is null&&state.Boards.SelectMany(b=>b.Boxes).Where(box=>box.Id!=sourceFolder).SelectMany(box=>box.Items).Any(row=>StringComparer.OrdinalIgnoreCase.Equals(row.Target,item.Target)))throw new LayoutEditException("该项目仍在其它文件夹中，请先移除重复引用。");
        if(!target.Any(row=>StringComparer.OrdinalIgnoreCase.Equals(row.Target,item.Target)))target.Add(item);
        source.RemoveAll(row=>StringComparer.OrdinalIgnoreCase.Equals(row.Target,item.Target));
        if(sourceFolder is null)DesktopPages.Forget(board,ItemKey(item));
        if(reserved is not null)board.Grid!.Placements[ItemKey(item)]=reserved;
        ReconcileBoard(board);
        if(destinationFolder is null&&x.HasValue&&y.HasValue)Place(board,ItemKey(item),x.Value,y.Value);
    }
    public static void MoveScreen(Board source,Board destination,string key)
    {
        if(source.Id==destination.Id)return;
        if(destination.Grid is null)throw new LayoutEditException("目标屏幕尚未就绪。");
        var old=DesktopPages.Position(source,key)??throw new LayoutEditException("来源项目已变化。");
        var slot=destination.Pages is null?null:ReserveSpace(destination,old.W,old.H);
        if(key.StartsWith("folder:",StringComparison.Ordinal))
        {var box=source.Boxes.Single(box=>FolderKey(box)==key);source.Boxes.Remove(box);destination.Boxes.Add(box);}
        else
        {var item=source.RootItems.Single(item=>ItemKey(item)==key);source.RootItems.Remove(item);destination.RootItems.Add(item);}
        DesktopPages.Forget(source,key);if(slot is not null)destination.Grid!.Placements[key]=slot;ReconcileBoard(source);ReconcileBoard(destination);
    }
}

public sealed class LayoutEditException(string message) : InvalidOperationException(message);
