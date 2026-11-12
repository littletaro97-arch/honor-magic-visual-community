using System.Text.Json;
using System.Text.Json.Nodes;
using static MagicCommunity.NativeSystemPanel;

namespace MagicCommunity;

// Consumes the existing keyboard/layout events. Focus changes establish a new
// baseline; unrelated snapshots and repeated Shift/Space releases do not toast.
internal sealed class NativeInputNotice
{
    private string target = "", layout = "", mode = "";
    private long revision;
    internal JsonObject? Current { get; private set; }
    internal void Observe(JsonElement input, bool inputEvent)
    {
        var selected = Array(input, "layouts").FirstOrDefault(row => Flag(row, "selected"));
        string nextTarget = Text(input, "foregroundWindow"), nextLayout = Text(selected, "id");
        if (!Flag(input, "available") || nextLayout.Length == 0 || nextTarget.Length == 0 || nextTarget == "0")
        { target = layout = mode = ""; return; }
        bool chinese = Text(selected, "locale").StartsWith("zh", StringComparison.OrdinalIgnoreCase);
        string nextMode = chinese && Flag(input, "imeAvailable") ? Flag(input, "imeOpen") && Flag(input, "imeNative") ? "中文输入" : "英文输入" : "";
        bool changedLayout = layout.Length > 0 && layout != nextLayout;
        bool changedMode = mode.Length > 0 && nextMode.Length > 0 && mode != nextMode;
        if (inputEvent && target == nextTarget && (changedLayout || changedMode))
        {
            string name = Text(selected, "name", Text(selected, "locale"));
            Current = new JsonObject { ["key"] = (++revision).ToString(System.Globalization.CultureInfo.InvariantCulture),
                ["title"] = "输入法", ["detail"] = nextMode.Length > 0 ? name + " · " + nextMode : name };
        }
        target = nextTarget; layout = nextLayout; mode = nextMode;
    }
}
