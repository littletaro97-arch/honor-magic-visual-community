using System.Diagnostics;
using System.Windows.Interop;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
namespace MagicCommunity;

internal enum CommunityUpdateState{Checking,Unconfigured,NoRelease,Latest,Available,Failed}
internal sealed record CommunityRelease(string Version,string Notes,Uri Page,Uri? Download=null);
internal sealed record CommunityUpdateResult(CommunityUpdateState State,CommunityRelease? Release=null,string? Detail=null);
internal interface ICommunityUpdateSource{Task<CommunityUpdateResult> Check(CancellationToken cancellation);}
internal sealed class UnconfiguredUpdateSource:ICommunityUpdateSource{
    public Task<CommunityUpdateResult> Check(CancellationToken cancellation){cancellation.ThrowIfCancellationRequested();return Task.FromResult(new CommunityUpdateResult(CommunityUpdateState.Unconfigured,Detail:"更新来源暂未接入，计划使用 GitHub Releases。"));}
}
internal sealed class CommunityUpdateWindow:CommunityWindow
{
    private static WeakReference<CommunityUpdateWindow>? current;
    private readonly ICommunityUpdateSource source;
    private readonly CancellationTokenSource lifetime=new();
    private NativeWpfTheme theme;
    private readonly bool? previewTheme;private NativePowerSubscription? power;private HwndSource? hwndSource;
    private readonly Border chrome=new();
    private readonly TextBlock status=new(),detail=new(),version=new();
    private readonly StackPanel notes=new();
    private readonly Border releasePanel=new();
    private readonly Button button=new(),releaseLink=new();
    private CommunityUpdateResult? result;
    private bool checking;
    internal static void Open()=>Open(null);
    internal static void Open(string? root){
        if(current?.TryGetTarget(out var window)==true&&window.IsLoaded){window.Activate();return;}
        window=new();current=new(window);window.Show();
    }
    internal CommunityUpdateWindow(ICommunityUpdateSource? source=null,bool? dark=null){
        this.source=source??new GitHubCommunityUpdateSource();previewTheme=dark;theme=new(dark);theme.Apply(this);
        Title="检查更新";Width=480;Height=520;MinWidth=380;MinHeight=420;ResizeMode=ResizeMode.CanResize;WindowStartupLocation=WindowStartupLocation.CenterScreen;
        Background=Brushes.Transparent;
        FontFamily=CommunityFont.Family;FontSize=13;
        var root=new DockPanel{Margin=new(24,12,24,24)};chrome.Child=root;chrome.CornerRadius=new(NativeWpfTheme.FloatingRadius);Content=chrome;
        var footer=new StackPanel{HorizontalAlignment=HorizontalAlignment.Center,Margin=new(0,14,0,0)};DockPanel.SetDock(footer,Dock.Bottom);root.Children.Add(footer);
        button.Content="检查更新";button.Width=210;button.Height=40;button.Foreground=Brushes.White;button.Background=SystemColors.HighlightBrush;button.BorderThickness=new(0);button.Click+=async(_,_)=>{if(result?.State==CommunityUpdateState.Available)OpenRelease();else await Check();};footer.Children.Add(button);
        releaseLink.Content="GitHub 发布页 ↗";releaseLink.Height=28;releaseLink.Margin=new(0,6,0,0);releaseLink.Background=Brushes.Transparent;releaseLink.BorderThickness=new(0);releaseLink.Foreground=theme.Ink;releaseLink.Click+=(_,_)=>OpenPage(GitHubCommunityUpdateSource.ReleasesPage);footer.Children.Add(releaseLink);
        var body=new Grid();body.RowDefinitions.Add(new(){Height=GridLength.Auto});body.RowDefinitions.Add(new(){Height=new(1,GridUnitType.Star)});root.Children.Add(body);
        var heading=new StackPanel{MaxWidth=400,Margin=new(0,35,0,0),HorizontalAlignment=HorizontalAlignment.Center};body.Children.Add(heading);
        var image=new BitmapImage();image.BeginInit();image.CacheOption=BitmapCacheOption.OnLoad;image.UriSource=new("pack://application:,,,/MagicCommunity;component/UpdateIcon.png");image.DecodePixelWidth=180;image.EndInit();image.Freeze();heading.Children.Add(new Image{Source=image,Width=90,Height=90,Stretch=Stretch.Uniform,Margin=new(0,0,0,24)});
        status.FontSize=20;status.TextAlignment=TextAlignment.Center;status.Foreground=theme.Ink;status.TextWrapping=TextWrapping.Wrap;heading.Children.Add(status);
        version.Text="当前版本："+AppController.Version;version.FontSize=14;version.Foreground=theme.Muted;version.HorizontalAlignment=HorizontalAlignment.Center;version.Margin=new(0,10,0,0);heading.Children.Add(version);
        detail.Foreground=theme.Muted;detail.TextAlignment=TextAlignment.Center;detail.TextWrapping=TextWrapping.Wrap;detail.Margin=new(0,14,0,14);heading.Children.Add(detail);
        Grid.SetRow(releasePanel,1);releasePanel.CornerRadius=new(8);releasePanel.Background=theme.Surface;releasePanel.Padding=new(12);releasePanel.Child=new ScrollViewer{Content=notes,VerticalScrollBarVisibility=ScrollBarVisibility.Auto};body.Children.Add(releasePanel);
        SourceInitialized+=(_,_)=>{hwndSource=HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);hwndSource?.AddHook(MaterialMessage);power=new(this,ApplyMaterial);ApplyMaterial();};
        Loaded+=async(_,_)=>await Check();Closed+=(_,_)=>{power?.Dispose();hwndSource?.RemoveHook(MaterialMessage);hwndSource=null;lifetime.Cancel();lifetime.Dispose();notes.Children.Clear();};
        Paint(new(CommunityUpdateState.Checking));ApplyMaterial();
    }
    private IntPtr MaterialMessage(IntPtr h,int m,IntPtr w,IntPtr l,ref bool handled){if(m is 0x1a or 0x218 or 0x31a)ApplyMaterial();return IntPtr.Zero;}
    private void ApplyMaterial(){theme=new(previewTheme);NativeWindowMaterial.Apply(this,chrome,theme);status.Foreground=releaseLink.Foreground=theme.Ink;detail.Foreground=theme.Muted;version.Foreground=theme.Muted;releasePanel.Background=theme.WindowCard;if(result is {} current)Paint(current);}
    internal static void ValidateRelease(CommunityRelease release){
        static bool Github(Uri uri)=>uri.IsAbsoluteUri&&uri.Scheme==Uri.UriSchemeHttps&&uri.Host.Equals("github.com",StringComparison.OrdinalIgnoreCase)&&string.IsNullOrEmpty(uri.UserInfo);
        if(string.IsNullOrWhiteSpace(release.Version)||release.Version.Length>80||release.Notes.Length>65536||!Github(release.Page)||release.Download is {} download&&!Github(download))throw new InvalidDataException("更新信息或 GitHub 链接无效。");
    }
    private async Task Check(){
        if(checking)return;checking=true;Paint(new(CommunityUpdateState.Checking));
        try{var next=await source.Check(lifetime.Token);if(next.State==CommunityUpdateState.Available){if(next.Release is null)throw new InvalidDataException("更新信息缺少版本。");ValidateRelease(next.Release);}if(!lifetime.IsCancellationRequested)Paint(next);}
        catch(OperationCanceledException)when(lifetime.IsCancellationRequested){}
        catch(Exception error){if(!lifetime.IsCancellationRequested)Paint(new(CommunityUpdateState.Failed,Detail:error.Message));}
        finally{checking=false;}
    }
    internal void Paint(CommunityUpdateResult next){
        result=next;status.Text=next.State switch{CommunityUpdateState.Checking=>"正在检查更新…",CommunityUpdateState.NoRelease=>"暂无已发布版本",CommunityUpdateState.Latest=>"已是最新版本",CommunityUpdateState.Available=>"发现新版本 "+next.Release?.Version,CommunityUpdateState.Failed=>"暂时无法检查更新",_=>"更新来源尚未接入"};
        detail.Text=next.Detail??"";button.IsEnabled=next.State!=CommunityUpdateState.Checking;button.Content=next.State==CommunityUpdateState.Available?(next.Release?.Download is null?"查看新版本":"下载安装包"):"检查更新";
        releasePanel.Visibility=next.State==CommunityUpdateState.Available?Visibility.Visible:Visibility.Collapsed;notes.Children.Clear();
        if(next.Release is {} release){notes.Children.Add(new TextBlock{Text="更新内容",FontWeight=FontWeights.SemiBold,Margin=new(0,0,0,8),Foreground=theme.Ink});notes.Children.Add(new TextBlock{Text=release.Notes,TextWrapping=TextWrapping.Wrap,Foreground=theme.Ink});}
        System.Windows.Automation.AutomationProperties.SetName(status,status.Text);
    }
    private void OpenRelease(){if(result?.Release is not {} release)return;ValidateRelease(release);OpenPage(release.Download??release.Page);}
    private void OpenPage(Uri page){try{Process.Start(new ProcessStartInfo(page.AbsoluteUri){UseShellExecute=true})?.Dispose();}catch(Exception error){Paint(new(CommunityUpdateState.Failed,Detail:"无法打开发布页："+error.Message));}}
    internal void Export(string path){chrome.Measure(new Size(Width,Height));chrome.Arrange(new Rect(0,0,Width,Height));chrome.UpdateLayout();var bitmap=new RenderTargetBitmap((int)Width,(int)Height,96,96,PixelFormats.Pbgra32);bitmap.Render(chrome);var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));using var file=File.Create(path);png.Save(file);}
}
