using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace MagicCommunity;

// The card's own tuning panel, opened from a row inside the card.
//
// MyMagicBar kept these parameters next to the menu rather than in the application
// settings, and for a reason: they are shape and position, so the thing to judge
// them against is the card itself, not a form three pages away. Width and height are
// free numbers instead of tiers, position steps a few DIP at a time, and the ten
// system doors are individually optional - the same ten, in the same order, with the
// same targets that MyMagicBar's MenuSettings carried.
public sealed class AccountCardSettingsWindow : CommunityWindow
{
    private readonly AppController controller;
    private readonly CommunityPreferences preferences;
    private readonly NativeWpfTheme palette;
    private readonly StackPanel body = new();
    // One trailing refresh per 40 ms: a slider drag produces many ValueChanged
    // events, but the card only needs its last shape. The timer runs only while a
    // change is pending, so no idle tick survives a settled drag.
    private readonly DispatcherTimer refresh = new() { Interval = TimeSpan.FromMilliseconds(40) };
    private bool refreshPending;
    // Set when the reader left for good; the card is going away with this panel, so
    // closing must not try to hand it the foreground back.
    private bool dismissed;
    // A window that is already closing deactivates too; that is not a click outside.
    private bool closing;

    public AccountCardSettingsWindow(AppController controller, bool? theme = null)
    {
        this.controller = controller;
        preferences = CommunityPreferences.Load(controller.Root);
        // The renderer reads this copy, not the file, so a drag never touches disk.
        controller.CardDraft?.Invoke(preferences);
        palette = new NativeWpfTheme(theme);
        Title = "菜单卡片设置";
        Width = 430;
        Height = 640;
        MinWidth = 380;
        MinHeight = 420;
        WindowStartupLocation = WindowStartupLocation.Manual;
        ShowInTaskbar = false;
        // The panel does take focus, which is why the card is told to hold this very
        // window before it is shown (NativeSystemUiActions.OpenAccountTune). Without
        // that ordering the card would read the activation as the reader leaving.
        ShowActivated = true;
        Topmost = true;
        FontFamily = CommunityFont.Family;
        FontSize = 12;
        Foreground = palette.Ink;
        Background = palette.MaterialBackground(false);

        body.Margin = new Thickness(22, 18, 22, 18);
        body.Children.Add(new TextBlock
        {
            Text = "菜单卡片设置",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
            Foreground = palette.Ink,
            Margin = new Thickness(0, 0, 0, 2)
        });
        body.Children.Add(new TextBlock
        {
            Text = "改动立即生效，卡片会跟着变。",
            FontSize = 11,
            Foreground = palette.Muted,
            Margin = new Thickness(0, 0, 0, 12)
        });

        var widthReadout = new TextBlock();
        var heightReadout = new TextBlock();
        Row("宽度", Slider(260, 560, preferences.AccountCardWidth, widthReadout,
            v => preferences.AccountCardWidth = v));
        Row("高度", Slider(320, 780, preferences.AccountCardHeight, heightReadout,
            v => preferences.AccountCardHeight = v));

        var posX = new TextBlock();
        var posY = new TextBlock();
        Row("左右微调", Stepper(-200, 200, preferences.AccountCardOffsetX, 4, posX,
            v => preferences.AccountCardOffsetX = v));
        Row("上下微调", Stepper(-100, 400, preferences.AccountCardOffsetY, 4, posY,
            v => preferences.AccountCardOffsetY = v));
        // A real screen coordinate, the way MyMagicBar stored X and Y. Negative means
        // the card keeps following the avatar.
        var posFixedX = new TextBlock();
        var posFixedY = new TextBlock();
        Row("屏幕 X", Stepper(-1, 3840, preferences.AccountCardX, 8, posFixedX,
            v => preferences.AccountCardX = v));
        Row("屏幕 Y", Stepper(-1, 2160, preferences.AccountCardY, 8, posFixedY,
            v => preferences.AccountCardY = v));

        var rowHeightReadout = new TextBlock();
        var gapReadout = new TextBlock();
        var radiusReadout = new TextBlock();
        var fontReadout = new TextBlock();
        Row("行高", Slider(28, 52, preferences.AccountCardRowHeight, rowHeightReadout,
            v => preferences.AccountCardRowHeight = v));
        Row("行间距", Slider(0, 16, preferences.AccountCardGap, gapReadout,
            v => preferences.AccountCardGap = v));
        Row("字号", Slider(11, 16, preferences.AccountCardFontSize, fontReadout,
            v => preferences.AccountCardFontSize = v));
        Row("圆角", Slider(8, 32, preferences.AccountCardRadius, radiusReadout,
            v => preferences.AccountCardRadius = v));

        var motion = new CheckBox
        {
            Content = "打开动画",
            IsChecked = preferences.AccountCardMotion,
            Foreground = palette.Ink,
            VerticalAlignment = VerticalAlignment.Center
        };
        motion.Checked += (_, _) => { preferences.AccountCardMotion = true; Preview(); };
        motion.Unchecked += (_, _) => { preferences.AccountCardMotion = false; Preview(); };
        Row("开启动画", motion);

        body.Children.Add(Divider());
        body.Children.Add(new TextBlock
        {
            Text = "系统入口",
            FontSize = 13,
            FontWeight = FontWeights.SemiBold,
            Foreground = palette.Ink,
            Margin = new Thickness(0, 10, 0, 2)
        });
        body.Children.Add(new TextBlock
        {
            Text = "勾选后出现在卡片里，按下面的顺序排列。",
            FontSize = 11,
            Foreground = palette.Muted,
            Margin = new Thickness(0, 0, 0, 8)
        });

        Door("系统设置", preferences.AccountEntrySystem, v => preferences.AccountEntrySystem = v);
        Door("任务管理器", preferences.AccountEntryTaskManager, v => preferences.AccountEntryTaskManager = v);
        Door("显示设置", preferences.AccountEntryDisplay, v => preferences.AccountEntryDisplay = v);
        Door("声音设置", preferences.AccountEntrySound, v => preferences.AccountEntrySound = v);
        Door("网络设置", preferences.AccountEntryNetwork, v => preferences.AccountEntryNetwork = v);
        Door("电源设置", preferences.AccountEntryPower, v => preferences.AccountEntryPower = v);
        Door("蓝牙设置", preferences.AccountEntryBluetooth, v => preferences.AccountEntryBluetooth = v);
        Door("通知设置", preferences.AccountEntryNotification, v => preferences.AccountEntryNotification = v);
        Door("语言设置", preferences.AccountEntryLanguage, v => preferences.AccountEntryLanguage = v);
        Door("位置隐私", preferences.AccountEntryLocation, v => preferences.AccountEntryLocation = v);

        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Content = body
        };

        refresh.Tick += (_, _) =>
        {
            if (!refreshPending) { refresh.Stop(); return; }
            refreshPending = false;
            _ = (controller.RefreshCard ?? controller.RefreshPreferences)?.Invoke(default);
        };
        // The panel sits beside the card it edits, so it is placed from the card's real
        // rectangle once its own handle exists. Nothing here closes the card.
        SourceInitialized += (_, _) => Place();
        Loaded += (_, _) => Place();
        // Hand the foreground back to the card before this window goes away. The card
        // is a topmost window in the renderer process; leaving it visible but inactive
        // would make the next click on the desktop miss it, and the reader could only
        // dismiss it by reopening it. This is the same grant the window actions use.
        Closing += (_, _) =>
        {
            closing = true;
            if (dismissed) return;
            long card = controller.CardWindow?.Invoke() ?? 0;
            if (card == 0 || !GetWindowRect(new IntPtr(card), out _)) return;
            GetWindowThreadProcessId(new IntPtr(card), out uint owner);
            if (owner != 0) AllowSetForegroundWindow(owner);
            SetForegroundWindow(new IntPtr(card));
        };
        // The panel never takes ownership of the card: closing it writes once, and the
        // live previews before that only touch memory plus one refresh.
        Closed += (_, _) =>
        {
            refresh.Stop();
            // Disk once, on close. The file is written before the draft is dropped so
            // no publish in between can read the older values back into the card.
            try { preferences.Save(controller); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            controller.CardDraft?.Invoke(null);
            _ = controller.HoldCard?.Invoke(0);
            // The bars were not republished while the panel was open; do it once now
            // that the file matches what the card already shows.
            _ = controller.RefreshPreferences?.Invoke(default);
        };
        // Re-assert the hold whenever this panel takes focus. It covers a card that
        // was reopened while the panel stood: that is a new window and a new token,
        // and without this the next click here would dismiss it.
        Activated += (_, _) =>
        {
            long self = new WindowInteropHelper(this).Handle.ToInt64();
            if (self != 0) _ = controller.HoldCard?.Invoke(self);
        };
        // The card stays for as long as the reader is working in this panel, and a
        // click anywhere else is the reader leaving both. Without this the card would
        // survive the very click that always dismissed it.
        Deactivated += (_, _) =>
        {
            if (closing) return;
            long card = controller.CardWindow?.Invoke() ?? 0;
            if (card == 0) { dismissed = true; Close(); return; }
            // A click that lands in neither the panel nor the card means the reader
            // left, and both go away, as a click outside the card alone always did.
            // A focus that is merely transient (the card activating itself, our own
            // process) is not a click and is ignored.
            IntPtr foreground = GetForegroundWindow();
            if (foreground == IntPtr.Zero) return;
            GetWindowThreadProcessId(foreground, out uint focusPid);
            GetWindowThreadProcessId(new IntPtr(card), out uint cardPid);
            if (focusPid != 0 && (focusPid == cardPid || focusPid == Environment.ProcessId)) return;
            dismissed = true;
            _ = Dismiss();
        };
    }

    // The card already missed the deactivation that started this dismissal (it was
    // still held at that instant), so the hold is released and the card is closed
    // explicitly. A click outside therefore still closes both, as it always did.
    private async Task Dismiss()
    {
        try { if (controller.HoldCard is { } hold) await hold(0); } catch (Exception error) when (error is not OutOfMemoryException) { }
        try { if (controller.CloseCard is { } close) await close(); } catch (Exception error) when (error is not OutOfMemoryException) { }
        Close();
    }

    private void Preview()
    {
        refreshPending = true;
        if (!refresh.IsEnabled) refresh.Start();
    }

    // Preferred: the card's right edge plus a 10 DIP gap; then its left edge; then
    // below it. The work area of the card's own monitor has the last word, so the
    // panel can never land off-screen on a secondary display. Everything here is in
    // physical pixels: the app is PerMonitorV2, so a DIP would mean a different
    // number on every screen.
    private void Place()
    {
        long card = controller.CardWindow?.Invoke() ?? 0;
        var self = new WindowInteropHelper(this).Handle;
        if (self == IntPtr.Zero) return;
        if (card == 0 || !GetWindowRect(new IntPtr(card), out var cardRect))
        {
            Left = (SystemParameters.PrimaryScreenWidth - Width) / 2;
            Top = (SystemParameters.PrimaryScreenHeight - Height) / 2;
            return;
        }
        uint dpi = GetDpiForWindow(new IntPtr(card));
        double scale = dpi == 0 ? 1 : dpi / 96.0;
        int gap = (int)Math.Round(10 * scale);
        if (!GetWindowRect(self, out var selfRect)) return;
        int panelWidth = selfRect.right - selfRect.left, panelHeight = selfRect.bottom - selfRect.top;
        if (panelWidth <= 1 || panelHeight <= 1)
        {
            panelWidth = (int)Math.Ceiling(Width * scale);
            panelHeight = (int)Math.Ceiling(Height * scale);
        }
        var monitor = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
        if (!GetMonitorInfoW(MonitorFromWindow(new IntPtr(card), 2), ref monitor)) return;
        RECT work = monitor.rcWork;
        int x, y;
        if (cardRect.right + gap + panelWidth <= work.right) { x = cardRect.right + gap; y = cardRect.top; }
        else if (cardRect.left - gap - panelWidth >= work.left) { x = cardRect.left - gap - panelWidth; y = cardRect.top; }
        else { x = cardRect.left; y = cardRect.bottom + gap; }
        x = Math.Clamp(x, work.left, Math.Max(work.left, work.right - panelWidth));
        y = Math.Clamp(y, work.top, Math.Max(work.top, work.bottom - panelHeight));
        // Size stays WPF's business (DIPs); only the origin is ours, in pixels.
        uint selfDpi = GetDpiForWindow(self);
        double selfScale = selfDpi == 0 ? 1 : selfDpi / 96.0;
        Left = x / selfScale;
        Top = y / selfScale;
        SetWindowPos(self, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFO info);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] private static extern bool AllowSetForegroundWindow(uint processId);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();

    private Border Divider() => new()
    {
        Height = 1,
        Background = palette.Divider,
        Opacity = .5,
        Margin = new Thickness(0, 14, 0, 4)
    };

    private void Row(string label, UIElement control)
    {
        var grid = new Grid { Margin = new Thickness(0, 5, 0, 5) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.Children.Add(new TextBlock
        {
            Text = label,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = palette.Ink
        });
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        body.Children.Add(grid);
    }

    private UIElement Slider(double min, double max, double value, TextBlock readout, Action<double> apply)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal };
        var slider = new Slider
        {
            Minimum = min,
            Maximum = max,
            Value = Math.Clamp(value, min, max),
            Width = 186,
            VerticalAlignment = VerticalAlignment.Center,
            IsMoveToPointEnabled = true
        };
        readout.Text = ((int)Math.Round(slider.Value)).ToString();
        readout.Foreground = palette.Muted;
        readout.Width = 46;
        readout.TextAlignment = TextAlignment.Right;
        readout.VerticalAlignment = VerticalAlignment.Center;
        slider.ValueChanged += (_, e) =>
        {
            double next = Math.Round(e.NewValue);
            readout.Text = ((int)next).ToString();
            apply(next);
            Preview();
        };
        panel.Children.Add(slider);
        panel.Children.Add(readout);
        return panel;
    }

    private UIElement Stepper(double min, double max, double value, double step, TextBlock readout, Action<double> apply)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal };
        double current = Math.Clamp(value, min, max);
        readout.Text = Describe(current);
        readout.Foreground = palette.Muted;
        readout.Width = 46;
        readout.TextAlignment = TextAlignment.Right;
        readout.VerticalAlignment = VerticalAlignment.Center;

        Button Step(string glyph, double delta)
        {
            var button = new Button
            {
                Content = glyph,
                Width = 30,
                Height = 26,
                Margin = new Thickness(0, 0, 6, 0),
                Foreground = palette.Ink,
                Background = palette.WindowCard,
                BorderThickness = new Thickness(0),
                Cursor = System.Windows.Input.Cursors.Hand
            };
            button.Click += (_, _) =>
            {
                current = Math.Clamp(current + delta, min, max);
                readout.Text = Describe(current);
                apply(current);
                Preview();
            };
            return button;
        }

        panel.Children.Add(Step("−", -step));
        panel.Children.Add(readout);
        panel.Children.Add(Step("+", step));
        return panel;
    }

    // A negative coordinate is not a number the reader typed; it is the "follow the
    // avatar" state, so the readout says that instead of "-1".
    private static string Describe(double value) => value < 0 ? "跟随" : ((int)Math.Round(value)).ToString();

    private void Door(string label, bool value, Action<bool> apply)
    {
        var box = new CheckBox
        {
            Content = label,
            IsChecked = value,
            Foreground = palette.Ink,
            Margin = new Thickness(0, 3, 0, 3)
        };
        box.Checked += (_, _) => { apply(true); Preview(); };
        box.Unchecked += (_, _) => { apply(false); Preview(); };
        body.Children.Add(box);
    }
}
