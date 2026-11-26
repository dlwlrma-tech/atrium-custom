namespace Atrium.Settings;

// The moment every preview shows: Atrium's first public release on GitHub (31 October 2026, 18:33 China
// Standard Time). Fixed, so no preview reads the clock or keeps a timer for it.
internal static class PreviewClock
{
    public static readonly DateTime Now = new(2026, 10, 31, 18, 33, 0);
    public static readonly DateTime Utc = new(2026, 10, 31, 10, 33, 0, DateTimeKind.Utc);
}
