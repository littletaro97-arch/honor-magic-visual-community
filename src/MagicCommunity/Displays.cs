using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace MagicCommunity;
public sealed record Display(string Id, string Label, string Device, int X, int Y, int Width, int Height, bool Primary)
{
    public int PixelX{get;init;}
    public int PixelY{get;init;}
    public int PixelWidth{get;init;}
    public int PixelHeight{get;init;}
    public uint Dpi { get; init; }
    public string HardwareIdentity { get; init; } = "";

    // ── P0 定位基准（2026-11-07）────────────────────────────────────────────
    // 桌面宿主、顶栏、Dock 一律以「显示器全区域 rcMonitor」为基准，
    // 不再用工作区 rcWork 当原点。
    //
    // 原因：rcWork 是「扣掉系统任务栏之后剩下的区域」。任务栏停靠在底部时
    // rcWork.top 恰好等于 rcMonitor.top（=0），所以看起来是对的；一旦任务栏
    // 换到上/左/右，rcWork 的原点就带着任务栏厚度，整个桌面和顶栏会整体偏移，
    // 屏幕那条边空出一条。
    //
    // X/Y/Width/Height 仍然是 rcWork，只留给「窗口居中」这类真正需要可用区的
    // 场景（见 Displays.Place）。顶栏自己的占位交给 AppBar 协议协商。
    public int DesktopLeft => PixelWidth > 0 ? PixelX : X;
    public int DesktopTop => PixelHeight > 0 ? PixelY : Y;
    public int DesktopWidth => PixelWidth > 0 ? PixelWidth : Width;
    public int DesktopHeight => PixelHeight > 0 ? PixelHeight : Height;
}
public static class Displays
{
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct MonitorInfo
    {
        public uint Size; public Rect Monitor, Work; public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Device;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct DisplayDevice
    {
        public uint Size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Description;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Key;
    }
    private delegate bool Callback(IntPtr monitor, IntPtr dc, ref Rect rect, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, Callback callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice monitor, uint flags);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("shcore.dll")] private static extern int GetDpiForMonitor(IntPtr monitor, int kind, out uint x, out uint y);
    private static string Hardware(string id)
    {
        // The interface gives the registry instance. Persist only a hash of EDID,
        // never its raw serial. Without a unique serial use conservative reflow.
        var parts=id.Split('#');if(parts.Length<3)return "";
        try {
            using var key=Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\DISPLAY\"+parts[1]+"\\"+parts[2]+@"\Device Parameters");
            if(key?.GetValue("EDID") is not byte[] edid || edid.Length<128 || edid.Length>4096)return "";
            bool serial=BitConverter.ToUInt32(edid,12) is not (0 or uint.MaxValue);
            for(int offset=54;offset+18<=126;offset+=18)if(edid[offset]==0&&edid[offset+1]==0&&edid[offset+3]==0xff)
                serial|=System.Text.Encoding.ASCII.GetString(edid,offset+5,13).Trim('\0',' ','\r','\n') is {Length:>2} value && value.Any(c=>c!='0');
            return serial?Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(edid)):"";
        } catch(Exception e)when(e is System.Security.SecurityException or UnauthorizedAccessException or System.IO.IOException){return "";}
    }
    public static List<Display> Read()
    {
        var displays = new List<Display>();
        Exception? error = null;
        Callback callback = (IntPtr monitor, IntPtr dc, ref Rect rect, IntPtr data) =>
        {
            var info = new MonitorInfo { Size = (uint)Marshal.SizeOf<MonitorInfo>() };
            if (!GetMonitorInfo(monitor, ref info)) { error = new Win32Exception(); return false; }
            var device = new DisplayDevice { Size = (uint)Marshal.SizeOf<DisplayDevice>() };
            var found = EnumDisplayDevices(info.Device, 0, ref device, 1); // EDD_GET_DEVICE_INTERFACE_NAME
            // Interface identity survives primary/secondary order changes. Fallback is labeled explicitly.
            string id = found && !string.IsNullOrWhiteSpace(device.Id) ? device.Id : "temporary:" + info.Device;
            uint dpi=0;try{if(GetDpiForMonitor(monitor,0,out var dx,out _)==0)dpi=dx;}catch(DllNotFoundException){}catch(EntryPointNotFoundException){}
            displays.Add(new(id, $"{((info.Flags & 1) != 0 ? "主屏" : "副屏")} · {info.Monitor.Right-info.Monitor.Left} × {info.Monitor.Bottom-info.Monitor.Top}" + (id.StartsWith("temporary:") ? "（临时标识）" : ""),
                info.Device, info.Work.Left, info.Work.Top, info.Work.Right-info.Work.Left, info.Work.Bottom-info.Work.Top, (info.Flags & 1) != 0){PixelX=info.Monitor.Left,PixelY=info.Monitor.Top,PixelWidth=info.Monitor.Right-info.Monitor.Left,PixelHeight=info.Monitor.Bottom-info.Monitor.Top,Dpi=dpi,HardwareIdentity=Hardware(id)});
            return true;
        };
        if (!EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero) || error is not null) throw error ?? new Win32Exception();
        return displays.OrderByDescending(x => x.Primary).ToList();
    }
    public static void Place(Window window, Display display)
    {
        var hwnd = new WindowInteropHelper(window).Handle;
        // Move first; then obtain the target monitor's actual window DPI after WM_DPICHANGED.
        if (!SetWindowPos(hwnd, IntPtr.Zero, display.X+24, display.Y+24, 0, 0, 0x1 | 0x4 | 0x10)) throw new Win32Exception();
        uint dpi = GetDpiForWindow(hwnd); if (dpi == 0) dpi = 96;
        int width = Math.Min((int)Math.Round(580 * dpi / 96.0), display.Width-48);
        int height = Math.Min((int)Math.Round(630 * dpi / 96.0), display.Height-48);
        if (!SetWindowPos(hwnd, IntPtr.Zero, display.X+24, display.Y+24, Math.Max(240,width), Math.Max(200,height), 0x4 | 0x10)) throw new Win32Exception();
    }
}
