using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Atrium.Settings;

internal static partial class GameFinder
{
    internal sealed record Game(string Title, string Entry, string Icon);

    internal static bool IsFolder(string entry) => entry.EndsWith('\\');

    // As Stage Manager matches it: lower case, a prefix of its programs' paths.
    internal static string FolderEntry(string folder) => Path.GetFullPath(folder).TrimEnd('\\').ToLowerInvariant() + "\\";

    internal static List<string> ParseLibraryPaths(string vdf) =>
        [.. LibraryPathRegex().Matches(vdf).Select(m => m.Groups[1].Value.Replace(@"\\", @"\"))];

    internal static string ParseVdfField(string vdf, string field)
    {
        var m = Regex.Match(vdf, "\"" + Regex.Escape(field) + "\"\\s+\"([^\"]*)\"", RegexOptions.IgnoreCase);
        return m.Success ? m.Groups[1].Value : "";
    }

    internal static bool IsExcludedRuntime(string name) =>
        name.StartsWith("Steamworks", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("Redistributable", StringComparison.OrdinalIgnoreCase);

    internal static (string Name, string Folder, string LaunchExe)? ParseEpicManifest(string json)
    {
        try
        {
            if (JsonNode.Parse(json) is not JsonObject o) return null;
            if (o["AppCategories"] is JsonArray categories && !categories.Any(c => c?.GetValue<string>() == "games")) return null;
            string name = o["DisplayName"]?.GetValue<string>() ?? "", folder = o["InstallLocation"]?.GetValue<string>() ?? "";
            string exe = o["LaunchExecutable"]?.GetValue<string>() ?? "";
            return name.Length > 0 && folder.Length > 0 ? (name, folder, exe) : null;
        }
        catch { return null; }
    }

    internal static List<Game> MergeGames(IEnumerable<Game> games)
    {
        // A program inside a library's folder is that game already.
        var list = games as IReadOnlyCollection<Game> ?? [.. games];
        var folders = list.Select(g => g.Entry).Where(IsFolder).ToList();
        return [.. list.Where(g => IsFolder(g.Entry) || !folders.Any(f => g.Entry.StartsWith(f, StringComparison.OrdinalIgnoreCase)))
                    .DistinctBy(g => g.Entry, StringComparer.OrdinalIgnoreCase)
                    .OrderBy(g => g.Title, StringComparer.CurrentCultureIgnoreCase)];
    }

    [GeneratedRegex("\"path\"\\s+\"([^\"]+)\"")]
    private static partial Regex LibraryPathRegex();
}
