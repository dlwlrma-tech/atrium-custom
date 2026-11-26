using System.Numerics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    // ---- The time: the month over its capsule, from the clock ----

    private sealed class ClockScene : ModuleScene
    {
        private const double CW = 336, Pad = 14, MonthH = 48, WeekH = 26, CellH = 46, Footer = 40, CapGap = 10;
        private static readonly string[] WeekNames = ["一", "二", "三", "四", "五", "六", "日"];
        // Weekday and month names as the core writes them in English ("Tue, March 10").
        private static readonly string[] WeekShort = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
        private static readonly string[] Months =
        [
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December",
        ];
        private readonly PanelInk _card = new(), _cap = new();
        private readonly TextBlock _title, _prev, _next, _settings, _time, _date, _lunar;
        private readonly TextBlock[] _week = new TextBlock[7], _days = new TextBlock[42], _labels = new TextBlock[42];
        private readonly Border _today = Plate(AccentBrush, 12);
        private readonly Rectangle _line = new() { IsHitTestVisible = false };

        private sealed record Frame(double Left, double Top);

        public ClockScene(MainWindow m) : base(m, m.ClockStage, m.ClockCanvas)
        {
            _title = Words(16, _card.Text, true);
            _prev = Symbol("\uE76B", 11, _card.Text);
            _next = Symbol("\uE76C", 11, _card.Text);
            _settings = Words(12.5, _card.Sub, false, TextAlignment.Center);
            _settings.Text = Loc.T("Date & time settings");
            _line.Fill = _card.Divider;
            Marks.Children.Add(_today);
            foreach (var e in new UIElement[] { _title, _prev, _next, _line, _settings }) Marks.Children.Add(e);
            for (int i = 0; i < 7; i++) Marks.Children.Add(_week[i] = Words(12, _card.Faint, false, TextAlignment.Center));
            for (int i = 0; i < 42; i++)
            {
                Marks.Children.Add(_days[i] = Words(14.5, _card.Text, false, TextAlignment.Center));
                Marks.Children.Add(_labels[i] = Words(9.5, _card.Sub, false, TextAlignment.Center));
            }
            _time = Words(20, _cap.Text, true);
            _date = Words(13, _cap.Text, false, TextAlignment.Right);
            _lunar = Words(11.5, _cap.Sub, false, TextAlignment.Right);
            Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(_time, FontNumeralAlignment.Tabular);
            foreach (var e in new UIElement[] { _time, _date, _lunar }) Marks.Children.Add(e);
        }

        private static double CardH => MonthH + WeekH + 6 * CellH + 8 + Footer;
        protected override bool IsOpen => M.CurrentSettings.ShowClock;
        protected override Size PopupSize => new(CW, CardH + CapGap + BarHeight);
        protected override Vector2 Anchor => new((float)(Bar!.ClockX >= 0 ? Bar.ClockX + Bar.ClockW / 2 : BarRight), (float)PopupBottom);

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double bh = BarHeight, left = BarRight - CW * S, top = PopupBottom - (CardH + CapGap + bh) * S;
            return (new[]
            {
                new GlassTile(left, top, CW * S, CardH * S, Radius * S),
                new GlassTile(left, top + (CardH + CapGap) * S, CW * S, bh * S, bh * S / 2),
            }, new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => (piece == 0 ? _card : _cap).Set(dark);

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            var now = PreviewClock.Now;
            // The month and its buttons (PaintMonth).
            _title.Text = Loc.English ? $"{Loc.T(Months[now.Month - 1])} {now.Year}" : $"{now.Year}年{now.Month}月";
            Box(_title, Pad + 6, 0, CW / 2 + 20 - Pad - 6, MonthH);
            double by = MonthH / 2;
            Box(_next, CW - Pad - 34, by - 17, 34, 34);
            Box(_prev, CW - Pad - 34 - 38, by - 17, 34, 34);

            // The weekdays from the user's first one, then six weeks from the week holding the 1st.
            int first = ((int)System.Globalization.CultureInfo.CurrentCulture.DateTimeFormat.FirstDayOfWeek + 6) % 7; // 0 = Monday
            double cellW = (CW - 2 * Pad) / 7, gridTop = MonthH + WeekH;
            for (int i = 0; i < 7; i++)
            {
                int wd = (first + i) % 7;
                _week[i].Text = Loc.T(WeekShort[wd]);
                _week[i].Foreground = wd >= 5 ? _card.Sub : _card.Faint;
                Box(_week[i], Pad + i * cellW, MonthH, cellW, WeekH);
            }
            var firstDay = new DateTime(now.Year, now.Month, 1);
            int lead = (((int)firstDay.DayOfWeek + 6) % 7 - first + 7) % 7;
            var day = firstDay.AddDays(-lead);
            _today.Visibility = Visibility.Collapsed;
            for (int i = 0; i < 42; i++, day = day.AddDays(1))
            {
                double x = Pad + i % 7 * cellW, y = gridTop + i / 7 * CellH;
                var cell = new Rect(x + 2, y + 2, cellW - 4, CellH - 4);
                bool inMonth = day.Month == now.Month, today = day.Date == now.Date;
                double fade = inMonth || today ? 1 : 0.38;
                string label = WidgetFaces.DayLabel(day, out bool festival);
                _days[i].Text = day.Day.ToString();
                _days[i].FontWeight = today ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal;
                _days[i].Foreground = today ? WhiteInk : _card.Text;
                _labels[i].Text = label;
                _labels[i].Foreground = today ? WhiteInk : festival ? AccentBrush : _card.Sub;
                _days[i].Opacity = _labels[i].Opacity = fade;
                Box(_days[i], cell.X, cell.Y + 3, cell.Width, 21);
                Box(_labels[i], cell.X - 2, cell.Y + 23, cell.Width + 4, cell.Height - 25);
                if (today)
                {
                    _today.Visibility = Visibility.Visible;
                    Put(_today, cell.X, cell.Y, cell.Width, cell.Height);
                }
            }

            // The system's settings.
            double fy = gridTop + 6 * CellH + 8;
            Put(_line, Pad + 8, fy, CW - 2 * Pad - 16, 1);
            Box(_settings, Pad, fy + 5, TextWidth(Loc.T("Date & time settings"), 12.5) + 28, Footer - 10);

            // The capsule (PaintCapsule): the time with its seconds, the day and its lunar date.
            double bh = BarHeight, top = CardH + CapGap, edge = Math.Max(16, bh / 2 - 4);
            _time.Text = now.ToString("HH:mm:ss");
            double tw = TextWidth("00:00:00", 20, true);
            Box(_time, edge, top, tw + 4, bh);
            int todayWd = ((int)now.DayOfWeek + 6) % 7;
            _date.Text = Loc.English
                ? $"{Loc.T(WeekShort[todayWd])}, {Loc.T(Months[now.Month - 1])} {now.Day}"
                : $"{now.Month}月{now.Day}日 星期{WeekNames[todayWd]}";
            _lunar.Text = WidgetFaces.LunarLine(now);
            double tl = edge + tw + 16, mid = top + bh / 2;
            Box(_date, tl, mid - 19, CW - edge - tl, 20);
            Box(_lunar, tl, mid, CW - edge - tl, 18);
            SetBarClock(); // the bar's clock, by the minute
            return false;
        }
    }
}
