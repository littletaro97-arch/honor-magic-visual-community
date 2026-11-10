using System.IO;
using System.Text.Json;
namespace MagicCommunity;

public sealed class DesktopIconGridSettings
{
    public int Columns{get;set;}=14;
    public int Rows{get;set;}=9;
    public int RefWidth{get;set;}
    public int RefHeight{get;set;}
    public string ColumnSide{get;set;}="split";
    public string RowSide{get;set;}="split";
    public DesktopIconGridSettings Copy()=>new(){Columns=Columns,Rows=Rows,RefWidth=RefWidth,RefHeight=RefHeight,ColumnSide=ColumnSide,RowSide=RowSide};
    internal void Validate(){if(Columns is <1 or >128||Rows is <1 or >128||RefWidth is <1 or >131072||RefHeight is <1 or >131072||ColumnSide is not ("split" or "left" or "right")||RowSide is not ("split" or "top" or "bottom"))throw new InvalidDataException("桌面图标设置无效。");}
}
public sealed class DesktopSortSettings
{
    public string Mode{get;set;}="custom";
    public bool Descending{get;set;}
    internal void Validate(){if(Mode is not ("custom" or "name" or "size" or "type" or "modified"))throw new InvalidDataException("排序方式无效。");}
}
public readonly record struct DesktopGridInsets(double Left,double Top,double Right,double Bottom);
public sealed record DesktopDockFootprint(bool Enabled,string Edge,double Thickness)
{
    public static readonly DesktopDockFootprint None=new(false,"Bottom",0);
    public DesktopGridInsets Insets(Display display,double scale)
    {
        if(!Enabled)return default;
        // P0: the desktop viewport is now the whole monitor (rcMonitor), so the
        // inset is simply the overlay thickness on that edge.
        // The old formula was (rcMonitor.edge + amount - rcWork.edge), which is 0
        // only while the taskbar sits at the bottom; with a top/left/right taskbar
        // it produced a wrong (often zero) inset.
        double inset=Thickness;
        return Edge switch{
            "Left"=>new(inset,0,0,0),
            "Right"=>new(0,0,inset,0),
            "Top"=>new(0,inset,0,0),
            _=>new(0,0,0,inset)};
    }
}
public static class DesktopGridLayout
{
    public const double MinimumCell=56,Edge=24,VerticalEdge=8;
    public static (int Width,int Height) Physical(Display display)=>(display.DesktopWidth,display.DesktopHeight);
    public static (int Width,int Height) Used(Board board)
    {
        var all=DesktopPages.Grids(board).SelectMany(g=>g.Placements.Values).ToArray();
        return(all.Select(r=>r.X+r.W).DefaultIfEmpty(1).Max(),all.Select(r=>r.Y+r.H).DefaultIfEmpty(1).Max());
    }
    public static DesktopIconGridSettings Settings(Board board,Display display)
    {
        if(board.IconGrid is {} settings)return settings.Copy();
        var physical=Physical(display);
        return new(){Columns=board.Grid?.Columns??14,Rows=board.Grid?.Rows??9,RefWidth=physical.Width,RefHeight=physical.Height};
    }
    public static AccessoryProjection Project(Board board,Display display,double scale,DesktopDockFootprint? dock=null,DesktopIconGridSettings? draft=null,bool protectAnchors=true,double preferredCell=UnifiedDesktopGrid.Cell)
    {
        if(!double.IsFinite(scale)||scale is <.5 or >8)throw new LayoutEditException("屏幕缩放比例无效。");
        var settings=draft??Settings(board,display);settings.Validate();
        var physical=Physical(display);
        var inset=(dock??DesktopDockFootprint.None).Insets(display,scale);
        double width=Math.Max(1,display.DesktopWidth/scale-inset.Left-inset.Right),height=Math.Max(1,display.DesktopHeight/scale-inset.Top-inset.Bottom);
        bool adaptive=!display.Primary&&board.IconGrid is null&&draft is null;
        int columns=settings.Columns,rows=settings.Rows;
        if(adaptive){
            if(!double.IsFinite(preferredCell)||preferredCell<=0)throw new LayoutEditException("参考网格大小无效。");
            columns=Math.Clamp((int)Math.Floor(Math.Max(1,width-2*Edge)/preferredCell),1,128);
            rows=Math.Clamp((int)Math.Floor(Math.Max(1,height-2*VerticalEdge)/preferredCell),1,128);
            // A large card still needs its own span; this is a content constraint,
            // not a fixed minimum number of rows/columns on a secondary display.
            var sizes=DesktopGridActions.Expected(board).Values;
            columns=Math.Max(columns,sizes.Select(size=>size.W).DefaultIfEmpty(1).Max());
            rows=Math.Max(rows,sizes.Select(size=>size.H).DefaultIfEmpty(1).Max());
        }
        if(draft is null&&board.IconGrid is not null){
            columns=Math.Clamp((int)Math.Round((double)physical.Width/settings.RefWidth*columns,MidpointRounding.AwayFromZero),1,128);
            rows=Math.Clamp((int)Math.Round((double)physical.Height/settings.RefHeight*rows,MidpointRounding.AwayFromZero),1,128);
        }
        int wantedColumns=columns,wantedRows=rows;
        if(protectAnchors&&!adaptive){var used=Used(board);columns=Math.Max(columns,used.Width);rows=Math.Max(rows,used.Height);}
        double cell=Math.Max(1,Math.Floor(Math.Min(Math.Max(1,width-2*Edge)/columns,Math.Max(1,height-2*VerticalEdge)/rows)));
        if(adaptive)cell=Math.Min(cell,preferredCell);
        string notice=columns!=wantedColumns||rows!=wantedRows?$"当前分辨率建议 {wantedColumns} 列 × {wantedRows} 行；为保留已有位置，继续显示 {columns} 列 × {rows} 行。":"";
        if(cell<MinimumCell)notice=$"当前缩放下建议最多 {Math.Max(1,(int)((width-2*Edge)/MinimumCell))} 列 × {Math.Max(1,(int)((height-2*VerticalEdge)/MinimumCell))} 行；已有位置保留，请在桌面图标设置中调整。";
        return new((inset.Left+(width-columns*cell)/2)*scale,(inset.Top+(height-rows*cell)/2)*scale,cell*scale,cell*scale,scale,columns,rows)
            {WorkspaceWidth=width,WorkspaceHeight=height,Notice=notice,DockInsets=inset,AdaptiveSecondary=adaptive};
    }
    public static void ResizeAdaptiveSecondary(Board board,AccessoryProjection projection)
    {
        if(!projection.AdaptiveSecondary||board.IconGrid is not null)throw new LayoutEditException("仅自动副屏网格可以自适应重排。");
        if(board.Pages is null)throw new LayoutEditException("副屏网格尚未初始化。");
        foreach(var grid in DesktopPages.Grids(board)){grid.Columns=projection.Columns;grid.Rows=projection.Rows;grid.Bounded=true;grid.NativeInitialized=true;grid.FoldersOnly=false;}
        // Reconcile preserves valid anchors and page IDs. Only overflow moves;
        // additional pages keep content reachable when a rotated display shrinks.
        DesktopPages.Reconcile(board);
        if(board.Unplaced.Count>0){board.Paged=true;DesktopPages.Reconcile(board);}
    }
    public static void ResizeWithoutMoving(Board board,AccessoryProjection projection)
    {
        // Geometry changes never use Free() to relocate existing anchors.
        var grids=DesktopPages.Grids(board).ToArray();
        if(grids.Any(grid=>grid.Placements.Values.Any(r=>r.X<0||r.Y<0||r.X+r.W>projection.Columns||r.Y+r.H>projection.Rows)))throw new LayoutEditException("新工作区装不下原位置，已保留原布局。");
        foreach(var grid in grids){grid.Columns=projection.Columns;grid.Rows=projection.Rows;grid.Bounded=true;grid.NativeInitialized=true;grid.FoldersOnly=false;}
    }
    public static int Shift(int delta,string side)=>side is "left" or "top"?delta:side=="split"?delta/2:0;
    internal static string Stamp(Board board)=>JsonSerializer.Serialize(board,StateStore.Json);
    internal static Board Copy(Board board)=>JsonSerializer.Deserialize<Board>(Stamp(board),StateStore.Json)!;
}
internal sealed class DesktopLayoutUndo
{
    private sealed record Entry(string Board,string Before,string After,int Bytes);
    private readonly List<Entry> entries=[];
    private int bytes;
    private const int Limit=4*1024*1024;
    internal void CheckSize(Board board,Board after){if((DesktopGridLayout.Stamp(board).Length+DesktopGridLayout.Stamp(after).Length)*2L>Limit)throw new LayoutEditException("布局过大，无法保留本次撤销快照；原布局未改动。");}
    internal void Push(Board before,Board after){
        string a=DesktopGridLayout.Stamp(before),b=DesktopGridLayout.Stamp(after);int size=checked((a.Length+b.Length)*2);
        if(size>Limit)throw new InvalidOperationException("Undo snapshot exceeds checked bound.");
        while(entries.Count>0&&(entries.Count>=8||bytes+size>Limit)){bytes-=entries[0].Bytes;entries.RemoveAt(0);}
        entries.Add(new(after.Id,a,b,size));bytes+=size;
    }
    private static string ContentStamp(Board board){var copy=DesktopGridLayout.Copy(board);copy.ActivePageId=null;return DesktopGridLayout.Stamp(copy);}
    internal bool CanUndo(Board board)=>entries.LastOrDefault() is {} e&&e.Board==board.Id&&ContentStamp(JsonSerializer.Deserialize<Board>(e.After,StateStore.Json)!)==ContentStamp(board);
    internal bool Undo(AppController controller,string boardId){
        var current=controller.State.Boards.Single(b=>b.Id==boardId);if(!CanUndo(current))return false;
        var entry=entries[^1];var previous=JsonSerializer.Deserialize<Board>(entry.Before,StateStore.Json)!;
        if(previous.Pages?.Any(p=>p.Id==current.ActivePageId)==true)previous.ActivePageId=current.ActivePageId;
        controller.Commit(state=>{int i=state.Boards.FindIndex(b=>b.Id==boardId);state.Boards[i]=previous;}, "已撤销上次桌面布局操作。");
        entries.RemoveAt(entries.Count-1);bytes-=entry.Bytes;return true;
    }
}
internal sealed class DesktopGridEditSession
{
    private readonly AppController controller;
    private readonly DesktopLayoutUndo undo;
    private readonly Dictionary<string,Session> sessions=new(StringComparer.Ordinal);
    private sealed record Session(string Id,Board Before,int Columns,int Rows);
    internal DesktopGridEditSession(AppController controller,DesktopLayoutUndo undo){this.controller=controller;this.undo=undo;}
    internal object Handle(string boardId,Display display,double scale,DesktopDockFootprint dock,JsonElement request,AccessoryProjection? initialProjection=null)
    {
        string action=request.GetProperty("action").GetString()??"";
        var current=controller.State.Boards.Single(b=>b.Id==boardId);
        string token=request.TryGetProperty("editId",out var id)?id.GetString()??"":"";
        if(action=="begin"){
            if(controller.Store.ReadOnly||controller.Collections.IsBusy)throw new LayoutEditException("当前布局暂时不可编辑。");
            var projection=initialProjection??DesktopGridLayout.Project(current,display,scale,dock);
            var session=new Session(Guid.NewGuid().ToString("N"),DesktopGridLayout.Copy(current),projection.Columns,projection.Rows);
            sessions[boardId]=session;token=session.Id;
        }
        if(!sessions.TryGetValue(boardId,out var active)||active.Id!=token)throw new LayoutEditException("桌面编辑会话已变化，请重新打开。");
        if(action=="cancel"){sessions.Remove(boardId);return new{closed=true};}
        if(action is not ("begin" or "update" or "commit"))throw new InvalidDataException("未知桌面图标设置操作。");
        bool stale=DesktopGridLayout.Stamp(current)!=DesktopGridLayout.Stamp(active.Before);
        var settings=DesktopGridLayout.Settings(active.Before,display);
        var physical=DesktopGridLayout.Physical(display);settings.RefWidth=physical.Width;settings.RefHeight=physical.Height;
        settings.Columns=request.TryGetProperty("columns",out var c)?c.GetInt32():active.Columns;
        settings.Rows=request.TryGetProperty("rows",out var r)?r.GetInt32():active.Rows;
        if(request.TryGetProperty("columnSide",out var columnSide))settings.ColumnSide=columnSide.GetString()??"";
        if(request.TryGetProperty("rowSide",out var rowSide))settings.RowSide=rowSide.GetString()??"";
        settings.Validate();
        var draft=DesktopGridLayout.Copy(active.Before);
        int dx=DesktopGridLayout.Shift(settings.Columns-active.Columns,settings.ColumnSide),dy=DesktopGridLayout.Shift(settings.Rows-active.Rows,settings.RowSide);
        int overflow=0,extentX=0,extentY=0;
        foreach(var grid in DesktopPages.Grids(draft)){
            grid.Columns=settings.Columns;grid.Rows=settings.Rows;
            foreach(var key in grid.Placements.Keys.ToArray()){
                var rect=grid.Placements[key] with{X=grid.Placements[key].X+dx,Y=grid.Placements[key].Y+dy};grid.Placements[key]=rect;
                extentX=Math.Max(extentX,rect.X+rect.W);extentY=Math.Max(extentY,rect.Y+rect.H);
                if(rect.X<0||rect.Y<0||rect.X+rect.W>settings.Columns||rect.Y+rect.H>settings.Rows)overflow++;
            }
        }
        var p=DesktopGridLayout.Project(draft,display,scale,dock,settings,false);
        bool fits=p.StepX/scale>=DesktopGridLayout.MinimumCell;
        string warning=stale?"桌面内容或页面已变化，请取消后重新打开设置。":overflow>0?$"有 {overflow} 项超出网格，已有项目至少占用到 {extentX} 列 × {extentY} 行；请增大行列或取消整理。":!fits?"格子小于 56 DIP，请减少行列后再完成。":"";
        bool canCommit=!stale&&overflow==0&&fits;
        if(action=="commit"){
            if(!canCommit)throw new LayoutEditException(warning);draft.IconGrid=settings.Copy();undo.CheckSize(current,draft);
            controller.Commit(state=>{
                var target=state.Boards.Single(b=>b.Id==boardId);
                if(DesktopGridLayout.Stamp(target)!=DesktopGridLayout.Stamp(active.Before))throw new LayoutEditException("桌面已变化，原布局保留。");
                target.IconGrid=settings.Copy();state.SchemaVersion=Math.Max(7,state.SchemaVersion);
                var grids=DesktopPages.Grids(target).ToArray();var source=DesktopPages.Grids(draft).ToArray();
                for(int i=0;i<grids.Length;i++){grids[i].Columns=settings.Columns;grids[i].Rows=settings.Rows;grids[i].Placements=new(source[i].Placements,StringComparer.Ordinal);}
                DesktopGridActions.Validate(target);
            },$"已保存：{settings.Columns} 列 × {settings.Rows} 行");
            undo.Push(active.Before,controller.State.Boards.Single(b=>b.Id==boardId));sessions.Remove(boardId);return new{closed=true,saved=true};
        }
        int maxColumns=Math.Max(1,Math.Min(Math.Max(24,active.Columns),(int)((p.WorkspaceWidth-48)/56)));
        int maxRows=Math.Max(1,Math.Min(Math.Max(16,active.Rows),(int)((p.WorkspaceHeight-16)/56)));
        double cell=p.StepX/scale;
        int fillColumns=Math.Min(maxColumns,Math.Max(settings.Columns,(int)((p.WorkspaceWidth-48)/cell)));
        return new{editId=active.Id,columns=settings.Columns,rows=settings.Rows,baseColumns=active.Columns,baseRows=active.Rows,columnSide=settings.ColumnSide,rowSide=settings.RowSide,
            maxColumns,maxRows,minColumns=display.Primary?Math.Min(10,maxColumns):1,minRows=display.Primary?Math.Min(6,maxRows):1,cell,icon=cell*44/96,font=Math.Max(8.5,cell*12/96),
            workspaceWidth=p.WorkspaceWidth,workspaceHeight=p.WorkspaceHeight,scale,physicalWidth=physical.Width,physicalHeight=physical.Height,
            paddingX=(p.WorkspaceWidth-settings.Columns*cell)/2,paddingY=(p.WorkspaceHeight-settings.Rows*cell)/2,
            nextColumnCell=Math.Floor(Math.Min((p.WorkspaceWidth-48)/(settings.Columns+1),(p.WorkspaceHeight-16)/settings.Rows)),nextRowCell=Math.Floor(Math.Min((p.WorkspaceWidth-48)/settings.Columns,(p.WorkspaceHeight-16)/(settings.Rows+1))),
            usedColumns=extentX,usedRows=extentY,pageCount=DesktopPages.Grids(draft).Count(),overflow,canCommit,warning,fillColumns,
            projection=new{originX=p.OriginX/scale,originY=p.OriginY/scale,stepX=cell,stepY=cell},
            placements=draft.Grid!.Placements.Select(v=>new{key=v.Key,x=v.Value.X,y=v.Value.Y,w=v.Value.W,h=v.Value.H}).ToArray()};
    }
    internal void Clear()=>sessions.Clear();
}
