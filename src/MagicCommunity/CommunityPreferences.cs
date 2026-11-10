using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
namespace MagicCommunity;
public sealed class CommunityPreferences
{
    private static bool? saver;private static bool modernSaver;
    // Every subscribed window must repaint a valid event, even when another
    // window has already updated this process-wide cache for the same event.
    internal static bool AcceptPowerSetting(Guid id,int value){bool modern=id==NativePowerSubscription.Energy;if(!modern&&(id!=NativePowerSubscription.Legacy||modernSaver)||value<0||value>(modern?2:1))return false;saver=value!=0;modernSaver|=modern;return true;}
    private string iconScheme="honor-white";
    public string IconScheme{get=>iconScheme;set=>iconScheme=value=="honor-translucent"?"honor-translucent":"honor-white";}
    // The desktop state keeps no surface at all, so the wallpaper shows through; the
    // original does the same and only gives up transparency under a maximised window.
    public bool TransparentToolbar{get;set;}=true;
    public bool TransparentToolbarMask{get;set;}=false;
    public bool CollapsedFolderIconNames{get;set;}=false;
    public bool DesktopIconNames{get;set;}=true;
    // What the bar paints once an application covers the work area: the original goes
    // black, the previous clone kept its glass, and "transparent" ignores the window.
    private string coveredBackdrop="black";
    public string CoveredBackdrop{get=>coveredBackdrop;set=>coveredBackdrop=value is "black" or "acrylic" or "transparent"?value:"black";}
    // A colour lockup carries its own paint, so a dark one (ASUS, Samsung, Alienware)
    // turns into a blank cell once the bar goes black - and so does a tray icon that
    // the app shipped as a dark glyph (Tailscale). On by default: the renderer swaps
    // such a lockup for its mono cut, and puts a plate of the opposite tone under such
    // an icon, only when the measured paint cannot be seen against this backdrop.
    // Off keeps every mark exactly as the vendor or the app shipped it.
    public bool AutoContrast{get;set;}=true;
    // The brightness card. Nothing on the machine says who moved the value, and an
    // ambient sensor moves the same one a hand does, so the default is to speak only
    // for our own writes; "follow" brings the old every-change behaviour back.
    public bool BrightnessHints{get;set;}=true;
    public bool BrightnessFollowSystem{get;set;}=false;
    public bool ShowPages{get;set;}=true;
    public bool FadePages{get;set;}=true;
    public bool WindowAnimations{get;set;}=true;
    public bool ReduceOnBatterySaver{get;set;}=true;
    public bool FolderAnimations{get;set;}=true;
    public bool DockDissolve{get;set;}=false;
    // Context menus: one column that scrolls, or the v0.4.9 column layout. The column
    // layout is what the upstream snapshot brought, and its last column runs off the
    // monitor on a long list, so the default here is the scrolling one.
    public bool ContextMenuColumns{get;set;}=false;
    // "当前应用"/"窗口标题" put a mark in front of the name so it is not glued to its
    // neighbour. The laptop mark is what SeelenUI ships and what this bar has always
    // drawn; a bar reads as a plain separator, and "none" leaves the name on its own.
    public string FocusedSeparator{get;set;}="desktop";
    // The bar slides aside when a window comes near it. Some readers want it to go
    // quietly instead, so the dissolve is a switch rather than a fixed behaviour.
    public bool ToolbarDissolve{get;set;}=true;
    // T39: the whole tray block folds its icons into the button while it is dragged.
    // Off keeps the plain ghost, and no Scale/Opacity is ever written.
    public bool ToolbarDragButtonOnly{get;set;}=true;
    // The account card, tuned the way MyMagicBar tuned its menu: width and height
    // as free numbers, a nudge in each direction, an animation switch, and ten
    // system doors that are each optional and all off by default so the card stays
    // short. The targets are the same ones MyMagicBar's MenuSettings carried.
    public double AccountCardWidth{get;set;}=370;
    public double AccountCardHeight{get;set;}=528;
    // Screen position. Negative means "follow the avatar", which is the default.
    public double AccountCardX{get;set;}=-1;
    public double AccountCardY{get;set;}=-1;
    public double AccountCardOffsetX{get;set;}=0;
    public double AccountCardOffsetY{get;set;}=0;
    // The visual parameters from the roadmap: row height, gap between rows and the
    // corner. Font size is read from AccountCardFontSize the same way.
    public double AccountCardRowHeight{get;set;}=38;
    public double AccountCardGap{get;set;}=0;
    public double AccountCardFontSize{get;set;}=13;
    public double AccountCardRadius{get;set;}=16;
    public bool AccountCardMotion{get;set;}=true;
    public bool AccountEntrySystem{get;set;}
    public bool AccountEntryTaskManager{get;set;}
    public bool AccountEntryDisplay{get;set;}
    public bool AccountEntrySound{get;set;}
    public bool AccountEntryNetwork{get;set;}
    public bool AccountEntryPower{get;set;}
    public bool AccountEntryBluetooth{get;set;}
    public bool AccountEntryNotification{get;set;}
    public bool AccountEntryLanguage{get;set;}
    public bool AccountEntryLocation{get;set;}
    public static CommunityPreferences Load(string root){try{string path=Path.Combine(root,"data/ui-preferences.json");if(!File.Exists(path)||new FileInfo(path).Length>16384)return new();return JsonSerializer.Deserialize<CommunityPreferences>(File.ReadAllText(path))??new();}catch(Exception e)when(e is IOException or JsonException or UnauthorizedAccessException){return new();}}
    // The bar's own menu carries no window and no controller, so the layout switch needs
    // the root-only form; the settings window still reports through the controller.
    public void Save(string root){StateStore.AtomicWrite(Path.Combine(root,"data/ui-preferences.json"),JsonSerializer.Serialize(this),null);}
    public void Save(AppController controller){if(controller.Store.ReadOnly)throw new InvalidOperationException("当前配置为只读。");Save(controller.Root);controller.SetStatus("界面设置已保存。");}
    public static (bool Saver,bool Plugged) Power(){GetSystemPowerStatus(out var p);return(saver??(p.Saver==1),p.Ac==1);}
    [StructLayout(LayoutKind.Sequential)]private struct PowerStatus{public byte Ac,Flag,Percent,Saver;public uint Life,Full;}
    [DllImport("kernel32.dll")]private static extern bool GetSystemPowerStatus(out PowerStatus p);
}
