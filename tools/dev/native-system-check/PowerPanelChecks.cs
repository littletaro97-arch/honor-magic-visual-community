using System.Text.Json;
using System.Text.Json.Nodes;
using MagicCommunity;

static class PowerPanelChecks
{
    public static JsonArray Run(Dictionary<string,bool> checks)
    {
        void Check(string key,bool ok){checks[key]=ok;if(!ok)throw new InvalidOperationException(key);}
        var fixtures=new JsonArray();
        NativeSystemPanel Build(string power,bool honor=true){
            using var config=JsonDocument.Parse("{}");using var snapshot=JsonDocument.Parse("{\"system\":{\"power\":"+power+"}}");
            return NativeSystemPanel.Build("@magic/quick-settings",new(config.RootElement,snapshot.RootElement,new Dictionary<string,JsonElement>()),true,honorServices:honor);
        }
        var unsigned=Build("{\"batteryPercent\":62}",false);
        Check("unsigned-honor-controls-absent",!unsigned.Commands.ContainsKey("honor-manager")&&!unsigned.Data["rows"]!.AsArray().Any(x=>x!["id"]!.GetValue<string>().StartsWith("honor-")));
        var full=Build("""{"batteryPercent":62,"acLineStatus":0,"powerSaver":1,"mode":"performance","batteries":[{"id":"0","name":"Battery A","healthPercent":84.28,"cycleCount":201},{"id":"1","name":"Battery B","healthPercent":null,"cycleCount":null}]}""");
        JsonObject Row(NativeSystemPanel panel,string id)=>panel.Data["rows"]!.AsArray().First(x=>x!["id"]!.GetValue<string>()==id)!.AsObject();
        Check("power-mode-shows-real-state",Row(full,"power-mode")["detail"]!.GetValue<string>()=="最佳性能");
        Check("saver-shows-real-state",Row(full,"saver")["detail"]!.GetValue<string>()=="已开启");
        Check("battery-model-health-cycles-retained",Row(full,"battery:0")["label"]!.GetValue<string>()=="Battery A"&&Row(full,"battery:0")["detail"]!.GetValue<string>().Contains("84%")&&Row(full,"battery:0")["detail"]!.GetValue<string>().Contains("201"));
        Check("missing-driver-fields-stay-unavailable",Row(full,"battery:1")["detail"]!.GetValue<string>()=="电池健康度 暂不可用 · 循环次数 暂不可用");
        Check("settings-links-retained",full.Commands["power-mode"].Local=="settings-uri"&&full.Commands["honor-manager"].Local=="honor-power");
        Check("honor-state-not-inferred",Row(full,"honor-mode")["detail"]!.GetValue<string>()=="暂不可用"&&!full.Commands.ContainsKey("honor-mode"));
        foreach(var pair in new[]{("balanced","平衡"),("efficiency","更长续航"),("unknown","暂不可用")})Check("power-mode-"+pair.Item1,Row(Build("{\"mode\":\""+pair.Item1+"\"}"),"power-mode")["detail"]!.GetValue<string>()==pair.Item2);
        var missing=Build("{\"batteryPercent\":50}");Check("battery-permission-failure-visible",Row(missing,"battery:unknown")["detail"]!.GetValue<string>().Contains("暂不可用"));
        Check("missing-saver-not-off",Row(missing,"saver")["detail"]!.GetValue<string>()=="暂不可用");
        var absent=Build("null");Check("no-battery-not-fabricated",!absent.Data["rows"]!.AsArray().Any(x=>x!["id"]!.GetValue<string>().StartsWith("battery:")));
        Check("missing-power-snapshot-safe",Row(absent,"power-mode")["detail"]!.GetValue<string>()=="暂不可用");
        foreach(var pair in new[]{("power-restored",full),("power-unavailable",missing)})fixtures.Add(new JsonObject{["id"]=pair.Item1,["kind"]="@magic/quick-settings",["width"]=360,["height"]=650,["data"]=pair.Item2.Data.DeepClone()});
        return fixtures;
    }
}
