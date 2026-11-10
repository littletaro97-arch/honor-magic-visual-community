using System.Diagnostics;
using System.IO;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace MagicCommunity;

// The installed entry uses the same short-lived renderer and handshake as the
// development launcher. Only a manual launch that owns the desktop starts it.
internal sealed class InstalledEntranceTransition
{
    private readonly string marker, token;
    private readonly object gate = new();
    private bool ownsMarker, completed, exited;

    private InstalledEntranceTransition(string folder, string token)
    {
        this.token = token;
        marker = Path.Combine(folder, "transition-" + token + ".signal");
    }

    internal static InstalledEntranceTransition? Start(AppController controller, bool logon)
    {
        if (logon || !InstalledDesktop.IsCurrent(controller.Root)) return null;
        var preferences = CommunityPreferences.Load(controller.Root);
        if (!preferences.WindowAnimations) return null;
        InstalledEntranceTransition? transition = null;
        Process? helper = null;
        try
        {
            string executable = CompositionRendererIdentity.Resolve();
            string media = Path.Combine(AppContext.BaseDirectory, "transitions", "entrance.mp4");
            string folder = InstalledProfileDirectories.PrepareTransitionDirectory(controller.Root);
            string token = Guid.NewGuid().ToString("N");
            transition = new(folder, token);
            using var cover = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\MagicTransition-" + token + "-opaque", out bool created);
            if (!created) throw new IOException("进场动画事件已被占用。");
            using (var file = new FileStream(transition.marker, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            {
                transition.ownsMarker = true;
                file.Write(Encoding.UTF8.GetBytes(token + ":pending"));
            }

            var start = new ProcessStartInfo(executable) { UseShellExecute = false, WorkingDirectory = AppContext.BaseDirectory };
            foreach (string value in new[] { "--desktop-transition", "entrance", transition.marker, token, media,
                Environment.ProcessId.ToString(), preferences.ReduceOnBatterySaver ? "1" : "0" }) start.ArgumentList.Add(value);
            helper = Process.Start(start) ?? throw new IOException("进场动画进程未启动。");
            using var exit = new ProcessExitHandle(helper);
            if (WaitHandle.WaitAny([cover, exit], TimeSpan.FromSeconds(8)) != 0)
                throw new IOException("进场动画未在规定时间内就绪。");
            controller.Log("installed_entrance_cover_ready", new { helperPid = helper.Id });
            return transition;
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            transition?.Complete(false);
            controller.Log("installed_entrance_unavailable", new { error = error.Message });
            return null;
        }
        finally
        {
            if (transition is not null && helper is not null) _ = transition.ObserveExit(helper, controller);
            else transition?.CleanUp();
        }
    }

    internal void Complete(bool ready)
    {
        lock (gate)
        {
            if (!ownsMarker || completed || exited) return;
            try { File.WriteAllText(marker, token + (ready ? ":ready" : ":failed"), new UTF8Encoding(false)); completed = true; }
            catch (IOException) { /* The helper also watches the owner and has a finite deadline. */ }
            catch (UnauthorizedAccessException) { }
        }
    }

    private async Task ObserveExit(Process helper, AppController controller)
    {
        using (helper)
        {
            try
            {
                await helper.WaitForExitAsync().ConfigureAwait(false);
                string trace = File.Exists(marker + ".trace") ? File.ReadAllText(marker + ".trace") : "";
                controller.Log("installed_entrance_ended", new { exitCode = helper.ExitCode, firstFrame = trace.Contains("alpha-first-frame"), covered = trace.Contains("opaque-complete") });
            }
            catch (Exception error) when (error is not OutOfMemoryException)
            { controller.Log("installed_entrance_observer_failed", new { error = error.Message }); }
            finally { CleanUp(); }
        }
    }

    private void CleanUp()
    {
        lock (gate)
        {
            exited = true;
            if (!ownsMarker) return;
            foreach (string file in new[] { marker, marker + ".trace" })
                try { File.Delete(file); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    private sealed class ProcessExitHandle : WaitHandle
    {
        internal ProcessExitHandle(Process process) => SafeWaitHandle = new SafeWaitHandle(process.Handle, ownsHandle: false);
    }
}
