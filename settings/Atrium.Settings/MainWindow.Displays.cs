using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    private List<DesktopDisplay> _displays = [];

    private void InitDisplays()
    {
        DisplayDelay.Configure(Loc.T("Cross-display dwell time"), 100, 2000, 50, "ms");
        DisplayDelay.UserValueChanged += value =>
        {
            if (_loading) return;
            CurrentSettings.DockDisplayDelay = value;
            Changed();
        };
        RefreshDisplays();
    }

    private void RefreshDisplays()
    {
        bool loading = _loading;
        _loading = true;
        _displays = DesktopDisplay.Read();
        ConnectedDisplayList.ItemsSource = _displays;
        DisplaySummary.Text = Loc.F("{0} display(s) connected", _displays.Count);
        var choices = new List<DesktopDisplay> { new("", Loc.T("Follow the system primary display"), "", true) };
        choices.AddRange(_displays);
        // Keep an unplugged selection visible; reconnection restores it without resaving.
        foreach (string id in new[] { CurrentSettings.DockDisplay, CurrentSettings.DesktopDisplay }.Distinct())
        {
            if (id.Length > 0 && !choices.Any(d => d.Id == id))
                choices.Add(new(id, Loc.T("Disconnected display (using the primary until it returns)"), "", false));
        }

        DockDisplayPicker.ItemsSource = choices;
        DesktopDisplayPicker.ItemsSource = choices;
        DockDisplayPicker.SelectedItem = choices.First(d => d.Id == CurrentSettings.DockDisplay);
        DesktopDisplayPicker.SelectedItem = choices.First(d => d.Id == CurrentSettings.DesktopDisplay);
        DisplayFallback.Text = new[] { CurrentSettings.DockDisplayMode == "fixed" ? CurrentSettings.DockDisplay : "", CurrentSettings.DesktopDisplay }
            .Any(id => id.Length > 0 && !_displays.Any(d => d.Id == id))
            ? Loc.T("The chosen display is disconnected; the primary is used and the choice returns when it reconnects.") : "";
        DisplayFallback.Visibility = DisplayFallback.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        _loading = loading;
    }

    private void LoadDisplayControls()
    {
        var mode = CurrentSettings.DockDisplayMode switch
        {
            "follow" => DockDisplayFollow,
            "fixed" => DockDisplayFixed,
            "all" => DockDisplayAll,
            _ => DockDisplayPrimary,
        };
        mode.IsChecked = true;
        DisplayDelay.SetValue(CurrentSettings.DockDisplayDelay);
        TaskbarAppsPicker.SelectedIndex = CurrentSettings.TaskbarApps switch { "primaryAndWindow" => 1, "window" => 2, _ => 0 };
        TaskbarRightPrimaryOnlySwitch.IsOn = CurrentSettings.TaskbarRightPrimaryOnly;
        UpdateDisplayControls();
        RefreshDisplays();
    }

    private void UpdateDisplayControls()
    {
        DockDisplayPicker.IsEnabled = CurrentSettings.DockDisplayMode == "fixed";
        DisplayDelay.IsEnabled = CurrentSettings.DockDisplayMode == "follow";
    }

    private void OnDisplayModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.DockDisplayMode = (string)((RadioButton)sender).Tag;
        UpdateDisplayControls();
        RefreshDisplays();
        Changed();
    }

    private void OnDockDisplayChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || e.AddedItems.Count == 0) return;
        CurrentSettings.DockDisplay = ((DesktopDisplay)e.AddedItems[0]).Id;
        Changed();
    }

    private void OnDesktopDisplayChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || e.AddedItems.Count == 0) return;
        CurrentSettings.DesktopDisplay = ((DesktopDisplay)e.AddedItems[0]).Id;
        Changed();
    }

    private void OnTaskbarAppsChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || TaskbarAppsPicker.SelectedIndex < 0) return;
        CurrentSettings.TaskbarApps = TaskbarAppsPicker.SelectedIndex switch { 1 => "primaryAndWindow", 2 => "window", _ => "all" };
        Changed();
    }

    private void OnTaskbarRightPrimaryOnlyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.TaskbarRightPrimaryOnly = TaskbarRightPrimaryOnlySwitch.IsOn;
        Changed();
    }

    private void OnRefreshDisplays(object sender, RoutedEventArgs e) => RefreshDisplays();
    private void OnSystemDisplays(object sender, RoutedEventArgs e) => Process.Start(new ProcessStartInfo("ms-settings:display") { UseShellExecute = true });
}
