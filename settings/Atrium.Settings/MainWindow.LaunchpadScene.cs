using System.Numerics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;

namespace Atrium.Settings;

// Launchpad's preview (ui/launchpad.cpp, drawn smaller): its window of glass in the middle of the
// screen (under a black veil, light ink, with the original color), the search field, a page of apps that
// settle from a little larger as it opens, the page dots.
public sealed partial class MainWindow
{
    private sealed class LaunchpadScene : ModuleScene
    {
        private const int Cols = 6, Rows = 3;
        private const double CellW = 92, CellH = 88, IconPx = 44, SideW = 40, SearchTop = 22, SearchW = 220, SearchH = 30;
        private const double GridTop = 74, DotsFromBottom = 22, MinRadius = 26, OpenMs = 300;
        private const double CW = Cols * CellW + 2 * SideW, CH = GridTop + Rows * CellH + 2 * DotsFromBottom;
        // A page of apps, drawn here: a name, a symbol, a colour.
        private static readonly (string Name, string Glyph, uint Top, uint Bottom)[] Apps =
        [
            ("File Explorer", "\uE8B7", 0xFFD66B, 0xF2A922), ("Browser", "\uE774", 0x5AC8FA, 0x0A84FF), ("Mail", "\uE715", 0x6AAEFF, 0x2D6BEA),
            ("Calendar", "\uE787", 0xFF7A70, 0xE5383B), ("Photos", "\uEB9F", 0xFFB86B, 0xF0623A), ("Music", "\uE8D6", 0xFF6F91, 0xE0245E),
            ("Settings", "\uE713", 0xA7AFBC, 0x67707D), ("Notepad", "\uE70B", 0x7DD3C0, 0x2BA58C), ("Calculator", "\uE8EF", 0x8E9AAF, 0x4A5568),
            ("Maps", "\uE707", 0x7CDB8A, 0x30B158), ("Camera", "\uE722", 0x9DA6B5, 0x515A68), ("Clock", "\uE823", 0xB39DFF, 0x6E56CF),
            ("Weather", "\uE706", 0x8CD3FF, 0x3A8DDE), ("Store", "\uE719", 0x6FA8FF, 0x3355D6), ("Terminal", "\uE756", 0x5C6370, 0x2B2F36),
            ("Videos", "\uE714", 0xC58CFF, 0x8B3FE0), ("Notes", "\uE70B", 0xFFE27A, 0xF5C400), ("Contacts", "\uE77B", 0xB8C2CC, 0x7A8592),
        ];

        private readonly PanelInk _ink = new();
        private readonly Border _veil = new()
        {
            IsHitTestVisible = false,
            Background = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(115, 0, 0, 0)), // kVeil
        };
        private bool _glassDark;
        private readonly Border _search;
        private readonly TextBlock _searchGlyph, _hint;
        private readonly Grid[] _icons = new Grid[Apps.Length];
        private readonly TextBlock[] _names = new TextBlock[Apps.Length];
        private readonly Ellipse[] _dots = [new(), new(), new()];
        private double _opened = double.MaxValue;

        private sealed record Frame(double Left, double Top);

        public LaunchpadScene(MainWindow m, Border stage, Canvas root) : base(m, stage, root)
        {
            ContinuousCorners.SetEnabled(_veil, true);
            Marks.Children.Add(_veil);
            _search = Plate(_ink.Search, SearchH / 2);
            _search.BorderBrush = _ink.Border;
            _search.BorderThickness = new Thickness(1);
            _searchGlyph = Symbol("\uE721", 13, _ink.Sub);
            _hint = Words(13.5, _ink.Sub);
            _hint.Text = Loc.T("Search");
            foreach (var e in new UIElement[] { _search, _searchGlyph, _hint }) Marks.Children.Add(e);
            for (int i = 0; i < Apps.Length; i++)
            {
                _icons[i] = DrawnIcon(Apps[i].Glyph, Apps[i].Top, Apps[i].Bottom);
                _icons[i].RenderTransformOrigin = new Point(0.5, 0.5);
                _icons[i].RenderTransform = new ScaleTransform();
                _names[i] = Words(12.5, _ink.Text, false, TextAlignment.Center);
                _names[i].Text = Loc.T(Apps[i].Name);
                Marks.Children.Add(_icons[i]);
                Marks.Children.Add(_names[i]);
            }
            foreach (var d in _dots)
            {
                d.IsHitTestVisible = false;
                Marks.Children.Add(d);
            }
        }

        protected override bool IsOpen => M.CurrentSettings.ShowLaunchpad;
        protected override Size PopupSize => new(CW, CH);
        protected override Vector2 Anchor => new((float)(Bar!.LaunchX >= 0 ? Bar.LaunchX + Bar.P / 2 : Bar.StartX + Bar.P / 2), (float)PopupBottom);
        private bool Tint => M.CurrentSettings.LaunchpadMaterial == "tint";
        private bool Full => M.CurrentSettings.LaunchpadLayout == "fullscreen";
        protected override void Ink(int piece, bool dark) => _ink.Set((_glassDark = dark) && !Tint);
        protected override void Entered() => _opened = double.MaxValue;
        protected override void PoppedIn() => _opened = Now;

        // In the middle of the stage above the bar, as the window is in the middle of the screen; full screen,
        // the glass over all of the stage and the apps in its middle.
        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double w = CW * S, h = CH * S;
            double left = (W - w) / 2, top = Math.Max(12, (PopupBottom - h) / 2);
            var glass = Full ? new GlassTile(0, 0, W, H, 0) : new GlassTile(left, top, w, h, Math.Max(Radius, MinRadius) * S);
            return (new[] { glass }, new Frame(left, top));
        }

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            _ink.Set(_glassDark && !Tint);
            _veil.Visibility = Tint ? Visibility.Visible : Visibility.Collapsed;
            _veil.CornerRadius = new CornerRadius(Full ? 0 : Math.Max(Radius, MinRadius));
            if (Full) Put(_veil, -f.Left / S, -f.Top / S, W / S, H / S);
            else Put(_veil, 0, 0, CW, CH);
            Put(_search, (CW - SearchW) / 2, SearchTop, SearchW, SearchH);
            Box(_searchGlyph, (CW - SearchW) / 2 + 10, SearchTop, 20, SearchH);
            Box(_hint, (CW - SearchW) / 2 + 34, SearchTop, SearchW - 48, SearchH);
            // The apps settle from a little larger as the window fades in.
            double e = _opened == double.MaxValue ? 0 : Motion.EaseOut(Math.Clamp((Now - _opened) / OpenMs, 0, 1)), k = 1 + 0.08 * (1 - e);
            for (int i = 0; i < Apps.Length; i++)
            {
                double x = SideW + i % Cols * CellW, y = GridTop + i / Cols * CellH;
                double top = y + (CellH - IconPx - 8 - 16) / 2;
                Put(_icons[i], x + (CellW - IconPx) / 2, top, IconPx, IconPx);
                var scale = (ScaleTransform)_icons[i].RenderTransform;
                scale.ScaleX = scale.ScaleY = k;
                _icons[i].Opacity = e;
                Box(_names[i], x + 4, top + IconPx + 8, CellW - 8, 16);
                _names[i].Opacity = e;
            }
            // Three pages, the first shown.
            for (int i = 0; i < _dots.Length; i++)
            {
                _dots[i].Fill = i == 0 ? _ink.Text : _ink.Faint;
                Put(_dots[i], CW / 2 + (i - 1) * 18 - 4, CH - DotsFromBottom - 4, 8, 8);
            }
            return _opened == double.MaxValue || Now - _opened < OpenMs;
        }
    }
}
