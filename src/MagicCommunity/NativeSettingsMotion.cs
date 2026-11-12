using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace MagicCommunity;

// Finite WPF compositor clocks only: no idle polling, and rapid reversals start
// from the displayed position. Controls retain native keyboard/UIA semantics.
internal static class NativeSettingsMotion
{
    internal const int DurationMs=180;
    private static DoubleAnimation Transition(double from,double to)=>new(from,to,TimeSpan.FromMilliseconds(DurationMs))
        { EasingFunction=new CubicEase{EasingMode=EasingMode.EaseOut},FillBehavior=FillBehavior.Stop };

    internal static ToggleButton Toggle(bool value,Brush off,Func<bool> animate)
    {
        var button=new ToggleButton{IsChecked=value,Width=40,Height=23,VerticalAlignment=VerticalAlignment.Center};
        var template=new ControlTemplate(typeof(ToggleButton));
        var track=new FrameworkElementFactory(typeof(Grid));track.SetValue(UIElement.SnapsToDevicePixelsProperty,true);
        FrameworkElementFactory Shape(string name,Brush brush){var b=new FrameworkElementFactory(typeof(Border),name);b.SetValue(Border.CornerRadiusProperty,new CornerRadius(12));b.SetValue(Border.BackgroundProperty,brush);return b;}
        track.AppendChild(Shape("OffTrack",off));var on=Shape("OnTrack",SystemColors.HighlightBrush);on.SetValue(UIElement.OpacityProperty,value?1d:0d);track.AppendChild(on);
        var thumb=Shape("Thumb",Brushes.White);thumb.SetValue(FrameworkElement.WidthProperty,19d);thumb.SetValue(FrameworkElement.HeightProperty,19d);thumb.SetValue(FrameworkElement.MarginProperty,new Thickness(2));thumb.SetValue(FrameworkElement.HorizontalAlignmentProperty,HorizontalAlignment.Left);
        thumb.SetValue(UIElement.RenderTransformProperty,new TranslateTransform(value?17:0,0));track.AppendChild(thumb);template.VisualTree=track;button.Template=template;
        int revision=0;
        void Sync(bool motion){int current=++revision;button.ApplyTemplate();if(template.FindName("Thumb",button) is not Border knob||template.FindName("OnTrack",button) is not Border active)return;
            var transform=knob.RenderTransform as TranslateTransform;
            if(transform is null||transform.IsFrozen){transform=new TranslateTransform(button.IsChecked==true?17:0,0);knob.RenderTransform=transform;}
            double x=transform.X,opacity=active.Opacity,target=button.IsChecked==true?17:0;
            transform.BeginAnimation(TranslateTransform.XProperty,null);active.BeginAnimation(UIElement.OpacityProperty,null);transform.X=target;active.Opacity=target>0?1:0;
            if(motion&&button.IsLoaded&&animate()){var movement=Transition(x,target);movement.Completed+=(_,_)=>{if(current!=revision)return;transform.BeginAnimation(TranslateTransform.XProperty,null);active.BeginAnimation(UIElement.OpacityProperty,null);};transform.BeginAnimation(TranslateTransform.XProperty,movement);active.BeginAnimation(UIElement.OpacityProperty,Transition(opacity,active.Opacity));}
        }
        button.Loaded+=(_,_)=>Sync(false);button.Unloaded+=(_,_)=>Sync(false);
        button.Checked+=(_,_)=>Sync(true);button.Unchecked+=(_,_)=>Sync(true);
        return button;
    }

    internal sealed class Reveal
    {
        internal Border Host{get;}=new(){ClipToBounds=true};
        private int revision;
        internal Reveal(UIElement child,bool visible){Host.Child=child;Host.Height=visible?double.NaN:0;Host.Opacity=visible?1:0;Host.Visibility=visible?Visibility.Visible:Visibility.Collapsed;}
        internal void SetVisible(bool visible,bool animate)
        {
            int current=++revision;double from=Host.Visibility==Visibility.Visible?Host.ActualHeight:0,alpha=Host.Opacity;
            Host.BeginAnimation(FrameworkElement.HeightProperty,null);Host.BeginAnimation(UIElement.OpacityProperty,null);
            Host.Visibility=Visibility.Visible;Host.IsHitTestVisible=visible;
            double width=Host.ActualWidth;if(width<=0&&Host.Parent is FrameworkElement parent)width=parent.ActualWidth;
            Host.Child.Measure(new Size(Math.Max(1,width),double.PositiveInfinity));double to=visible?Host.Child.DesiredSize.Height:0;
            Host.Height=to;Host.Opacity=visible?1:0;
            if(!animate||!Host.IsLoaded){Host.Height=visible?double.NaN:0;Host.Visibility=visible?Visibility.Visible:Visibility.Collapsed;return;}
            var height=Transition(from,to);height.Completed+=(_,_)=>{if(current!=revision)return;Host.BeginAnimation(FrameworkElement.HeightProperty,null);Host.BeginAnimation(UIElement.OpacityProperty,null);Host.Height=visible?double.NaN:0;Host.Visibility=visible?Visibility.Visible:Visibility.Collapsed;};
            Host.BeginAnimation(FrameworkElement.HeightProperty,height);Host.BeginAnimation(UIElement.OpacityProperty,Transition(alpha,visible?1:0));
        }
    }
}
