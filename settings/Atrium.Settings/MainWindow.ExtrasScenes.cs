using System.Numerics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The previews of the newer entries' popups, laid out with the core's numbers (ui/now_playing_card.cpp,
// ui/folder_stack_layout.h): the now-playing card with its live bars, and the folder stack fanning out of
// its entry (or its column).
public sealed partial class MainWindow
{
    // ---- The now-playing card: a track playing, its bars moving, its progress going on ----

    private sealed class NowPlayingScene : ModuleScene
    {
        // ui/now_playing_card.cpp's numbers: the panels' type (body 13.5, caption 11), padding 14.
        private const double CW = 360, Pad = 14, Body = 13.5, Caption = 11, Art = 52, ArtR = 12, TextX = Pad + Art + 12;
        private const double BarsW = 20, BarsRight = CW - Pad - 6, BarsY = Pad + Art / 2;
        private const double TrackY = Pad + Art + 16, TrackH = 4, TimeW = 38, TrackL = Pad + TimeW + 6, TrackR = CW - Pad - TimeW - 6;
        private const double ControlsY = TrackY + 34, ControlStep = 58, ControlR = 16, PlayR = 20, CH = ControlsY + PlayR + Pad, MinRadius = 34;
        private static readonly double[] Shape = [0.58, 0.86, 1.0, 0.80, 0.52];
        private static readonly double[] Rate = [0.92, 1.27, 1.71, 1.13, 0.81];
        private readonly PanelInk _ink = new();
        private readonly Border _art = new() { CornerRadius = new CornerRadius(ArtR), IsHitTestVisible = false };
        private readonly TextBlock _title, _artist, _elapsed, _left, _heart, _lyrics;
        private readonly Border[] _bars = new Border[5];
        private readonly Border _track, _done;
        // The controls as the card draws them: pause as two bars, previous and next as two triangles.
        private readonly Border[] _pause = new Border[2];
        private readonly Microsoft.UI.Xaml.Shapes.Path _previous, _next;
        private readonly double _start = Environment.TickCount64;
        private int _shownSecond = -1;

        private sealed record Frame(double Left, double Top);

        public NowPlayingScene(MainWindow m, Border stage, Canvas root) : base(m, stage, root)
        {
            ContinuousCorners.SetEnabled(_art, true); // G3, as the card rounds the art
            _art.Background = new LinearGradientBrush
            {
                StartPoint = new Point(0, 0),
                EndPoint = new Point(1, 1),
                GradientStops =
                {
                    new GradientStop { Color = Color.FromArgb(255, 0xFF, 0x6B, 0x9A), Offset = 0 },
                    new GradientStop { Color = Color.FromArgb(255, 0x6A, 0x5A, 0xF0), Offset = 1 },
                },
            };
            _title = Words(Body, _ink.Text, true);
            _title.Text = "Midnight City";
            _artist = Words(Body, _ink.Sub);
            _artist.Text = "M83";
            _elapsed = Words(Caption, _ink.Sub);
            _left = Words(Caption, _ink.Sub, false, TextAlignment.Right);
            var tint = new SolidColorBrush(Color.FromArgb(255, 0xD0, 0x6C, 0xE0)); // the art's colour
            for (int i = 0; i < _bars.Length; i++) _bars[i] = Plate(tint, BarsW / 18);
            _track = Plate(_ink.Track, TrackH / 2);
            _done = Plate(_ink.Text, TrackH / 2);
            for (int i = 0; i < _pause.Length; i++) _pause[i] = Plate(_ink.Text, 1.8);
            _previous = Triangles("M9.5,-6 L1,0 L9.5,6 Z M0,-6 L-8.5,0 L0,6 Z");
            _next = Triangles("M-9.5,-6 L-1,0 L-9.5,6 Z M0,-6 L8.5,0 L0,6 Z");
            // Either side of the controls: the heart (NetEase) and the lyrics (NetEase and QQ Music).
            _heart = Symbol("\uEB51", 16, _ink.Text);
            string lyrics = Loc.T("Lyrics", "button");
            _lyrics = Words(lyrics.Length > 2 ? 11 : 16.5, _ink.Text, true, TextAlignment.Center);
            _lyrics.Text = lyrics;
            foreach (var e in new UIElement[]
            {
                _art, _title, _artist, _track, _done, _elapsed, _left, _heart, _lyrics, _previous, _next,
            })
            {
                Marks.Children.Add(e);
            }
            foreach (var e in _bars) Marks.Children.Add(e);
            foreach (var e in _pause) Marks.Children.Add(e);
        }

        private Microsoft.UI.Xaml.Shapes.Path Triangles(string markup) => new()
        {
            IsHitTestVisible = false,
            Data = (Geometry)Microsoft.UI.Xaml.Markup.XamlBindingHelper.ConvertValue(typeof(Geometry), markup),
            Fill = _ink.Text,
            Stroke = _ink.Text,
            StrokeThickness = 1.5,
            StrokeLineJoin = PenLineJoin.Round,
        };

        protected override bool IsOpen => M.CurrentSettings.ShowNowPlaying;
        protected override Size PopupSize => new(CW, CH);
        // One slot right of the status entry, where the bar puts it.
        protected override Vector2 Anchor => new((float)(Bar!.StatusX >= 0 ? Bar.StatusX + Bar.P * 1.6 : BarRight - 2 * Bar.P), (float)PopupBottom);

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double w = CW * S, h = CH * S;
            // Its right side lined up with the bar's.
            double left = Math.Clamp(BarRight - w, 16, Math.Max(16, W - 16 - w)), top = PopupBottom - h;
            return (new[] { new GlassTile(left, top, w, h, Math.Min(Math.Max(Radius, MinRadius), CH / 2) * S) }, new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => _ink.Set(dark);

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            Put(_art, Pad, Pad, Art, Art);
            double right = BarsRight - BarsW - 10, mid = Pad + Art / 2;
            Box(_title, TextX, mid - 19, right - TextX, 19);
            Box(_artist, TextX, mid + 1, right - TextX, 19);
            // The bars: each on its own rhythm inside a loud song's level.
            double t = (Now - _start) / 1000, bw = BarsW / 9, gap = (BarsW - 5 * bw) / 4;
            bool moving = M.CurrentSettings.NowPlayingMotion != "off";
            double reach = M.CurrentSettings.NowPlayingMotion switch { "subtle" => 0.55, "vivid" => 1.0, _ => 0.9 };
            for (int i = 0; i < _bars.Length; i++)
            {
                double a = 2 * Math.PI * Rate[i] * t, swing = 0.5 + 0.32 * Math.Sin(a) + 0.18 * Math.Sin(1.5 * a + 1.7 * i);
                double hgt = moving ? 0.22 + 0.78 * reach * Shape[i] * (0.38 + 0.62 * swing) : 0.22 + 0.18 * Shape[i];
                double hh = Math.Max(bw, BarsW * hgt);
                Put(_bars[i], BarsRight - BarsW + i * (bw + gap), BarsY - hh / 2, bw, hh);
            }
            // The progress: a four-minute song, a quarter in, going on; the times change once a second.
            double length = 244, pos = (61 + t) % length, done = pos / length;
            Put(_track, TrackL, TrackY - TrackH / 2, TrackR - TrackL, TrackH);
            Put(_done, TrackL, TrackY - TrackH / 2, Math.Max(TrackH, (TrackR - TrackL) * done), TrackH);
            if ((int)pos != _shownSecond)
            {
                _shownSecond = (int)pos;
                _elapsed.Text = $"{(int)pos / 60}:{(int)pos % 60:00}";
                _left.Text = $"-{(int)(length - pos) / 60}:{(int)(length - pos) % 60:00}";
                Box(_elapsed, Pad, TrackY - 8, TrackL - 6 - Pad, 16);
                Box(_left, TrackR + 6, TrackY - 8, CW - Pad - (TrackR + 6), 16);
            }
            double cx = CW / 2;
            Put(_pause[0], cx - 7.5, ControlsY - 8.5, 5, 17);
            Put(_pause[1], cx + 2.5, ControlsY - 8.5, 5, 17);
            Canvas.SetLeft(_previous, cx - ControlStep);
            Canvas.SetTop(_previous, ControlsY);
            Canvas.SetLeft(_next, cx + ControlStep);
            Canvas.SetTop(_next, ControlsY);
            Box(_heart, cx - 2 * ControlStep - ControlR, ControlsY - ControlR, 2 * ControlR, 2 * ControlR);
            Box(_lyrics, cx + 2 * ControlStep - ControlR, ControlsY - ControlR, 2 * ControlR, 2 * ControlR);
            return moving; // it plays on while the bars move
        }
    }

    // ---- The folder stack: the folders fan out of the entry (or list in a card) ----

    private sealed class StacksScene : ModuleScene
    {
        // ui/folder_stack_layout.h.
        private const double Gap = 12, Icon = 40, StepY = 50, Arc = 1000, LabelH = 28, LabelPad = 12, LabelGap = 10, Margin = 16;
        private const double ColumnW = 264, RowH = 44, ColumnPad = 8, ColumnIcon = 26;
        private const double OpenMs = 340, StaggerMs = 22, Tilt = 0.6;
        private readonly PanelInk _ink = new();
        private readonly List<(Grid Icon, TextBlock Glyph, Image Picture, TextBlock Name, Border Plate)> _items = [];
        private double _opened = double.MaxValue;

        private sealed record StackItem(string Name, string Path, bool IsFile);

        // The names the frame was laid out for: the folders may change before it shows.
        private sealed record Frame(double Left, double Top, double W, double H, bool Fan, StackItem[] Items, double[] LabelW);

        public StacksScene(MainWindow m, Border stage, Canvas root) : base(m, stage, root) { }

        private StackItem[] Items()
        {
            var folders = M.CurrentSettings.FolderStacks;
            var files = M.CurrentSettings.FolderStackFiles;
            var configured = folders.Select(p => (Path: p, DefaultIsFile: false))
                .Concat(files.Select(p => (Path: p, DefaultIsFile: true)))
                .Select(entry =>
                {
                    bool isFile = IsStackFile(entry.Path, entry.DefaultIsFile);
                    return new StackItem(StackItemName(entry.Path, isFile), entry.Path, isFile);
                });
            StackItem[] items = folders.Count > 0 || files.Count > 0
                ? [.. configured]
                :
                [
                    new(Loc.T("Desktop"), "", false),
                    new(Loc.T("Downloads"), "", false),
                    new(Loc.T("Documents"), "", false),
                    new(Loc.T("Pictures"), "", false),
                    new(Loc.T("Music"), "", false),
                ];
            return [.. items.Take(Math.Clamp(M.CurrentSettings.FolderStackMax, 4, 24)).Take(7)];
        }

        private bool Fan => M.CurrentSettings.FolderStackLayout != "column";
        protected override bool IsOpen =>
            M.CurrentSettings.ShowFolderStacks || M.CurrentSettings.FolderStacks.Count > 0 || M.CurrentSettings.FolderStackFiles.Count > 0;
        protected override Size PopupSize => Fan ? new Size(360, Gap + Icon + 6 * StepY + Margin) : new Size(ColumnW, 2 * ColumnPad + 5 * RowH);
        // One slot left of the tray, where the bar puts it.
        protected override Vector2 Anchor => new((float)(Bar!.TrayX >= 0 ? Bar.TrayX - Bar.P * 0.6 : BarRight - 3 * Bar.P), (float)PopupBottom);
        protected override void Ink(int piece, bool dark) => _ink.Set(dark);
        protected override void Entered() => _opened = double.MaxValue;
        protected override void PoppedIn() => _opened = Now;
        protected override void Refreshed() => _opened = Environment.TickCount64; // another layout or other folders: it opens again

        private static double Lean(double rise) => Arc - Math.Sqrt(Math.Max(0, Arc * Arc - rise * rise));
        // kStackTilt: the name along the arc's radius, leaning a little less than the arc turns (radians).
        private static double Turn(double rise) => Tilt * Math.Asin(Math.Min(1, rise / Arc));
        private static double LabelWidth(string name) => Math.Clamp(TextWidth(name, 13) + 2 * LabelPad, 56, 240);
        private double Unfold(int i) => _opened == double.MaxValue ? 0 : Motion.EaseOutBack(Math.Clamp((Now - _opened - StaggerMs * i) / OpenMs, 0, 1), 1.25);

        protected override bool Step(double dt) => Fan && _opened != double.MaxValue && Now - _opened < OpenMs + 7 * StaggerMs + 40;

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            var items = Items();
            double ax = Anchor.X, foot = PopupBottom + Gap * S;
            if (!Fan)
            {
                double w = ColumnW * S, h = (2 * ColumnPad + items.Length * RowH) * S;
                double left = Math.Clamp(ax - w / 2, 16, Math.Max(16, W - 16 - w));
                return (new[] { new GlassTile(left, PopupBottom - h, w, h, Math.Min(Radius, 22) * S) },
                    new Frame(left, PopupBottom - h, w, h, false, items, []));
            }
            // The names' capsules, each its own piece of glass, flying out with their folders.
            double[] widths = [.. items.Select(item => LabelWidth(item.Name))];
            var tiles = new List<GlassTile>();
            for (int i = 0; i < items.Length; i++)
            {
                double e = Unfold(i), rise = Gap + Icon / 2 + i * StepY, turn = Turn(rise), reach = (Icon / 2 + LabelGap + widths[i] / 2) * S;
                double lx = ax + Lean(rise) * S - Math.Cos(turn) * reach, ly = foot - rise * S - Math.Sin(turn) * reach;
                double x = ax + (lx - widths[i] * S / 2 - ax) * e, y = foot + (ly - foot) * e;
                if (e > 0.02)
                {
                    tiles.Add(new GlassTile(
                        x, y - LabelH / 2 * S, widths[i] * S * Math.Min(1, e), LabelH * S,
                        LabelH / 2 * S, turn * 180 / Math.PI * Math.Min(1, e)));
                }
            }
            return ([.. tiles], new Frame(ax, foot, 0, 0, true, items, widths));
        }

        // Turned about its centre; the transform kept from frame to frame.
        private static void Turned(UIElement e, double degrees)
        {
            if (e.RenderTransform is not RotateTransform turn)
            {
                e.RenderTransformOrigin = new Point(0.5, 0.5);
                e.RenderTransform = turn = new RotateTransform();
            }
            turn.Angle = degrees;
        }

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            var items = f.Items;
            while (_items.Count < items.Length)
            {
                var glyph = new TextBlock
                {
                    Text = "\U0001F4C1",
                    FontFamily = new FontFamily("Segoe UI Emoji"),
                    TextAlignment = TextAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    IsHitTestVisible = false,
                };
                var picture = new Image { Stretch = Stretch.Uniform, Visibility = Visibility.Collapsed, IsHitTestVisible = false };
                var icon = new Grid { IsHitTestVisible = false, Children = { glyph, picture } };
                var name = Words(13, _ink.Text, false, TextAlignment.Center);
                var plate = Plate(_ink.Hover, 10);
                Marks.Children.Add(plate);
                Marks.Children.Add(icon);
                Marks.Children.Add(name);
                _items.Add((icon, glyph, picture, name, plate));
            }
            for (int i = 0; i < _items.Count; i++)
            {
                bool shown = i < items.Length;
                _items[i].Icon.Visibility = _items[i].Name.Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
                _items[i].Plate.Visibility = Visibility.Collapsed;
                if (shown && !Equals(_items[i].Icon.Tag, items[i]))
                {
                    _items[i].Icon.Tag = items[i];
                    _items[i].Name.Text = items[i].Name;
                    M.SetStackItemIcon(items[i].Path, items[i].IsFile, _items[i].Picture, _items[i].Glyph);
                }
            }
            if (!f.Fan)
            {
                PlaceMarks(f.Left, f.Top);
                foreach (var item in _items) item.Name.RenderTransform = item.Icon.RenderTransform = null;
                for (int i = 0; i < items.Length; i++)
                {
                    // Over a bottom bar the first folder is the lowest row.
                    double y = ColumnPad + (items.Length - 1 - i) * RowH, e = Motion.EaseOut((Now - _opened - 14 * i) / 240);
                    _items[i].Glyph.FontSize = ColumnIcon * 0.8;
                    _items[i].Picture.Width = _items[i].Picture.Height = ColumnIcon;
                    Put(_items[i].Icon, ColumnPad + 10, y + 6 * (1 - Math.Clamp(e, 0, 1)), ColumnIcon, RowH);
                    _items[i].Name.TextAlignment = TextAlignment.Left;
                    Box(_items[i].Name, ColumnPad + 10 + ColumnIcon + 12, y + 6 * (1 - Math.Clamp(e, 0, 1)), ColumnW - 2 * ColumnPad - ColumnIcon - 34, RowH);
                    _items[i].Icon.Opacity = _items[i].Name.Opacity = Math.Clamp(e, 0, 1);
                }
                return _opened != double.MaxValue && Now - _opened < 240 + 14 * items.Length;
            }
            PlaceMarks(0, 0, 1); // the fan is laid out in the stage's own units
            bool moving = false;
            for (int i = 0; i < items.Length; i++)
            {
                double e = Unfold(i), rise = Gap + Icon / 2 + i * StepY;
                moving |= e < 1 || _opened == double.MaxValue;
                double cx = f.Left + Lean(rise) * S, cy = f.Top - rise * S;
                double x = f.Left + (cx - f.Left) * e, y = f.Top + (cy - f.Top) * e, k = 0.35 + 0.65 * Math.Min(e, 1);
                _items[i].Glyph.FontSize = Math.Max(1.0, Icon * 0.78 * S * k);
                _items[i].Picture.Width = _items[i].Picture.Height = Math.Max(0.0, Icon * S * k);
                Put(_items[i].Icon, x - Icon / 2 * S, y - Icon / 2 * S, Icon * S, Icon * S);
                Turned(_items[i].Icon, Turn(rise) * 180 / Math.PI * Math.Min(1, e));
                double lw = (i < f.LabelW.Length ? f.LabelW[i] : LabelWidth(items[i].Name)) * S;
                double turn = Turn(rise), reach = Icon / 2 * S + LabelGap * S + lw / 2;
                double lcx = cx - Math.Cos(turn) * reach, lcy = cy - Math.Sin(turn) * reach;
                double lx = f.Left + (lcx - lw / 2 - f.Left) * e, ly = f.Top + (lcy - f.Top) * e;
                _items[i].Name.FontSize = Math.Max(1.0, 13 * S);
                _items[i].Name.TextAlignment = TextAlignment.Center;
                Box(_items[i].Name, lx, ly - LabelH / 2 * S, lw, LabelH * S);
                Turned(_items[i].Name, turn * 180 / Math.PI * Math.Min(1, e));
                _items[i].Icon.Opacity = Math.Clamp(e * 3, 0, 1);
                _items[i].Name.Opacity = Math.Clamp((e - 0.3) / 0.7, 0, 1);
            }
            return moving && _opened != double.MaxValue;
        }
    }
}
