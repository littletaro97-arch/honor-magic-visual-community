using System.Text.Json;
using MagicCommunity;
internal static class Nov09DetailsChecks
{
    internal static async Task Run(string path)
    {
        var checks=new Dictionary<string,bool>();void Check(string name,bool pass){checks[name]=pass;if(!pass)throw new InvalidOperationException(name);}
        NotificationWindowChecks.Run(checks);
        var preferences=JsonSerializer.Deserialize<NotificationPlacementPreferences>("{\"FixedMonitor\":\"second\",\"Monitors\":{\"first\":{\"Anchor\":\"wrong\",\"X\":99999},\"second\":{\"Anchor\":\"bottom-right\",\"Y\":-99999}}}")!;preferences.Normalize();
        Check("invalid-anchor-falls-back",preferences.For("first").Anchor=="top-center");
        Check("offsets-bounded",preferences.For("first").X==4096&&preferences.For("second").Y==-4096);
        var restored=JsonSerializer.Deserialize<NotificationPlacementPreferences>(JsonSerializer.Serialize(preferences))!;
        Check("per-display-reconnect-keeps-choice",restored.FixedMonitor=="second"&&restored.For("second").Anchor=="bottom-right");
        Check("new-display-default-no-overwrite",restored.For("new-display").Anchor=="top-center"&&restored.For("second").Anchor=="bottom-right");
        var notices=JsonSerializer.SerializeToElement(new{notifications=new[]{new{id="123",appId="QQ"},new{id="456",appId="Other"}}});
        Check("notification-identity-accepted",NativeNotificationWindows.HasSource(notices,"123","QQ"));
        Check("notification-spoofed-source-rejected",!NativeNotificationWindows.HasSource(notices,"123","Other"));
        Check("expired-notification-rejected",!NativeNotificationWindows.HasSource(notices,"789","QQ"));
        Check("absent-provider-rejected",!NativeNotificationWindows.HasSource(default,"123","QQ"));
        var owner=new NativeNotificationWindows();int samples=0,waits=0;
        JsonElement Empty()=>JsonSerializer.SerializeToElement(new{windows=Array.Empty<object>()});
        JsonElement Present()=>JsonSerializer.SerializeToElement(new{windows=new[]{new{id="owned",aumid="QQ",executable="C:/Owned/QQ.exe",focused=false,minimized=true}}});
        var observed=await owner.WaitForWindow("QQ",[],()=>Task.FromResult(++samples>=3?Present():Empty()),_=>{waits++;return Task.CompletedTask;});
        Check("late-tray-window-observed",observed.ValueKind==JsonValueKind.Object&&samples==3&&waits==2);
        samples=waits=0;
        observed=await owner.WaitForWindow("QQ",[],()=>{samples++;return Task.FromResult(Empty());},_=>{waits++;return Task.CompletedTask;});
        Check("no-window-is-not-success",observed.ValueKind!=JsonValueKind.Object&&samples==8&&waits==7);
        samples=0;observed=await owner.WaitForWindow("NotQQ",[],()=>{samples++;return Task.FromResult(Present());},_=>Task.CompletedTask);
        Check("unrelated-window-never-selected",observed.ValueKind!=JsonValueKind.Object&&samples==8);
        Check("pc-manager-nonmatching-exe-rejected",PcManagerApplication.Validate(Environment.ProcessPath)==null);
        Check("pc-manager-missing-path-rejected",PcManagerApplication.Validate("C:/Missing/PCManager.exe")==null);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        File.WriteAllText(path,JsonSerializer.Serialize(new{allPass=checks.Values.All(v=>v),checks,scope="isolated data and owned hidden window; no real notification/app actions"},new JsonSerializerOptions{WriteIndented=true}));
    }
}
