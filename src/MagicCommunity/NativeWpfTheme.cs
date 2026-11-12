using Microsoft.Win32;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Media;
namespace MagicCommunity;
// Same neutral ramps and Windows accent as native-appearance.h, not sampled prototype colors.
internal sealed class NativeWpfTheme
{
    internal const double ControlRadius=6, PanelRadius=10, FloatingRadius=16;
    public bool Dark{get;}
    public Brush Background=>Gray(50);
    public Brush Surface=>Gray(Dark?75:25);
    public Brush Sidebar=>Gray(Dark?25:75);
    public Brush Ink=>Gray(900);
    public Brush Muted=>Gray(500);
    internal static int Personalization(string name,int fallback)
    {
        try{return Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",name,fallback) is int value?value:fallback;}
        catch(Exception error)when(error is System.IO.IOException or UnauthorizedAccessException or System.Security.SecurityException or ArgumentException){return fallback;}
    }
    public NativeWpfTheme(bool? dark=null)=>Dark=dark??(Personalization("AppsUseLightTheme",1)==0);
    private Brush Gray(int level){double l=level switch{25=>Dark?.18:.99,50=>Dark?.21:.96,75=>Dark?.24:.93,500=>Dark?.63:.54,_=>Dark?.92:.18};double linear=l*l*l,v=linear<=.0031308?12.92*linear:1.055*Math.Pow(linear,1/2.4)-.055;byte b=(byte)Math.Round(v*255);var brush=new SolidColorBrush(Color.FromRgb(b,b,b));brush.Freeze();return brush;}
    private static Brush Tint(uint rgb,byte alpha){var b=new SolidColorBrush(Color.FromArgb(alpha,(byte)(rgb>>16),(byte)(rgb>>8),(byte)rgb));b.Freeze();return b;}
    public Brush MaterialBackground(bool glass)=>Tint(Dark?0x202020u:0xffffffu,glass?(byte)(Dark?224:230):(byte)255);
    public Brush WindowCard=>Tint(0xffffff,Dark?(byte)20:(byte)179);
    public Brush WindowSidebar=>Tint(Dark?0xffffffu:0x000000u,13);
    public Brush Selection=>Tint(Dark?0xffffffu:0x000000u,64);
    public Brush Interaction(bool selected=false){var b=new LinearGradientBrush{StartPoint=new(0,0),EndPoint=new(0,1)};b.GradientStops.Add(new(TintColor(Dark?45:80),0));b.GradientStops.Add(new(TintColor(Dark?24:40),.45));b.GradientStops.Add(new(Color.FromArgb((byte)(selected?36:28),255,238,220),1));b.Freeze();return b;}
    private static Color TintColor(int alpha)=>Color.FromArgb((byte)alpha,255,255,255);
    public Brush Divider=>Tint(Dark?0xffffffu:0x14171cu,26);
    public void Apply(Window window){window.Background=Background;window.Foreground=Ink;window.FontFamily=CommunityFont.Family;window.FontSize=12;ApplyInteractions(window.Resources);}
    // Product-wide policy: retain input/focus semantics without the old blue
    // hover/default/focus adorners. Functional accent colors are explicit values.
    public void ApplyInteractions(ResourceDictionary resources)
    {
        resources[SystemColors.HighlightBrushKey]=Brushes.Transparent;
        resources[SystemColors.HighlightTextBrushKey]=Ink;
        resources[SystemColors.InactiveSelectionHighlightBrushKey]=Brushes.Transparent;
        resources[SystemColors.InactiveSelectionHighlightTextBrushKey]=Ink;
        resources[SystemColors.HotTrackBrushKey]=Ink;
        foreach(var type in new[]{typeof(Button),typeof(ToggleButton),typeof(CheckBox),typeof(RadioButton),typeof(ComboBox),typeof(TextBox),typeof(ListBox),typeof(ListBoxItem),typeof(ComboBoxItem)}){
            var style=new Style(type);style.Setters.Add(new Setter(Control.FocusVisualStyleProperty,null));
            style.Setters.Add(new Setter(Control.ForegroundProperty,Ink));
            if(type==typeof(Button)||type==typeof(TextBox)||type==typeof(ListBox)||type==typeof(ListBoxItem)||type==typeof(ComboBoxItem)){
                style.Setters.Add(new Setter(Control.BackgroundProperty,type==typeof(ListBoxItem)||type==typeof(ComboBoxItem)?Brushes.Transparent:Surface));
                style.Setters.Add(new Setter(Control.BorderBrushProperty,Divider));
                style.Setters.Add(new Setter(Control.BorderThicknessProperty,new Thickness(type==typeof(ListBoxItem)||type==typeof(ComboBoxItem)?0:1)));
                style.Setters.Add(new Setter(Control.TemplateProperty,NeutralTemplate(type)));
            }
            if(type==typeof(Button))style.Setters.Add(new Setter(Control.HorizontalContentAlignmentProperty,HorizontalAlignment.Center));
            if(type==typeof(TextBox)){
                style.Setters.Add(new Setter(TextBox.SelectionBrushProperty,Selection));
                style.Setters.Add(new Setter(TextBox.CaretBrushProperty,Ink));
            }
            if(type==typeof(ListBoxItem)||type==typeof(ComboBoxItem)){
                var selected=new Trigger { Property=type==typeof(ListBoxItem)?ListBoxItem.IsSelectedProperty:ComboBoxItem.IsSelectedProperty,Value=true };
                selected.Setters.Add(new Setter(Control.FontWeightProperty,FontWeights.SemiBold));style.Triggers.Add(selected);
            }
            var disabled=new Trigger { Property=UIElement.IsEnabledProperty,Value=false };disabled.Setters.Add(new Setter(UIElement.OpacityProperty,.45));style.Triggers.Add(disabled);
            resources[type]=style;
        }
    }
    private static Binding TemplateParent(string name)=>new(name) { RelativeSource=new(RelativeSourceMode.TemplatedParent) };
    private MultiBinding Feedback(bool selected=false){var binding=new MultiBinding{Converter=new FeedbackBrush(Interaction(selected))};binding.Bindings.Add(TemplateParent("Background"));binding.Bindings.Add(TemplateParent("Foreground"));return binding;}
    private sealed class FeedbackBrush(Brush neutral):IMultiValueConverter
    {
        private static readonly Brush accent=Accent();
        private static Brush Accent(){var brush=new SolidColorBrush(Color.FromArgb(20,0,0,0));brush.Freeze();return brush;}
        public object Convert(object[] values,Type target,object parameter,System.Globalization.CultureInfo culture){
            // Accent actions/selection retain their blue and white contrast;
            // they do not receive the neutral control's pale top gradient.
            if(values.Length==2&&values[0] is SolidColorBrush bg&&values[1] is SolidColorBrush fg&&fg.Color.R>200&&fg.Color.G>200&&fg.Color.B>200&&bg.Color.A>128&&Math.Max(bg.Color.R,Math.Max(bg.Color.G,bg.Color.B))-Math.Min(bg.Color.R,Math.Min(bg.Color.G,bg.Color.B))>32)return accent;
            return neutral;
        }
        public object[] ConvertBack(object value,Type[] targets,object parameter,System.Globalization.CultureInfo culture)=>throw new NotSupportedException();
    }
    private ControlTemplate NeutralTemplate(Type type)
    {
        var border=new FrameworkElementFactory(typeof(Border),"GlassRegion");border.SetValue(Border.CornerRadiusProperty,new CornerRadius(type==typeof(ListBox)?PanelRadius:ControlRadius));
        border.SetBinding(Border.BackgroundProperty,TemplateParent("Background"));border.SetBinding(Border.BorderBrushProperty,TemplateParent("BorderBrush"));
        border.SetBinding(Border.BorderThicknessProperty,TemplateParent("BorderThickness"));
        // State feedback is an overlay. Never replace a selected/action button's
        // explicit background while leaving its white foreground in place.
        var layers=new FrameworkElementFactory(typeof(Grid));border.AppendChild(layers);
        var interaction=new FrameworkElementFactory(typeof(Border),"InteractionRegion");
        interaction.SetValue(Border.CornerRadiusProperty,new CornerRadius(ControlRadius));
        interaction.SetValue(UIElement.IsHitTestVisibleProperty,false);layers.AppendChild(interaction);
        var inset=new FrameworkElementFactory(typeof(Border));inset.SetBinding(Border.PaddingProperty,TemplateParent("Padding"));layers.AppendChild(inset);
        if(type==typeof(TextBox)){
            var host=new FrameworkElementFactory(typeof(ScrollViewer),"PART_ContentHost");host.SetValue(UIElement.FocusableProperty,false);inset.AppendChild(host);
        }else if(type==typeof(ListBox)){
            var scroll=new FrameworkElementFactory(typeof(ScrollViewer));scroll.SetValue(ScrollViewer.CanContentScrollProperty,true);
            scroll.SetValue(ScrollViewer.VerticalScrollBarVisibilityProperty,ScrollBarVisibility.Auto);scroll.SetValue(ScrollViewer.HorizontalScrollBarVisibilityProperty,ScrollBarVisibility.Disabled);
            scroll.AppendChild(new FrameworkElementFactory(typeof(ItemsPresenter)));inset.AppendChild(scroll);
        }else{
            var content=new FrameworkElementFactory(typeof(ContentPresenter));content.SetValue(ContentPresenter.RecognizesAccessKeyProperty,true);
            content.SetBinding(ContentPresenter.ContentProperty,TemplateParent("Content"));content.SetBinding(ContentPresenter.ContentTemplateProperty,TemplateParent("ContentTemplate"));
            content.SetBinding(FrameworkElement.HorizontalAlignmentProperty,TemplateParent("HorizontalContentAlignment"));content.SetBinding(FrameworkElement.VerticalAlignmentProperty,TemplateParent("VerticalContentAlignment"));inset.AppendChild(content);
        }
        var template=new ControlTemplate(type) { VisualTree=border };
        if(type!=typeof(ListBox)){
            var hover=new MultiTrigger();hover.Conditions.Add(new Condition(UIElement.IsMouseOverProperty,true));hover.Conditions.Add(new Condition(UIElement.IsEnabledProperty,true));hover.Setters.Add(new Setter(Border.BackgroundProperty,Feedback(),"InteractionRegion"));hover.Setters.Add(new Setter(Border.BorderBrushProperty,Divider,"GlassRegion"));template.Triggers.Add(hover);
            var focus=new Trigger{Property=UIElement.IsKeyboardFocusWithinProperty,Value=true};focus.Setters.Add(new Setter(Border.BackgroundProperty,Feedback(true),"InteractionRegion"));focus.Setters.Add(new Setter(Border.BorderThicknessProperty,new Thickness(1),"GlassRegion"));focus.Setters.Add(new Setter(Border.BorderBrushProperty,Divider,"GlassRegion"));template.Triggers.Add(focus);
            if(type==typeof(ListBoxItem)||type==typeof(ComboBoxItem)){var selected=new Trigger{Property=type==typeof(ListBoxItem)?ListBoxItem.IsSelectedProperty:ComboBoxItem.IsSelectedProperty,Value=true};selected.Setters.Add(new Setter(Border.BackgroundProperty,Feedback(true),"InteractionRegion"));selected.Setters.Add(new Setter(Border.BorderThicknessProperty,new Thickness(1),"GlassRegion"));selected.Setters.Add(new Setter(Border.BorderBrushProperty,Divider,"GlassRegion"));template.Triggers.Add(selected);}
        }
        return template;
    }
}
