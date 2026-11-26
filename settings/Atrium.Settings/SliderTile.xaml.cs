using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace Atrium.Settings;

// One adjustable parameter: name, value and slider.
public sealed partial class SliderTile : UserControl
{
    private string _unit = "";
    private string? _maxLabel;
    private double _min, _max;

    public event Action<double>? UserValueChanged;
    public event Action<bool>? Engaged;

    public SliderTile()
    {
        InitializeComponent();
        GotFocus += (_, _) => SetEngaged(true);
        LostFocus += (_, _) => SetEngaged(false);
    }

    public void Configure(string title, double min, double max, double step, string unit, string? maxLabel = null)
    {
        _unit = unit; _maxLabel = maxLabel; _min = min; _max = max;
        TitleText.Text = title;
        Track.Minimum = min; Track.Maximum = max; Track.StepFrequency = step;
        AutomationProperties.SetName(Track, title);
    }

    // In the inspector: a plain row, no card (lit while in use all the same).
    public bool Flat
    {
        get;
        set
        {
            field = value;
            Surface.Style = (Style)Application.Current.Resources[value ? "FlatTileStyle" : "TileStyle"];
            Surface.Background = Tokens.Brush(value ? "TransparentBrush" : "CardBrush");
        }
    }

    // Sets the slider without raising UserValueChanged semantics for the caller (the caller guards with its own flag).
    public void SetValue(double v)
    {
        Track.Value = Math.Clamp(v, _min, _max);
        Refresh(Track.Value);
    }

    private void Refresh(double v)
    {
        if (_maxLabel != null && Math.Round(v) >= _max)
        {
            ValueRun.Text = _maxLabel;
            UnitRun.Text = "";
            return;
        }
        ValueRun.Text = Math.Round(v).ToString(System.Globalization.CultureInfo.InvariantCulture);
        UnitRun.Text = _unit is "" or "%" ? _unit : " " + _unit;
    }

    private void OnTrackChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        Refresh(e.NewValue);
        UserValueChanged?.Invoke(e.NewValue);
    }

    // The tile being adjusted is lit a little (the hero shows that parameter).
    private void SetEngaged(bool on)
    {
        Surface.Background = Tokens.Brush(on ? "TileActiveBrush" : Flat ? "TransparentBrush" : "CardBrush");
        Engaged?.Invoke(on);
    }
}
