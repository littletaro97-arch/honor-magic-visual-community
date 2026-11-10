using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;
namespace MagicCommunity;
// An on-demand WPF settings window, following the existing v0.3 native architecture.
public sealed class CommunitySettingsWindow : CommunityWindow
{
    private readonly AppController controller;
    private readonly CommunityPreferences preferences;
    private readonly StackPanel pane=new();
    private readonly StackPanel navigation=new();
    private bool dark;
    private readonly bool gallery;
    private Brush ink,muted,surface,back;
    private Border? sidebar;private NativePowerSubscription? powerSubscription;
    private string page="桌面";
    public CommunitySettingsWindow(AppController controller,bool? theme=null)
    {
        this.controller=controller;gallery=theme.HasValue;preferences=CommunityPreferences.Load(controller.Root);
        var palette=new NativeWpfTheme(theme);dark=palette.Dark;ink=palette.Ink;muted=palette.Muted;surface=palette.WindowCard;back=palette.MaterialBackground(false);palette.Apply(this);
        Title="";System.Windows.Automation.AutomationProperties.SetName(this,"magic 视界社区版设置");Width=880;Height=624;MinWidth=660;MinHeight=440;FontFamily=CommunityFont.Family;FontSize=12;Foreground=ink;Background=Brushes.Transparent;WindowStartupLocation=WindowStartupLocation.CenterScreen;
        var root=new DockPanel{Background=back};Content=root;var side=new Border{Width=190,Background=palette.WindowSidebar,BorderBrush=palette.Divider,BorderThickness=new(0,0,1,0),Padding=new(10,18,10,10)};DockPanel.SetDock(side,Dock.Left);root.Children.Add(side);sidebar=side;side.Child=navigation;
        foreach(var label in new[]{"桌面","外观","省电","账号","关于"}){var button=new Button{Content=label,Height=36,HorizontalContentAlignment=HorizontalAlignment.Left,Padding=new(16,0,0,0),Margin=new(0,2,0,2),BorderThickness=new(0)};var template=new ControlTemplate(typeof(Button));var shape=new FrameworkElementFactory(typeof(Border));shape.SetValue(Border.CornerRadiusProperty,new CornerRadius(NativeWpfTheme.ControlRadius));shape.SetBinding(Border.BackgroundProperty,new System.Windows.Data.Binding("Background"){RelativeSource=new RelativeSource(RelativeSourceMode.TemplatedParent)});var content=new FrameworkElementFactory(typeof(ContentPresenter));content.SetBinding(ContentPresenter.ContentProperty,new System.Windows.Data.Binding("Content"){RelativeSource=new RelativeSource(RelativeSourceMode.TemplatedParent)});content.SetValue(FrameworkElement.VerticalAlignmentProperty,VerticalAlignment.Center);content.SetValue(FrameworkElement.MarginProperty,new Thickness(16,0,0,0));shape.AppendChild(content);template.VisualTree=shape;button.Template=template;
            button.Click+=(_,_)=>Navigate(label);navigation.Children.Add(button);}
        var scroll=new ScrollViewer{VerticalScrollBarVisibility=ScrollBarVisibility.Visible,HorizontalScrollBarVisibility=ScrollBarVisibility.Disabled,Padding=new(30,26,26,26),Content=pane};
        scroll.Resources.Add(typeof(ScrollBar),SettingsScrollBarStyle());root.Children.Add(scroll);
        SourceInitialized+=(_,_)=>{powerSubscription=new(this,Render);HwndSource.FromHwnd(new WindowInteropHelper(this).Handle)?.AddHook(ThemeMessage);int value=dark?1:0;DwmSetWindowAttribute(new WindowInteropHelper(this).Handle,20,ref value,4);ApplyMaterial();};
        Activated+=(_,_)=>Render();controller.Changed+=Changed;Closed+=(_,_)=>{controller.Changed-=Changed;powerSubscription?.Dispose();};Render();
    }
    private IntPtr ThemeMessage(IntPtr h,int message,IntPtr w,IntPtr l,ref bool handled)
    {
        if(message==0x1a||message==0x218){var palette=new NativeWpfTheme();dark=palette.Dark;ink=palette.Ink;muted=palette.Muted;surface=palette.WindowCard;back=palette.MaterialBackground(false);palette.Apply(this);Background=Brushes.Transparent;((DockPanel)Content).Background=back;if(sidebar is not null)sidebar.Background=palette.WindowSidebar;int value=dark?1:0;DwmSetWindowAttribute(h,20,ref value,4);Render();}return IntPtr.Zero;
    }
    private void ApplyMaterial()
    {
        var handle=new WindowInteropHelper(this).Handle;bool glass=false;
        if(handle!=IntPtr.Zero){
            bool enabled=!SystemParameters.HighContrast&&!CommunityPreferences.Power().Saver&&!SystemParameters.IsRemoteSession&&((int?)Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize","EnableTransparency",1)!=0);
            int backdrop=enabled?3:1;var margins=new Margins{Left=enabled?-1:0,Right=enabled?-1:0,Top=enabled?-1:0,Bottom=enabled?-1:0};
            glass=enabled&&DwmSetWindowAttribute(handle,38,ref backdrop,4)==0&&DwmExtendFrameIntoClientArea(handle,ref margins)==0;
            if(HwndSource.FromHwnd(handle)?.CompositionTarget is {} target)target.BackgroundColor=Colors.Transparent;
        }
        var palette=new NativeWpfTheme(dark);back=palette.MaterialBackground(glass);Background=Brushes.Transparent;
        if(Content is DockPanel root)root.Background=back;
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]private struct Margins{public int Left,Right,Top,Bottom;}
    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd,ref Margins margins);
    private TextBlock Text(string s,double size,Brush? color=null)=>new(){Text=s,FontSize=size,Foreground=color??ink,VerticalAlignment=VerticalAlignment.Center,TextWrapping=TextWrapping.Wrap};
    // Same neutral 3-DIP rounded thumb as the native top-bar menus. Keep the
    // original 12-DIP slot, Track commands and keyboard/wheel semantics.
    internal static Style SettingsScrollBarStyle()=> (Style)System.Windows.Markup.XamlReader.Parse("""
        <Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
               xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TargetType="{x:Type ScrollBar}">
          <Setter Property="Width" Value="12"/><Setter Property="Focusable" Value="False"/>
          <Setter Property="Foreground" Value="{Binding Foreground, RelativeSource={RelativeSource AncestorType={x:Type Window}}}"/>
          <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="{x:Type ScrollBar}">
            <Track x:Name="PART_Track" IsDirectionReversed="True" Minimum="{TemplateBinding Minimum}"
                   Maximum="{TemplateBinding Maximum}" Value="{TemplateBinding Value}"
                   ViewportSize="{TemplateBinding ViewportSize}" Orientation="{TemplateBinding Orientation}">
              <Track.DecreaseRepeatButton><RepeatButton Command="ScrollBar.PageUpCommand" Focusable="False">
                <RepeatButton.Template><ControlTemplate><Border Background="Transparent"/></ControlTemplate></RepeatButton.Template>
              </RepeatButton></Track.DecreaseRepeatButton>
              <Track.Thumb><Thumb MinHeight="24" Focusable="False"><Thumb.Template><ControlTemplate TargetType="{x:Type Thumb}">
                <Grid Background="Transparent"><Border x:Name="Ink" Width="3" HorizontalAlignment="Center" CornerRadius="1.5"
                  Background="{Binding Foreground, RelativeSource={RelativeSource AncestorType={x:Type ScrollBar}}}" Opacity=".25"/></Grid>
                <ControlTemplate.Triggers>
                  <DataTrigger Binding="{Binding IsMouseOver,RelativeSource={RelativeSource AncestorType={x:Type ScrollBar}}}" Value="True"><Setter TargetName="Ink" Property="Width" Value="6"/><Setter TargetName="Ink" Property="CornerRadius" Value="3"/><Setter TargetName="Ink" Property="Opacity" Value=".43"/></DataTrigger>
                  <Trigger Property="IsDragging" Value="True"><Setter TargetName="Ink" Property="Width" Value="6"/><Setter TargetName="Ink" Property="CornerRadius" Value="3"/><Setter TargetName="Ink" Property="Opacity" Value=".43"/></Trigger>
                  <DataTrigger Binding="{Binding Source={x:Static SystemParameters.HighContrast}}" Value="True"><Setter TargetName="Ink" Property="Opacity" Value="1"/></DataTrigger>
                </ControlTemplate.Triggers>
              </ControlTemplate></Thumb.Template></Thumb></Track.Thumb>
              <Track.IncreaseRepeatButton><RepeatButton Command="ScrollBar.PageDownCommand" Focusable="False">
                <RepeatButton.Template><ControlTemplate><Border Background="Transparent"/></ControlTemplate></RepeatButton.Template>
              </RepeatButton></Track.IncreaseRepeatButton>
            </Track>
          </ControlTemplate></Setter.Value></Setter>
        </Style>
        """);
    private void Changed()=>Dispatcher.BeginInvoke(new Action(Render));
    private StackPanel Group(string label){pane.Children.Add(new TextBlock{Text=label,FontSize=10,Foreground=muted,Margin=new(2,18,0,7)});var rows=new StackPanel();pane.Children.Add(new Border{Background=surface,BorderBrush=new NativeWpfTheme(dark).Divider,BorderThickness=new(1),CornerRadius=new(NativeWpfTheme.PanelRadius),Child=rows,Padding=new(1)});return rows;}
    private void Row(StackPanel group,string title,string value="",string subtitle="",UIElement? control=null)
    {
        if(group.Children.Count>0)group.Children.Add(new Border{Height=1,Background=muted,Opacity=.15});
        var grid=new Grid{MinHeight=54,Margin=new(16,0,16,0)};grid.ColumnDefinitions.Add(new(){Width=new GridLength(1,GridUnitType.Star)});grid.ColumnDefinitions.Add(new(){Width=GridLength.Auto});
        var labels=new StackPanel{VerticalAlignment=VerticalAlignment.Center,Margin=new(0,9,14,9)};labels.Children.Add(Text(title,subtitle.Length>0?13:12));if(subtitle.Length>0)labels.Children.Add(new TextBlock{Text=subtitle,FontSize=11,Foreground=muted,Margin=new(0,4,0,0)});grid.Children.Add(labels);
        var right=control??Text(value,12,muted);Grid.SetColumn(right,1);grid.Children.Add(right);group.Children.Add(grid);
    }
    private ToggleButton Toggle(bool value,Action<bool>? set,bool enabled=true)
    {
        var t=new ToggleButton{IsChecked=value,Width=40,Height=23,IsEnabled=enabled&&!controller.Store.ReadOnly,VerticalAlignment=VerticalAlignment.Center,ToolTip=enabled?null:"需先打开「显示页码条」"};
        var template=new ControlTemplate(typeof(ToggleButton));var body=new FrameworkElementFactory(typeof(Border));body.Name="Track";body.SetValue(Border.CornerRadiusProperty,new CornerRadius(12));body.SetValue(Border.BackgroundProperty,muted);
        var thumb=new FrameworkElementFactory(typeof(Border));thumb.Name="Thumb";thumb.SetValue(FrameworkElement.WidthProperty,19d);thumb.SetValue(FrameworkElement.HeightProperty,19d);thumb.SetValue(Border.CornerRadiusProperty,new CornerRadius(10));thumb.SetValue(Border.BackgroundProperty,Brushes.White);thumb.SetValue(FrameworkElement.MarginProperty,new Thickness(2));thumb.SetValue(FrameworkElement.HorizontalAlignmentProperty,HorizontalAlignment.Left);body.AppendChild(thumb);template.VisualTree=body;
        var trigger=new Trigger{Property=ToggleButton.IsCheckedProperty,Value=true};trigger.Setters.Add(new Setter(Border.BackgroundProperty,SystemColors.HighlightBrush,"Track"));trigger.Setters.Add(new Setter(FrameworkElement.HorizontalAlignmentProperty,HorizontalAlignment.Right,"Thumb"));template.Triggers.Add(trigger);t.Template=template;
        if(set is not null)t.Click+=(_,_)=>{try{set(t.IsChecked==true);preferences.Save(controller);Render();}catch(Exception ex){MessageBox.Show(this,ex.Message,"设置未保存");Render();}};return t;
    }
    private bool keyboardBusy;
    private ToggleButton KeyboardToggle(bool forChord)
    {
        var button=Toggle(forChord?controller.KeyboardPreferences.WinA:controller.KeyboardPreferences.Win,null);
        button.IsEnabled=!gallery&&!keyboardBusy&&!controller.Store.ReadOnly&&controller.ApplyKeyboardPreferences is not null&&!ShortcutPreferences.IsReadOnly(controller.Root);
        button.Click+=async(_,_)=>{
            if(keyboardBusy||controller.ApplyKeyboardPreferences is not {} apply){Render();return;}
            keyboardBusy=true;
            var current=controller.KeyboardPreferences;
            var chosen=button.IsChecked==true;
            Render();
            try {await apply(forChord?current with{WinA=chosen}:current with{Win=chosen});}
            catch(Exception e) when(e is not OutOfMemoryException){MessageBox.Show(this,e.Message,"快捷键设置未保存");}
            finally {keyboardBusy=false;Render();}
        };
        return button;
    }
    private void Link(StackPanel group,string label,Action action)
    {
        if(group.Children.Count>0)group.Children.Add(new Border{Height=1,Background=muted,Opacity=.15});
        var content=new Grid();content.ColumnDefinitions.Add(new(){Width=new GridLength(1,GridUnitType.Star)});content.ColumnDefinitions.Add(new(){Width=GridLength.Auto});
        var title=Text(label,12);title.Margin=new(0,9,14,9);content.Children.Add(title);
        var arrow=Text("↗",12,muted);Grid.SetColumn(arrow,1);content.Children.Add(arrow);
        var button=new Button{Content=content,MinHeight=54,Margin=new(0),Padding=new(16,0,16,0),HorizontalContentAlignment=HorizontalAlignment.Stretch,Background=Brushes.Transparent,Foreground=ink,BorderThickness=new(0)};
        System.Windows.Automation.AutomationProperties.SetName(button,label);
        button.Click+=(_,_)=>{try{action();}catch(Exception ex){MessageBox.Show(this,ex.Message,"暂时无法打开");}};group.Children.Add(button);
    }
    internal void Navigate(string destination)
    {
        if(!navigation.Children.OfType<Button>().Any(button=>(string)button.Content==destination))throw new ArgumentOutOfRangeException(nameof(destination));
        page=destination;Render();
    }
    private static void Open(string target)=>Process.Start(new ProcessStartInfo(target){UseShellExecute=true});
    private void Render()
    {
        ApplyMaterial();
        pane.Children.Clear();int i=0;foreach(Button b in navigation.Children){bool selected=(string)b.Content==page;b.Background=selected?new NativeWpfTheme(dark).Interaction(true):Brushes.Transparent;b.Foreground=selected?ink:muted;b.FontWeight=selected?FontWeights.SemiBold:FontWeights.Normal;i++;}
        pane.Children.Add(new TextBlock{Text=page,FontSize=22,FontWeight=FontWeights.SemiBold,Foreground=ink,Margin=new(0,0,0,4)});
        if(page=="桌面"){
            var board=controller.State.Boards.FirstOrDefault(b=>controller.Screens.Any(s=>s.Id==b.Id));var g=Group("布局");Row(g,"网格",board?.Grid is {} grid?$"{grid.Rows} 行 × {grid.Columns} 列":"尚未接管桌面");Row(g,"容量",board?.Grid is {} grid2?$"{grid2.Placements.Values.Sum(r=>r.W*r.H)} / {grid2.Columns*grid2.Rows}":"—");
            var pages=Group("分页");Row(pages,"显示页码条",subtitle:"显示在顶栏",control:Toggle(preferences.ShowPages,v=>preferences.ShowPages=v));Row(pages,"闲置后淡出",subtitle:"2 秒后淡出，靠近恢复",control:Toggle(preferences.FadePages,v=>preferences.FadePages=v,preferences.ShowPages));if(!preferences.ShowPages){pages.Children[^1].Opacity=.42;pages.Children[^1].IsEnabled=false;ToolTipService.SetToolTip(pages.Children[^1],"需先打开「显示页码条」");ToolTipService.SetShowOnDisabled(pages.Children[^1],true);}
            var wallpaper=Group("壁纸");Link(wallpaper,"副屏壁纸保护",()=>new SecondaryWallpaperWindow(controller){Owner=this}.Show());
            var keyboard=Group("快捷键");
            Row(keyboard,"单按 Win 打开社区版开始菜单",subtitle:"关闭时打开 Windows 开始菜单",control:KeyboardToggle(false));
            Row(keyboard,"Win+A 打开社区版控制中心",subtitle:"关闭时打开 Windows 快速设置",control:KeyboardToggle(true));
            var window=Group("窗口");Row(window,"窗口动画",control:Toggle(preferences.WindowAnimations,v=>preferences.WindowAnimations=v));Row(window,"大文件夹动效",control:Toggle(preferences.FolderAnimations,v=>preferences.FolderAnimations=v));Row(window,"Dock 消散动画",subtitle:"自动隐藏时原地消散，跟随窗口动画及节电设置",control:Toggle(preferences.DockDissolve,v=>preferences.DockDissolve=v));
        }else if(page=="外观"){
            var theme=Group("显示");Row(theme,"深色模式",subtitle:"与 Windows 及控制中心的显示与亮度同步",control:Toggle(NativeWpfTheme.Personalization("AppsUseLightTheme",1)==0,v=>NativeAppearanceSettings.SetDark(v)));
            Row(theme,"透明顶栏",subtitle:"无全屏应用时移除底色；浅色系统用白字，深色系统用黑字",control:Toggle(preferences.TransparentToolbar,v=>preferences.TransparentToolbar=v));
            if(preferences.TransparentToolbar)
                Row(theme,"过渡遮罩",subtitle:"顶部轻度着色并向下渐隐，随系统深浅色切换",control:Toggle(preferences.TransparentToolbarMask,v=>preferences.TransparentToolbarMask=v));
            var names=Group("图标名称显示");
            Row(names,"大文件夹图标名称显示",subtitle:"开启后，收起的大文件夹也显示内部图标名称",control:Toggle(preferences.CollapsedFolderIconNames,v=>preferences.CollapsedFolderIconNames=v));
            Row(names,"桌面图标名称显示",subtitle:"关闭后，仅鼠标悬停时显示桌面图标名称",control:Toggle(preferences.DesktopIconNames,v=>preferences.DesktopIconNames=v));
            var icons=Group("荣耀图标");
            Row(icons,"图标套装",subtitle:"应用于桌面、顶部 bar 和 Dock，选择后立即生效");
            icons.Children.Add(HonorIconSchemeCards.Create(dark,preferences.IconScheme,!controller.Store.ReadOnly,selected=>{
                string previous=preferences.IconScheme;try{preferences.IconScheme=selected;preferences.Save(controller);}catch(Exception ex){preferences.IconScheme=previous;MessageBox.Show(this,ex.Message,"设置未保存");}Render();
            }));
            Row(Group("适配规则"),"自动匹配","",subtitle:"优先匹配荣耀库；未收录的应用保留真实图标。");
            Row(Group("素材"),"两套官方素材","",subtitle:"单套缺少对应素材时使用同库可用版本；保留自定义图标和托盘状态。");
        }else if(page=="省电"){
            var power=CommunityPreferences.Power();var g=Group("当前");Row(g,"电源模式",controller.PowerMode+" · "+(power.Plugged?"已接通电源":"使用电池"));Row(g,"低功耗渲染",power.Saver?"已生效":"未生效");var behavior=Group("自动跟随 Windows");Row(behavior,"节电时降低渲染","自动",subtitle:"Windows 节电会限制原生毛玻璃，关闭节电后自动恢复。");Row(behavior,"空闲时整理自有进程内存",subtitle:"达到阈值且不在节电模式时裁剪社区版工作集；私有提交内存保持不变",control:Toggle(preferences.WorkingSetTrim,v=>preferences.WorkingSetTrim=v));var after=Group("节电时");Row(after,"背景材质",dark?"深黑实色":"浅白实色");Row(after,"动画","仅保留操作反馈");Row(after,"工作集整理","自动暂停",subtitle:"避免节电状态下产生额外磁盘回读");
        }else if(page=="账号"){
            var g=Group("当前账号");string user=gallery?"示例账号":Environment.UserName;var profile=new StackPanel{Orientation=Orientation.Horizontal,Margin=new(16,18,16,18)};profile.Children.Add(new Border{Width=44,Height=44,CornerRadius=new(22),Background=SystemColors.HighlightBrush,Child=new TextBlock{Text=System.Globalization.StringInfo.GetNextTextElement(user),Foreground=SystemColors.HighlightTextBrush,FontSize=20,HorizontalAlignment=HorizontalAlignment.Center,VerticalAlignment=VerticalAlignment.Center}});var labels=new StackPanel{VerticalAlignment=VerticalAlignment.Center,Margin=new(14,0,0,0)};labels.Children.Add(Text(user,12));labels.Children.Add(new TextBlock{Text=gallery?"荣耀账号 · 示例状态":HonorPcManager.SignedIn switch{true=>"荣耀账号 · 已登录",false=>"荣耀账号 · 未登录",_=>"荣耀账号 · 状态未知"},Foreground=muted,FontSize=11,Margin=new(0,5,0,0)});profile.Children.Add(labels);g.Children.Add(profile);Link(g,"账号中心",HonorPcManager.OpenAccount);
        }else{
            var g=Group("了解 magic 视界");Link(g,"magic 视界官方版本",()=>Open("https://www.honor.com/cn/tech/magic-vision/"));Link(g,"magic 视界社区版介绍",()=>Open(new Uri(Path.Combine(AppContext.BaseDirectory,"community-intro.html")).AbsoluteUri+"?version="+Uri.EscapeDataString(AppController.Version)));Row(Group("版本"),"当前版本",AppController.Version);var links=Group("支持");Link(links,"检查更新",()=>CommunityUpdateWindow.Open(controller.Root));Link(links,"建议和反馈",()=>{var feedback=new FeedbackWindow(controller,this);feedback.Show();});Link(links,"第三方组件说明",()=>Open(Path.Combine(AppContext.BaseDirectory,"community-licenses.html")));
        }
    }
    internal void ExportPage(string name,string path,int exportHeight=580)
    {
        page=name;Render();var root=(FrameworkElement)Content;root.Measure(new Size(880,exportHeight));root.Arrange(new Rect(0,0,880,exportHeight));root.UpdateLayout();var bitmap=new System.Windows.Media.Imaging.RenderTargetBitmap(880,exportHeight,96,96,PixelFormats.Pbgra32);bitmap.Render(root);var png=new System.Windows.Media.Imaging.PngBitmapEncoder();png.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));using var stream=File.Create(path);png.Save(stream);
    }
    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]private static extern int DwmSetWindowAttribute(IntPtr h,int key,ref int value,int bytes);
}
