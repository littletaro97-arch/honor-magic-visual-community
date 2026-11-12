using System.Windows;
using System.Windows.Controls.Primitives;

namespace MagicCommunity;

public sealed partial class CommunitySettingsWindow
{
    private CommunityStartupSettings.Status startupState=new(false,false,"");
    private bool startupBusy;
    private void RefreshStartup()
    {
        // Read on open/activation/save, never on the desktop update or timer path.
        if(!startupBusy)startupState=gallery?new(false,false,"效果预览不会更改开机启动。"):CommunityStartupSettings.ReadCurrent(controller.Root);
    }
    private void RenderStartup()
    {
        var group=Group("启动");
        var button=Toggle(startupState.Enabled,null);
        button.IsEnabled=startupState.CanChange&&!gallery&&!startupBusy&&!controller.Store.ReadOnly;
        button.ToolTip=button.IsEnabled?null:startupState.Detail;
        System.Windows.Automation.AutomationProperties.SetName(button,"开机自启");
        button.Click+=async(_,_)=>{
            if(startupBusy||!button.IsEnabled)return;
            bool enabled=button.IsChecked==true;startupBusy=true;button.IsEnabled=false;
            motionUntil=Environment.TickCount64+(MotionAllowed?NativeSettingsMotion.DurationMs+30:0);
            try {
                await Task.Run(()=>CommunityStartupSettings.SetCurrent(controller.Root,enabled));
                controller.Log("startup_preference_saved",new{enabled,currentAccountOnly=true});
            }
            catch(Exception e)when(e is not OutOfMemoryException){
                controller.Log("startup_preference_failed",new{error=e.Message});
                if(IsVisible)MessageBox.Show(this,e.Message,"开机自启未保存");
            }
            finally{startupBusy=false;RefreshStartup();if(IsVisible)RequestRender();}
        };
        Row(group,"开机自启",subtitle:startupState.Detail,control:button);
        Link(group,"Windows 启动应用设置",()=>Open("ms-settings:startupapps"));
    }
}
