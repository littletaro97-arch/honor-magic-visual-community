using System.Text.Json;
using System.Text.Json.Nodes;
using static MagicCommunity.NativeSystemPanel;
namespace MagicCommunity;

// One media selection policy and one lyric stream for the toolbar and cards.
// Retain the timeline anchor across lyric-only publications; no extra polling.
internal sealed class NativeMusicCardProjection
{
    private string timeline="";
    private long observedTick;
    private double position;
    internal void Reset(){timeline="";observedTick=0;position=0;}
    internal JsonObject Project(JsonElement provider,string lyrics,long tick,long utcMilliseconds)
    {
        var media=NativeSystemPresentation.CurrentMedia(provider);
        bool available=Flag(provider,"available"),hasSession=available&&media.ValueKind==JsonValueKind.Object;
        var data=JsonNode.Parse(hasSession?media.GetRawText():"{}")!.AsObject();
        data["available"]=available;data["hasSession"]=hasSession;
        string signature=hasSession?media.GetRawText():"";
        if(signature!=timeline){
            timeline=signature;observedTick=tick;position=Math.Max(0,Number(media,"positionMs"));
            double updated=Number(media,"positionUpdatedUnixMs"),age=utcMilliseconds-updated;
            if(Number(media,"status")==4&&updated>0&&age>0&&age<3600000)position+=age*Math.Clamp(Number(media,"playbackRate",1),.25,4);
            position=Math.Min(position,Math.Max(0,Number(media,"durationMs")));
        }
        data["positionMs"]=position;data["observedTick"]=observedTick;
        data["lyricsText"]=hasSession&&Number(media,"status")==4?lyrics:"";
        return data;
    }
}
