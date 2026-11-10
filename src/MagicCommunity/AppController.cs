using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace MagicCommunity;
public sealed class AppController
{
    public const string Version = "0.4.10.32";
    public bool DesktopActive { get; set; }
    public string PowerMode { get; internal set; }="暂不可用";
    public string Root { get; }
    public StateStore Store { get; }
    public LayoutState State { get; private set; }
    public ManagedDesktopFiles Collections{get;}
    internal SecondaryWallpaperGuard? WallpaperGuard {get;set;}
    public List<DesktopItem> Desktop { get; private set; } = [];
    public List<Display> Screens { get; private set; } = [];
    public string Status { get; private set; } = "正在读取桌面…";
    internal ShortcutPreferences KeyboardPreferences {get;set;}=new();
    internal Func<ShortcutPreferences,Task>? ApplyKeyboardPreferences {get;set;}
    // The settings window owns no renderer pipe. The native session lends it the same
    // module catalog and the same toggle the bar's own right-click menu uses, so the
    // two entry points always edit one configuration instead of drifting apart.
    internal Func<CancellationToken,Task<IReadOnlyList<BarModuleState>>>? ReadBarModules {get;set;}
    internal Func<string,string,bool,CancellationToken,Task>? SetBarModule {get;set;}
    // Preferences are read by the renderer out of the published view, so saving the file
    // is not enough: the session has to restate the view for the new values to arrive.
    internal Func<CancellationToken,Task>? RefreshPreferences {get;set;}
    // The card's own tuning panel repaints the standing card, not just the bar.
    internal Func<CancellationToken,Task>? RefreshCard {get;set;}
    // The panel's unsaved copy, which the renderer must see before disk is written.
    internal Action<CommunityPreferences?>? CardDraft {get;set;}
    // The account card's window handle, or zero when it is not standing.
    internal Func<long>? CardWindow {get;set;}
    // Names the panel window so the card does not dismiss when it takes focus.
    internal Func<long,Task>? HoldCard {get;set;}
    // Clicking outside the panel and the card dismisses both, the way clicking
    // outside the card alone does.
    internal Func<Task>? CloseCard {get;set;}
    public event Action? Changed;
    public Action<LayoutState>? PrepareDesktopChange { get; set; }
    private bool desktopLoaded;
    internal DesktopUndoPositions UndoPositions { get; }=new();
    public AppController(string root)
    {
        Root = Path.GetFullPath(root);
        Store = new(Path.Combine(Root, "data"));
        State = Store.Load();
        if(!Store.ReadOnly)DesktopFileChanges.Recover(Root,Store,State);
        Collections=new(Store.DirectoryPath,ManagedDesktopFiles.DesktopRoots());Collections.Recover(State,Store.ReadOnly);
        RefreshDisplays();
        Log("startup", new { version = Version, runtime = Environment.Version.ToString(), render = State.SoftwareRendering ? "SoftwareOnly" : "Default", readOnly = Store.ReadOnly });
    }
    public void RefreshDisplays()
    {
        if(Collections.IsBusy||!DesktopSessionState.CanRefresh())return;
        var screens=Displays.Read();
        if(screens.Count==0||screens.Any(s=>s.Id.StartsWith("temporary:",StringComparison.Ordinal)))return;
        Screens = screens;
        var next = StateStore.Copy(State);
        var notices = Store.ReadOnly ? new List<string>() : DesktopDisplayRecovery.Reconcile(next, Screens);
        foreach (var display in Screens)
            if (!next.Boards.Any(x => x.Id == display.Id)) next.Boards.Add(new() { Id = display.Id, Name = display.Label });
        if (JsonSerializer.Serialize(next, StateStore.Json) != JsonSerializer.Serialize(State, StateStore.Json))
        {
            if (!Store.ReadOnly){next.Revision=State.Revision+1;Store.Save(next);}
            State = next;
        }
        if(notices.Count>0){Status=string.Join(" ",notices);Log("desktop_display_recovery",new{revision=State.Revision,notices,boards=State.Boards.Select(b=>new{b.Id,pages=b.Pages?.Count??1,active=b.ActivePageId,placements=DesktopPages.Grids(b).Sum(g=>g.Placements.Count),cards=DesktopCards.Items(b).Count(),customGrid=b.IconGrid is not null})});}
        Changed?.Invoke();
    }
    public void RefreshDesktop()
    {
        if(Collections.IsBusy||!DesktopSessionState.CanRefresh()||Screens.Count==0)return;
        var timing=Stopwatch.StartNew();
        try
        {
            var desktop = ShellDesktop.Enumerate(DesktopActive);
            if(!Store.ReadOnly)
            {
                UndoPositions.Capture(State);var next=StateStore.Copy(State);UnifiedDesktopGrid.PruneUnshownRoots(next,ShellDesktop.HiddenTargets);UndoPositions.Restore(next,desktop,Screens);DesktopGridActions.Reconcile(next,desktop,Screens);
                if(Store.NeedsMigration||JsonSerializer.Serialize(next,StateStore.Json)!=JsonSerializer.Serialize(State,StateStore.Json)){next.Revision=State.Revision+1;Store.Save(next);State=next;}
            }
            Desktop=desktop;desktopLoaded=true;
            SetStatus($"已读取 {Desktop.Count} 个桌面项目，包含文件夹、公共桌面和 Shell 虚拟项。");
            Log("desktop_enumerated", new { count = Desktop.Count, folders = Desktop.Count(x => x.IsFolder), virtualItems = Desktop.Count(x => x.IsVirtual), elapsedMs=timing.Elapsed.TotalMilliseconds });
        }
        catch(DesktopEnumerationUnavailableException ex){Log("desktop_snapshot_deferred",new{reason=ex.Message,retainedItems=Desktop.Count,revision=State.Revision,elapsedMs=timing.Elapsed.TotalMilliseconds});}
        catch (Exception ex) { Error("读取桌面失败", ex); }
    }
    public void Change(Action<LayoutState> action, string status)
    {
        try
        {
            Commit(action,status);
        }
        catch (Exception ex) { Error("修改未保存", ex); }
    }
    public void Commit(Action<LayoutState> action,string status)
    {
        if(Collections.IsBusy)throw new LayoutEditException("文件移动或系统权限确认尚未完成，请稍候再修改布局。");
        CommitCollection(action,status);
    }
    private void CommitCollection(Action<LayoutState> action,string status)
    {
        UndoPositions.Capture(State);var next=StateStore.Copy(State);action(next);if(desktopLoaded)DesktopGridActions.Reconcile(next,Desktop,Screens);PrepareDesktopChange?.Invoke(next);Collections.ProtectReferences(State,next);next.Revision=State.Revision+1;Store.Save(next);State=next;SetStatus(status);
    }
    private static void RemoveTargets(LayoutState state,IEnumerable<string> targets)
    {
        var removed=targets.ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach(var board in state.Boards){board.RootItems.RemoveAll(item=>removed.Contains(item.Target));foreach(var box in board.Boxes)box.Items.RemoveAll(item=>removed.Contains(item.Target));DesktopGridActions.ReconcileBoard(board);}
    }
    public void CollectDesktop(string folderId,IReadOnlyList<DesktopItem> items)
    {
        if(Store.ReadOnly)throw new LayoutEditException("布局只读，未移动文件。");
        _=State.Boards.SelectMany(board=>board.Boxes).Single(box=>box.Id==folderId);
        Collections.Collect(items,files=>CommitCollection(state=>{RemoveTargets(state,files.Select(file=>file.Original));LayoutActions.Add(state.Boards.SelectMany(board=>board.Boxes).Single(box=>box.Id==folderId),files.Select(file=>file.Item()));},"已移动收纳；可在文件夹内移出并恢复原位置。"),()=>State);
        RefreshDesktop();
    }
    public void RestoreCollected(IReadOnlyList<DesktopItem> items,Action<LayoutState> finish)
    {
        if(Store.ReadOnly)throw new LayoutEditException("布局只读，未移动文件。");
        Collections.Restore(items,files=>CommitCollection(state=>{RemoveTargets(state,files.Select(file=>file.Stored));finish(state);},"已移出，原桌面位置已恢复。"),()=>State);RefreshDesktop();
    }
    public void RestoreBackup()
    {
        try { if(Collections.HasStoredFiles||Collections.Problem is not null)throw new LayoutEditException("存在已移动收纳的项目或未完成记录，恢复旧布局前需先核对操作记录，避免路径错配。");State = Store.RestoreBackup(); SetStatus("已恢复备份，原损坏文件保留在数据目录。"); }
        catch (Exception ex) { Error("恢复失败", ex); }
    }
    public void SetStatus(string text) { Status = text; Changed?.Invoke(); }
    public void RecoverShellChanges()
    {
        UndoPositions.Capture(State);var next=StateStore.Copy(State);DesktopFileChanges.Recover(Root,Store,next,includeExternal:false);State=next;RefreshDesktop();
    }
    public void FinishNativeDrag(string id)
        =>FinishNativeDrags([id]);
    public void FinishNativeDrags(IEnumerable<string> ids)
    {
        var next=StateStore.Copy(State);int deferred=DesktopFileChanges.FinishMany(Root,Store,next,ids);State=next;if(deferred>0)Log("desktop_drag_cleanup_deferred",new{count=deferred});RefreshDesktop();
    }
    public void Error(string operation, Exception ex)
    {
        Log("error", new { operation, type = ex.GetType().Name, detail = ex.ToString() });
        SetStatus(operation + "：" + ex.Message);
        System.Windows.MessageBox.Show(operation + "：" + ex.Message, "Magic 视界社区版", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Warning);
    }
    public void Open(string target)
    {
        try
        {
            if (!target.StartsWith("::", StringComparison.Ordinal) && !File.Exists(target) && !Directory.Exists(target)) throw new FileNotFoundException("项目已移动或删除，可从收纳盒移除引用。", target);
            if (target.StartsWith("::", StringComparison.Ordinal))
            {
                var start = new ProcessStartInfo("explorer.exe") { UseShellExecute = true };
                start.ArgumentList.Add("shell:" + target); Process.Start(start);
            }
            else Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
        }
        catch (Exception ex) { Error("无法打开", ex); }
    }
    public void Reveal(DesktopItem item)
    {
        if (item.IsVirtual) { Open(item.Target); return; }
        var directory = item.IsFolder ? item.Target : Path.GetDirectoryName(item.Target);
        if (directory is not null) Open(directory);
    }
    public void OpenFolder(string relative)
    {
        var path = Path.GetFullPath(Path.Combine(Root, relative));
        if (!path.StartsWith(Root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) && path != Root) throw new InvalidOperationException("目录超出项目。");
        Directory.CreateDirectory(path); Open(path);
    }
    public string Feedback(string description, IReadOnlyList<FeedbackImage>? images=null)
    {
        if (string.IsNullOrWhiteSpace(description)) throw new ArgumentException("请填写问题或建议。");
        if(description.Length>20000)throw new ArgumentException("反馈描述最多 20000 字。");
        var path = FeedbackStore.Save(Path.Combine(Root,"feedback"),new
        {
            createdAt = DateTimeOffset.Now, version = Version, status = "待评估", schedule = "待主审排期",
            description, runtime = Environment.Version.ToString(), os = Environment.OSVersion.ToString(),
            configuredRender = State.SoftwareRendering ? "SoftwareOnly" : "Default", actualRender = System.Windows.Media.RenderOptions.ProcessRenderMode.ToString(), screens = Screens,
            scope = "本地反馈，不会联网上传；只附带用户主动选择或粘贴的图片副本，不附带图片来源的绝对路径。"
        },images??[]);
        Log("feedback_saved", new { file = Path.GetFileName(Path.GetDirectoryName(path)), images=images?.Count??0 });
        return path;
    }
    public void Log(string name, object value)
    {
        try
        {
            var directory = Path.Combine(Root, "logs"); Directory.CreateDirectory(directory);
            File.AppendAllText(Path.Combine(directory, DateTime.Now.ToString("yyyy-MM-dd") + ".jsonl"),
                JsonSerializer.Serialize(new { at = DateTimeOffset.Now, name, value }) + Environment.NewLine);
        }
        catch { /* Diagnostic write failure must not block recovery or opening files. */ }
    }
}

// One row of the bar module catalog, as the settings page needs it: the id and the
// label the renderer draws, plus which of the two surfaces already carry it.
public sealed record BarModuleState(string Id,string Label,string Icon,bool Toolbar,bool Dock);
