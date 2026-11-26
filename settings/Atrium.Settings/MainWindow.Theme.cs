using Microsoft.UI.Xaml;
using Windows.Foundation;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    // Windows' colours: switched between light and dark while the window is open, the window follows.
    private readonly UISettings _colors = new();
    private TypedEventHandler<UISettings, object>? _colorsChanged;

    private void InitTheme()
    {
        _colorsChanged = (_, _) => DispatcherQueue.TryEnqueue(FollowSystemTheme);
        _colors.ColorValuesChanged += _colorsChanged;
        FollowSystemTheme();
    }

    private void UnsubscribeTheme()
    {
        if (_colorsChanged == null) return;
        _colors.ColorValuesChanged -= _colorsChanged;
        _colorsChanged = null;
    }

    private void FollowSystemTheme()
    {
        if (_updatesClosed || App.ThemeOverride != null) return;
        var background = _colors.GetColorValue(UIColorType.Background);
        bool dark = background.R + background.G + background.B < 3 * 128;
        RootGrid.RequestedTheme = dark ? ElementTheme.Dark : ElementTheme.Light;
        Tokens.SetDark(dark);
        var bar = AppWindow.TitleBar;
        bar.ButtonForegroundColor = dark ? Microsoft.UI.Colors.White : Microsoft.UI.Colors.Black;
        bar.ButtonHoverForegroundColor = bar.ButtonPressedForegroundColor = bar.ButtonForegroundColor;
        bar.ButtonInactiveForegroundColor = dark ? Color.FromArgb(0x80, 0xFF, 0xFF, 0xFF) : Color.FromArgb(0x80, 0, 0, 0);
        bar.ButtonHoverBackgroundColor = dark ? Color.FromArgb(0x1A, 0xFF, 0xFF, 0xFF) : Color.FromArgb(0x0F, 0, 0, 0);
    }
}
