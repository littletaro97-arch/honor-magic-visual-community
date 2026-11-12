using System.Text.Json;

namespace MagicCommunity;

internal sealed partial class NativeSystemUiSession
{
    private async Task PublishMusicPanel(CancellationToken cancellation)
    {
        string? token;
        lock (currentGate) token = currentPanel?.Data["nativeMusicView"]?.GetValue<bool>() == true ? currentToken : null;
        if (token is null) return;
        var panel = music.Panel(); panel.Data["hostPid"] = Environment.ProcessId;
        lock (currentGate) { if (currentToken != token) return; currentPanel = panel; }
        await popups.Update(JsonSerializer.SerializeToElement(new { token, data = panel.Data }), cancellation);
    }
}
