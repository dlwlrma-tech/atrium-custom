using System.Numerics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    // ---- The input method: its entry on the bar, stepping through the languages ----

    // With the bubble on (imeBubble), each change also rises over the bar as the bubble does while the
    // bar is hidden: it swells up from below, floats, swells once more and bursts into drops, on the
    // core's timeline (native/core/ui/ime_bubble_model.h).
    private sealed class ImeScene : ModuleScene
    {
        private static readonly string[] Glyphs = ["中", "a", "A"];
        private const double EmergeMs = 360, FloatMs = 700, SwellMs = 46, BurstMs = 440; // kEmergeMs, kFloatMs, kSwellMs, kBurstMs
        private const double BubbleR = 32, BirthDip = 18, FloatRise = 10, RestOverBar = 70; // DIP
        private readonly Microsoft.UI.Dispatching.DispatcherQueueTimer _tick;
        private int _index;
        private string _prev = "";
        private double _changed;
        private double _born = -1; // the bubble's start, -1: none
        private readonly Canvas _bubble = new() { IsHitTestVisible = false };
        private readonly Ellipse _skin = new()
        {
            Fill = new SolidColorBrush(Color.FromArgb(0x55, 0xFF, 0xFF, 0xFF)),
            Stroke = new SolidColorBrush(Color.FromArgb(0xC0, 0xFF, 0xFF, 0xFF)),
            StrokeThickness = 1.2,
        };
        private readonly Ellipse _shine = new() { Fill = new SolidColorBrush(Color.FromArgb(0x90, 0xFF, 0xFF, 0xFF)) };
        private readonly TextBlock _letter = new()
        {
            Foreground = new SolidColorBrush(Color.FromArgb(0xFF, 0x1F, 0x23, 0x29)),
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            TextAlignment = TextAlignment.Center,
        };
        private readonly Ellipse[] _drops = new Ellipse[11];
        private readonly double[] _dropTurn = new double[11], _dropReach = new double[11], _dropSize = new double[11];
        private readonly Random _random = new();

        public ImeScene(MainWindow m) : base(m, m.ImeStage, m.ImeCanvas)
        {
            _bubble.Children.Add(_skin);
            _bubble.Children.Add(_shine);
            _bubble.Children.Add(_letter);
            for (int i = 0; i < _drops.Length; ++i)
                _bubble.Children.Add(_drops[i] = new Ellipse { Fill = new SolidColorBrush(Color.FromArgb(0xB0, 0xFF, 0xFF, 0xFF)) });
            m.ImeCanvas.Children.Add(_bubble); // over the bar
            _tick = m.DispatcherQueue.CreateTimer();
            _tick.Interval = TimeSpan.FromMilliseconds(1600);
            _tick.Tick += (_, _) =>
            {
                _prev = Glyphs[_index];
                _index = (_index + 1) % Glyphs.Length;
                Now = _changed = Environment.TickCount64;
                if (M.CurrentSettings.ImeStatus && M.CurrentSettings.ImeBubble) Burst();
                Kick();
            };
        }

        // A new bubble, its drops each burst of their own way.
        private void Burst()
        {
            _born = Now;
            for (int i = 0; i < _drops.Length; ++i)
            {
                _dropTurn[i] = (i + _random.NextDouble() * 0.6) / _drops.Length * 2 * Math.PI;
                _dropReach[i] = 22 + _random.NextDouble() * 22;
                _dropSize[i] = 2.5 + _random.NextDouble() * 3;
            }
        }

        private bool ArrangeBubble()
        {
            double t = Now - _born;
            if (_born < 0 || t >= EmergeMs + FloatMs + SwellMs + BurstMs || Bar == null)
            {
                _born = -1;
                _bubble.Opacity = 0;
                return false;
            }
            double cx = W / 2, rest = Bar.BarY - RestOverBar * S, r = BubbleR * S, y, k, skin = 1, burst = 0;
            if (t < EmergeMs)
            {
                double p = t / EmergeMs;
                y = rest + BirthDip * S * (1 - Motion.EaseOut(p));
                k = Math.Max(0, Motion.EaseOutBack(p, 1.7));
                skin = Math.Min(1, p * 3);
            }
            else if (t < EmergeMs + FloatMs)
            {
                double q = (t - EmergeMs) / FloatMs;
                y = rest - FloatRise * S * (q * q * (3 - 2 * q));
                k = 1 + 0.025 * Math.Sin(q * 3 * Math.PI); // it breathes
            }
            else
            {
                y = rest - FloatRise * S;
                double swell = Math.Min(1, (t - EmergeMs - FloatMs) / SwellMs);
                burst = Math.Clamp((t - EmergeMs - FloatMs - SwellMs) / BurstMs, 0, 1);
                k = 1 + 0.08 * swell + 0.25 * Motion.EaseOut(burst);
                skin = 1 - Motion.EaseOut(Math.Min(1, burst * 2.5)); // the skin tears at once
            }
            double rk = r * k;
            Place(_skin, cx - rk, y - rk, 2 * rk, 2 * rk);
            _skin.Opacity = skin;
            Place(_shine, cx - rk * 0.55, y - rk * 0.72, rk * 0.5, rk * 0.28);
            _shine.Opacity = skin;
            _letter.Text = Glyphs[_index];
            _letter.FontSize = Math.Max(1.0, 24 * S * k);
            Place(_letter, cx - rk, y - 16 * S * k, 2 * rk, 32 * S * k);
            _letter.Opacity = k <= 0.01 ? 0 : skin;
            for (int i = 0; i < _drops.Length; ++i)
            {
                // Thrown from the rim, slowing, drooping and dissolving.
                double reach = r * 1.08 + _dropReach[i] * S * Motion.EaseOut(burst), droop = 14 * S * burst * burst;
                double d = _dropSize[i] * S * (1 - 0.5 * burst);
                double dx = cx + Math.Cos(_dropTurn[i]) * reach, dy = y + Math.Sin(_dropTurn[i]) * reach + droop;
                Place(_drops[i], dx - d / 2, dy - d / 2, d, d);
                _drops[i].Opacity = burst > 0 ? 1 - burst : 0;
            }
            _bubble.Opacity = 1;
            return true;
        }

        private static void Place(FrameworkElement e, double x, double y, double w, double h)
        {
            Canvas.SetLeft(e, x);
            Canvas.SetTop(e, y);
            e.Width = Math.Max(0, w);
            e.Height = Math.Max(0, h);
        }

        protected override bool IsOpen => false;
        protected override Size PopupSize => new(1, 0);
        protected override Vector2 Anchor => new((float)(Bar!.ImeX >= 0 ? Bar.ImeX + Bar.P / 2 : BarRight), (float)PopupBottom);
        protected override (GlassTile[] Tiles, object State) Pieces() => (Array.Empty<GlassTile>(), 0);
        protected override void Ink(int piece, bool dark) { }

        protected override void Entered()
        {
            _index = 0;
            _prev = "";
            _changed = 0;
            _born = -1;
            _bubble.Opacity = 0;
            Face.ShowIme(Glyphs[0]);
            if (!M._scenesPaused) _tick.Start();
        }

        protected override void Left() => _tick.Stop(); protected override void Paused() => _tick.Stop(); protected override void Resumed() => _tick.Start();

        protected override bool Arrange(object state, double dt)
        {
            double e = _prev.Length == 0 ? 1 : Motion.EaseOut((Now - _changed) / 180);
            Face.ShowIme(Glyphs[_index], _prev, e);
            return ArrangeBubble() | e < 1;
        }
    }
}
