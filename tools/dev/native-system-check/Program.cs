using MagicCommunity;
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Security.Cryptography;
if(args.Length==2&&args[0]=="--music-checks"){await ToolbarMusicChecks.Run(args[1]);return 0;}
if(args.Length==2&&args[0]=="--lyrics-user-samples"){await ToolbarMusicChecks.Live(args[1]);return 0;}
if(args.Length==2&&args[0]=="--lyrics-checks"){await ToolbarLyricsChecks.Run(args[1]);return 0;}
if(args.Length==2&&args[0]=="--lyrics-live-check"){await ToolbarLyricsChecks.Live(args[1]);return 0;}
if(args.Length==4&&args[0]=="--installed-first-start")return await InstalledFirstStartChecks.Run(args[1],args[2],args[3]);
if(args.Length==4&&args[0]=="--preview-motion-config")return await PreviewMotionConfigChecks.Run(args[1],args[2],args[3]);

if(args.Length==2&&args[0]=="--transport-check"){await TransportChecks.Run(args[1]);return 0;}

if(args.Length==4&&args[0]=="--brightness-inventory"){
 var output=Path.GetFullPath(args[3]);Directory.CreateDirectory(Path.GetDirectoryName(output)!);
 Directory.CreateDirectory(Path.Combine(Path.GetDirectoryName(output)!,"brightness-owned-state"));
 await using var owner=await NativeSystemClient.Start(args[1],args[2],CancellationToken.None,Path.Combine(Path.GetDirectoryName(output)!,"brightness-owned-state"));
 await owner.Request("brightness-subscribe",new{enabled=true});
 var state=await owner.Request("brightness-read",new{refresh=true});
 var displays=state.GetProperty("displays").EnumerateArray().Select(d=>new{primary=d.GetProperty("primary").GetBoolean(),monitor=d.GetProperty("monitorId").GetString(),available=d.GetProperty("available").GetBoolean(),transport=d.GetProperty("transport").GetString(),inches=d.GetProperty("diagonalInches").GetDouble(),width=d.GetProperty("widthPx").GetDouble(),height=d.GetProperty("heightPx").GetDouble()}).ToArray();
 var before=await owner.Request("health",new{});await owner.Request("brightness-subscribe",new{enabled=false});var after=await owner.Request("health",new{});
 File.WriteAllText(output,JsonSerializer.Serialize(new{readOnly=true,realBrightnessWrites=false,displays,before=before.GetProperty("brightness"),after=after.GetProperty("brightness")},new JsonSerializerOptions{WriteIndented=true}));return 0;
}
if(args.Length>0&&args[0]=="--ui-contract"){var contracts=new Dictionary<string,bool>();PresentationChecks.Run(contracts);UiContractChecks.Run(contracts,args[1],args[2]);File.WriteAllText(Path.Combine(args[2],"checks.json"),JsonSerializer.Serialize(new{allPass=contracts.Values.All(x=>x),checks=contracts},new JsonSerializerOptions{WriteIndented=true}));return 0;}
if(args.Length==2 && args[0]=="--power-panel-checks"){
    var powerChecks=new Dictionary<string,bool>();var fixtures=PowerPanelChecks.Run(powerChecks);Directory.CreateDirectory(args[1]);
    File.WriteAllText(Path.Combine(args[1],"power-panel-checks.json"),JsonSerializer.Serialize(new{allPass=powerChecks.Values.All(x=>x),checks=powerChecks},new JsonSerializerOptions{WriteIndented=true}));
    File.WriteAllText(Path.Combine(args[1],"fixtures.json"),fixtures.ToJsonString());return 0;
}
if (args.Length > 0 && args[0] == "--guard-owned-wait") { await Task.Delay(Timeout.Infinite); return 0; }
if(args.Length==2&&args[0]=="--taskbar-journal-read-only"){
    var clock=Stopwatch.StartNew();bool preserve=NativeTaskbarRecovery.UserChanged(args[1]);
    Console.WriteLine(JsonSerializer.Serialize(new{bytes=new FileInfo(args[1]).Length,preserveCurrentPreferences=preserve,elapsedMs=clock.Elapsed.TotalMilliseconds,scope="read_only_no_taskbar_or_lease_changes"}));return 0;
}
if(args.Length>0&&args[0]=="--tray-inventory"){
    await using var owner=await NativeSystemClient.Start(args[1],args[2],CancellationToken.None);using var process=Process.GetProcessById(owner.ProcessId);process.Refresh();long before=process.PrivateMemorySize64;
    await owner.Request("tray-subscribe",new{enabled=true,production=true,initializeExisting=true});await Task.Delay(1800);var inventory=await owner.Request("tray-read",new{});process.Refresh();long active=process.PrivateMemorySize64;int handles=process.HandleCount;
    await owner.Request("tray-subscribe",new{enabled=false});await Task.Delay(350);var released=await owner.Request("health",new{});process.Refresh();
    File.WriteAllText(args[3],JsonSerializer.Serialize(new{allPass=inventory.GetProperty("icons").GetArrayLength()>0&&released.GetProperty("tray").GetProperty("nativeHooks").GetInt32()==0,icons=inventory.GetProperty("icons").GetArrayLength(),images=inventory.GetProperty("icons").EnumerateArray().Count(x=>x.GetProperty("png").GetString()!.Length>0),promoted=inventory.GetProperty("icons").EnumerateArray().Count(x=>x.GetProperty("promoted").GetBoolean()),inventory,beforePrivateBytes=before,activePrivateBytes=active,releasedPrivateBytes=process.PrivateMemorySize64,activeHandles=handles,releasedHandles=process.HandleCount,releasedTray=released.GetProperty("tray"),scope="Real Explorer inventory reconstructed with one TaskbarCreated broadcast; no third-party actions, no taskbar hiding, no desktop switch"},new JsonSerializerOptions{WriteIndented=true}));return 0;
}
if (args.Length > 0 && args[0] == "--live-tray")
{
    var liveChecks=new Dictionary<string,bool>();
    await using var liveBackend=await NativeSystemClient.Start(args[1],args[2],CancellationToken.None);
    await TrayChecks.Run(liveBackend,liveChecks,true);
    File.WriteAllText(args[3],JsonSerializer.Serialize(new{allPass=liveChecks.Values.All(x=>x),checks=liveChecks,scope="Real Explorer capture of our own temporary Shell_NotifyIcon; no initial broadcast, no desktop switch"},new JsonSerializerOptions{WriteIndented=true}));return 0;
}
if (args.Length > 0 && args[0] == "--provider-cycles") return await ProviderCycleChecks.Run(args[1], args[2], args[3]);
if (args.Length > 0 && args[0] == "--profile") return await ResourceChecks.Run(args[1], args[2], args[3]);
if (args.Length > 0 && args[0] == "--rogue")
{
    try { using var pipe = new NamedPipeClientStream(".", args[1], PipeDirection.InOut, PipeOptions.Asynchronous); await pipe.ConnectAsync(1500); byte[] frame = Encoding.UTF8.GetBytes("{}"); await pipe.WriteAsync(BitConverter.GetBytes(frame.Length)); await pipe.WriteAsync(frame); using var timeout = new CancellationTokenSource(1500); byte[] response = new byte[4]; int n = await pipe.ReadAsync(response, timeout.Token); return n == 0 ? 0 : 1; } catch (IOException) { return 0; }
}
if (args.Length > 0 && args[0] == "--project-config")
{
    string file = Path.Combine(args[3], "configuration.json"); byte[] before = File.ReadAllBytes(file);
    var expected=JsonNode.Parse(before)!.AsObject(); NativeSystemBarDefaults.NormalizeToolbarMusic(expected);
    using var config = JsonDocument.Parse(expected.ToJsonString()); var ids = new HashSet<string>();
    foreach (string surface in new[] { "toolbar", "dock" }) foreach (string section in new[] { "left", "center", "right" }) foreach (var item in config.RootElement.GetProperty(surface).GetProperty("sections").GetProperty(section).EnumerateArray()) ids.Add(item.GetProperty("id").GetString()!);
    await using var backend = await NativeSystemClient.Start(args[1], args[2], CancellationToken.None, args[3]);
    var clock = Stopwatch.StartNew(); var presentation = new NativeSystemPresentationSession(backend); JsonObject view;
    try { view = await presentation.Initialize(); } finally { await presentation.DisposeAsync(); }
    clock.Stop(); var projected = new HashSet<string>(); int windows = 0;
    foreach (string surface in new[] { "toolbar", "dock" }) foreach (string section in new[] { "left", "center", "right" }) foreach (var item in view[surface]![section]!.AsArray()) { if (item!["id"] is JsonValue id) projected.Add(id.GetValue<string>()); if (item["windows"] is JsonArray rows) windows += rows.Count; }
    var health = await backend.Request("health", new { }); bool released = !health.GetProperty("windowsSubscribed").GetBoolean() && !health.GetProperty("systemSubscribed").GetBoolean() && health.GetProperty("bluetooth").GetProperty("watchers").GetInt32() == 0 && health.GetProperty("audio").GetProperty("deviceCallbacks").GetInt32() == 0 && !health.GetProperty("notifications").GetProperty("fallbackPolling").GetBoolean();
    var projectionResult = new { allPass = ids.IsSubsetOf(projected) && before.SequenceEqual(File.ReadAllBytes(file)) && released, configuredItems = ids.Count, configuredItemsRetained = ids.Count(projected.Contains), windowAssociations = windows, payloadBytes = Encoding.UTF8.GetByteCount(view.ToJsonString()), initializationAndDisposeMs = clock.Elapsed.TotalMilliseconds, unavailableItems = view["warnings"]!.AsArray().Count, providerFailures = view["providerFailures"]!.AsObject().Count, subscriptionsReleased = released, sourceUnchanged = before.SequenceEqual(File.ReadAllBytes(file)), desktopActivated = false, scope = "Isolated imported profile through native provider session into renderer contract; no renderer windows or commands attached" };
    File.WriteAllText(args[4], JsonSerializer.Serialize(projectionResult, new JsonSerializerOptions { WriteIndented = true })); Console.WriteLine(JsonSerializer.Serialize(projectionResult)); return projectionResult.allPass ? 0 : 1;
}
if (args.Length > 0 && args[0] == "--read-config")
{
    string file = Path.Combine(args[3], "configuration.json"); byte[] before = File.ReadAllBytes(file);
    await using var backend = await NativeSystemClient.Start(args[1], args[2], CancellationToken.None, args[3]); var value = await backend.Request("configuration-read", new { });
    using var expected = JsonDocument.Parse(before); bool equal = JsonElement.DeepEquals(expected.RootElement, value.GetProperty("configuration")); int items = 0;
    foreach (var surface in new[] { "toolbar", "dock" }) foreach (var section in new[] { "left", "center", "right" }) items += value.GetProperty("configuration").GetProperty(surface).GetProperty("sections").GetProperty(section).GetArrayLength();
    var profileResult = new { allPass = equal && before.SequenceEqual(File.ReadAllBytes(file)), items, sourceUnchanged = before.SequenceEqual(File.ReadAllBytes(file)), scope = "Native runtime reads an isolated imported profile; no UI activation or configuration write" }; File.WriteAllText(args[4], JsonSerializer.Serialize(profileResult, new JsonSerializerOptions { WriteIndented = true })); Console.WriteLine(JsonSerializer.Serialize(profileResult)); return profileResult.allPass ? 0 : 1;
}
if (args.Length > 0 && args[0] == "--orphan-parent")
{
    var owned = await NativeSystemClient.Start(args[1], args[2], CancellationToken.None); File.WriteAllText(args[3], owned.ProcessId.ToString()); await Task.Delay(Timeout.Infinite); return 1;
}
string exe = Path.GetFullPath(args[0]), hash = args[1], report = args[2];
var checks = new Dictionary<string, bool>(); var metrics = new Dictionary<string, object>();
void Require(string name, bool value) { checks[name] = value; if (!value) throw new InvalidOperationException(name); }
ProcessStartInfo Harness(params string[] arguments) { var info = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true }; info.ArgumentList.Add(typeof(Program).Assembly.Location); foreach (var value in arguments) info.ArgumentList.Add(value); return info; }
try
{
    AcceptanceChecks.Run(checks);
    PresentationChecks.Run(checks);
    await TaskbarChecks.Run(Path.Combine(Path.GetDirectoryName(exe)!, "MagicTaskbarGuard.exe"), Path.GetDirectoryName(report)!, checks);
    bool wrongHash = false; try { await NativeSystemClient.Start(exe, new string('0', 64), CancellationToken.None); } catch (InvalidDataException) { wrongHash = true; }
    Require("wrong-binary-hash-rejected-before-start", wrongHash);
    await using (var backend = await NativeSystemClient.Start(exe, hash, CancellationToken.None))
    {
        await TrayChecks.Run(backend, checks);
        var health = await backend.Request("health", new { }); Require("independent-protocol-handshake", health.GetProperty("implementation").GetString() == "win32-native");
        Require("no-background-demand-on-start", !health.GetProperty("windowsSubscribed").GetBoolean() && !health.GetProperty("systemSubscribed").GetBoolean());
        bool unknown = false; try { await backend.Request("does-not-exist", new { }); } catch (NativeSystemRequestException) { unknown = true; }
        Require("unknown-operation-rejected", unknown);
        foreach(var (win,winA) in new[]{(false,false),(true,false),(false,true),(true,true),(false,false)}){
            var keys=await backend.Request("shortcuts-config",new{win,winA,production=true});
            Require($"shortcuts-{win}-{winA}-remain-unhooked-without-subscription",keys.GetProperty("shortcutModes").GetInt32()==((win?1:0)|(winA?2:0))&&!keys.GetProperty("shortcutHookActive").GetBoolean());
        }
        bool deniedShortcuts=false;
        try{await backend.Request("shortcuts-config",new{win=false,winA=true});}catch(NativeSystemRequestException){deniedShortcuts=true;}
        Require("shortcuts-mode-requires-explicit-production-choice",deniedShortcuts);
        var passthrough=await backend.Request("shortcuts-subscribe",new{enabled=true,production=true});
        Require("shortcuts-all-off-never-install-hook",!passthrough.GetProperty("shortcutHookActive").GetBoolean());
        await backend.Request("shortcuts-subscribe",new{enabled=false});
        bool interval = false; try { await backend.Request("subscribe", new { windows = false, system = true, intervalMs = 10 }); } catch (NativeSystemRequestException) { interval = true; }
        Require("subsecond-sampling-rejected", interval);
        bool forged = false; try { await backend.Request("window-action", new { windowId = "1:2:3", action = "close" }); } catch (NativeSystemRequestException) { forged = true; }
        Require("unoffered-window-action-rejected", forged);
        var facts = await backend.Request("snapshot", new { includeSystem = true }); Require("real-memory-facts", facts.GetProperty("system").GetProperty("memoryTotalBytes").GetDouble() > 0); Require("real-display-facts", facts.GetProperty("displays").GetArrayLength() > 0);
        Require("unimplemented-capabilities-explicit", facts.GetProperty("unavailable").EnumerateArray().Any(x => x.GetString() == "third-party-plugins"));
        bool invalidVolume = false; try { await backend.Request("audio-action", new { action = "set-volume", value = 2.0, deviceId = "not-a-device" }); } catch (NativeSystemRequestException) { invalidVolume = true; }
        Require("invalid-volume-rejected-without-mutation", invalidVolume);
        var microphone=await backend.Request("microphone-read",new{});Require("microphone-volume-read-does-not-record-audio",microphone.TryGetProperty("volume",out _)||!microphone.GetProperty("available").GetBoolean());
        var mixer=await backend.Request("mixer-read",new{});Require("mixer-session-list-bounded",mixer.GetProperty("sessions").GetArrayLength()<=64);
        await backend.Request("mixer-subscribe",new{enabled=true});await backend.Request("mixer-read",new{});await backend.Request("mixer-read",new{});await backend.Request("mixer-subscribe",new{enabled=false});
        var mixerReleased=(await backend.Request("health",new{})).GetProperty("mixer");Require("mixer-close-releases-session-and-manager-callbacks",mixerReleased.GetProperty("sessionCallbacks").GetInt32()==0&&mixerReleased.GetProperty("managerCallbacks").GetInt32()==0&&mixerReleased.GetProperty("cachedSessions").GetInt32()==0);
        bool forgedSession=false;try{await backend.Request("mixer-action",new{sessionId="forged",deviceId="forged",action="set-volume",value=0.1});}catch(NativeSystemRequestException){forgedSession=true;}Require("unoffered-mixer-session-rejected",forgedSession);
        var brightness=await backend.Request("brightness-subscribe",new{enabled=true});
        Require("brightness-availability-explicit",brightness.TryGetProperty("available",out _));
        metrics["brightnessDisplays"]=brightness.GetProperty("displays").GetArrayLength();
        metrics["brightnessEvents"]=brightness.GetProperty("events").GetBoolean();
        bool invalidBrightness=false;try{await backend.Request("brightness-action",new{displayId="fixture-missing",value=2.0});}catch(NativeSystemRequestException){invalidBrightness=true;}Require("brightness-rejects-out-of-range-without-changing-level",invalidBrightness);
        bool missingBrightness=false;try{await backend.Request("brightness-action",new{displayId="fixture-missing",value=.5});}catch(NativeSystemRequestException){missingBrightness=true;}Require("brightness-rejects-unoffered-display",missingBrightness);
        await backend.Request("brightness-subscribe",new{enabled=false});
        var brightnessHealth=(await backend.Request("health",new{})).GetProperty("brightness");
        Require("brightness-release-has-no-subscription-or-polling",!brightnessHealth.GetProperty("subscribed").GetBoolean()&&!brightnessHealth.GetProperty("events").GetBoolean()&&!brightnessHealth.GetProperty("polling").GetBoolean());
        var audio = await backend.Request("audio-read", new { }); metrics["audioDeviceCount"] = audio.GetProperty("devices").GetArrayLength(); Require("audio-read-availability-explicit", audio.TryGetProperty("available", out _));
        var audioHealth = await backend.Request("audio-health", new { }); Require("one-shot-audio-read-releases-callbacks", audioHealth.GetProperty("deviceCallbacks").GetInt32() == 0 && audioHealth.GetProperty("volumeCallbacks").GetInt32() == 0);
        var audioSubscription = await backend.Request("audio-subscribe", new { enabled = true }); Require("audio-device-events-registered", audioSubscription.GetProperty("deviceCallbacks").GetInt32() == 1);
        await backend.Request("unsubscribe", new { }); var releasedAudio = await backend.Request("audio-health", new { }); Require("audio-unsubscribe-releases-events", releasedAudio.GetProperty("deviceCallbacks").GetInt32() == 0 && releasedAudio.GetProperty("volumeCallbacks").GetInt32() == 0);
        var layouts = await backend.Request("input-read", new { }); Require("keyboard-layouts-read", layouts.GetProperty("layouts").GetArrayLength() > 0);
        var inputForeground=Native.GetForegroundWindow();var inputClock=Stopwatch.StartNew();
        var inputTarget=await backend.Request("input-target",new{popupProcessId=0});
        metrics["inputTargetCaptureMs"]=inputClock.Elapsed.TotalMilliseconds;
        Require("input-capture-does-not-change-foreground",Native.GetForegroundWindow()==inputForeground);
        Require("input-capture-reports-availability",inputTarget.TryGetProperty("available",out _));
        string offeredLayout=layouts.GetProperty("layouts")[0].GetProperty("id").GetString()!;
        bool forgedInput=false;try{await backend.Request("input-select",new{layoutId=offeredLayout,windowId="input:forged"});}catch(NativeSystemRequestException e){forgedInput=e.Operation=="input-select"&&e.Message.Contains("Input target expired",StringComparison.Ordinal);}
        Require("input-rejects-forged-target-before-any-activation-or-layout-change",forgedInput);
        if(inputTarget.GetProperty("available").GetBoolean()){
            string targetId=inputTarget.GetProperty("id").GetString()!;
            var targetState=await backend.Request("input-read",new{windowId=targetId});
            Require("captured-input-target-has-layout-state",targetState.GetProperty("layouts").GetArrayLength()>0);
            bool inputNotTaskbar=false;try{await backend.Request("window-action",new{windowId=targetId,action="close"});}catch(NativeSystemRequestException){inputNotTaskbar=true;}
            Require("input-capability-cannot-be-used-to-close-a-window",inputNotTaskbar);
            var recaptured=await backend.Request("input-target",new{popupProcessId=0});
            Require("recapture-invalidates-previous-input-capability",recaptured.GetProperty("available").GetBoolean()&&recaptured.GetProperty("id").GetString()!=targetId);
            bool staleInput=false;try{await backend.Request("input-read",new{windowId=targetId});}catch(NativeSystemRequestException){staleInput=true;}
            Require("replaced-input-target-rejected",staleInput);
        }
        bool namedInputError=false;try{await backend.Request("input-select",new{layoutId="",windowId="input:forged"});}catch(NativeSystemRequestException e){namedInputError=e.Operation=="input-select"&&e.Message=="Invalid string field: layoutId";}
        Require("invalid-input-argument-reports-operation-and-field-without-values",namedInputError);
        var profiles = await backend.Request("input-profiles", new { }); metrics["inputProfileCount"] = profiles.GetProperty("profiles").GetArrayLength(); Require("tsf-profile-capability-explicit", profiles.TryGetProperty("available", out _) && !profiles.GetProperty("foregroundProfileSelectionKnown").GetBoolean());
        if (profiles.GetProperty("available").GetBoolean()) { var profileIds = profiles.GetProperty("profiles").EnumerateArray().Select(p => p.GetProperty("id").GetString()).ToArray(); Require("tsf-profile-identities-unique", profileIds.Distinct().Count() == profileIds.Length); }
        bool profileScope = false; try { await backend.Request("input-profile-select", new { scope = "foreground", profileId = "not-a-profile", windowId = "not-a-window" }); } catch (NativeSystemRequestException) { profileScope = true; }
        Require("session-input-switch-needs-explicit-desktop-scope", profileScope);
        bool invalidLayout = false; try { await backend.Request("input-select", new { layoutId = "not-a-layout", windowId = "not-a-window" }); } catch (NativeSystemRequestException) { invalidLayout = true; }
        Require("unoffered-keyboard-layout-rejected", invalidLayout);
        bool invalidMedia = false; try { await backend.Request("media-action", new { sessionId = "not-a-session", action = "toggle" }); } catch (NativeSystemRequestException) { invalidMedia = true; }
        Require("unoffered-media-session-rejected", invalidMedia);
        try { var media = await backend.Request("media-read", new { }); metrics["mediaSessionCount"] = media.GetProperty("sessions").GetArrayLength(); Require("media-query-reports-real-state", media.GetProperty("available").GetBoolean()); await backend.Request("media-subscribe", new { enabled = true }); Require("media-events-register", true); }
        catch (NativeSystemRequestException error) when (error.ErrorCode.HasValue) { metrics["mediaAccessError"] = error.ErrorCode.Value; Require("media-query-capability-failure-explicit", true); }
        await backend.Request("unsubscribe", new { }); var releasedMedia = await backend.Request("media-health", new { }); Require("media-unsubscribe-releases-events-and-sessions", releasedMedia.GetProperty("managerCallbacks").GetInt32() == 0 && releasedMedia.GetProperty("sessionCallbacks").GetInt32() == 0 && releasedMedia.GetProperty("cachedSessions").GetInt32() == 0);
        bool invalidWifi = false; try { await backend.Request("wifi-action", new { interfaceId = "not-an-interface", action = "disconnect" }); } catch (NativeSystemRequestException) { invalidWifi = true; }
        Require("unoffered-wifi-interface-rejected", invalidWifi);
        try { var wifi = await backend.Request("wifi-read", new { networkDetails = false }); metrics["wifiInterfaceCount"] = wifi.GetProperty("interfaces").GetArrayLength(); Require("wifi-default-query-does-not-request-location-data", !wifi.GetProperty("networkDetailsRequested").GetBoolean()); await backend.Request("wifi-subscribe", new { enabled = true }); Require("wifi-event-registration", true); }
        catch (NativeSystemRequestException error) when (error.ErrorCode.HasValue) { metrics["wifiAccessError"] = error.ErrorCode.Value; Require("wifi-unavailability-explicit", true); }
        await backend.Request("unsubscribe", new { }); var releasedWifi = await backend.Request("wifi-health", new { }); Require("wifi-subscription-and-cache-released", releasedWifi.GetProperty("callbacks").GetInt32() == 0 && releasedWifi.GetProperty("cachedInterfaces").GetInt32() == 0);
        bool invalidBluetooth = false; try { await backend.Request("bluetooth-unpair", new { deviceId = "not-a-device" }); } catch (NativeSystemRequestException) { invalidBluetooth = true; }
        Require("unoffered-bluetooth-device-rejected", invalidBluetooth);
        bool invalidDisconnect=false;try{await backend.Request("bluetooth-action",new{deviceId="not-a-device",action="disconnect"});}catch(NativeSystemRequestException){invalidDisconnect=true;}
        Require("unoffered-bluetooth-disconnect-rejected-before-device-io",invalidDisconnect);
        try { var bluetooth = await backend.Request("bluetooth-read", new { }); metrics["pairedBluetoothDeviceCount"] = bluetooth.GetProperty("devices").GetArrayLength(); Require("bluetooth-read-does-not-start-discovery", !bluetooth.GetProperty("discoveryStarted").GetBoolean()); await backend.Request("bluetooth-subscribe", new { enabled = true }); Require("paired-device-events-register", true); }
        catch (NativeSystemRequestException error) when (error.ErrorCode.HasValue) { metrics["bluetoothAccessError"] = error.ErrorCode.Value; Require("bluetooth-unavailability-explicit", true); }
        await backend.Request("unsubscribe", new { }); var releasedBluetooth = await backend.Request("bluetooth-health", new { }); Require("bluetooth-watchers-and-cache-released", releasedBluetooth.GetProperty("watchers").GetInt32() == 0 && releasedBluetooth.GetProperty("callbacks").GetInt32() == 0 && releasedBluetooth.GetProperty("cachedDevices").GetInt32() == 0);
        var notificationStatus = await backend.Request("notifications-status", new { }); metrics["notificationAccess"] = notificationStatus.GetProperty("access").GetString()!; metrics["notificationPackageIdentity"] = notificationStatus.GetProperty("hasPackageIdentity").GetBoolean(); Require("notification-permission-is-real-and-not-requested", !notificationStatus.GetProperty("authorizationRequested").GetBoolean());
        bool invalidNotification = false; try { await backend.Request("notifications-remove", new { notificationId = "not-a-notification" }); } catch (NativeSystemRequestException) { invalidNotification = true; }
        Require("unoffered-notification-rejected", invalidNotification);
        bool unconfirmedClear=false;try{await backend.Request("notifications-clear",new{confirmed=false});}catch(NativeSystemRequestException){unconfirmedClear=true;}
        Require("notification-clear-requires-explicit-click",unconfirmedClear);
        if(!notificationStatus.GetProperty("available").GetBoolean()){
            bool deniedClear=false;try{await backend.Request("notifications-clear",new{confirmed=true});}catch(NativeSystemRequestException){deniedClear=true;}
            Require("notification-clear-rechecks-consent-and-windows-access",deniedClear);
        }
        var notificationRead = await backend.Request("notifications-read", new { }); Require("notification-read-retains-explicit-access-state", notificationRead.TryGetProperty("access", out _)); metrics["notificationCount"] = notificationRead.GetProperty("notifications").GetArrayLength();
        if (notificationStatus.GetProperty("available").GetBoolean())
        {
            try
            {
                await backend.Request("notifications-subscribe", new { enabled = true }); Require("allowed-notification-subscription-registers-or-reports-fallback", true);
                var notificationMode = await backend.Request("notifications-health", new { });
                if (notificationMode.GetProperty("fallbackPolling").GetBoolean())
                {
                    await backend.Request("notifications-subscribe", new { enabled = true, intervalMs = 2000 });
                    var cursor = await backend.Request("health", new { });
                    await backend.Request("next", new { afterRevision = cursor.GetProperty("revision").GetInt64(), timeoutMs = 3000 });
                    var polled = await backend.Request("notifications-health", new { }); Require("notification-fallback-polls-only-on-demand", polled.GetProperty("polls").GetInt64() > 0); metrics["notificationMode"] = "bounded-polling";
                }
                else metrics["notificationMode"] = "event";
            }
            catch (NativeSystemRequestException error) when (error.ErrorCode.HasValue) { metrics["notificationEventError"] = error.ErrorCode.Value; Require("unsupported-notification-event-is-explicit", true); }
        }
        await backend.Request("unsubscribe", new { }); var releasedNotifications = await backend.Request("notifications-health", new { }); Require("notification-events-and-cache-released", releasedNotifications.GetProperty("callbacks").GetInt32() == 0 && releasedNotifications.GetProperty("cachedNotifications").GetInt32() == 0 && !releasedNotifications.GetProperty("fallbackPolling").GetBoolean());
        await backend.Request("notifications-subscribe",new{enabled=true});var notificationCursor=await backend.Request("health",new{});for(int repeat=0;repeat<20;repeat++)await backend.Request("notifications-subscribe",new{enabled=true});
        var notificationIdle=await backend.Request("next",new{afterRevision=notificationCursor.GetProperty("revision").GetInt64(),timeoutMs=80});Require("idempotent-notification-subscription-does-not-self-wake",!notificationIdle.GetProperty("changed").GetBoolean());await backend.Request("notifications-subscribe",new{enabled=false});
        var noConfig = await backend.Request("configuration-read", new { }); Require("configuration-is-opt-in", !noConfig.GetProperty("enabled").GetBoolean());
        var catalog = await backend.Request("configuration-catalog", new { }); Require("catalog-uses-community-module-ids", catalog.GetProperty("modules").EnumerateArray().All(x => x.GetProperty("id").GetString()!.StartsWith("magic.")));
        bool noNamespace = false; try { await backend.Request("configuration-write", new { expectedRevision = 0, configuration = new { } }); } catch (NativeSystemRequestException) { noNamespace = true; }
        Require("configuration-write-without-namespace-rejected", noNamespace);
        var before = await backend.Request("health", new { }); await Task.Delay(1200); var after = await backend.Request("health", new { }); Require("idle-does-not-sample", before.GetProperty("systemSamples").GetInt64() == after.GetProperty("systemSamples").GetInt64());
        var subscribed = await backend.Request("subscribe", new { windows = true, system = false, intervalMs = 1000 }); Require("window-subscription-active", subscribed.GetProperty("windowsSubscribed").GetBoolean());
        using (var fixture = new Fixture())
        {
            async Task<JsonElement> Until(Func<JsonElement, bool> condition) { long rev = 0; var watch = Stopwatch.StartNew(); while (watch.ElapsedMilliseconds < 4000) { var next = await backend.Request("next", new { afterRevision = rev, timeoutMs = 500 }); rev = next.GetProperty("revision").GetInt64(); if (next.TryGetProperty("snapshot", out var snapshot) && condition(snapshot)) return snapshot; } throw new TimeoutException("Owned fixture update was not received"); }
            bool Found(JsonElement state, string title) => state.GetProperty("windows").EnumerateArray().Any(w => w.GetProperty("title").GetString() == title);
            var initial = await Until(s => Found(s, fixture.Title)); var row = initial.GetProperty("windows").EnumerateArray().Single(w => w.GetProperty("title").GetString() == fixture.Title); string id = row.GetProperty("id").GetString()!;
            Require("owned-window-created-event", true);
            fixture.Rename(); await Until(s => Found(s, fixture.Title)); Require("owned-window-title-event", true);
            var inspection=await backend.Request("window-action",new{windowId=id,action="inspect"});
            Require("owned-window-inspect-verified",inspection.GetProperty("accepted").GetBoolean()&&inspection.GetProperty("pid").GetString()==Environment.ProcessId.ToString()&&inspection.GetProperty("created").GetString()!.Length>0&&inspection.GetProperty("path").GetString()!.Length>0);
            fixture.Cloak(true);await Until(s=>!Found(s,fixture.Title));Require("owned-cloaked-window-leaves-window-list",true);
            Require("owned-cloak-keeps-hwnd-visible-but-hidden-by-dwm",fixture.IsCloaked());
            fixture.Cloak(false);var uncloaked=await Until(s=>Found(s,fixture.Title));Require("owned-uncloaked-window-returns-to-window-list",true);
            Require("owned-uncloak-clears-dwm-cloak-state",!fixture.IsCloaked());
            id=uncloaked.GetProperty("windows").EnumerateArray().Single(w=>w.GetProperty("title").GetString()==fixture.Title).GetProperty("id").GetString()!;
            fixture.Taskbar(false);await Until(s=>!Found(s,fixture.Title));Require("noactivate-window-without-taskbar-opt-in-is-excluded",true);
            fixture.Taskbar(true);var restored=await Until(s=>Found(s,fixture.Title));id=restored.GetProperty("windows").EnumerateArray().Single(w=>w.GetProperty("title").GetString()==fixture.Title).GetProperty("id").GetString()!;
            fixture.RenameTo("Windows 默认锁屏界面 - owned ordinary fixture");await Until(s=>Found(s,fixture.Title));Require("ordinary-title-is-not-a-security-window-filter",true);
            IntPtr relay=IntPtr.Zero;
            while((relay=Native.FindWindowEx(IntPtr.Zero,relay,"MagicNativeSystemEvents",null))!=IntPtr.Zero){Native.GetWindowThreadProcessId(relay,out uint pid);if(pid==backend.ProcessId)break;}
            Require("session-fixture-relay-is-owned-backend",relay!=IntPtr.Zero);
            var beforeSessionTarget=await backend.Request("input-target",new{popupProcessId=0});
            Native.PostMessage(relay,0x02B1,new IntPtr(7),IntPtr.Zero);await Until(s=>!Found(s,fixture.Title));Require("synthetic-session-lock-clears-only-owned-backend-cache",true);
            if(beforeSessionTarget.GetProperty("available").GetBoolean()){
                bool clearedInput=false;try{await backend.Request("input-read",new{windowId=beforeSessionTarget.GetProperty("id").GetString()});}catch(NativeSystemRequestException){clearedInput=true;}
                Require("synthetic-session-lock-invalidates-input-capability",clearedInput);
            }
            Native.PostMessage(relay,0x02B1,new IntPtr(8),IntPtr.Zero);var resumed=await Until(s=>Found(s,fixture.Title));
            id=resumed.GetProperty("windows").EnumerateArray().Single(w=>w.GetProperty("title").GetString()==fixture.Title).GetProperty("id").GetString()!;Require("synthetic-session-unlock-reenumerates-existing-windows",true);
            var close = await backend.Request("window-action", new { windowId = id, action = "close" }); Require("owned-window-close-request", close.GetProperty("accepted").GetBoolean());
            await Until(s => !s.GetProperty("windows").EnumerateArray().Any(w => w.GetProperty("id").GetString() == id)); Require("owned-window-destroy-event", true);
            bool expired = false; try { await backend.Request("window-action", new { windowId = id, action = "close" }); } catch (NativeSystemRequestException) { expired = true; }
            Require("stale-window-identity-rejected", expired);
        }
        using (var producer = new Fixture())
        {
            long eventCursor = (await backend.Request("health", new { })).GetProperty("revision").GetInt64(); bool contract = true; int unchangedResponses = 0;
            for (int i = 0; i < 96; i++)
            {
                producer.RenameTo("Magic-owned-cursor-stress-" + i);
                var response = await backend.Request("next", new { afterRevision = eventCursor, timeoutMs = 1 }); long received = response.GetProperty("revision").GetInt64();
                if (!response.GetProperty("changed").GetBoolean()) { ++unchangedResponses; contract &= received <= eventCursor; }
                eventCursor = received;
            }
            metrics["unchangedResponsesDuringEventStress"] = unchangedResponses;
            Require("unchanged-event-response-never-acknowledges-new-revision", contract && unchangedResponses > 0);
        }
        var stopped = await backend.Request("unsubscribe", new { }); long revision = stopped.GetProperty("revision").GetInt64(), samples = stopped.GetProperty("systemSamples").GetInt64(), enumerations = stopped.GetProperty("windowEnumerations").GetInt64();
        var idle = await backend.Request("next", new { afterRevision = revision, timeoutMs = 300 }); Require("unsubscribed-next-does-not-poll", !idle.GetProperty("changed").GetBoolean());
        var quiet = await backend.Request("health", new { }); Require("unsubscribed-listeners-and-samples-stop", !quiet.GetProperty("windowsSubscribed").GetBoolean() && quiet.GetProperty("systemSamples").GetInt64() == samples && quiet.GetProperty("windowEnumerations").GetInt64() == enumerations);
        var system = await backend.Request("subscribe", new { windows = false, system = true, intervalMs = 1000 }); var demand = await backend.Request("next", new { afterRevision = system.GetProperty("revision").GetInt64(), timeoutMs = 1500 }); Require("system-demand-produces-sample", demand.GetProperty("snapshot").TryGetProperty("system", out _));
        await backend.Request("unsubscribe", new { });
        var quietCursor = await backend.Request("health", new { });
        var waitingEvent = backend.Request("next", new { afterRevision = quietCursor.GetProperty("revision").GetInt64(), timeoutMs = 1000 });
        await Task.Delay(30); var commandWatch = Stopwatch.StartNew(); await backend.Request("health", new { }); commandWatch.Stop(); await waitingEvent;
        metrics["commandWhileEventWaitMs"] = commandWatch.Elapsed.TotalMilliseconds; Require("event-wait-does-not-block-control-requests", commandWatch.ElapsedMilliseconds < 250);
        using (var ownership = Process.GetProcessById(backend.ProcessId))
        {
            ownership.Refresh(); int handles = ownership.HandleCount;
            for (int cycle = 0; cycle < 24; cycle++) { await backend.Request("subscribe", new { windows = true, system = true, intervalMs = 1000 }); await backend.Request("unsubscribe", new { }); }
            var released = await backend.Request("health", new { });
            Require("all-native-listeners-released", released.GetProperty("nativeHooks").GetInt32() == 0 && released.GetProperty("powerRegistrations").GetInt32() == 0 && !released.GetProperty("networkNotificationActive").GetBoolean());
            ownership.Refresh(); metrics["handleDeltaAfter24Cycles"] = ownership.HandleCount - handles; Require("subscription-handle-count-bounded", ownership.HandleCount - handles <= 4);
        }
        using (var nativeProcess = Process.GetProcessById(backend.ProcessId))
        {
            var modules = nativeProcess.Modules.Cast<ProcessModule>().Select(m => new { name = m.ModuleName, path = m.FileName }).ToArray();
            File.WriteAllText(Path.Combine(Path.GetDirectoryName(report)!, "native-loaded-modules.json"), JsonSerializer.Serialize(modules, new JsonSerializerOptions { WriteIndented = true }));
            metrics["loadedModuleCount"] = modules.Length;
            Require("no-legacy-runtime-modules-loaded-after-provider-queries", !modules.Any(m => System.Text.RegularExpressions.Regex.IsMatch(m.name, "(?i)seelen|sluhk|tauri|webview2")));
        }
        using var process = Process.GetProcessById(backend.ProcessId); process.Refresh(); var cpu = process.TotalProcessorTime; var watch = Stopwatch.StartNew(); await Task.Delay(3000); process.Refresh(); metrics["idleSeconds"] = watch.Elapsed.TotalSeconds; metrics["idleCpuCorePercent"] = (process.TotalProcessorTime - cpu).TotalMilliseconds / watch.Elapsed.TotalMilliseconds * 100; metrics["workingSetBytes"] = process.WorkingSet64; metrics["privateBytes"] = process.PrivateMemorySize64; metrics["threads"] = process.Threads.Count; metrics["binaryBytes"] = new FileInfo(exe).Length;
        int backendPid = backend.ProcessId; metrics["lastOwnedPid"] = backendPid;
    }
    // An unrelated process with the same user SID must not impersonate the
    // authenticated parent. This is a separate candidate, before parent connect.
    using (var owner = Process.GetCurrentProcess())
    {
        string pipeName = "MagicNativeSystem-" + Guid.NewGuid().ToString("N"); var image = new StringBuilder(32768); uint size = (uint)image.Capacity;
        if (!Native.QueryFullProcessImageName(owner.Handle, 0, image, ref size)) throw new InvalidOperationException("Parent path unavailable");
        var launch = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true }; foreach (var argument in new[] { "--pipe", pipeName, "--parent", owner.Id.ToString(), "--parent-created", owner.StartTime.ToFileTimeUtc().ToString(), "--parent-image", image.ToString() }) launch.ArgumentList.Add(argument);
        using var candidate = Process.Start(launch)!;
        try
        {
            using (var rogue = Process.Start(Harness("--rogue", pipeName))!) { using var timeout = new CancellationTokenSource(5000); await rogue.WaitForExitAsync(timeout.Token); Require("same-user-wrong-process-rejected", rogue.ExitCode == 0); }
            using var connection = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly); await connection.ConnectAsync(2000);
            async Task<JsonElement> Raw(string protocol, string operation) { byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(new { protocol, id = "fixture", operation, arguments = new { } }); await connection.WriteAsync(BitConverter.GetBytes(bytes.Length)); await connection.WriteAsync(bytes); using var timeout = new CancellationTokenSource(3000); byte[] length = new byte[4]; await connection.ReadExactlyAsync(length, timeout.Token); int count = BitConverter.ToInt32(length); if (count < 1 || count > 1024 * 1024) throw new InvalidDataException(); byte[] result = new byte[count]; await connection.ReadExactlyAsync(result, timeout.Token); using var doc = JsonDocument.Parse(result); return doc.RootElement.Clone(); }
            var rejected = await Raw("Unsupported/1", "health"); Require("wrong-protocol-rejected", !rejected.GetProperty("ok").GetBoolean());
            var accepted = await Raw(NativeSystemClient.Protocol, "health"); Require("trusted-parent-still-connects-after-rejection", accepted.GetProperty("ok").GetBoolean());
            await connection.WriteAsync(BitConverter.GetBytes(1024 * 1024 + 1)); using var finish = new CancellationTokenSource(5000); await candidate.WaitForExitAsync(finish.Token); Require("oversized-frame-fails-closed-and-exits", true);
        }
        finally { if (!candidate.HasExited) { candidate.Kill(); await candidate.WaitForExitAsync(); } }
    }
    await using (var pending = await NativeSystemClient.Start(exe, hash, CancellationToken.None))
    {
        var quiet = await pending.Request("health", new { }); using var cancel = new CancellationTokenSource(100); bool cancelled = false;
        try { await pending.Request("next", new { afterRevision = quiet.GetProperty("revision").GetInt64(), timeoutMs = 10000 }, cancel.Token); } catch (OperationCanceledException) { cancelled = true; }
        Require("cancelled-long-poll-closes-transport", cancelled);
    }
    await SamplingChecks.Run(exe,hash,Path.GetDirectoryName(report)!,checks);
    string configRoot = Path.Combine(Path.GetDirectoryName(report)!, "config-data-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(configRoot);
    JsonObject document = JsonNode.Parse("""
    {"protocol":"Magic.System.Configuration/1","schemaVersion":1,"revision":1,"toolbar":{"locked":false,"sections":{"left":[{"id":"clock-1","kind":"module","module":"magic.clock"}],"center":[],"right":[]},"appearance":{"edge":"Top","itemSize":16}},"dock":{"locked":false,"sections":{"left":[],"center":[{"id":"separator-1","kind":"separator"}],"right":[]},"appearance":{"edge":"Bottom","iconSize":40}},"preferences":{"language":"zh-CN","reduceMotion":false}}
    """)!.AsObject();
    string configFile = Path.Combine(configRoot, "configuration.json");
    await using (var configured = await NativeSystemClient.Start(exe, hash, CancellationToken.None, configRoot))
    {
        var empty = await configured.Request("configuration-read", new { }); Require("new-config-namespace-starts-empty", !empty.GetProperty("present").GetBoolean());
        var written = await configured.Request("configuration-write", new { expectedRevision = 0, configuration = document }); Require("native-schema-published-atomically", written.GetProperty("written").GetBoolean() && File.Exists(configFile)); byte[] original = File.ReadAllBytes(configFile);
        var read = await configured.Request("configuration-read", new { }); Require("native-schema-roundtrip", read.GetProperty("configuration").GetProperty("toolbar").GetProperty("sections").GetProperty("left")[0].GetProperty("module").GetString() == "magic.clock");
        await using (var presentation = new NativeSystemPresentationSession(configured))
        {
            var view = await presentation.Initialize(); Require("native-provider-session-produces-renderer-data", view["projectionOnly"]!.GetValue<bool>() && view["monitors"]!.AsArray().Count > 0 && view["toolbar"]!["left"]![0]!["nativeModule"]!.GetValue<string>() == "magic.clock");
            var listenerState = await configured.Request("health", new { }); Require("native-provider-session-subscribes-only-needed-data", listenerState.GetProperty("windowsSubscribed").GetBoolean() && !listenerState.GetProperty("systemSubscribed").GetBoolean() && listenerState.GetProperty("bluetooth").GetProperty("watchers").GetInt32() == 0);
            // Drain initial registration events, then stop during a long wait.
            await presentation.Next(1); var waiting = presentation.Next(10000); await Task.Delay(25); var disposeWatch = Stopwatch.StartNew(); await presentation.DisposeAsync(); await waiting;
            metrics["presentationDisposeWhileWaitingMs"] = disposeWatch.Elapsed.TotalMilliseconds; Require("presentation-dispose-wakes-event-wait-promptly", disposeWatch.ElapsedMilliseconds < 750);
        }
        Require("native-provider-session-dispose-releases-demand", !(await configured.Request("health", new { })).GetProperty("windowsSubscribed").GetBoolean());
        document["revision"] = 2; bool stale = false; try { await configured.Request("configuration-write", new { expectedRevision = 0, configuration = document }); } catch (NativeSystemRequestException) { stale = true; }
        Require("stale-config-revision-rejected", stale && File.ReadAllBytes(configFile).SequenceEqual(original));
        var invalid = document.DeepClone().AsObject(); invalid["toolbar"]!["sections"]!["left"]![0]!["module"] = "@seelen/not-native"; bool module = false; try { await configured.Request("configuration-write", new { expectedRevision = 1, configuration = invalid }); } catch (NativeSystemRequestException) { module = true; }
        Require("legacy-executable-module-id-not-accepted-as-native", module && File.ReadAllBytes(configFile).SequenceEqual(original));
        invalid = document.DeepClone().AsObject(); invalid["dock"]!["sections"]!["right"] = new JsonArray(invalid["toolbar"]!["sections"]!["left"]![0]!.DeepClone()); bool duplicate = false; try { await configured.Request("configuration-write", new { expectedRevision = 1, configuration = invalid }); } catch (NativeSystemRequestException) { duplicate = true; }
        Require("duplicate-config-item-id-rejected", duplicate && File.ReadAllBytes(configFile).SequenceEqual(original));
        File.WriteAllText(configFile, "{broken"); bool corrupt = false; try { await configured.Request("configuration-write", new { expectedRevision = 1, configuration = document }); } catch (NativeSystemRequestException) { corrupt = true; }
        Require("corrupt-existing-config-is-not-reset", corrupt && File.ReadAllText(configFile) == "{broken"); File.WriteAllBytes(configFile, original);
        var next = await configured.Request("configuration-write", new { expectedRevision = 1, configuration = document }); Require("config-revision-advances", next.GetProperty("revision").GetInt32() == 2);
        document["revision"]=3;document["toolbar"]!["sections"]!["right"]!.AsArray().Add(new JsonObject{["id"]="lyrics-owned",["kind"]="module",["module"]="magic.lyrics"});
        await configured.Request("configuration-write",new{expectedRevision=2,configuration=document});
        await using(var lyricsPresentation=new NativeSystemPresentationSession(configured)){
            var lyricView=await lyricsPresentation.Initialize();Require("lyrics-native-module-configuration-roundtrip",lyricView["toolbar"]!["right"]![0]!["nativeModule"]!.GetValue<string>()=="magic.music");
            var lyricHealth=await configured.Request("health",new{});Require("lyrics-module-demands-media-events",lyricHealth.GetProperty("media").GetProperty("subscribed").GetBoolean());
        }
        Require("lyrics-removal-releases-media-events",!(await configured.Request("health",new{})).GetProperty("media").GetProperty("subscribed").GetBoolean());
    }
    await using (var restarted = await NativeSystemClient.Start(exe, hash, CancellationToken.None, configRoot)) { var persistent = await restarted.Request("configuration-read", new { }); Require("config-survives-owned-backend-restart", persistent.GetProperty("revision").GetInt32() == 3); }
    int last = Convert.ToInt32(metrics["lastOwnedPid"]); bool exited = false; try { using var owned = Process.GetProcessById(last); exited = owned.HasExited; } catch (ArgumentException) { exited = true; }
    Require("graceful-stop-exits", exited);
    string childPidPath = Path.Combine(Path.GetDirectoryName(report)!, "orphan-pid-" + Guid.NewGuid().ToString("N") + ".txt");
    using (var parent = Process.Start(Harness("--orphan-parent", exe, hash, childPidPath))!) {
        int pid=0;
        try{
            var deadline=Stopwatch.StartNew();
            while(deadline.ElapsedMilliseconds<7000&&pid==0){
                try{if(File.Exists(childPidPath))int.TryParse(File.ReadAllText(childPidPath),out pid);}catch(IOException){}
                if(pid==0)await Task.Delay(50);
            }
            Require("orphan-test-parent-started",pid>0);
        }finally{if(!parent.HasExited)parent.Kill();await parent.WaitForExitAsync();}
        try{using var orphan=Process.GetProcessById(pid);using var limit=new CancellationTokenSource(6000);await orphan.WaitForExitAsync(limit.Token);}catch(ArgumentException){}
        Require("parent-death-releases-backend",true);
    }
}
catch (Exception error) { metrics["failure"] = error.ToString(); }
var result = new { allPass = checks.Count >= 20 && checks.Values.All(x => x) && !metrics.ContainsKey("failure"), checks, metrics, scope = "Owned native backend and offscreen fixture only; no desktop takeover, real window actions, startup, permission or layout changes" }; File.WriteAllText(report, JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true })); Console.WriteLine(JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true })); return result.allPass ? 0 : 1;

sealed class Fixture : IDisposable
{
    readonly Thread thread; readonly ManualResetEventSlim ready = new(); readonly Native.WndProc callback; IntPtr hwnd; readonly string className = "MagicSystemCheck-" + Guid.NewGuid().ToString("N"); public string Title { get; private set; } = "Magic-owned-fixture-" + Guid.NewGuid().ToString("N");
    public IntPtr Window => hwnd;
    public Fixture(string prefix = "MagicSystemCheck-") { className=prefix+Guid.NewGuid().ToString("N"); callback = Procedure; thread = new Thread(Run) { IsBackground = true }; thread.Start(); if (!ready.Wait(3000) || hwnd == IntPtr.Zero) throw new InvalidOperationException("Fixture creation failed"); }
    void Run() { var wc = new Native.WndClass { lpfnWndProc = callback, lpszClassName = className, hInstance = Native.GetModuleHandle(null) }; Native.RegisterClass(ref wc); hwnd = Native.CreateWindowEx(0x08040000, className, Title, 0x00CF0000, -30000, -30000, 180, 100, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero); Native.ShowWindow(hwnd, 4); ready.Set(); while (Native.GetMessage(out var msg, IntPtr.Zero, 0, 0) > 0) { Native.TranslateMessage(ref msg); Native.DispatchMessage(ref msg); } Native.UnregisterClass(className, wc.hInstance); }
    IntPtr Procedure(IntPtr h, uint m, IntPtr w, IntPtr l) { if (m == 2) { hwnd = IntPtr.Zero; Native.PostQuitMessage(0); } return Native.DefWindowProc(h, m, w, l); }
    public void RenameTo(string title) { Title = title; Native.SetWindowText(hwnd, Title); }
    public void Rename() { Title += "-renamed"; Native.SetWindowText(hwnd, Title); }
    public void Taskbar(bool enabled){Native.SetWindowLongPtr(hwnd,-20,new IntPtr(enabled?0x08040000:0x08000000));Native.NotifyWinEvent(0x800A,hwnd,0,0);}
    public void Cloak(bool value){int flag=value?1:0;Marshal.ThrowExceptionForHR(Native.DwmSetWindowAttribute(hwnd,13,ref flag,4));}
    public bool IsCloaked(){Marshal.ThrowExceptionForHR(Native.DwmGetWindowAttribute(hwnd,14,out uint state,4));return state!=0;}
    public void Dispose() { if (hwnd != IntPtr.Zero) Native.PostMessage(hwnd, 0x10, IntPtr.Zero, IntPtr.Zero); thread.Join(2000); ready.Dispose(); }
}
static class Native
{
    [DllImport("dwmapi.dll")]internal static extern int DwmSetWindowAttribute(IntPtr hwnd,uint attribute,ref int value,uint size);
    [DllImport("dwmapi.dll")]internal static extern int DwmGetWindowAttribute(IntPtr hwnd,uint attribute,out uint value,uint size);
    [DllImport("user32.dll")]internal static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll",CharSet=CharSet.Unicode)]internal static extern IntPtr FindWindowEx(IntPtr parent,IntPtr after,string cls,string? title);
    [DllImport("user32.dll")]internal static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("user32.dll")]internal static extern IntPtr SetWindowLongPtr(IntPtr hwnd,int index,IntPtr value);
    [DllImport("user32.dll")]internal static extern void NotifyWinEvent(uint eventId,IntPtr hwnd,int objectId,int childId);
    internal delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] internal struct WndClass { public uint style; public WndProc lpfnWndProc; public int cbClsExtra, cbWndExtra; public IntPtr hInstance, hIcon, hCursor, hbrBackground; public string? lpszMenuName, lpszClassName; }
    [StructLayout(LayoutKind.Sequential)] internal struct Msg { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; public uint lPrivate; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] internal static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder image, ref uint size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] internal static extern IntPtr GetModuleHandle(string? name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern ushort RegisterClass(ref WndClass wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern bool UnregisterClass(string name, IntPtr instance);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern IntPtr CreateWindowEx(uint ex, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] internal static extern bool ShowWindow(IntPtr h, int show);
    [DllImport("user32.dll")] internal static extern int GetMessage(out Msg msg, IntPtr h, uint min, uint max);
    [DllImport("user32.dll")] internal static extern bool TranslateMessage(ref Msg msg);
    [DllImport("user32.dll")] internal static extern IntPtr DispatchMessage(ref Msg msg);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] internal static extern void PostQuitMessage(int code);
    [DllImport("user32.dll")] internal static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern bool SetWindowText(IntPtr h, string title);
}
