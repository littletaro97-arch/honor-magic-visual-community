using System.Runtime.InteropServices;
using System.Text.Json;
using MagicCommunity;
internal static class IconIdentityChecks
{
    internal static void Run(string output)
    {
        string owned=Path.Combine(Path.GetDirectoryName(Path.GetFullPath(output))!,"icon-identity-owned");
        if(Directory.Exists(owned))throw new IOException("Use a fresh owned fixture directory.");Directory.CreateDirectory(owned);
        var checks=new Dictionary<string,bool>();void Check(string name,bool pass){checks[name]=pass;if(!pass)throw new InvalidDataException(name);}
        string exe=Path.Combine(owned,"msedge.exe"),custom=Path.Combine(owned,"custom.ico"),steam=Path.Combine(owned,"steam.exe");
        foreach(var file in new[]{exe,custom,steam})File.WriteAllText(file,"Owned fixture, never executed");
        Check("executable-identity",ApplicationIconIdentity.Resolve(exe)==exe);
        Check("missing-file-never-themed",ApplicationIconIdentity.Resolve(Path.Combine(owned,"QQ.exe"))=="");
        Check("display-name-not-trusted",ApplicationIconIdentity.Resolve(Path.Combine(owned,"ChatGPT.txt"))=="");
        Check("unc-not-resolved",ApplicationIconIdentity.Resolve(@"\\invalid\share\QQ.exe")=="");
        Check("real-target-wins-unrecognized-aumid",ApplicationIconIdentity.Resolve(exe,"unrecognized.custom.id")==exe);
        Check("system-package-case-folding",ApplicationIconIdentity.Resolve("","Windows.ImmersiveControlPanel_cw5n1h2txyewy!Microsoft.Windows.ImmersiveControlPanel")=="aumid:windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel");
        Check("package-identity-retained",ApplicationIconIdentity.Resolve("","Fixture_123!App")=="aumid:Fixture_123!App");
        Check("generic-shell-arguments-not-themed",ApplicationIconIdentity.Target(steam,"-silent")=="");
        Check("steam-app-id",ApplicationIconIdentity.Target(steam,"-applaunch 730")=="steam:730");
        Check("pwa-target-not-browser",ApplicationIconIdentity.Target(exe,"--app-id=owned")=="");
        string url=Path.Combine(owned,"Renamed game.url");File.WriteAllText(url,"[InternetShortcut]\r\nURL=steam://rungameid/431960\r\n");Check("steam-shortcut",ApplicationIconIdentity.Resolve(url)=="steam:431960");
        File.WriteAllText(url,"[InternetShortcut]\r\nURL=https://example.invalid/QQ.exe");Check("website-not-application",ApplicationIconIdentity.Resolve(url)=="");
        Exception? failure=null;var thread=new Thread(()=>{
            object? shell=null,link=null;
            try{
                shell=Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell",true)!);
                string path=Path.Combine(owned,"Misleading QQ name.lnk");link=((dynamic)shell!).CreateShortcut(path);((dynamic)link).TargetPath=exe;((dynamic)link).Save();
                var before=File.ReadAllBytes(path);Check("shortcut-uses-real-target",ApplicationIconIdentity.Resolve(path)==exe);Check("resolution-does-not-rewrite-shortcut",before.SequenceEqual(File.ReadAllBytes(path)));
                ((dynamic)link).IconLocation=custom;((dynamic)link).Save();Check("custom-shortcut-icon-kept",ApplicationIconIdentity.Resolve(path)=="");
            }catch(Exception e){failure=e;}finally{foreach(var value in new[]{link,shell})if(value is not null&&Marshal.IsComObject(value))Marshal.FinalReleaseComObject(value);}
        }){IsBackground=true};thread.SetApartmentState(ApartmentState.STA);thread.Start();if(!thread.Join(15000))throw new TimeoutException("Owned STA probe timed out");if(failure is not null)throw failure;
        File.WriteAllText(output,JsonSerializer.Serialize(new{allPass=true,checks,scope="Owned fake files and shortcuts; no application executed or user shortcuts changed"},new JsonSerializerOptions{WriteIndented=true}));
    }
}
