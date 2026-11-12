using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace MagicCommunity;
public static class Program
{
    private static string? Argument(string[] args,string name)
    {
        int at=Array.IndexOf(args,name);return at>=0&&at+1<args.Length?args[at+1]:null;
    }
    [STAThread]
    public static int Main(string[] args)
    {
        if(args.Length==1&&args[0]=="--show-desktop")return ShowDesktopCommand.Run();
        if(args.Length==1&&args[0]=="--empty-recycle-bin")return RecycleBinCommand.Run(RecycleBinWatch.EmptyWithConfirmation, @"Local\MagicCommunity.EmptyRecycleBin");
        if(args.Length==1&&args[0]=="--media-source-probe")return NativeMediaSource.Run();
        if(args.Length>0&&args[0]=="--encodeck-accessibility")return OriginalEncoDeckAccessibility.Run(args);
        string root=args.Contains("--installed-desktop")?AppContext.BaseDirectory:Path.GetFullPath(Argument(args,"--root")??FindRoot());
        string? report=Argument(args,"--report");
        try
        {
            if(args.Contains("--installed-desktop")){
                if(args.Contains("--configure-installation")&&Win32Installation.IsUnpackaged&&!Win32Installation.Configure())return 0;
                string? installedRoot=InstalledDesktop.Resolve();
                if(installedRoot is null&&Win32Installation.IsUnpackaged){if(!Win32Installation.Configure())return 0;installedRoot=InstalledDesktop.Resolve();}
                if(installedRoot is null){
                    MessageBox.Show("尚未配置数据目录。请运行安装包中的“安装与卸载”向导，选择“配置数据目录”后再启动。","Magic 视界社区版");
                    return 2;
                }
                root=installedRoot;
                NativeTaskbarRecovery.RecoverStale(Path.Combine(root,"runtime/system/native-taskbar"));
            }
            if(args.Contains("--toolbar-extension-discovery-probe")){
                // No application/controller construction, watcher, provider connection or preference writes.
                var discovery=ToolbarExtensions.ProbeDiscovery(Argument(args,"--discovery-root")??ToolbarExtensions.DefaultDiscoveryRoot());
                StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(new{protocol="Magic.ToolbarExtension/1",version=AppController.Version,runtime=Environment.Version.ToString(),discovery,scope="Read-only discovery; no provider connection or enablement."},StateStore.Json),null);
                return discovery.Error is null?0:1;
            }
            if(args.Contains("--jump-list-checks")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(NativeJumpListTests.Run(root).GetAwaiter().GetResult(),StateStore.Json),null);return 0;}
            if(args.Contains("--jump-list-probe")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(NativeJumpListTests.Probe(root,Argument(args,"--target")??throw new ArgumentException("Missing --target")).GetAwaiter().GetResult(),StateStore.Json),null);return 0;}
            if(args.Contains("--apps-catalog-probe")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(NativeSystemUiSession.ProbeApplications().GetAwaiter().GetResult(),StateStore.Json),null);return 0;}
            if(args.Contains("--display-recovery-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopDisplayRecoveryTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--settings-gallery")){
                string output=Path.GetFullPath(Argument(args,"--render")??throw new ArgumentException("Missing --render"));
                string checkout=Path.GetFullPath(Argument(args,"--checkout")??throw new ArgumentException("Missing --checkout"));string artifacts=Path.Combine(checkout,"artifacts")+Path.DirectorySeparatorChar;
                if(!File.Exists(Path.Combine(checkout,".git"))||!root.StartsWith(artifacts,StringComparison.OrdinalIgnoreCase)||!output.StartsWith(artifacts,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("外观导出仅接受工作树的隔离 artifacts 目录。");
                Directory.CreateDirectory(output);var galleryApp=new Application{ShutdownMode=ShutdownMode.OnExplicitShutdown};new NativeWpfTheme().ApplyInteractions(galleryApp.Resources);galleryApp.Resources.MergedDictionaries.Add(new ResourceDictionary{Source=new Uri("/MagicCommunity;component/ScrollbarStyles.xaml",UriKind.Relative)});var fixtureController=new AppController(root){PowerMode="平衡"};foreach(var board in fixtureController.State.Boards){board.Grid=new DesktopGrid{Columns=14,Rows=9};for(int i=0;i<26;i++)board.Grid.Placements["fixture:"+i]=new GridRect(i%14,i/14,1,1);}
                if(args.Contains("--notification-position-states")){
                    fixtureController.Screens.Clear();
                    fixtureController.Screens.Add(new Display("fixture-main","主屏 · 2560 × 1440","fixture-main",0,0,2560,1400,true){PixelWidth=2560,PixelHeight=1440,Dpi=144});
                    fixtureController.Screens.Add(new Display("fixture-secondary","副屏 · 1440 × 2560","fixture-secondary",2560,0,1440,2520,false){PixelWidth=1440,PixelHeight=2560,Dpi=192});
                    foreach(bool dark in new[]{false,true})foreach(bool secondary in new[]{false,true})foreach(bool automatic in new[]{false,true}){
                        var window=new CommunitySettingsWindow(fixtureController,dark);window.ExportNotificationState(Path.Combine(output,$"positions-{(secondary?"secondary":"main")}-{(automatic?"auto":"fixed")}-{(dark?"dark":"light")}.png"),secondary,automatic);window.Close();
                    }
                    return 0;
                }
                if(args.Contains("--icon-name-states")){
                    foreach(bool dark in new[]{false,true})foreach(bool changed in new[]{false,true}){
                        var preferences=new CommunityPreferences{CollapsedFolderIconNames=changed,DesktopIconNames=!changed};
                        Directory.CreateDirectory(Path.Combine(root,"data"));StateStore.AtomicWrite(Path.Combine(root,"data/ui-preferences.json"),JsonSerializer.Serialize(preferences),null);
                        var window=new CommunitySettingsWindow(fixtureController,dark);window.ExportPage("外观",Path.Combine(output,$"settings-names-{(changed?"custom":"default")}-{(dark?"dark":"light")}.png"),1080);window.Close();
                    }return 0;
                }
                foreach(bool dark in new[]{false,true}){var window=new CommunitySettingsWindow(fixtureController,dark);foreach(string page in new[]{"桌面","省电","账号","关于"})window.ExportPage(page,Path.Combine(output,"settings-"+page+(dark?"-dark":"-light")+".png"));window.Close();}
                foreach(bool dark in new[]{false,true}){var info=new NativeWeatherInfoWindow("城市：威海 · 山东\n经纬度：37.50914,122.11356\n时区：Asia/Shanghai\n更新于：示例数据\n\n天气 Open-Meteo · 城市 GeoNames\n城市为手动搜索选定，不读取系统定位",dark);info.Export(Path.Combine(output,"weather-info-"+(dark?"dark":"light")+".png"));info.Close();}
                foreach(bool dark in new[]{false,true})foreach(bool populated in new[]{false,true})foreach(int dpi in new[]{96,192}){var cityWindow=new NativeWeatherCityWindow(dark);cityWindow.Export(Path.Combine(output,$"weather-city-{(populated?"results":"empty")}-{(dark?"dark":"light")}-{dpi}.png"),populated,dpi);cityWindow.Close();}return 0;
            }
            if(args.Contains("--composition-components-preview"))return CompositionDesktopRuntime.Preview(root,Argument(args,"--trial-root")??throw new ArgumentException("Missing --trial-root"),int.TryParse(Argument(args,"--benchmark-cycles"),out int previewCycles)?previewCycles:0,args.Contains("--popup-benchmark"));
            if(args.Contains("--composition-trial-test"))return CompositionDesktopTrial.CheckFixtures(root,report??throw new ArgumentException("Missing --report"));
            if(args.Contains("--composition-desktop-trial"))return CompositionDesktopTrial.Run(root,Argument(args,"--trial-root"),Argument(args,"--composition-host")??throw new ArgumentException("Missing --composition-host"),args.Contains("--all-displays"),args.Contains("--synthetic-icons"),int.TryParse(Argument(args,"--benchmark-cycles"),out int cycles)?cycles:0);
            if(args.Contains("--native-desktop-trial"))return NativeDesktopTrial.Run(root,Argument(args,"--trial-root"),args.Contains("--all-displays"));
            if(args.Contains("--shell-menu-worker"))return DesktopMenuHost.RunWorker(int.Parse(Argument(args,"--parent")!),long.Parse(Argument(args,"--parent-created")!),Argument(args,"--menu-pipe")!);
            if(args.Contains("--custom-card-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(CustomCardTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-creation-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopCreationTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-menu-query-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopMenuHost.TestFileQueries(root).GetAwaiter().GetResult(),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-menu-warm-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopMenuHost.TestWarmQueries(root,Argument(args,"--target")).GetAwaiter().GetResult(),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-menu-staged-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopMenuHost.TestStagedQueries(root).GetAwaiter().GetResult(),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-card-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopCardTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-refresh-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopRefreshTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-enumeration-probe")){var items=ShellDesktop.Enumerate();StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(new{accepted=true,count=items.Count,hiddenCount=ShellDesktop.HiddenTargets.Count,scope="Read only; no layout writes or Explorer refresh"},StateStore.Json),null);return 0;}
            if(args.Contains("--clipboard-move-data-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopClipboardMoveTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-group-recycle-test")){if(!args.Contains("--allow-fixture-recycle"))throw new ArgumentException("Missing --allow-fixture-recycle");StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopGroupOperationsTests.TestRecycleFolders(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-group-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopGroupOperationsTests.Run(root,args.Contains("--physical-fixtures"),args.Contains("--allow-fixture-recycle")),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-multiselect-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopMultiSelectionTests.Run(root,args.Contains("--allow-fixture-recycle")),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-menu-functional-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopMenuFunctionalTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-properties-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopPropertiesTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--clipboard-preparation-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopClipboardTimingTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--clipboard-payload-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(ShellClipboardCommands.TestPayloads(root),StateStore.Json),null);return 0;}
            if(args.Contains("--context-menu-self-test")){
                if(!NativeMenuBitmap.TestAlphaAndOwnership())throw new InvalidDataException("Menu alpha/ownership test failed.");
                var result=DesktopMenuHost.TestContract(root).GetAwaiter().GetResult();
                StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(new{allPass=true,alphaAndOwnershipPass=true,result},StateStore.Json),null);return 0;
            }
            if(args.Contains("--context-menu-probe")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopContextMenu.Inspect(Argument(args,"--target")),StateStore.Json),null);return 0;}
            if(args.Contains("--restore-native-icons"))return NativeDesktopLease.RestoreRecord(root)?0:1;
            if(args.Contains("--native-icon-state")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("缺少 --report")),JsonSerializer.Serialize(new{visible=NativeDesktopLease.IconsVisible()},StateStore.Json),null);return 0;}
            if(args.Contains("--collection-audit"))
            {
                var store=new StateStore(Path.Combine(root,"data"));var state=store.Load();var files=new ManagedDesktopFiles(store.DirectoryPath,ManagedDesktopFiles.DesktopRoots());files.Recover(state,true);
                StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("缺少 --report")),JsonSerializer.Serialize(new{hasStoredFiles=files.HasStoredFiles,problem=files.Problem,layoutReadOnly=store.ReadOnly,scope="read_only_no_file_moves"},StateStore.Json),null);return 0;
            }
            if(args.Contains("--native-desktop-probe")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("缺少 --report")),JsonSerializer.Serialize(NativeDesktop.Read(),StateStore.Json),null);return 0;}
            if(args.Contains("--export-layout-v1")){LayoutCompatibility.ExportFile(root,Argument(args,"--output")??throw new ArgumentException("缺少 --output"));return 0;}
            if(args.Contains("--update-window-checks")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(CommunityUpdateTests.Run(Path.Combine(root,"update-gallery")),StateStore.Json),null);return 0;}
            if(args.Contains("--desktop-grid-settings-test")){StateStore.AtomicWrite(Path.GetFullPath(report??throw new ArgumentException("Missing --report")),JsonSerializer.Serialize(DesktopGridSettingsTests.Run(root),StateStore.Json),null);return 0;}
            if(args.Contains("--self-test")){SelfTests.Run(root,report??Path.Combine(root,"evidence/preview/self-test.json"));return 0;}
            if(args.Contains("--native-taskbar-recover")){
                string head=Path.Combine(root,".git/HEAD");if(!File.Exists(head)||File.ReadAllText(head).Trim()!="ref: refs/heads/main")throw new InvalidOperationException("Taskbar recovery requires the main workspace.");
                NativeTaskbarRecovery.RecoverStale(Path.Combine(root,"runtime/system/native-taskbar"));return 0;
            }
            bool smoke=args.Contains("--smoke");
            string key=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(root.ToUpperInvariant())))[..24];
            using var mutex=new Mutex(false,"Local\\MagicCommunity-"+key);
            bool owns=smoke;
            if(!smoke){try{owns=mutex.WaitOne(0);}catch(AbandonedMutexException){owns=true;}}
            if(!owns)
            {
                try
                {
                    using var client=new NamedPipeClientStream(".","MagicCommunity-"+key,PipeDirection.Out);
                    client.Connect(4000);using var writer=new StreamWriter(client){AutoFlush=true};writer.WriteLine(args.Contains("--quit")?"quit":args.Contains("--desktop")?"desktop":"show");
                }
                catch(Exception){MessageBox.Show("应用正在启动或已有实例未响应。请先关闭已有窗口，再从固定入口打开。","Magic 视界社区版");}
                return 0;
            }
            try
            {
                var controller=new AppController(root);
                if(controller.State.SoftwareRendering)RenderOptions.ProcessRenderMode=RenderMode.SoftwareOnly;
                var app=new Application{ShutdownMode=ShutdownMode.OnExplicitShutdown};
                new NativeWpfTheme().ApplyInteractions(app.Resources);
                app.Resources.MergedDictionaries.Add(new ResourceDictionary{Source=new Uri("/MagicCommunity;component/ScrollbarStyles.xaml",UriKind.Relative)});
                using var cancellation=new CancellationTokenSource();
                MainWindow? main=null;
                MainWindow Main()
                {
                    if(main is not null)return main;
                    main=new MainWindow(controller);app.MainWindow=main;
                    main.Closed+=(_,_)=>{cancellation.Cancel();app.Shutdown();};ConfigureMain(main);return main;
                }
                CommunitySettingsWindow? communitySettings=null;
                void Settings()=>OpenSettings(null);
                void OpenSettings(string? page){if(communitySettings is null){communitySettings=new(controller);communitySettings.Closed+=(_,_)=>communitySettings=null;}if(page is not null)communitySettings.Navigate(page);communitySettings.Show();if(communitySettings.WindowState==WindowState.Minimized)communitySettings.WindowState=WindowState.Normal;communitySettings.Activate();}
                DesktopBridge? bridge=null;
                CompositionDesktopRuntime? composition=null;
                var installedExit=new InstalledExitSequence(
                    ()=>InstalledDesktopTransition.StartExit(controller),
                    async ()=>{controller.DesktopActive=false;if(composition is not null)await composition.StopForExit();else bridge?.Dispose();},
                    ()=>{cancellation.Cancel();app.Shutdown();},
                    error=>controller.Log("installed_exit_failed",new{error=error.Message}));
                bool nativeRendering=args.Contains("--composition-desktop")||CompositionDesktopRuntime.Selected(root);
                void StartDesktop()
                {
                    if(bridge is not null&&!bridge.HasLostTakeover)return;
                    composition?.Dispose();composition=null;bridge?.Dispose();bridge=null;
                    controller.DesktopActive=true;controller.RefreshDesktop();
                    bridge=new DesktopBridge(controller,Settings,()=>{var settings=Main();settings.Show();settings.Activate();settings.ShowFeedback();},()=>
                    {
                        if(InstalledDesktop.IsCurrent(root)){_ = installedExit.Request();return;}
                        var stop=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"WindowsPowerShell/v1.0/powershell.exe")){UseShellExecute=false,CreateNoWindow=true};
                        foreach(var value in new[]{"-NoProfile","-ExecutionPolicy","Bypass","-File",Path.Combine(root,"tools/stop-desktop.ps1")})stop.ArgumentList.Add(value);
                        Process.Start(stop);
                    },compositionRenderer:nativeRendering,showAbout:()=>OpenSettings("关于"));
                    if(nativeRendering)composition=CompositionDesktopRuntime.Start(controller,bridge,args.Contains("--logon"));
                    main?.Hide();
                }

                app.DispatcherUnhandledException+=(_,e)=>{controller.Log("unhandled_error",new{detail=e.Exception.ToString()});MessageBox.Show("应用发生错误，已保存日志。布局只在成功保存后更新，请重新打开固定入口。","Magic 视界社区版");e.Handled=true;app.Shutdown(1);};
                if(!smoke)_=Listen(key,command=>
                {
                    if(command=="quit"){if(InstalledDesktop.IsCurrent(root)&&composition is not null)_=installedExit.Request();else{controller.DesktopActive=false;cancellation.Cancel();app.Shutdown();}}
                    else if(command=="desktop")StartDesktop();
                    else if(command=="show"){var settings=Main();settings.Show();if(settings.WindowState==WindowState.Minimized)settings.WindowState=WindowState.Normal;settings.Activate();}
                },cancellation.Token);
                void ConfigureMain(MainWindow main)
                {
                main.ContentRendered+=(_,_)=>
                {
                    controller.RefreshDesktop();
                    if(!smoke)return;
                    var timer=new DispatcherTimer{Interval=TimeSpan.FromSeconds(2)};
                    timer.Tick+=(_,_)=>
                    {
                        timer.Stop();
                        try
                        {
                            string? image=Argument(args,"--render");
                            if(image is not null)SaveRender(main,image);
                            if(image is not null)
                            {
                                var feedback=new FeedbackWindow(controller,main);feedback.PopulateSmoke(SelfTests.ExampleImage());feedback.Show();feedback.UpdateLayout();
                                SaveRender(feedback,Path.Combine(Path.GetDirectoryName(Path.GetFullPath(image))!,"feedback-render.png"));feedback.Close();
                            }
                            var screenWindows=new List<object>();
                            foreach(var display in controller.Screens)
                            {
                                var boardWindow=new MainWindow(controller,display.Id);
                                boardWindow.Show();Displays.Place(boardWindow,display);
                                var boardHandle=new WindowInteropHelper(boardWindow).Handle;
                                if(!GetWindowRect(boardHandle,out var rect))throw new InvalidOperationException("Cannot read preview window geometry");
                                screenWindows.Add(new{displayId=display.Id,handle=boardHandle.ToInt64(),dpi=Displays.GetDpiForWindow(boardHandle),visible=IsWindowVisible(boardHandle),rect=new{left=rect.Left,top=rect.Top,right=rect.Right,bottom=rect.Bottom}});
                                boardWindow.Close();
                            }
                            var hwnd=new WindowInteropHelper(main).Handle;
                            var counters=new MemoryCounters{Size=(uint)Marshal.SizeOf<MemoryCounters>()};
                            if(!K32GetProcessMemoryInfo(new IntPtr(-1),ref counters,counters.Size))throw new InvalidOperationException("Cannot read process counters");
                            var receipt=new{version=AppController.Version,at=DateTimeOffset.Now,pid=Environment.ProcessId,runtime=Environment.Version.ToString(),render=RenderOptions.ProcessRenderMode.ToString(),
                                desktopCount=controller.Desktop.Count,folders=controller.Desktop.Count(x=>x.IsFolder),virtualItems=controller.Desktop.Count(x=>x.IsVirtual),displays=controller.Screens,
                                screenWindows,mainWindowVisible=IsWindowVisible(hwnd),privateWorkingSetBytes=(ulong)counters.PrivateWorkingSet,privateCommitBytes=(ulong)counters.PrivateUsage,
                                image,scope="Programmatic window/render smoke test only; no real user drag or mixed-DPI acceptance"};
                            if(report is not null)StateStore.AtomicWrite(Path.GetFullPath(report),JsonSerializer.Serialize(receipt,StateStore.Json),null);
                            app.Shutdown(0);
                        }
                        catch(Exception ex){if(report is not null)StateStore.AtomicWrite(Path.GetFullPath(report),JsonSerializer.Serialize(new{error=ex.ToString()},StateStore.Json),null);app.Shutdown(1);}
                    };
                    timer.Start();
                };
                }
                int exitCode;
                try
                {
                    if(args.Contains("--desktop")||args.Contains("--composition-desktop"))StartDesktop();else Main().Show();
                    exitCode=app.Run();
                }
                finally{composition?.Dispose();bridge?.Dispose();}
                if(InstalledDesktop.IsCurrent(root) && composition?.StartupFailure is {} cause)
                    throw new IOException("顶栏和 Dock 未能启动："+cause.Message,cause);
                return exitCode;
            }
            finally{if(!smoke)mutex.ReleaseMutex();}
        }
        catch(Exception ex)
        {
            if(args.Contains("--desktop-grid-settings-test")||args.Contains("--update-window-checks")){if(report is not null)StateStore.AtomicWrite(Path.GetFullPath(report),JsonSerializer.Serialize(new{error=ex.ToString()},StateStore.Json),null);return 1;}
            if(args.Contains("--shell-menu-worker"))return 1; // Worker bootstrap failures must never open a dialog.
            string? diagnostic=null;
            try
            {
                diagnostic=Path.Combine(root,"logs","startup-failure-"+DateTime.UtcNow.ToString("yyyyMMdd-HHmmssfff")+"-"+Environment.ProcessId+".json");
                StateStore.AtomicWrite(diagnostic,JsonSerializer.Serialize(new{at=DateTimeOffset.Now,version=AppController.Version,pid=Environment.ProcessId,root,error=ex.ToString()},StateStore.Json),null);
            }
            catch{diagnostic=null;}
            if(report is not null)StateStore.AtomicWrite(Path.GetFullPath(report),JsonSerializer.Serialize(new{error=ex.ToString()},StateStore.Json),null);
            else MessageBox.Show("启动失败："+ex.Message+(diagnostic is null?"":"\n诊断记录："+diagnostic),"Magic 视界社区版",MessageBoxButton.OK,MessageBoxImage.Error);
            return 1;
        }
    }
    private static string FindRoot()
    {
        var directory=new DirectoryInfo(AppContext.BaseDirectory);
        while(directory is not null){if(File.Exists(Path.Combine(directory.FullName,"app.current.json")))return directory.FullName;directory=directory.Parent;}
        throw new DirectoryNotFoundException("请使用项目中的固定启动入口。");
    }
    private static async Task Listen(string key,Action<string> command,CancellationToken cancellation)
    {
        try
        {
            while(!cancellation.IsCancellationRequested)
            {
                using var server=new NamedPipeServerStream("MagicCommunity-"+key,PipeDirection.In,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous|PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync(cancellation).ConfigureAwait(false);
                using var timeout=CancellationTokenSource.CreateLinkedTokenSource(cancellation);timeout.CancelAfter(3000);
                try
                {
                    // Read a bounded complete command; pipe reads may return partial messages.
                    var bytes=new byte[16];int n=0;
                    while(n<bytes.Length)
                    {
                        int read=await server.ReadAsync(bytes.AsMemory(n),timeout.Token).ConfigureAwait(false);
                        if(read==0)break;n+=read;if(bytes.AsSpan(0,n).Contains((byte)'\n'))break;
                    }
                    string message=Encoding.UTF8.GetString(bytes,0,n).Trim('\r','\n','\uFEFF');
                    if(message is "show" or "desktop" or "quit")await Application.Current.Dispatcher.InvokeAsync(()=>command(message));
                }
                catch(OperationCanceledException)when(!cancellation.IsCancellationRequested){/* Reject stalled client and accept another. */}
            }
        }
        catch(OperationCanceledException)when(cancellation.IsCancellationRequested){}
        catch(Exception){/* App remains usable; subsequent launches surface activation failure. */}
    }
    private static void SaveRender(Window window,string path)
    {
        var element=(FrameworkElement)window.Content;element.UpdateLayout();
        var bitmap=new RenderTargetBitmap((int)Math.Ceiling(element.ActualWidth),(int)Math.Ceiling(element.ActualHeight),96,96,PixelFormats.Pbgra32);
        // Draw at (0,0); rendering a child visual directly preserves its parent margin and clips the lower/right edge.
        var drawing=new DrawingVisual();using(var context=drawing.RenderOpen())context.DrawRectangle(new VisualBrush(element),null,new Rect(0,0,element.ActualWidth,element.ActualHeight));
        bitmap.Render(drawing);var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);using var stream=File.Create(path);encoder.Save(stream);
    }
    [StructLayout(LayoutKind.Sequential)]private struct NativeRect{public int Left,Top,Right,Bottom;}
    [DllImport("user32.dll")]private static extern bool GetWindowRect(IntPtr hwnd,out NativeRect rect);
    [DllImport("user32.dll")]private static extern bool IsWindowVisible(IntPtr hwnd);
    [StructLayout(LayoutKind.Sequential)]private struct MemoryCounters
    {
        public uint Size,Faults;public nuint PeakWorkingSet,WorkingSet,PeakPaged,Paged,PeakNonpaged,Nonpaged,Pagefile,PeakPagefile,PrivateUsage,PrivateWorkingSet;public ulong SharedCommit;
    }
    [DllImport("kernel32.dll")]private static extern bool K32GetProcessMemoryInfo(IntPtr process,ref MemoryCounters counters,uint size);
}
