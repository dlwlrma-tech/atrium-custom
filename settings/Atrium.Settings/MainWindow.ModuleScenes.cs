using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    // ---- The start menu: the panel and its capsule, from the bar's left end ----

    private sealed class StartScene : ModuleScene
    {
        private static readonly int[] Cols = [7, 9, 11, 13], Rows = [3, 4, 5, 6]; // StartLayout::kStartGrids
        // The cells follow the icons' size (startIcon), as in the menu (kStartIcons).
        private static readonly double[] CellW = [62, 72, 94, 116], CellH = [82, 88, 100, 112], IconPx = [34, 40, 52, 64];
        private const double GridTop = 86, GridPad = 28, Foot = 42, Gap = 12;
        // Its tiles, drawn here: a name, a symbol, a colour.
        private static readonly (string Name, string Glyph, uint Top, uint Bottom)[] Apps =
        [
            ("File Explorer", "\uE8B7", 0xFFD66B, 0xF2A922), ("Browser", "\uE774", 0x5AC8FA, 0x0A84FF), ("Mail", "\uE715", 0x6AAEFF, 0x2D6BEA),
            ("Calendar", "\uE787", 0xFF7A70, 0xE5383B), ("Photos", "\uEB9F", 0xFFB86B, 0xF0623A), ("Music", "\uE8D6", 0xFF6F91, 0xE0245E),
            ("Settings", "\uE713", 0xA7AFBC, 0x67707D), ("Notepad", "\uE70B", 0x7DD3C0, 0x2BA58C), ("Calculator", "\uE8EF", 0x8E9AAF, 0x4A5568),
            ("Maps", "\uE707", 0x7CDB8A, 0x30B158), ("Camera", "\uE722", 0x9DA6B5, 0x515A68), ("Clock", "\uE823", 0xB39DFF, 0x6E56CF),
        ];

        private readonly PanelInk _panel = new(), _shelf = new();
        private readonly Border _search, _all;
        private readonly TextBlock _searchGlyph, _hint, _allGlyph, _allLabel;
        private readonly Rectangle _caret = new() { Fill = AccentBrush, IsHitTestVisible = false, Opacity = 0.9 };
        private readonly Grid[] _icons;
        private readonly TextBlock[] _names;
        private readonly double[] _x, _y;
        private bool _placed;
        private double _staged = double.MaxValue;
        // The panel's size, springing (kResize) to the chosen one; the icons' size picks the cells.
        private int _size = -1, _icon = -1;
        private double _pw, _pv, _mh, _mv;
        // Where it opens: on the bar's left end, or in the middle (startCentered).
        private bool _centered, _wantCentered;
        // The capsule.
        private readonly Ellipse _avatar = new() { Fill = new SolidColorBrush(Color.FromArgb(230, 0x0A, 0x84, 0xFF)), IsHitTestVisible = false };
        private readonly TextBlock _initial = Words(13, new SolidColorBrush(Colors.White), true, TextAlignment.Center), _user;
        private readonly Rectangle _divider = new() { IsHitTestVisible = false };
        private readonly Shelf[] _buttons;
        private double _dividerA, _dividerX;

        // A round button of the capsule; Right: from the capsule's right end to its own (gliding).
        private sealed class Shelf
        {
            public required string Id;
            public required FrameworkElement Mark;
            public TextBlock? Label;
            public double Right, Alpha, Width;
            public bool On, Placed;
        }

        private sealed record Frame(double Left, double Bottom, double PanelW, double MainH, double Scale);

        public StartScene(MainWindow m) : base(m, m.StartStage, m.StartCanvas)
        {
            _search = Plate(_panel.Search, 22);
            _search.BorderBrush = _panel.Border;
            _search.BorderThickness = new Thickness(1);
            _all = Plate(_panel.Wash45, 22);
            _searchGlyph = Symbol("\uE721", 16, _panel.Sub);
            _hint = Words(15, _panel.Faint);
            _hint.Text = Loc.T("Search apps and folders");
            _allGlyph = Symbol("\uE71D", 14, _panel.Text);
            _allLabel = Words(13, _panel.Text);
            _allLabel.Text = Loc.T("All apps");
            foreach (var e in new UIElement[] { _search, _searchGlyph, _hint, _caret, _all, _allGlyph, _allLabel }) Marks.Children.Add(e);
            _icons = new Grid[Apps.Length];
            _names = new TextBlock[Apps.Length];
            _x = new double[Apps.Length];
            _y = new double[Apps.Length];
            for (int i = 0; i < Apps.Length; i++)
            {
                _icons[i] = DrawnIcon(Apps[i].Glyph, Apps[i].Top, Apps[i].Bottom);
                _names[i] = Words(11.5, _panel.Text, false, TextAlignment.Center, true);
                _names[i].Text = Loc.T(Apps[i].Name);
                Marks.Children.Add(_icons[i]);
                Marks.Children.Add(_names[i]);
            }
            _user = Words(14, _shelf.Text, true);
            _user.Text = Loc.T("User");
            _initial.Text = Loc.T("U");
            _divider.Fill = _shelf.Divider;
            foreach (var e in new UIElement[] { _avatar, _initial, _user, _divider }) Marks.Children.Add(e);
            _buttons =
            [
                new Shelf { Id = "power", Mark = Symbol("\uE7E8", 16, _shelf.Text) },
                new Shelf { Id = "tray", Mark = Symbol("\uE70E", 16, _shelf.Text) },
                new Shelf { Id = "battery", Mark = Symbol("", 16, _shelf.Text), Label = Words(12, _shelf.Sub) },
                new Shelf { Id = "volume", Mark = Symbol("\uE994", 16, _shelf.Text) },
                new Shelf { Id = "network", Mark = new WifiFan() },
            ];
            foreach (var b in _buttons)
            {
                Marks.Children.Add(b.Mark);
                if (b.Label != null) Marks.Children.Add(b.Label);
            }
        }

        private static Size Panel(int size, int icon) => new(Cols[size] * CellW[icon] + 2 * GridPad, GridTop + Rows[size] * CellH[icon] + Foot);

        private int Chosen => M.CurrentSettings.StartSize switch { "small" => 0, "large" => 2, "xlarge" => 3, _ => 1 };
        private int ChosenIcon => M.CurrentSettings.StartIcon switch { "small" => 0, "large" => 2, "xlarge" => 3, _ => 1 };
        protected override bool IsOpen => true;
        // The scene keeps one scale whichever size is chosen, that of the large menu (large grid,
        // large icons): the bar stays put and the menu grows or shrinks beside it. A larger menu is
        // drawn smaller as a whole (MenuScale) to stay in the stage.
        protected override Size PopupSize => new(Panel(2, 2).Width, Panel(2, 2).Height + Gap + BarHeight);
        protected override Vector2 Anchor => new((float)(Bar!.StartX + Bar.P / 2), (float)PopupBottom);

        // The menu's own scale (stage DIP per menu DIP): the scene's, less where the panel would not fit.
        private double MenuScale => S * Math.Min(1, Math.Min((W - 32) / Math.Max(1, _pw * S), (PopupBottom - 8) / Math.Max(1, (_mh + Gap + BarHeight) * S)));

        // On the bar's left end or in the middle of the screen, as in PopupRect; keep the panel inside the stage.
        private double MenuLeft
        {
            get
            {
                double w = _pw * MenuScale, left = _centered ? (W - w) / 2 : Bar!.BarX;
                return Math.Clamp(left, 16, Math.Max(16, W - 16 - w));
            }
        }

        protected override void Entered()
        {
            _size = Chosen;
            _icon = ChosenIcon;
            (_pw, _mh) = (Panel(_size, _icon).Width, Panel(_size, _icon).Height);
            _pv = _mv = 0;
            _centered = _wantCentered = M.CurrentSettings.StartCentered;
            _placed = false;
            foreach (var b in _buttons) b.Placed = false;
            _dividerA = -1;
            _staged = double.MaxValue; // the tiles come in with the menu
        }

        protected override void PoppedIn() => _staged = Now;

        // The size chosen: the panel springs there (Step); before the glass is first shown, it is there at once.
        protected override void Refreshed()
        {
            if (!HasFrame) Entered();
            _size = Chosen;
            _icon = ChosenIcon;
            // Moved to the middle or back: it closes and opens there again, out of the start button.
            if (_wantCentered != M.CurrentSettings.StartCentered)
            {
                _wantCentered = M.CurrentSettings.StartCentered;
                Replay(() => _centered = _wantCentered);
            }
        }

        protected override bool Step(double dt)
        {
            var target = Panel(_size, _icon);
            if (_pw == target.Width && _mh == target.Height && _pv == 0 && _mv == 0) return false;
            ResizeSpring.Step(ref _pw, ref _pv, target.Width, dt / 1000);
            ResizeSpring.Step(ref _mh, ref _mv, target.Height, dt / 1000);
            if (Math.Abs(_pw - target.Width) < 0.25 && Math.Abs(_mh - target.Height) < 0.25 && Math.Abs(_pv) < 2 && Math.Abs(_mv) < 2)
            {
                (_pw, _mh) = (target.Width, target.Height);
                _pv = _mv = 0;
            }
            return true;
        }

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double left = MenuLeft, bottom = PopupBottom, bh = BarHeight, k = MenuScale;
            return (new[]
            {
                new GlassTile(left, bottom - (_mh + Gap + bh) * k, _pw * k, _mh * k, Radius * k),
                new GlassTile(left, bottom - bh * k, _pw * k, bh * k, bh * k / 2),
            }, new Frame(left, bottom, _pw, _mh, k));
        }

        protected override void Ink(int piece, bool dark) => (piece == 0 ? _panel : _shelf).Set(dark);

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Bottom, f.Scale);
            double bh = BarHeight, top = -(f.MainH + Gap + bh), w = f.PanelW;
            bool moving = false;

            // Search (PaintSearch), and the way to all the apps beside it.
            double allL = w - 24 - 108;
            Put(_search, 24, top + 20, allL - 8 - 24, 44);
            Box(_searchGlyph, 34, top + 20, 34, 44);
            Box(_hint, 24 + 46, top + 20, Math.Max(0, allL - 8 - 48 - 70), 44);
            Put(_caret, 24 + 46 + 1 - 0.75, top + 32, 1.5, 20);
            Put(_all, allL, top + 20, 108, 44);
            double tw = TextWidth(Loc.T("All apps"), 13), gx = allL + (108 - (16 + 6 + tw)) / 2;
            Box(_allGlyph, gx, top + 20, 16, 44);
            Box(_allLabel, gx + 22, top + 20, tw + 4, 44);

            // The tiles (PaintTiles): their cells for the chosen sizes, gliding there.
            int cols = Cols[_size];
            double cw = CellW[_icon], ch = CellH[_icon], ip = IconPx[_icon];
            double gridX = (w - cols * cw) / 2;
            for (int i = 0; i < Apps.Length; i++)
            {
                double hx = gridX + i % cols * cw, hy = GridTop + i / cols * ch;
                if (!_placed) (_x[i], _y[i]) = (hx, hy);
                _x[i] = Approach(_x[i], hx, dt, 16);
                _y[i] = Approach(_y[i], hy, dt, 16);
                if (Math.Abs(_x[i] - hx) < 0.3) _x[i] = hx; else moving = true;
                if (Math.Abs(_y[i] - hy) < 0.3) _y[i] = hy; else moving = true;
                // In one after another (kStageMs 260, kStaggerMs 12), rising 10 DIP.
                double e = Motion.EaseOut((Now - _staged - 12 * Math.Min(i, 18)) / 260);
                if (e < 1) moving = true;
                double x = _x[i], y = top + _y[i] + 10 * (1 - e);
                Put(_icons[i], x + cw / 2 - ip / 2, y + 8, ip, ip);
                Box(_names[i], x + 2, y + 8 + ip + 4, cw - 4, 38);
                _icons[i].Opacity = _names[i].Opacity = e;
            }
            _placed = true;
            moving |= ArrangeShelf(w, bh, dt);
            return moving;
        }

        // The capsule (PaintShelf): the account on the left; from the right, power, the tray (startTray),
        // battery, volume and network (startStatus), round buttons concentric with the capsule.
        private bool ArrangeShelf(double w, double barH, double dt)
        {
            double bh = Math.Min(44, barH - 12), edge = (barH - bh) / 2, cy = -barH / 2;
            var s = M.CurrentSettings;
            var look = StatusLook.Current();
            bool moving = false;
            double r = edge, dividerAt = -1;
            foreach (var b in _buttons)
            {
                b.On = b.Id switch
                {
                    "tray" => s.StartTray,
                    "battery" => s.StartStatus && look.Battery,
                    "volume" or "network" => s.StartStatus,
                    _ => true,
                };
                if (b.Id == "battery" && look.Battery)
                {
                    string label = $"{look.Percent}%";
                    b.Label!.Text = label;
                    ((TextBlock)b.Mark).Text = ((char)((look.Charging ? 0xEBAB : 0xEBA0) + Math.Clamp(look.Percent / 10, 0, 10))).ToString();
                    b.Width = 12 + 20 + 4 + TextWidth(label, 12) + 4 + 12;
                }
                else
                {
                    b.Width = bh;
                }

                if (b.Id == "tray" && b.On)
                {
                    dividerAt = r + 4;
                    r += 8;
                }
                double target = r;
                if (b.On) r += b.Width + 2;
                if (!b.Placed) (b.Right, b.Alpha, b.Placed) = (target, b.On ? 1 : 0, true);
                if (b.On) b.Right = Approach(b.Right, target, dt, 16);
                b.Alpha = Approach(b.Alpha, b.On ? 1 : 0, dt, b.On ? 20 : 11);
                if (Math.Abs(b.Right - target) < 0.3) b.Right = target; else if (b.On) moving = true;
                if (Math.Abs(b.Alpha - (b.On ? 1 : 0)) < 0.01) b.Alpha = b.On ? 1 : 0; else moving = true;

                double left = w - b.Right - b.Width;
                b.Mark.Opacity = b.Alpha;
                b.Mark.Visibility = b.Alpha > 0.005 ? Visibility.Visible : Visibility.Collapsed;
                if (b.Mark is WifiFan fan)
                {
                    var on = _shelf.Text.Color;
                    fan.Update(left + bh / 2, cy, 12, 3, on, PanelInk.A(on, 0.28));
                }
                else
                {
                    Box((TextBlock)b.Mark, b.Label != null ? left + 10 : left, cy - bh / 2, b.Label != null ? 24 : bh, bh);
                }

                if (b.Label != null)
                {
                    b.Label.Opacity = b.Alpha;
                    b.Label.Visibility = b.Mark.Visibility;
                    Box(b.Label, left + 36, cy - bh / 2, Math.Max(0, b.Width - 44), bh);
                }
            }
            // The tray's divider, before its button.
            bool divider = dividerAt >= 0;
            if (_dividerA < 0) (_dividerA, _dividerX) = (divider ? 1 : 0, Math.Max(dividerAt, 0));
            if (divider) _dividerX = Approach(_dividerX, dividerAt, dt, 16);
            _dividerA = Approach(_dividerA, divider ? 1 : 0, dt, divider ? 20 : 11);
            if (Math.Abs(_dividerA - (divider ? 1 : 0)) < 0.01) _dividerA = divider ? 1 : 0; else moving = true;
            if (divider && Math.Abs(_dividerX - dividerAt) >= 0.3) moving = true;
            _divider.Opacity = _dividerA;
            Put(_divider, w - _dividerX - 0.5, cy - 12, 1, 24);

            // The account, in what is left.
            double x0 = edge, room = w - r;
            double nw = Math.Clamp(TextWidth(_user.Text, 14, true), 0, Math.Max(0, room - x0 - 70));
            Put(_avatar, x0 + 8, cy - 14, 28, 28);
            Box(_initial, x0 + 8, cy - 14, 28, 28);
            Box(_user, x0 + 36 + 10, cy - barH / 2, nw + 2, barH);
            return moving;
        }
    }

    // ---- The status entry: the control centre it opens, and the ring as it follows the levels ----

    private sealed class StatusScene : ModuleScene
    {
        private const double CW = 340, CGap = 10, TopH = 150, SliderH = 68, ModuleH = 58, Pad = 14;
        private const double Loop = 12000, LevelShowMs = 1500;
        // Pieces: connections, battery, casting, brightness, sound, then the modules' tiles in their order.
        private readonly PanelInk[] _inks = [.. Enumerable.Range(0, 5 + BarSettings.ControlModuleIds.Length).Select(_ => new PanelInk())];
        // Connections.
        private readonly Ellipse[] _discs = [new() { Fill = AccentBrush, IsHitTestVisible = false }, new() { Fill = AccentBrush, IsHitTestVisible = false }];
        private readonly WifiFan _wifi = new();
        private readonly TextBlock _bt = Symbol("\uE702", 15, WhiteInk);
        private readonly TextBlock[] _rowTitle = new TextBlock[2], _rowSub = new TextBlock[2];
        // Battery (or power), casting, Stage Manager.
        private readonly Path _battTrack = RingPath(4), _battLevel = RingPath(4);
        private readonly Ellipse _powerDisc = new() { IsHitTestVisible = false }, _castDisc = new() { IsHitTestVisible = false };
        private readonly TextBlock _battGlyph, _battTitle, _battSub, _castGlyph, _castTitle, _castSub;
        // The optional modules (controlModules), a tile for each place.
        private readonly ModuleTile[] _tiles;
        // The sliders.
        private readonly LevelRow _bright, _sound;
        private readonly Border _chevron; private readonly TextBlock _chevronGlyph;
        // The levels the demonstration moves, and the ring's motion (BarPainter's StatusMotion).
        private readonly Microsoft.UI.Dispatching.DispatcherQueueTimer _tick;
        private StatusLook? _lastReal;
        private double _start, _volumeUntil, _brightUntil, _volumeMix, _brightMix, _fill = -1, _speed;
        private int _volume = 40, _brightness = 45;

        private sealed record Frame(double Left, double Top);
        private sealed class LevelRow { public required TextBlock Title, Value, Glyph; public required Border Track, Fill; }
        private sealed class ModuleTile { public required Ellipse Disc; public required TextBlock Glyph, Title, Sub; }

        public StatusScene(MainWindow m) : base(m, m.StatusStage, m.StatusCanvas)
        {
            var conn = _inks[0];
            foreach (var d in _discs) Marks.Children.Add(d);
            Marks.Children.Add(_wifi);
            Marks.Children.Add(_bt);
            for (int i = 0; i < 2; i++)
            {
                Marks.Children.Add(_rowTitle[i] = Words(13.5, conn.Text, true));
                Marks.Children.Add(_rowSub[i] = Words(11.5, conn.Sub));
            }
            _rowTitle[0].Text = "Wi-Fi";
            _rowSub[0].Text = Loc.T("Connected");
            _rowTitle[1].Text = Loc.T("Bluetooth");
            _rowSub[1].Text = Loc.T("On");
            var batt = _inks[1];
            _battTrack.Stroke = batt.Ring;
            _powerDisc.Fill = batt.Quiet;
            _battGlyph = Symbol("\uE945", 11, batt.Text);
            _battTitle = Words(15, batt.Text, true);
            _battSub = Words(11.5, batt.Sub);
            foreach (var e in new UIElement[] { _powerDisc, _battTrack, _battLevel, _battGlyph, _battTitle, _battSub }) Marks.Children.Add(e);
            var cast = _inks[2];
            _castDisc.Fill = cast.Quiet;
            _castGlyph = Symbol("\uE7F4", 13, cast.Text);
            _castTitle = Words(13.5, cast.Text, true);
            _castTitle.Text = Loc.T("Cast");
            _castSub = Words(11.5, cast.Sub);
            _castSub.Text = Loc.T("Wireless display");
            foreach (var e in new UIElement[] { _castDisc, _castGlyph, _castTitle, _castSub }) Marks.Children.Add(e);
            _tiles = [.. Enumerable.Range(0, BarSettings.ControlModuleIds.Length).Select(i =>
            {
                var ink = _inks[5 + i];
                var tile = new ModuleTile
                {
                    Disc = new Ellipse { IsHitTestVisible = false }, Glyph = Symbol("", 14, ink.Text),
                    Title = Words(13.5, ink.Text, true), Sub = Words(11.5, ink.Sub),
                };
                foreach (var e in new UIElement[] { tile.Disc, tile.Glyph, tile.Title, tile.Sub }) Marks.Children.Add(e);
                return tile;
            })];
            _bright = NewRow(_inks[3], Loc.T("Displays"), "\uE706");
            _sound = NewRow(_inks[4], Loc.T("Sound"), "\uE994");
            _chevron = Plate(_inks[4].Chevron, 12);
            _chevronGlyph = Symbol("\uE76C", 9.5, _inks[4].Text);
            Marks.Children.Add(_chevron);
            Marks.Children.Add(_chevronGlyph);
            _tick = m.DispatcherQueue.CreateTimer();
            _tick.Interval = TimeSpan.FromMilliseconds(250);
            _tick.Tick += (_, _) => { if (NeedsKick(Environment.TickCount64, StatusLook.Current())) Kick(); };
        }

        private LevelRow NewRow(PanelInk ink, string title, string glyph)
        {
            var row = new LevelRow
            {
                Title = Words(12.5, ink.Text, true),
                Value = Words(11.5, ink.Sub, false, TextAlignment.Right),
                Glyph = Symbol(glyph, 11, ink.OnFill),
                Track = Plate(ink.Track, 12),
                Fill = Plate(ink.Level, 12),
            };
            row.Title.Text = title;
            foreach (var e in new UIElement[] { row.Title, row.Value, row.Track, row.Fill, row.Glyph }) Marks.Children.Add(e);
            return row;
        }

        private List<string> Modules => M.CurrentSettings.ControlModules;
        private static double ModulesHeight(int count) => (count + 1) / 2 * (ModuleH + CGap); // ControlCenter::ModulesHeight
        // ControlCenter::MainHeight, with the modules' rows as they glide.
        private double MainH => TopH + CGap + Math.Max(0, _modH) + SliderH + CGap + SliderH;
        protected override bool IsOpen => M.CurrentSettings.ShowStatus;
        // As tall as it gets (every module): the scene keeps its scale as modules come and go.
        protected override Size PopupSize => new(CW, TopH + CGap + ModulesHeight(BarSettings.ControlModuleIds.Length) + SliderH + CGap + SliderH);
        protected override Vector2 Anchor => new((float)(Bar!.StatusX >= 0 ? Bar.StatusX + (Bar.P + Bar.StatusExtra) / 2 : BarRight), (float)PopupBottom);

        // The modules in motion: a module added grows out of its place, one taken away shrinks into it, the
        // others glide to their new places and the panel grows or shrinks with its rows (motion::kPop).
        private sealed class ModuleMotion { public double X, Y, W, Vx, Vy, Vw, Grow, GrowV; public bool Leaving; }
        private static readonly Spring ModuleSpring = new(0.36, 0.62);
        private readonly Dictionary<string, ModuleMotion> _moving = [];
        private readonly List<string> _drawn = []; // the modules drawn, leaving ones included, in order
        private double _modH = -1, _modV;

        // ControlCenter::ModuleRects: two to a row under the casting, one alone in its row takes it whole.
        private static Rect[] ModuleTargets(int n)
        {
            double col = (CW - CGap) / 2;
            return [.. Enumerable.Range(0, n).Select(i => new Rect(i % 2 == 1 ? col + CGap : 0, TopH + CGap + i / 2 * (ModuleH + CGap),
                                                                   i + 1 == n && i % 2 == 0 ? CW : col, ModuleH))];
        }

        protected override bool Step(double dt)
        {
            var targets = ModuleTargets(Modules.Count);
            bool first = _modH < 0, moving = false;
            for (int i = 0; i < Modules.Count; i++)
            {
                if (_moving.TryGetValue(Modules[i], out var m)) { m.Leaving = false; continue; }
                var t = targets[i];
                _moving[Modules[i]] = new ModuleMotion { X = t.X, Y = t.Y, W = t.Width, Grow = first ? 1 : 0 };
            }
            foreach (var (id, m) in _moving) m.Leaving = !Modules.Contains(id);
            _drawn.Clear();
            _drawn.AddRange(Modules);
            _drawn.AddRange(_moving.Keys.Where(id => !Modules.Contains(id)));
            double s = dt / 1000;
            foreach (var (id, m) in _moving)
            {
                int i = Modules.IndexOf(id);
                if (i >= 0)
                {
                    ModuleSpring.Step(ref m.X, ref m.Vx, targets[i].X, s);
                    ModuleSpring.Step(ref m.Y, ref m.Vy, targets[i].Y, s);
                    ModuleSpring.Step(ref m.W, ref m.Vw, targets[i].Width, s);
                }
                ModuleSpring.Step(ref m.Grow, ref m.GrowV, m.Leaving ? 0 : 1, s);
                moving |= Math.Abs(m.GrowV) > 0.002 || Math.Abs(m.Vx) + Math.Abs(m.Vy) + Math.Abs(m.Vw) > 0.05 ||
                          Math.Abs(m.Grow - (m.Leaving ? 0 : 1)) > 0.002 ||
                          (i >= 0 && Math.Abs(m.X - targets[i].X) + Math.Abs(m.Y - targets[i].Y) + Math.Abs(m.W - targets[i].Width) > 0.05);
            }
            foreach (string id in _moving.Where(kv => kv.Value.Leaving && kv.Value.Grow < 0.02).Select(kv => kv.Key).ToList()) _moving.Remove(id);
            double goal = ModulesHeight(Modules.Count);
            if (first) _modH = goal;
            ModuleSpring.Step(ref _modH, ref _modV, goal, s);
            if (Math.Abs(_modH - goal) < 0.05 && Math.Abs(_modV) < 0.05) (_modH, _modV) = (goal, 0);
            else moving = true;
            return moving;
        }

        // A module's glass now: its place, grown by its share.
        private Rect ModuleNow(string id)
        {
            if (!_moving.TryGetValue(id, out var m)) return Rect.Empty;
            double g = Math.Clamp(m.Grow, 0, 1.08), w = m.W * g, h = ModuleH * g;
            return new Rect(m.X + (m.W - w) / 2, m.Y + (ModuleH - h) / 2, Math.Max(0.01, w), Math.Max(0.01, h));
        }

        // ControlCenter::MainShapes: connections; battery over casting; brightness; sound; then the modules.
        private Rect[] Shapes
        {
            get
            {
                double col = (CW - CGap) / 2, sliders = TopH + CGap + Math.Max(0, _modH);
                var shapes = new List<Rect>
                {
                    new(0, 0, col, TopH), new(col + CGap, 0, col, (TopH - CGap) / 2), new(col + CGap, (TopH - CGap) / 2 + CGap, col, (TopH - CGap) / 2),
                    new(0, sliders, CW, SliderH), new(0, sliders + SliderH + CGap, CW, SliderH),
                };
                shapes.AddRange(_drawn.Select(ModuleNow).Where(r => !r.IsEmpty && r.Width > 1));
                return [.. shapes];
            }
        }

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double left = BarRight - CW * S, top = PopupBottom - MainH * S, r = Math.Min(Radius, 22) * S;
            return (Shapes.Select(s => new GlassTile(left + s.X * S, top + s.Y * S, s.Width * S, s.Height * S, r)).ToArray(), new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => _inks[piece].Set(dark);

        protected override void Entered()
        {
            (_start, _volumeUntil, _brightUntil, _volumeMix, _brightMix, _speed) = (Environment.TickCount64, 0, 0, 0, 0, 0);
            (_fill, _volume, _brightness, _lastReal) = (-1, 40, 45, null);
            (_modH, _modV) = (-1, 0); // the modules as they are, without a glide
            _moving.Clear();
            Step(0);
            if (!M._scenesPaused) _tick.Start();
        }

        protected override void Left() => _tick.Stop(); protected override void Paused() => _tick.Stop(); protected override void Resumed() => _tick.Start();

        // The demonstration: the volume up, the brightness up, the volume down, the brightness down,
        // as dragging the sliders does; each change shows on the ring for a while.
        private static double Ramp(double t, double at, double from, double to)
        { double u = Math.Clamp((t - at) / 700, 0, 1); return from + (to - from) * u * u * (3 - 2 * u); }

        private static (int Volume, int Brightness) DemoLevels(double t, bool laptop) =>
            ((int)Math.Round(t < 4000 ? Ramp(t, 800, 40, 80) : Ramp(t, 6400, 80, 40)),
             laptop ? (int)Math.Round(t < 7000 ? Ramp(t, 3600, 45, 85) : Ramp(t, 9200, 85, 45)) : 45);

        private static bool InRamp(double t, bool laptop) =>
            (t >= 800 && t <= 1500) || (t >= 6400 && t <= 7100) || (laptop && ((t >= 3600 && t <= 4300) || (t >= 9200 && t <= 9900)));

        private bool NeedsKick(long now, StatusLook real)
        {
            if (Ticking) return false;
            if (!HasFrame || real != _lastReal || _volumeMix > 0.001 || _brightMix > 0.001 || Math.Abs(_speed) > 0.001) return true;
            if ((_volumeUntil > 0 && now >= _volumeUntil) || (_brightUntil > 0 && now >= _brightUntil)) return true;
            if (!IsOpen) return false;
            double t = Math.Max(0, now - _start) % Loop;
            return InRamp(t, real.Battery) || DemoLevels(t, real.Battery) != (_volume, _brightness);
        }

        protected override StatusLook BarStatus() => Look();

        private StatusLook Look() => StatusLook.Current() is var r
            ? r with { Fill = _fill < 0 ? r.Fill : _fill, Volume = _volume, VolumeMix = _volumeMix, BrightMix = _brightMix } : null!;

        private static bool ApproachSnap(ref double v, double to, double dt)
        {
            v = Approach(v, to, dt, 12);
            if (Math.Abs(v - to) >= 0.001) return true;
            v = to; return false;
        }

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            var real = StatusLook.Current();
            _lastReal = real;
            bool laptop = real.Battery, moving = false;
            double t = Math.Max(0, Now - _start) % Loop;
            if (IsOpen)
            {
                var (volume, brightness) = DemoLevels(t, laptop);
                if (volume != _volume) { _volumeUntil = Now + LevelShowMs; _brightUntil = 0; moving = true; }
                if (brightness != _brightness) { _brightUntil = Now + LevelShowMs; _volumeUntil = 0; moving = true; }
                (_volume, _brightness) = (volume, brightness);
                if (InRamp(t, laptop)) moving = true;
            }
            bool showVolume = Now < _volumeUntil, showBright = Now < _brightUntil;
            if (!showVolume) _volumeUntil = 0;
            if (!showBright) _brightUntil = 0;
            moving |= ApproachSnap(ref _volumeMix, showVolume ? 1 : 0, dt);
            moving |= ApproachSnap(ref _brightMix, showBright ? 1 : 0, dt);
            double target = showBright ? _brightness / 100.0 : (showVolume || !laptop) ? _volume / 100.0 : real.Fill;
            if (_fill < 0) _fill = target;
            SmoothSpring.Step(ref _fill, ref _speed, target, dt / 1000);
            if (Math.Abs(_fill - target) >= 0.001 || Math.Abs(_speed) >= 0.001) moving = true;
            else (_fill, _speed) = (target, 0);
            UpdateBarStatus(Look());

            ArrangeConnections();
            ArrangeTiles(real);
            ArrangeRow(_bright, Shapes[3], _brightness, laptop, laptop ? $"{_brightness}%" : Loc.T("Not adjustable"), _inks[3], false);
            _sound.Glyph.Text = StatusGlyph.VolumeGlyph(Look());
            ArrangeRow(_sound, Shapes[4], _volume, true, $"{_volume}%", _inks[4], true);
            return moving;
        }

        private void ArrangeConnections()
        {
            var c = Shapes[0];
            double rh = (TopH - 16) / 2;
            for (int i = 0; i < 2; i++)
            {
                double cy = c.Y + 8 + i * rh + rh / 2;
                Put(_discs[i], c.X + 14, cy - 18, 36, 36);
                Box(_rowTitle[i], c.X + 60, cy - 18, c.Width - 70, 18);
                Box(_rowSub[i], c.X + 60, cy, c.Width - 70, 18);
                if (i == 0) _wifi.Update(c.X + 32, cy, 13, 3, WhiteInk.Color, WhiteInk.Color);
                else Box(_bt, c.X + 14, cy - 18, 36, 36);
            }
        }

        private void ArrangeTiles(StatusLook real)
        {
            // Battery: a small ring, the level and the state; a desktop's power instead.
            var b = Shapes[1];
            var ink = _inks[1];
            double cx = b.X + 32, cy = b.Y + b.Height / 2;
            _powerDisc.Visibility = real.Battery ? Visibility.Collapsed : Visibility.Visible;
            _battTrack.Visibility = _battLevel.Visibility = real.Battery ? Visibility.Visible : Visibility.Collapsed;
            if (real.Battery)
            {
                int pct = Math.Clamp(real.Percent, 0, 100);
                _battTrack.Data = new EllipseGeometry { Center = new Point(cx, cy), RadiusX = 15, RadiusY = 15 };
                _battLevel.Data = StatusGlyph.Arc(cx, cy, 15, -90, Math.Max(0.01, Math.Min(359.99, 360 * pct / 100.0)));
                _battLevel.Stroke = real.Charging ? new SolidColorBrush(Color.FromArgb(255, 0x30, 0xD1, 0x58))
                                  : pct <= 20 ? new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x45, 0x3A)) : ink.Text;
                _battGlyph.Visibility = real.Charging ? Visibility.Visible : Visibility.Collapsed;
                _battGlyph.FontSize = 11;
                Box(_battGlyph, cx - 12, cy - 12, 24, 24);
                _battTitle.FontSize = 15;
                _battTitle.Text = $"{pct}%";
                _battSub.Text = real.Charging ? (pct >= 100 ? Loc.T("Fully charged") : Loc.T("Charging")) : Loc.T("On battery");
            }
            else
            {
                Put(_powerDisc, cx - 16, cy - 16, 32, 32);
                _battGlyph.Visibility = Visibility.Visible;
                _battGlyph.FontSize = 13;
                Box(_battGlyph, cx - 14, cy - 14, 28, 28);
                _battTitle.FontSize = 13.5;
                _battTitle.Text = Loc.T("Power");
                _battSub.Text = Loc.T("Plugged in");
            }
            Box(_battTitle, cx + 24, cy - 18, b.Right - 8 - cx - 24, 19);
            Box(_battSub, cx + 24, cy + 1, b.Right - 8 - cx - 24, 17);

            // Casting.
            var k = Shapes[2];
            double kx = k.X + 32, ky = k.Y + k.Height / 2;
            Put(_castDisc, kx - 16, ky - 16, 32, 32);
            Box(_castGlyph, kx - 14, ky - 14, 28, 28);
            Box(_castTitle, kx + 24, ky - 18, k.Right - 8 - kx - 24, 19);
            Box(_castSub, kx + 24, ky + 1, k.Right - 8 - kx - 24, 17);

            ArrangeModules();
        }

        // ControlCenter::PaintModules: a round switch, its name and state. Stage Manager's shows the setting; the
        // system's switches are shown as they would be with nothing read yet: off.
        private void ArrangeModules()
        {
            var shown = _drawn.Where(id => ModuleNow(id) is { IsEmpty: false, Width: > 1 }).ToList();
            for (int i = 0; i < _tiles.Length; i++)
            {
                var tile = _tiles[i];
                bool visible = i < shown.Count;
                foreach (var e in new UIElement[] { tile.Disc, tile.Glyph, tile.Title, tile.Sub })
                    e.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
                if (!visible) continue;
                string id = shown[i];
                var r = ModuleNow(id);
                var ink = _inks[5 + i];
                double fade = Math.Clamp((_moving[id].Grow - 0.35) / 0.65, 0, 1);
                foreach (var e in new UIElement[] { tile.Disc, tile.Glyph, tile.Title, tile.Sub }) e.Opacity = fade;
                bool stage = id == "stage", on = stage && M.CurrentSettings.StageManager, wide = r.Width > CW / 2;
                double cx = r.X + 32, cy = r.Y + r.Height / 2;
                Put(tile.Disc, cx - 18, cy - 18, 36, 36);
                tile.Disc.Fill = on ? AccentBrush : ink.Quiet;
                tile.Glyph.Text = ModuleGlyph(id);
                tile.Glyph.Foreground = on ? WhiteInk : ink.Text;
                Box(tile.Glyph, cx - 14, cy - 14, 28, 28);
                tile.Title.Text = Loc.T(ModuleTitle(id));
                tile.Sub.Text = !on ? Loc.T("Off")
                    : !wide ? Loc.T("On")
                    : Loc.T(M.CurrentSettings.StageOnRight ? "On · recent apps on the right" : "On · recent apps on the left");
                Box(tile.Title, cx + 28, cy - 18, Math.Max(0, r.Right - Pad - cx - 28), 18);
                Box(tile.Sub, cx + 28, cy, Math.Max(0, r.Right - Pad - cx - 28), 18);
            }
        }

        // PaintSlider: the title, the value on the right, a wide pill filled from the left with the symbol at its start.
        private void ArrangeRow(LevelRow row, Rect card, int value, bool enabled, string right, PanelInk ink, bool more)
        {
            row.Title.Foreground = enabled ? ink.Text : ink.Disabled;
            row.Value.Foreground = enabled ? ink.Sub : ink.Disabled;
            row.Value.Text = right;
            Box(row.Title, card.X + Pad, card.Y + 8, card.Width - 130 - Pad, 20);
            Box(row.Value, card.Right - 130, card.Y + 8, 130 - Pad - (more ? 30 : 0), 20);
            var track = new Rect(card.X + Pad, card.Y + 34, card.Width - 2 * Pad, 24);
            Put(row.Track, track.X, track.Y, track.Width, track.Height);
            row.Track.Background = enabled ? ink.Track : ink.DisabledTrack;
            row.Fill.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
            double k = Math.Clamp(value, 0, 100) / 100.0;
            Put(row.Fill, track.X, track.Y, track.Height + (track.Width - track.Height) * k, track.Height);
            row.Glyph.Foreground = enabled ? ink.OnFill : ink.DisabledGlyph;
            Box(row.Glyph, track.X + 2, track.Y, track.Height, track.Height);
            if (more)
            {
                Put(_chevron, card.Right - Pad - 24, card.Y + 6, 24, 24);
                Box(_chevronGlyph, card.Right - Pad - 24, card.Y + 6, 24, 24);
            }
        }
    }
}
