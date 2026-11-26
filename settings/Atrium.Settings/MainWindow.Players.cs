using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Atrium.Settings;

// The players the now-playing card knows (native/core/shell/media_players.h), under its preview as the
// notification sources are under theirs: icon, name and a switch, what the card does with each in the
// row's tooltip. A player switched off is left off the bar: its ids go into nowPlayingApps, blocked
// (nowPlayingAppMode block); with every one on, all players show.
public sealed partial class MainWindow
{
    private static readonly (string Title, string Note, string[] Programs, string Icon, string[] Ids)[] KnownPlayers =
    [
        ("NetEase Cloud Music",
            "Its progress is read from the player itself. The heart and the lyrics use its shortcuts (Ctrl+Alt+L, Ctrl+Alt+D). It takes no position from other apps.",
            ["cloudmusic.exe"], "netease", ["cloudmusic.exe"]),
        ("QQ Music", "Progress and seeking. The lyrics use its shortcut (Ctrl+Alt+W).",
            ["QQMusic.exe"], "qqmusic", ["QQMusic.exe", "Tencent.QQMusic"]),
        ("Spotify", "Progress and seeking.", ["Spotify.exe"], "spotify", ["Spotify.exe", "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!"]),
        ("Apple Music", "Progress, and the artist and the album apart.", ["AppleMusic.exe"], "applemusic",
            ["AppleMusic.exe", "AppleInc.AppleMusicWin_nzyj5cx40ttqa!"]),
    ];

    private readonly List<(SourceVm Vm, ToggleSwitch Switch)> _players = [];

    private StackPanel BuildPlayers()
    {
        var res = Application.Current.Resources;
        var rows = new StackPanel { Padding = new Thickness(0, 4, 0, 4) };
        foreach (var (name, note, programs, icon, ids) in KnownPlayers)
        {
            var vm = new SourceVm(this, name, note, "\uE8D6", programs, s => PlayerShown(s, ids), (s, on) => ShowPlayer(s, ids, on)) { Bundled = icon };
            var sw = new ToggleSwitch { Style = (Style)res["SwitchStyle"], IsOn = vm.IsOn, VerticalAlignment = VerticalAlignment.Center };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(sw, vm.Title);
            sw.Toggled += (_, _) => { if (!_loading) vm.IsOn = sw.IsOn; };
            _players.Add((vm, sw));
            rows.Children.Add(PlayerRow(vm, sw));
            _ = LoadRowIconAsync(vm);
        }
        var title = new TextBlock { Text = Loc.T("Players"), Style = (Style)res["SectionStyle"], Margin = new Thickness(4, 0, 0, 6) };
        var limits = LimitNote(
            Loc.T("Limited by each player's interface:\nNetEase Cloud Music's progress can't be dragged, QQ Music's tracks can't be liked, Spotify's can be neither liked nor shown with lyrics, and Apple Music's progress can't be dragged, nor its tracks liked or shown with lyrics."));
        var group = new Border { Style = (Style)res["GroupStyle"], Child = rows };
        return new StackPanel { Margin = new Thickness(0, 14, 0, 0), Children = { title, limits, group } };
    }

    // A grey line between a section's title and its rows: what an app's own interface keeps Atrium from doing.
    private static TextBlock LimitNote(string text) =>
        new() { Text = text, Style = (Style)Application.Current.Resources["FootnoteStyle"], Margin = new Thickness(4, 0, 0, 10) };

    // A row as the sources' (MainWindow.xaml, SourcesList): the icon (assets/apps), the name, the switch.
    private static Grid PlayerRow(SourceVm vm, ToggleSwitch sw)
    {
        var row = new Grid
        {
            Padding = new Thickness(14, 8, 14, 8),
            ColumnSpacing = 14,
            MinHeight = 60,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
        };
        ToolTipService.SetToolTip(row, vm.Note);
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var glyph = new FontIcon { Glyph = vm.Glyph, FontSize = 20, Foreground = Tokens.Brush("InkSecondaryBrush") };
        var image = new Image { Width = 36, Height = 36 };
        vm.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName != nameof(AppRowVm.Icon)) return;
            image.Source = vm.Icon;
            glyph.Visibility = vm.FallbackVisibility;
        };
        var icon = new Grid { Width = 40, Height = 40, VerticalAlignment = VerticalAlignment.Center, Children = { glyph, image } };
        var name = new TextBlock
        {
            Text = vm.Title,
            Style = (Style)Application.Current.Resources["RowTitleStyle"],
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
        };
        Grid.SetColumn(name, 1);
        Grid.SetColumn(sw, 2);
        row.Children.Add(icon);
        row.Children.Add(name);
        row.Children.Add(sw);
        return row;
    }

    private void RefreshPlayers()
    {
        foreach (var (vm, sw) in _players) sw.IsOn = vm.IsOn;
    }

    private static bool MatchesPlayerId(string entry, string[] ids)
    {
        string trimmed = entry.Trim();
        if (trimmed.Length == 0) return false;
        string file = Path.GetFileName(trimmed);
        foreach (string id in ids)
        {
            if (string.Equals(trimmed, id, StringComparison.OrdinalIgnoreCase) ||
                (file.Length > 0 && string.Equals(file, id, StringComparison.OrdinalIgnoreCase)))
            {
                return true;
            }
            if (id.EndsWith('!') && trimmed.StartsWith(id, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
            if (!id.EndsWith('!') && !id.EndsWith('_') &&
                (trimmed.StartsWith(id + "_", StringComparison.OrdinalIgnoreCase) ||
                 trimmed.StartsWith(id + "!", StringComparison.OrdinalIgnoreCase)))
            {
                return true;
            }
            if ((trimmed.EndsWith('!') || trimmed.EndsWith('_')) && id.StartsWith(trimmed, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    private static bool Listed(BarSettings s, string[] ids) => s.NowPlayingApps.Any(a => MatchesPlayerId(a, ids));

    private static bool PlayerShown(BarSettings s, string[] ids) => s.NowPlayingAppMode switch
    {
        "block" => !Listed(s, ids),
        "allow" => Listed(s, ids),
        _ => true,
    };

    private static void ShowPlayer(BarSettings s, string[] ids, bool on)
    {
        if (s.NowPlayingAppMode == "allow")
        {
            var allowed = s.NowPlayingApps.ToList();
            if (on)
            {
                foreach (string id in ids)
                {
                    if (!allowed.Contains(id, StringComparer.OrdinalIgnoreCase) && allowed.Count < 64)
                        allowed.Add(id);
                }
            }
            else
            {
                allowed.RemoveAll(a => MatchesPlayerId(a, ids));
            }
            s.NowPlayingApps = [.. allowed.Distinct(StringComparer.OrdinalIgnoreCase).Take(64)];
            return;
        }

        var custom = s.NowPlayingAppMode == "block"
            ? s.NowPlayingApps.Where(a => !KnownPlayers.Any(p => MatchesPlayerId(a, p.Ids))).ToList()
            : [];
        var knownOff = KnownPlayers
            .Where(p => !p.Ids.SequenceEqual(ids, StringComparer.OrdinalIgnoreCase) && !PlayerShown(s, p.Ids))
            .SelectMany(p => p.Ids)
            .ToList();
        if (!on) knownOff.AddRange(ids);
        s.NowPlayingApps = [.. custom.Concat(knownOff).Distinct(StringComparer.OrdinalIgnoreCase).Take(64)];
        s.NowPlayingAppMode = s.NowPlayingApps.Count == 0 ? "all" : "block";
    }
}
