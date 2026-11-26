using System.Diagnostics;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Atrium.Settings;

// Installed games, for Stage Manager to leave alone (the user picks which): Steam's and Epic's
// libraries by their folders (an entry ending in a backslash: every program in it is the game's,
// also after an update renames one), and the games Windows' Game Bar has seen, by program.
internal static partial class GameFinder
{
    public static List<Game> Find()
    {
        var games = new List<Game>();
        try { Steam(games); } catch { }
        try { Epic(games); } catch { }
        try { GameBar(games); } catch { }
        return MergeGames(games);
    }

    private static void Steam(List<Game> games)
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        if (key?.GetValue("SteamPath") is not string steam || steam.Length == 0) return;
        var libraries = new List<string> { steam };
        string vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (File.Exists(vdf)) libraries.AddRange(ParseLibraryPaths(File.ReadAllText(vdf)));

        foreach (string library in libraries.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            string apps = Path.Combine(library, "steamapps");
            if (!Directory.Exists(apps)) continue;
            foreach (string manifest in Directory.EnumerateFiles(apps, "appmanifest_*.acf"))
            {
                string text = File.ReadAllText(manifest);
                string name = ParseVdfField(text, "name"), dir = ParseVdfField(text, "installdir");
                // Steam's own runtimes are not games.
                if (name.Length == 0 || dir.Length == 0 || IsExcludedRuntime(name)) continue;

                string folder = Path.Combine(apps, "common", dir);
                if (Directory.Exists(folder))
                    games.Add(new Game(name, FolderEntry(folder), MainProgram(folder)));
            }
        }
    }

    private static void Epic(List<Game> games)
    {
        string manifests = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), @"Epic\EpicGamesLauncher\Data\Manifests");
        if (!Directory.Exists(manifests)) return;
        foreach (string item in Directory.EnumerateFiles(manifests, "*.item"))
        {
            try
            {
                if (ParseEpicManifest(File.ReadAllText(item)) is not { } manifest || !Directory.Exists(manifest.Folder)) continue;
                string launch = manifest.LaunchExe.Length > 0 ? Path.Combine(manifest.Folder, manifest.LaunchExe) : "";
                games.Add(new Game(manifest.Name, FolderEntry(manifest.Folder), File.Exists(launch) ? launch : MainProgram(manifest.Folder)));
            }
            catch { }
        }
    }

    private static void GameBar(List<Game> games)
    {
        using var children = Registry.CurrentUser.OpenSubKey(@"System\GameConfigStore\Children");
        if (children == null) return;
        string windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        foreach (string id in children.GetSubKeyNames())
        {
            using var child = children.OpenSubKey(id);
            // A packaged game is matched by its app id, not its path: left out.
            if (child?.GetValue("MatchedExeFullPath") is not string exe || !File.Exists(exe) ||
                exe.StartsWith(windows, StringComparison.OrdinalIgnoreCase) || exe.Contains(@"\WindowsApps\", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            games.Add(new Game(Title(exe), Path.GetFullPath(exe).ToLowerInvariant(), exe));
        }
    }

    private static string Title(string exe)
    {
        try
        {
            var v = FileVersionInfo.GetVersionInfo(exe);
            foreach (string? t in new[] { v.ProductName, v.FileDescription })
                if (!string.IsNullOrWhiteSpace(t)) return t.Trim();
        }
        catch { }
        return Path.GetFileNameWithoutExtension(exe);
    }

    [GeneratedRegex("crash|unins|setup|redist|vc_?redist|dxsetup|report|updat|helper|install|benchmark|prereq", RegexOptions.IgnoreCase)]
    private static partial Regex NotTheGameRegex();

    // The game's own program in its folder (for its icon): the largest one, not a tool beside it.
    internal static string MainProgram(string folder)
    {
        try
        {
            var options = new EnumerationOptions { RecurseSubdirectories = true, MaxRecursionDepth = 3, IgnoreInaccessible = true };
            return Directory.EnumerateFiles(folder, "*.exe", options)
                .Where(f => !NotTheGameRegex().IsMatch(Path.GetFileName(f)))
                .Select(f => new FileInfo(f))
                .OrderByDescending(f => f.Length)
                .FirstOrDefault()?.FullName ?? "";
        }
        catch { return ""; }
    }
}
