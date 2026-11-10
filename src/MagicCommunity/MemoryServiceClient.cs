using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace MagicCommunity;

internal readonly record struct MemoryServiceReceipt(bool Ok, int PurgeStatus, int EmptyStatus,
    bool PrivilegeEnabled, uint PrivilegeError, long AvailableBefore, long AvailableAfter)
{
    internal long AvailableDeltaBytes => AvailableAfter - AvailableBefore;
    internal bool PrivilegeNotHeld => PurgeStatus == PureNaturalNative.StatusPrivilegeNotHeld ||
                                      EmptyStatus == PureNaturalNative.StatusPrivilegeNotHeld;
    internal bool Effective => Ok && (PurgeStatus == 0 || EmptyStatus == 0);
}

// Client for the LocalSystem memory service. The desktop process keeps its own
// (unprivileged) token: the service performs the system memory-list clean and
// returns a value receipt, so the UI never needs an elevated launcher.
internal readonly record struct ServiceStopReceipt(string Name, uint Before, uint After, uint Win32Error, bool Stopped)
{
    // SERVICE_STOPPED == 4; SERVICE_RUNNING == 4 is the running value in the SCM enum.
    internal static string Describe(uint state) => state switch
    {
        1 => "STOPPED",
        2 => "START_PENDING",
        3 => "STOP_PENDING",
        4 => "RUNNING",
        5 => "CONTINUE_PENDING",
        6 => "PAUSE_PENDING",
        7 => "PAUSED",
        _ => "UNKNOWN:" + state
    };
    internal string BeforeText => Describe(Before);
    internal string AfterText => Describe(After);
}

internal readonly record struct FolderMigrateReceipt(string Folder, bool Moved, bool Junctioned,
    bool RolledBack, uint Win32Error, string Source, string Destination);

internal static class MemoryServiceClient
{
    internal const string PipeName = "MagicCommunityMemory-v1";
    internal const string ServiceName = "MagicCommunityMemoryService";

    internal static bool Installed() =>
        Process.GetProcessesByName("MagicMemoryService").Length > 0;

    // One connection per command: the server disconnects after a reply and a
    // message-mode pipe cannot be reused safely by StreamReader/Writer pairs.
    internal static string? Send(string command, int timeoutMs = 20000)
    {
        try
        {
            using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut,
                PipeOptions.None, System.Security.Principal.TokenImpersonationLevel.Identification);
            pipe.Connect(timeoutMs);
            pipe.ReadMode = PipeTransmissionMode.Message;
            byte[] payload = Encoding.UTF8.GetBytes(command);
            pipe.Write(payload, 0, payload.Length);
            pipe.Flush();
            var buffer = new byte[8192];
            int read = pipe.Read(buffer, 0, buffer.Length);
            return read <= 0 ? null : Encoding.UTF8.GetString(buffer, 0, read);
        }
        catch (Exception error) when (error is TimeoutException or IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    internal static MemoryServiceReceipt? Clean()
    {
        string? reply = Send("clean", 60000);
        if (reply is null) return null;
        try
        {
            using var document = JsonDocument.Parse(reply);
            var value = document.RootElement;
            if (!value.TryGetProperty("ok", out var ok) || ok.ValueKind != JsonValueKind.True) return null;
            return new MemoryServiceReceipt(true,
                value.GetProperty("purgeStatus").GetInt32(),
                value.GetProperty("emptyStatus").GetInt32(),
                value.GetProperty("privilegeEnabled").GetBoolean(),
                value.GetProperty("privilegeError").GetUInt32(),
                value.GetProperty("availableBefore").GetInt64(),
                value.GetProperty("availableAfter").GetInt64());
        }
        catch (JsonException) { return null; }
        catch (KeyNotFoundException) { return null; }
    }

    // P4: the service moves one allow-listed shell folder and leaves a junction.
    internal static FolderMigrateReceipt? MigrateFolder(string folder, string targetRoot, string userProfile)
    {
        string? reply = Send("migrate-folder:" + folder + "\0" + targetRoot + "\0" + userProfile, 120000);
        if (reply is null) return null;
        try
        {
            using var document = JsonDocument.Parse(reply);
            var value = document.RootElement;
            return new FolderMigrateReceipt(
                value.GetProperty("folder").GetString() ?? folder,
                value.GetProperty("moved").GetBoolean(),
                value.GetProperty("junctioned").GetBoolean(),
                value.GetProperty("rolledBack").GetBoolean(),
                value.GetProperty("win32Error").GetUInt32(),
                value.GetProperty("source").GetString() ?? "",
                value.GetProperty("destination").GetString() ?? "");
        }
        catch (JsonException) { return null; }
        catch (KeyNotFoundException) { return null; }
    }

    internal static bool Healthy() => Send("health", 8000)?.Contains("\"ok\":true", StringComparison.Ordinal) == true;

    // P3: the service owns an allow-list (SysMain, whesvc); an unknown name is
    // rejected server-side so this client cannot stop arbitrary services.
    internal static ServiceStopReceipt? StopService(string name)
    {
        string? reply = Send("stop-service:" + name, 60000);
        if (reply is null) return null;
        try
        {
            using var document = JsonDocument.Parse(reply);
            var value = document.RootElement;
            return new ServiceStopReceipt(
                value.GetProperty("service").GetString() ?? name,
                value.GetProperty("before").GetUInt32(),
                value.GetProperty("after").GetUInt32(),
                value.GetProperty("win32Error").GetUInt32(),
                value.GetProperty("stopped").GetBoolean());
        }
        catch (JsonException) { return null; }
        catch (KeyNotFoundException) { return null; }
    }
}
