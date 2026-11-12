using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace MagicCommunity;

// Keep verified Magic tool windows owned by the existing Explorer desktop root.
// Refuse different DPI contexts: cross-process SetParent can otherwise reset DPI.
public static class NativeDesktopHost
{
    public static NativeIconRect ReadBounds(long value,string root)
    {
        NativeRegion.Verify(value,root);
        if(!GetWindowRect(new(value),out var rect))throw new Win32Exception(Marshal.GetLastWin32Error());
        return new(rect.Left,rect.Top,rect.Right,rect.Bottom);
    }
    public static long Attach(long value,Display display,string root,bool show=false)
    {
        NativeRegion.Verify(value,root);var hwnd=new IntPtr(value);
        var list=NativeDesktop.FindDesktopListView();var parent=GetParent(list);
        var name=new StringBuilder(256);GetClassName(parent,name,name.Capacity);
        if(list==IntPtr.Zero||parent==IntPtr.Zero||name.ToString()!="SHELLDLL_DefView")throw new InvalidOperationException("Explorer 桌面视图已变化。");
        GetWindowThreadProcessId(parent,out uint pid);var process=OpenProcess(0x1000,false,pid);
        if(process==IntPtr.Zero)throw new Win32Exception();
        try
        {
            var image=new StringBuilder(32768);uint length=(uint)image.Capacity;
            if(!QueryFullProcessImageName(process,0,image,ref length)||!StringComparer.OrdinalIgnoreCase.Equals(image.ToString(),Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"explorer.exe")))throw new InvalidOperationException("桌面视图不属于 Windows Explorer。");
        }
        finally{CloseHandle(process);}
        if(!AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(hwnd),GetWindowDpiAwarenessContext(parent)))throw new InvalidOperationException("文件夹与原生桌面的 DPI 模式不一致，已停止挂接。");
        long style=GetWindowLongPtr(hwnd,-16).ToInt64(),extended=GetWindowLongPtr(hwnd,-20).ToInt64();
        // Preserve DWM transparency with a top-level tool surface owned by Explorer.
        var owner=GetAncestor(parent,2);if(owner==IntPtr.Zero)throw new InvalidOperationException("原生桌面所有者不可用。");
        GetWindowThreadProcessId(owner,out uint ownerPid);if(ownerPid!=pid)throw new InvalidOperationException("桌面所有者身份不一致。");
        SetParent(hwnd,IntPtr.Zero);
        SetWindowLongPtr(hwnd,-16,new IntPtr((style&~0x40cf0000L)|0x80000000L));
        // A desktop click must deactivate Shell flyouts (Win+A/Win+N). Keeping
        // WS_EX_NOACTIVATE here prevents that normal mouse activation. The host
        // guard manages desktop depth on foreground events; attaching/showing never activates.
        SetWindowLongPtr(hwnd,-20,new IntPtr((extended&~0x08040108L)|0x00000080L));
        SetWindowLongPtr(hwnd,-8,owner);
        if(GetParent(hwnd)!=owner)throw new Win32Exception(Marshal.GetLastWin32Error(),"挂接原生桌面所有者失败。");
        var point=new NativeDesktop.Point{X=display.X,Y=display.Y};
        // The host runtime can cache visibility even when the recovery guard hides the HWND.
        // Final readiness therefore sets OS visibility without activating the desktop.
        // Lower the owned group together. SWP_NOOWNERZORDER would strand this
        // surface below Explorer: the next click then hits the native desktop.
        if(!SetWindowPos(hwnd,new IntPtr(1),point.X,point.Y,display.Width,display.Height,0x10|0x20|(show?0x40u:0u)))throw new Win32Exception();
        if(show&&!IsWindowVisible(hwnd))throw new InvalidOperationException("桌面窗口显示失败，保留 Windows 原图标。");
        return owner.ToInt64();
    }
    public static int RestoreComposition(long value,string root)
    {
        NativeRegion.Verify(value,root);
        var margins=new Margins{Left=-1,Right=-1,Top=-1,Bottom=-1};
        return DwmExtendFrameIntoClientArea(new(value),ref margins);
    }
    public static bool IsKeyboardForeground(long value,string root)
    {
        NativeRegion.Verify(value,root);
        var foreground=GetForegroundWindow();
        return foreground!=IntPtr.Zero&&GetAncestor(foreground,2)==new IntPtr(value);
    }
    [StructLayout(LayoutKind.Sequential)]private struct Margins{public int Left,Right,Top,Bottom;}
    [StructLayout(LayoutKind.Sequential)]private struct WindowRect{public int Left,Top,Right,Bottom;}
    [DllImport("user32.dll",SetLastError=true)]private static extern bool GetWindowRect(IntPtr hwnd,out WindowRect rect);
    [DllImport("dwmapi.dll")]private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd,ref Margins margins);
    [DllImport("user32.dll")]private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]private static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")]private static extern IntPtr GetAncestor(IntPtr hwnd,uint flags);
    [DllImport("user32.dll")]private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]private static extern int GetClassName(IntPtr hwnd,StringBuilder text,int count);
    [DllImport("user32.dll")]private static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("kernel32.dll")]private static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder path,ref uint length);
    [DllImport("kernel32.dll")]private static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")]private static extern IntPtr GetWindowDpiAwarenessContext(IntPtr hwnd);
    [DllImport("user32.dll")]private static extern bool AreDpiAwarenessContextsEqual(IntPtr first,IntPtr second);
    [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")]private static extern IntPtr GetWindowLongPtr(IntPtr hwnd,int index);
    [DllImport("user32.dll",EntryPoint="SetWindowLongPtrW",SetLastError=true)]private static extern IntPtr SetWindowLongPtr(IntPtr hwnd,int index,IntPtr value);
    [DllImport("user32.dll",SetLastError=true)]private static extern IntPtr SetParent(IntPtr child,IntPtr parent);
    [DllImport("user32.dll",SetLastError=true)]private static extern bool SetWindowPos(IntPtr hwnd,IntPtr after,int x,int y,int width,int height,uint flags);
}
