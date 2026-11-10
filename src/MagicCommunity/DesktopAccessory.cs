using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace MagicCommunity;

public sealed record AccessoryProjection(double OriginX,double OriginY,double StepX,double StepY,double Scale,int Columns,int Rows)
{
    public double WorkspaceWidth{get;init;}
    public double WorkspaceHeight{get;init;}
    public string Notice{get;init;}="";
    public DesktopGridInsets DockInsets{get;init;}
    public bool AdaptiveSecondary{get;init;}
    public NativeIconRect Frame(GridRect rect,Display display)
    {
        double width=rect.W*StepX-UnifiedDesktopGrid.Inset*StepX/96,height=rect.H*StepY-UnifiedDesktopGrid.Inset*StepX/96;
        int left=(int)Math.Round(OriginX+rect.X*StepX),top=(int)Math.Round(OriginY+rect.Y*StepY);
        // P0: icon frames are relative to the monitor origin (rcMonitor),
        // matching the desktop host window's own coordinate system.
        return new(display.DesktopLeft+left,display.DesktopTop+top,display.DesktopLeft+left+(int)Math.Round(width),display.DesktopTop+top+(int)Math.Round(height));
    }
    public static AccessoryProjection Create(Display display,double scale,NativeDesktopSnapshot? native=null)
    {
        if(!double.IsFinite(scale)||scale is <.5 or >8)throw new LayoutEditException("屏幕缩放比例无效。");
        double width=display.DesktopWidth/scale,height=display.DesktopHeight/scale;
        double cell=width<UnifiedDesktopGrid.Cell*6+UnifiedDesktopGrid.MarginX*2?80:UnifiedDesktopGrid.Cell;
        cell=Math.Min(cell,Math.Max(1,Math.Min(width,height)));
        int columns=Math.Clamp((int)Math.Floor(Math.Max(cell,width-2*UnifiedDesktopGrid.MarginX)/cell),1,128);
        if(columns>=6&&columns%2!=0)columns--;
        int rows=Math.Clamp((int)Math.Floor(Math.Max(cell,height-2*UnifiedDesktopGrid.MarginY)/cell),1,128);
        return new((width-columns*cell)*scale/2,(height-rows*cell)*scale/2,cell*scale,cell*scale,scale,columns,rows);
    }
}
public static class AccessoryPlacement
{
    public static bool Overlaps(NativeIconRect a,NativeIconRect b)=>a.Left<b.Right&&a.Right>b.Left&&a.Top<b.Bottom&&a.Bottom>b.Top;
    public static bool NativeFree(GridRect rect,Display display,AccessoryProjection projection,NativeDesktopSnapshot native)
    {
        var frame=projection.Frame(rect,display);return frame.Left>=display.DesktopLeft&&frame.Top>=display.DesktopTop&&frame.Right<=display.DesktopLeft+display.DesktopWidth&&frame.Bottom<=display.DesktopTop+display.DesktopHeight&&!native.Icons.Any(icon=>Overlaps(frame,icon));
    }
    public static void Ensure(Board board,Display display,AccessoryProjection projection,NativeDesktopSnapshot native,bool relocate)
    {
        if(!native.Available)throw new LayoutEditException("原生桌面位置暂不可读，修改未保存。");
        var grid=board.Grid!;grid.FoldersOnly=true;
        foreach(var key in grid.Placements.Keys.Where(key=>key.StartsWith("item:",StringComparison.Ordinal)).ToArray())grid.Placements.Remove(key);
        grid.Columns=projection.Columns;grid.Rows=projection.Rows;grid.Bounded=true;
        DesktopGridActions.ReconcileBoard(board);
        foreach(var box in board.Boxes)
        {
            string key=DesktopGridActions.FolderKey(box);var old=grid.Placements[key];if(NativeFree(old,display,projection,native))continue;
            if(!relocate)throw new LayoutEditException("目标位置被 Windows 图标占用或超出当前屏幕，原位置已保留。");
            bool found=false;
            for(int y=0;y<=projection.Rows-box.Rows&&!found;y++)for(int x=0;x<=projection.Columns-box.Columns;x++)
            {
                var rect=new GridRect(x,y,box.Columns,box.Rows);if(!NativeFree(rect,display,projection,native))continue;
                try{DesktopGridActions.Place(board,key,x,y);found=true;break;}catch(LayoutEditException){}
            }
            if(!found)throw new LayoutEditException("当前屏幕没有可避让原生图标的位置；请先在整理入口移到较空的屏幕。");
        }
        grid.NativeInitialized=true;DesktopGridActions.Validate(board);
    }
}

// Region clipping applies only to this session's verified Magic widget HWNDs.
public static class NativeRegion
{
    public static void Verify(long value,string root)
    {
        var hwnd=new IntPtr(value);if(!IsWindow(hwnd))throw new InvalidOperationException("文件夹窗口已变化。");
        GetWindowThreadProcessId(hwnd,out uint pid);var title=new StringBuilder(256);GetWindowText(hwnd,title,title.Capacity);
        if(title.ToString()!="Magic 视界 · 桌面收纳")throw new InvalidOperationException("拒绝修改非 Magic 文件夹窗口。");
        if(CompositionRendererIdentity.Verify(value,root))return;
        string expected=NativeHostSelection.Resolve(root);
        var process=OpenProcess(0x1000,false,pid);if(process==IntPtr.Zero)throw new Win32Exception();
        try{var path=new StringBuilder(32768);uint length=(uint)path.Capacity;if(!QueryFullProcessImageName(process,0,path,ref length)||!StringComparer.OrdinalIgnoreCase.Equals(path.ToString(),expected))throw new InvalidOperationException("文件夹宿主身份不匹配。");}
        finally{CloseHandle(process);}
    }
    public static int Apply(long value,double width,double height,IReadOnlyList<(double Left,double Top,double Right,double Bottom)> regions,string root)
    {
        Verify(value,root);if(!double.IsFinite(width)||!double.IsFinite(height)||width is <1 or >32768||height is <1 or >32768||regions.Count>128)throw new InvalidDataException("窗口命中区域无效。");
        var hwnd=new IntPtr(value);if(!GetClientRect(hwnd,out var client)||!GetWindowRect(hwnd,out var window))throw new Win32Exception();
        var origin=new NativeDesktop.Point();if(!ClientToScreen(hwnd,ref origin))throw new Win32Exception();double sx=client.Right/width,sy=client.Bottom/height;
        if(sx is <.1 or >8||sy is <.1 or >8)throw new InvalidDataException();
        IntPtr combined=CreateRectRgn(0,0,0,0);if(combined==IntPtr.Zero)throw new Win32Exception();
        try
        {
            foreach(var rect in regions)
            {
                if(!new[]{rect.Left,rect.Top,rect.Right,rect.Bottom}.All(double.IsFinite)||rect.Left<0||rect.Top<0||rect.Right>width||rect.Bottom>height||rect.Right<=rect.Left||rect.Bottom<=rect.Top)throw new InvalidDataException("命中区域越界。");
                var part=CreateRectRgn((int)Math.Floor(rect.Left*sx)+origin.X-window.Left,(int)Math.Floor(rect.Top*sy)+origin.Y-window.Top,(int)Math.Ceiling(rect.Right*sx)+origin.X-window.Left,(int)Math.Ceiling(rect.Bottom*sy)+origin.Y-window.Top);
                if(part==IntPtr.Zero)throw new Win32Exception();try{if(CombineRgn(combined,combined,part,2)==0)throw new Win32Exception();}finally{DeleteObject(part);}
            }
            if(SetWindowRgn(hwnd,combined,true)==0)throw new Win32Exception();combined=IntPtr.Zero;return regions.Count;
        }
        finally{if(combined!=IntPtr.Zero)DeleteObject(combined);}
    }
    public static uint Dpi(IntPtr hwnd)=>GetDpiForWindow(hwnd);
    [StructLayout(LayoutKind.Sequential)]private struct Rect{public int Left,Top,Right,Bottom;}
    [DllImport("user32.dll")]private static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")]private static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]private static extern int GetWindowText(IntPtr hwnd,StringBuilder text,int count);
    [DllImport("kernel32.dll")]private static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder path,ref uint length);
    [DllImport("kernel32.dll")]private static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")]private static extern bool GetClientRect(IntPtr hwnd,out Rect rect);
    [DllImport("user32.dll")]private static extern bool GetWindowRect(IntPtr hwnd,out Rect rect);
    [DllImport("user32.dll")]private static extern bool ClientToScreen(IntPtr hwnd,ref NativeDesktop.Point point);
    [DllImport("user32.dll")]private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("gdi32.dll")]private static extern IntPtr CreateRectRgn(int left,int top,int right,int bottom);
    [DllImport("gdi32.dll")]private static extern int CombineRgn(IntPtr target,IntPtr first,IntPtr second,int mode);
    [DllImport("gdi32.dll")]private static extern bool DeleteObject(IntPtr value);
    [DllImport("user32.dll",SetLastError=true)]private static extern int SetWindowRgn(IntPtr hwnd,IntPtr region,bool redraw);
}

public sealed class NativeDesktopTracker:IDisposable
{
    private readonly Action notify;private readonly AutoResetEvent signal=new(false);private readonly Timer debounce;
    private NativeDesktopSnapshot snapshot=new(false,"正在读取原生桌面。",0,0,0,[]);private volatile bool disposed;private long revision;
    private readonly TaskCompletionSource initialized=new(TaskCreationOptions.RunContinuationsAsynchronously);private readonly WinEventCallback callback;private readonly IntPtr hook;
    public NativeDesktopSnapshot Snapshot=>Volatile.Read(ref snapshot);public long Revision=>Interlocked.Read(ref revision);
    public NativeDesktopTracker(Action notify)
    {
        this.notify=notify;debounce=new(_=>{if(!disposed)signal.Set();},null,Timeout.Infinite,Timeout.Infinite);
        callback=(_,_,hwnd,_,_,_,_)=>{try{if(!disposed&&hwnd.ToInt64()==Snapshot.ListWindow)debounce.Change(180,Timeout.Infinite);}catch(ObjectDisposedException){}};
        hook=SetWinEventHook(0x8000,0x800b,IntPtr.Zero,callback,0,0,0);
        var thread=new Thread(Run){IsBackground=true,Name="Magic native desktop positions"};thread.SetApartmentState(ApartmentState.STA);thread.Start();Refresh();
    }
    private void Run(){while(true){signal.WaitOne();if(disposed)return;var next=NativeDesktop.Read();var prior=Snapshot;Volatile.Write(ref snapshot,next);initialized.TrySetResult();if(JsonSerializer.Serialize(prior)!=JsonSerializer.Serialize(next)){Interlocked.Increment(ref revision);notify();}}}
    public Task Ready(CancellationToken cancellation)=>initialized.Task.WaitAsync(TimeSpan.FromSeconds(6),cancellation);
    public void Refresh(){if(!disposed)debounce.Change(0,Timeout.Infinite);}
    public void Dispose(){disposed=true;if(hook!=IntPtr.Zero)UnhookWinEvent(hook);debounce.Dispose();signal.Set();}
    private delegate void WinEventCallback(IntPtr hook,uint code,IntPtr hwnd,int objectId,int childId,uint thread,uint time);
    [DllImport("user32.dll")]private static extern IntPtr SetWinEventHook(uint first,uint last,IntPtr module,WinEventCallback callback,uint process,uint thread,uint flags);
    [DllImport("user32.dll")]private static extern bool UnhookWinEvent(IntPtr hook);
}
