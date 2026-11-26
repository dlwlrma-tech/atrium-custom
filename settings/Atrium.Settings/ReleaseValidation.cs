using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace Atrium.Settings;

internal static class ReleaseValidation
{
    internal static void VerifyZipDigest(string zipPath, string? expectedDigest, Func<string, string>? localize = null)
    {
        if (expectedDigest is null) return;
        using var stream = File.OpenRead(zipPath);
        string actual = "sha256:" + Convert.ToHexString(SHA256.HashData(stream));
        if (!actual.Equals(expectedDigest, StringComparison.OrdinalIgnoreCase))
        {
            string message = "The update package failed verification. Download again.";
            throw new IOException(localize?.Invoke(message) ?? message);
        }
    }

    internal static void ExtractZipSafely(string zipPath, string destination)
    {
        string destRoot = Path.GetFullPath(destination);
        if (!destRoot.EndsWith(Path.DirectorySeparatorChar)) destRoot += Path.DirectorySeparatorChar;
        using var archive = ZipFile.OpenRead(zipPath);
        foreach (var entry in archive.Entries)
        {
            string name = entry.FullName;
            if (Path.IsPathRooted(name) || name.StartsWith('/') || name.StartsWith('\\') ||
                name.Split('/', '\\').Any(segment => segment == ".."))
            {
                throw new InvalidDataException($"Zip entry contains unsafe path: {name}");
            }
            string fullPath = Path.GetFullPath(Path.Combine(destRoot, name));
            if (!fullPath.StartsWith(destRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Zip entry escapes destination: {name}");
        }
        Directory.CreateDirectory(destRoot);
        foreach (var entry in archive.Entries)
        {
            string fullPath = Path.GetFullPath(Path.Combine(destRoot, entry.FullName));
            if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\'))
            {
                Directory.CreateDirectory(fullPath);
            }
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
                entry.ExtractToFile(fullPath, overwrite: true);
            }
        }
    }

    internal static void ValidatePackage(
        string directory,
        Version expected,
        Func<string, Version>? versionReader = null,
        Func<string, string>? localize = null)
    {
        string core = Path.Combine(directory, "Atrium.exe");
        string settings = Path.Combine(directory, "Settings", "Atrium.Settings.exe");
        string settingsDll = Path.ChangeExtension(settings, ".dll");
        if (!File.Exists(core) || !File.Exists(settings) || !File.Exists(settingsDll))
        {
            string missing = "The update package is missing program files. Download again.";
            throw new IOException(localize?.Invoke(missing) ?? missing);
        }
        versionReader ??= ReadFileVersion;
        if (versionReader(core) != expected || versionReader(settingsDll) != expected)
        {
            string mismatch = "The update package does not match the release. Download again.";
            throw new IOException(localize?.Invoke(mismatch) ?? mismatch);
        }
    }

    private static Version ReadFileVersion(string path)
    {
        var info = FileVersionInfo.GetVersionInfo(path);
        return new Version(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart, info.FilePrivatePart);
    }

    internal static void SaveJsonAtomic<T>(string path, T value)
    {
        if (Path.GetDirectoryName(Path.GetFullPath(path)) is { Length: > 0 } dir)
            Directory.CreateDirectory(dir);
        string temporary = $"{path}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                JsonSerializer.Serialize(stream, value);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: true);
        }
        catch
        {
            try
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            throw;
        }
    }

    internal static string? ReadFailureWithLock(string resultPath, string mutexName, Action<Exception>? logError = null)
    {
        using var mutex = new Mutex(false, mutexName);
        bool acquired = false;
        try
        {
            try { acquired = mutex.WaitOne(TimeSpan.FromSeconds(2)); }
            catch (AbandonedMutexException) { acquired = true; }
            if (!acquired || !File.Exists(resultPath)) return null;
            try
            {
                using var result = JsonDocument.Parse(File.ReadAllText(resultPath));
                string? error = result.RootElement.TryGetProperty("error", out var prop) && prop.ValueKind == JsonValueKind.String
                    ? prop.GetString()
                    : null;
                File.Delete(resultPath);
                return string.IsNullOrEmpty(error) ? null : error;
            }
            catch (Exception error) when (error is JsonException or FormatException or InvalidOperationException)
            {
                logError?.Invoke(error);
                try { if (File.Exists(resultPath)) File.Delete(resultPath); }
                catch (IOException cleanup) { logError?.Invoke(cleanup); }
                return null;
            }
        }
        finally
        {
            if (acquired) mutex.ReleaseMutex();
        }
    }

    internal static T? TryRecoverPrepared<T>(
        Func<T?> readPrepared,
        string statePath,
        string workDirectory,
        Action<Exception>? logError = null) where T : class
    {
        try
        {
            return readPrepared();
        }
        catch (Exception error) when (error is IOException or JsonException or FormatException
            or InvalidDataException or ArgumentException)
        {
            logError?.Invoke(error);
            try { if (File.Exists(statePath)) File.Delete(statePath); }
            catch (IOException cleanup) { logError?.Invoke(cleanup); }
            try { if (Directory.Exists(workDirectory)) Directory.Delete(workDirectory, true); }
            catch (IOException cleanup) { logError?.Invoke(cleanup); }
            return null;
        }
    }
}
