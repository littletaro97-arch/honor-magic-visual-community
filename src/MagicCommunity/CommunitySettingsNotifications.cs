using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;

namespace MagicCommunity;
public sealed partial class CommunitySettingsWindow
{
    private string? notificationEditingMonitor;
    private void RenderNotificationPlacement()
    {
        var prefs = preferences.NotificationPlacement;
        var screens = controller.Screens.OrderByDescending(s => s.Primary).ToArray();
        var group = Group("位置");
        Row(group, "主副屏自动切换", subtitle: "跟随当前交互窗口，使用对应屏幕保存的位置", control: Toggle(prefs.AutoSwitch, v => prefs.AutoSwitch = v,rebuild:true));
        if (screens.Length == 0) { Row(group,"暂未检测到显示器"); return; }
        var screen = screens.FirstOrDefault(s => s.Id == notificationEditingMonitor) ?? screens[0];
        notificationEditingMonitor = screen.Id;
        Button choose;
        if(screens.Length==2){
            choose=PositionButton(screen.Primary?"前往副屏":"返回主屏");
            choose.Click+=(_,_)=>{notificationEditingMonitor=screens.First(s=>s.Id!=screen.Id).Id;Render();};
        }else choose=ScreenChoice(screens,screen,value=>{notificationEditingMonitor=value.Id;Render();});
        Row(group,"调整屏幕", subtitle:screen.Label, control:choose);
        Row(group,"分别记住每块屏幕的位置",subtitle:"副屏断开时回到主屏，重新连接后恢复");
        if (!prefs.AutoSwitch)
        {
            var fixedList=ScreenChoice(screens,screens.FirstOrDefault(s=>s.Id==prefs.FixedMonitor)??screens[0],
                value=>SaveNotificationPosition(()=>prefs.FixedMonitor=value.Primary?"primary":value.Id));
            Row(group,"固定弹出屏幕",subtitle:prefs.FixedMonitor!="primary"&&!screens.Any(s=>s.Id==prefs.FixedMonitor)?"已保存的副屏未连接，暂时在主屏显示":"自动切换关闭时使用此屏幕",control:fixedList);
        }
        var position=prefs.For(screen.Id);
        bool portrait=screen.PixelHeight>screen.PixelWidth;
        var preview=new Grid { Height=portrait?300:222, MaxWidth=portrait?270:540, Margin=new(16,12,16,10), HorizontalAlignment=HorizontalAlignment.Stretch };
        preview.RowDefinitions.Add(new(){Height=GridLength.Auto});preview.RowDefinitions.Add(new(){Height=new(1,GridUnitType.Star)});preview.RowDefinitions.Add(new(){Height=GridLength.Auto});
        for(int c=0;c<3;c++)preview.ColumnDefinitions.Add(new(){Width=new(1,GridUnitType.Star)});
        var frame=new Border { CornerRadius=new(18), Background=dark?new SolidColorBrush(Color.FromRgb(49,52,58)):new SolidColorBrush(Color.FromRgb(235,238,244)), BorderBrush=muted,BorderThickness=new(1),Opacity=.6};
        Grid.SetColumnSpan(frame,3);Grid.SetRowSpan(frame,3);preview.Children.Add(frame);
        string[] anchors=["top-left","top-center","top-right","bottom-left","bottom-center","bottom-right"];
        string[] labels=["左上","中上","右上","左下","中下","右下"];
        for(int i=0;i<6;i++)
        {
            string anchor=anchors[i]; var button=new Button { Content=labels[i], MinHeight=36,Margin=new(5,12,5,12),Padding=new(6,4,6,4),Background=position.Anchor==anchor?SystemColors.HighlightBrush:surface,Foreground=position.Anchor==anchor?SystemColors.HighlightTextBrush:ink,IsEnabled=!controller.Store.ReadOnly };
            Grid.SetColumn(button,i%3);Grid.SetRow(button,i<3?0:2);
            button.Click+=(_,_)=>SaveNotificationPosition(()=>prefs.Set(screen.Id,new(){Anchor=anchor}));preview.Children.Add(button);
        }
        var center=new TextBlock { Text=(screen.Primary?"主屏幕":"副屏幕")+" · "+(portrait?"竖屏":"横屏")+"\n"+labels[Array.IndexOf(anchors,position.Anchor)]+" · 偏移 "+position.X+", "+position.Y+" px",TextAlignment=TextAlignment.Center,TextWrapping=TextWrapping.Wrap,FontSize=13,Foreground=ink,VerticalAlignment=VerticalAlignment.Center,Margin=new(8)};
        Grid.SetRow(center,1);Grid.SetColumnSpan(center,3);preview.Children.Add(center);group.Children.Add(preview);
        var fine=new Grid { Width=150,Height=120,Margin=new(0,8,0,8) };
        for(int i=0;i<3;i++){fine.RowDefinitions.Add(new());fine.ColumnDefinitions.Add(new());}
        void Adjust(string label,int dx,int dy,int column,int row,bool reset=false)
        {
            var button=new Button {Content=label,MinWidth=44,Height=34,Margin=new(3),Background=surface,Foreground=ink,IsEnabled=!controller.Store.ReadOnly};
            button.Click+=(_,_)=>SaveNotificationPosition(()=>prefs.Set(screen.Id,new(){Anchor=position.Anchor,X=reset?0:position.X+dx,Y=reset?0:position.Y+dy}));Grid.SetColumn(button,column);Grid.SetRow(button,row);fine.Children.Add(button);
        }
        Adjust("←",-4,0,0,1);Adjust("↑",0,-4,1,0);Adjust("归零",0,0,1,1,true);Adjust("↓",0,4,1,2);Adjust("→",4,0,2,1);
        Row(group,"微调位置",subtitle:"每次移动 4 px，自动限制在可用范围",control:fine);
        Row(Group("统一应用"),"通知、音量、亮度与输入法提示",subtitle:"音量与亮度仍支持拖动滑块及鼠标滚轮调节");
        Link(Group("恢复"),"恢复所有通知位置",()=>SaveNotificationPosition(()=>{preferences.NotificationPlacement=new();notificationEditingMonitor=null;}));
    }
    private Button PositionButton(string label)=>new(){Content=label,MinWidth=110,MaxWidth=220,MinHeight=36,Padding=new(12,5,12,5),Margin=new(0,6,0,6),Background=surface,Foreground=ink};
    private Button ScreenChoice(Display[] screens,Display selected,Action<Display> choose)
    {
        var button=PositionButton(selected.Label+" ▾");
        button.IsEnabled=screens.Length>1&&!controller.Store.ReadOnly;
        button.Click+=(_,_)=>{
            var menu=new ContextMenu{Background=surface,Foreground=ink,PlacementTarget=button,Placement=PlacementMode.Bottom};
            foreach(var screen in screens){var item=new MenuItem{Header=screen.Label,Foreground=ink,Background=surface,IsCheckable=true,IsChecked=screen.Id==selected.Id};item.Click+=(_,_)=>choose(screen);menu.Items.Add(item);}
            menu.IsOpen=true;
        };
        return button;
    }
    internal void ExportNotificationState(string path,bool secondary,bool automatic)
    {
        notificationEditingMonitor=controller.Screens.FirstOrDefault(s=>s.Primary!=secondary)?.Id;
        preferences.NotificationPlacement.AutoSwitch=automatic;
        if(secondary&&notificationEditingMonitor is not null)preferences.NotificationPlacement.FixedMonitor=notificationEditingMonitor;
        ExportPage("通知与提示",path,1080);
    }
    private void SaveNotificationPosition(Action change)
    {
        if(controller.Store.ReadOnly)return;
        try {change();preferences.Save(controller);}catch(Exception error){MessageBox.Show(this,error.Message,"通知位置未保存");}
        Render();
    }
}
