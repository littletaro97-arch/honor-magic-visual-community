using System.Text.Json.Nodes;
namespace MagicCommunity;
internal sealed partial class NativeSystemUiSession
{
    private void ProjectPcManager(JsonObject projected)
    {
        foreach(string catalog in new[]{"toolbarModules","dockModules"})
            if(projected[catalog] is JsonArray modules)
                for(int i=modules.Count-1;i>=0;i--)
                    if(modules[i]?["id"]?.GetValue<string>()=="magic.pc-manager" && (pcManager is null || catalog=="dockModules"))modules.RemoveAt(i);
        foreach(string surface in new[]{"toolbar","dock"})foreach(string group in new[]{"left","center","right"})
            if(projected[surface]?[group] is JsonArray rows)
                for(int i=rows.Count-1;i>=0;i--)
                    if(rows[i]?["nativeModule"]?.GetValue<string>()=="magic.pc-manager")
                    {
                        if(pcManager is null){rows.RemoveAt(i);continue;}
                        rows[i]!["label"]="电脑管家";rows[i]!["tooltip"]="打开"+pcManager.Brand+"电脑管家负一屏";rows[i]!["dataAvailable"]=true;
                    }
        if(projected["warnings"] is JsonArray warnings)
            for(int i=warnings.Count-1;i>=0;i--)if(warnings[i]?["module"]?.GetValue<string>()=="magic.pc-manager")warnings.RemoveAt(i);
    }
}
