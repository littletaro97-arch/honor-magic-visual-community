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
    public bool TransparentToolbar{get;set;}=false;
    public bool TransparentToolbarMask{get;set;}=false;
    public bool CollapsedFolderIconNames{get;set;}=false;
    public bool DesktopIconNames{get;set;}=true;
    public bool ShowPages{get;set;}=true;
    public bool FadePages{get;set;}=true;
    public bool WindowAnimations{get;set;}=true;
    public bool ReduceOnBatterySaver{get;set;}=true;
    public bool WorkingSetTrim{get;set;}=true;
    public bool FolderAnimations{get;set;}=true;
    public bool DockDissolve{get;set;}=false;
    public static CommunityPreferences Load(string root){try{string path=Path.Combine(root,"data/ui-preferences.json");if(!File.Exists(path)||new FileInfo(path).Length>16384)return new();return JsonSerializer.Deserialize<CommunityPreferences>(File.ReadAllText(path))??new();}catch(Exception e)when(e is IOException or JsonException or UnauthorizedAccessException){return new();}}
    public void Save(AppController controller){if(controller.Store.ReadOnly)throw new InvalidOperationException("当前配置为只读。");StateStore.AtomicWrite(Path.Combine(controller.Root,"data/ui-preferences.json"),JsonSerializer.Serialize(this),null);controller.SetStatus("界面设置已保存。");}
    public static (bool Saver,bool Plugged) Power(){GetSystemPowerStatus(out var p);return(saver??(p.Saver==1),p.Ac==1);}
    [StructLayout(LayoutKind.Sequential)]private struct PowerStatus{public byte Ac,Flag,Percent,Saver;public uint Life,Full;}
    [DllImport("kernel32.dll")]private static extern bool GetSystemPowerStatus(out PowerStatus p);
}
