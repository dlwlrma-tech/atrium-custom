$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-ownership.fixtures.ps1')
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceUninstall = Join-Path $project 'tools\Uninstall.ps1'
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('Atrium-uninstall-tests-' + [Guid]::NewGuid().ToString('N'))
$stubSourcePath = Join-Path $fixtureRoot 'UninstallStub.cs'
$stubExe = Join-Path $fixtureRoot 'stub.exe'

function Write-TestManifest([string]$Dir, [string]$Version = '2.8.0') { Write-SharedPackageManifest $Dir $Version }
function Populate-Install([string]$Dir, [hashtable]$Mode, [switch]$IncludeLegacyFiles) {
    $sDir = Join-Path $Dir 'Settings'; $lDir = Join-Path $Dir 'locales'
    New-Item -ItemType Directory -Path $sDir, $lDir -Force | Out-Null
    Copy-Item -LiteralPath $stubExe -Destination (Join-Path $Dir 'Atrium.exe') -Force
    Copy-Item -LiteralPath $stubExe -Destination (Join-Path $sDir 'Atrium.Settings.exe') -Force
    Copy-Item -LiteralPath $sourceUninstall -Destination (Join-Path $Dir 'Uninstall.ps1') -Force
    [void](Write-Utf8Bytes (Join-Path $Dir 'LICENSE') 'MIT')
    [void](Write-Utf8Bytes (Join-Path $lDir 'manifest.json') '{"source":"en"}')
    $modSample = Get-SharedModifiedOfficialSample
    [void](Write-Utf8Bytes (Join-Path $Dir $modSample.Path) $modSample.Official)
    [void](Write-Utf8Bytes (Join-Path $sDir 'Atrium.Settings.dll') 'dll')
    [void](Write-Utf8Bytes (Join-Path $sDir 'Update.ps1') '# updater')
    if ($IncludeLegacyFiles) { [void](Write-Utf8Bytes (Join-Path $Dir 'Atrium.Core.exe') 'legacy') }
    $lines = foreach ($k in $Mode.Keys) { "$k=$($Mode[$k])" }
    [void](Write-Utf8Bytes (Join-Path $Dir 'mode.txt') ($lines -join "`r`n"))
    [void](Write-Utf8Bytes (Join-Path $sDir 'mode.txt') ($lines -join "`r`n"))
}
function Wait-Log([string]$LogPath, [string]$Pattern) {
    $until = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $until) {
        if ((Test-Path -LiteralPath $LogPath) -and ((Get-Content -LiteralPath $LogPath -Raw) -match $Pattern)) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for log pattern: $Pattern"
}
function Invoke-Uninstaller([string]$InstallDir, [string]$LegacyManifestPath = '') {
    $script = Join-Path $InstallDir 'Uninstall.ps1'
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"", '-Confirmed', '-Quiet')
    if ($LegacyManifestPath) { $args += @('-LegacyManifestPath', "`"$LegacyManifestPath`"") }
    $psi = New-Object System.Diagnostics.ProcessStartInfo($ps51, ($args -join ' '))
    $psi.UseShellExecute = $false; $psi.RedirectStandardError = $true; $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        $stderr = $proc.StandardError.ReadToEnd()
        if (!$proc.WaitForExit(30000)) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; [void]$proc.WaitForExit(3000)
            throw "Uninstall.ps1 timed out in $InstallDir"
        }
        return [pscustomobject]@{ ExitCode = $proc.ExitCode; Error = $stderr }
    } finally { $proc.Dispose() }
}
function Stop-CaseProcesses([string]$CaseDir, [System.Collections.ArrayList]$Extra = $null) {
    if ($Extra) {
        foreach ($p in $Extra) {
            try { if ($p -and !$p.HasExited) { $p.Kill(); [void]$p.WaitForExit(2000) }; if ($p) { $p.Dispose() } } catch { }
        }
    }
    $prefix = $CaseDir.TrimEnd('\') + '\'
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
        try {
            if ($p.Path -and $p.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; [void]$p.WaitForExit(2000)
            }
        } catch { }
    }
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $stubSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class UninstallStub
{
    private const int WM_CLOSE = 0x0010;
    private const int WM_CLOSE_FOR_UNINSTALL = 0x8000 + 32;
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr wp, IntPtr lp);
    [DllImport("kernel32.dll")] private static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool QueryFullProcessImageName(IntPtr proc, uint flags, StringBuilder buf, ref uint size);

    static Dictionary<string, string> ReadMode(string dir)
    {
        var m = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string p = Path.Combine(dir, "mode.txt");
        if (!File.Exists(p)) return m;
        foreach (string line in File.ReadAllLines(p)) { int i = line.IndexOf('='); if (i > 0) m[line.Substring(0, i).Trim()] = line.Substring(i + 1).Trim(); }
        return m;
    }
    static string Get(Dictionary<string, string> m, string k) { string v; return m.TryGetValue(k, out v) ? v : ""; }
    static void Log(Dictionary<string, string> m, string msg)
    {
        string p = Get(m, "events_log"); if (p.Length == 0) return;
        for (int i = 0; i < 20; i++) { try { File.AppendAllText(p, msg + Environment.NewLine); return; } catch { Thread.Sleep(20); } }
    }
    static string ExePath(uint pid)
    {
        IntPtr p = OpenProcess(0x1000, false, pid);
        if (p == IntPtr.Zero) return "";
        try {
            var sb = new StringBuilder(1024); uint sz = (uint)sb.Capacity;
            return QueryFullProcessImageName(p, 0, sb, ref sz) ? sb.ToString(0, (int)sz) : "";
        }
        finally { CloseHandle(p); }
    }
    static IntPtr FindForExe(string cls, string exe)
    {
        for (IntPtr w = IntPtr.Zero; (w = FindWindowEx(IntPtr.Zero, w, cls, null)) != IntPtr.Zero;)
        {
            uint pid; GetWindowThreadProcessId(w, out pid);
            if (string.Equals(ExePath(pid), exe, StringComparison.OrdinalIgnoreCase)) return w;
        }
        return IntPtr.Zero;
    }

    private delegate IntPtr WndProcDelegate(IntPtr h, uint m, IntPtr w, IntPtr l);
    private static WndProcDelegate _hostProc;
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEX
    {
        public uint cbSize, style; public IntPtr lpfnWndProc; public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground; public string lpszMenuName, lpszClassName; public IntPtr hIconSm;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int ptX, ptY; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern ushort RegisterClassEx(ref WNDCLASSEX wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowEx(uint ex, string cls, string name, uint style, int x, int y,
        int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] private static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern void PostQuitMessage(int code);
    [DllImport("user32.dll")] private static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")] private static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")] private static extern IntPtr DispatchMessage(ref MSG msg);

    sealed class SimpleForm : Form
    {
        private readonly Dictionary<string, string> _m;
        private readonly bool _ignoreClose, _unsaved;
        private readonly string _tag;
        private bool _forceClose;
        public SimpleForm(Dictionary<string, string> m, string tag, bool ignoreClose, bool unsaved)
        {
            _m = m; _tag = tag; _ignoreClose = ignoreClose; _unsaved = unsaved;
            ShowInTaskbar = false; WindowState = FormWindowState.Minimized; Opacity = 0;
        }
        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_CLOSE_FOR_UNINSTALL)
            {
                if (_ignoreClose) return;
                Log(_m, _tag + ":close-for-uninstall");
                string holdEv = Get(_m, "hold_close_event");
                if (holdEv.Length > 0)
                {
                    using (var ev = EventWaitHandle.OpenExisting(holdEv)) ev.WaitOne(15000);
                }
                _forceClose = true;
                Close();
                return;
            }
            if (m.Msg == WM_CLOSE && (_ignoreClose || (_unsaved && !_forceClose))) return;
            base.WndProc(ref m);
        }
    }

    [STAThread]
    static int Main(string[] args)
    {
        string self = Assembly.GetExecutingAssembly().Location;
        string dir = Path.GetDirectoryName(self);
        string exe = Path.GetFileName(self);
        if (Array.IndexOf(args, "--noop") >= 0) return 0;
        var m = ReadMode(dir);
        string tag = Get(m, "tag");
        string hostCls = Get(m, "host_class");
        if (Array.IndexOf(args, "--exit") >= 0)
        {
            IntPtr w = FindForExe(hostCls, self);
            if (w == IntPtr.Zero) { Log(m, tag + ":exit-none"); return 0; }
            uint pid; GetWindowThreadProcessId(w, out pid);
            using (Process p = Process.GetProcessById((int)pid))
            {
                PostMessage(w, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
                return p.WaitForExit(10000) ? 0 : 1;
            }
        }
        if (exe.Equals("Atrium.Settings.exe", StringComparison.OrdinalIgnoreCase))
        {
            Log(m, tag + ":settings:start");
            var f = new SimpleForm(m, tag, Get(m, "ignore_close") == "1", Get(m, "unsaved") == "1");
            f.Shown += (s, e) => Log(m, tag + ":settings:ready");
            Application.Run(f);
            return 0;
        }
        IntPtr inst = GetModuleHandle(null);
        _hostProc = (h, msg, wp, lp) =>
        {
            if (msg == WM_CLOSE) { Log(m, tag + ":core:wm_close"); DestroyWindow(h); PostQuitMessage(0); return IntPtr.Zero; }
            return DefWindowProc(h, msg, wp, lp);
        };
        var wc = new WNDCLASSEX
        {
            cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX)), hInstance = inst,
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_hostProc), lpszClassName = hostCls
        };
        RegisterClassEx(ref wc);
        CreateWindowEx(0x80, hostCls, "Atrium", 0x80000000, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, inst, IntPtr.Zero);
        Log(m, tag + ":core:ready");
        MSG message;
        while (GetMessage(out message, IntPtr.Zero, 0, 0) > 0) { TranslateMessage(ref message); DispatchMessage(ref message); }
        return 0;
    }
}
'@
    [void](Write-Utf8Bytes $stubSourcePath $stubSource)
    & $csc /nologo /target:winexe /r:System.Windows.Forms.dll /r:System.Drawing.dll /out:$stubExe $stubSourcePath
    if ($LASTEXITCODE -ne 0) { throw 'Failed to compile UninstallStub.exe.' }
    $testHostClass = 'AtriumCoreHostTest_' + [Guid]::NewGuid().ToString('N').Substring(0, 12)

    # Case 1: Manifest-driven uninstall keeps user files & modified official files, closes unsaved Settings via 0x8000+32
    $userSamples = Get-SharedUserFileSamples
    $modSample = Get-SharedModifiedOfficialSample
    $cDir = Join-Path $fixtureRoot 'case1-manifest'; $inst = Join-Path $cDir 'Atrium'
    $log = Join-Path $cDir 'events.log'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-Install $inst @{ tag = 'c1'; host_class = $testHostClass; events_log = $log; unsaved = '1' }
        [void](Write-Utf8Bytes (Join-Path $inst 'development.txt') 'dev')
        Write-TestManifest $inst '2.8.0'
        $bMod = Write-Utf8Bytes (Join-Path $inst $modSample.Path) $modSample.Modified
        $kept = @{}
        foreach ($rel in $userSamples.Keys) { $kept[$rel] = Write-Utf8Bytes (Join-Path $inst $rel) $userSamples[$rel] }
        $bCust = Write-Utf8Bytes (Join-Path $inst 'custom-dir\notes.txt') 'u-notes'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $inst 'Atrium.exe') -WindowStyle Hidden -PassThru))
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $inst 'Settings\Atrium.Settings.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $log 'c1:core:ready'; Wait-Log $log 'c1:settings:ready'
        $res = Invoke-Uninstaller $inst
        if ($res.ExitCode -ne 0) { throw "Case 1 failed (exit $($res.ExitCode)): $($res.Error)" }
        Wait-Log $log 'c1:close-for-uninstall'; Wait-Log $log 'c1:core:wm_close'
        foreach ($gone in @('Atrium.exe', 'LICENSE', 'Uninstall.ps1', 'package-manifest.json', 'development.txt',
            'Settings\Atrium.Settings.exe', 'Settings\Atrium.Settings.dll', 'locales\manifest.json')) {
            if (Test-Path -LiteralPath (Join-Path $inst $gone)) { throw "Case 1: official file remained: $gone" }
        }
        Assert-SameBytes (Join-Path $inst $modSample.Path) $bMod 'Case 1 (modified official)'
        foreach ($rel in $userSamples.Keys) { Assert-SameBytes (Join-Path $inst $rel) $kept[$rel] 'Case 1' }
        Assert-SameBytes (Join-Path $inst 'custom-dir\notes.txt') $bCust 'Case 1'
    } finally { Stop-CaseProcesses $cDir $tracked }
    Write-Host 'Uninstall Case 1 passed (manifest uninstall preserved user & modified official files).'

    # Case 2: Manifest-less uninstall requires exact legacy catalog; modified/unowned files kept, empty install removes root
    $cDir = Join-Path $fixtureRoot 'case2-legacy'; $inst = Join-Path $cDir 'Atrium'
    $instUnknown = Join-Path $cDir 'Atrium-unknown'; $instMismatch = Join-Path $cDir 'Atrium-mismatch'
    $instEmpty = Join-Path $cDir 'Atrium-empty'; $log = Join-Path $cDir 'events.log'
    try {
        Populate-Install $inst @{ tag = 'c2'; host_class = $testHostClass; events_log = $log } -IncludeLegacyFiles
        $legacyCatalog = Join-Path $cDir 'legacy-manifests.json'
        Write-SharedLegacyCatalog $inst $legacyCatalog '2.7.0'
        $bMod = Write-Utf8Bytes (Join-Path $inst $modSample.Path) $modSample.Modified
        $kept = @{}
        foreach ($rel in $userSamples.Keys) { $kept[$rel] = Write-Utf8Bytes (Join-Path $inst $rel) $userSamples[$rel] }
        $bCust = Write-Utf8Bytes (Join-Path $inst 'custom-dir\notes.txt') 'legacy-u-notes'
        $res = Invoke-Uninstaller $inst $legacyCatalog
        if ($res.ExitCode -ne 0) { throw "Case 2a failed (exit $($res.ExitCode)): $($res.Error)" }
        foreach ($gone in @('Atrium.exe', 'Atrium.Core.exe', 'LICENSE', 'Uninstall.ps1',
            'Settings\Atrium.Settings.exe', 'Settings\Atrium.Settings.dll', 'locales\manifest.json')) {
            if (Test-Path -LiteralPath (Join-Path $inst $gone)) { throw "Case 2a: legacy file remained: $gone" }
        }
        Assert-SameBytes (Join-Path $inst $modSample.Path) $bMod 'Case 2a (modified official)'
        foreach ($rel in $userSamples.Keys) { Assert-SameBytes (Join-Path $inst $rel) $kept[$rel] 'Case 2a' }
        Assert-SameBytes (Join-Path $inst 'custom-dir\notes.txt') $bCust 'Case 2a'
        Populate-Install $instUnknown @{ tag = 'c2u'; host_class = $testHostClass; events_log = $log } -IncludeLegacyFiles
        $keptUnknown = @{}
        foreach ($rel in @('Settings\Assets\my-photo.png', 'Settings\Fonts\personal.otf', 'Settings\my-extension.dll')) {
            $keptUnknown[$rel] = Write-Utf8Bytes (Join-Path $instUnknown $rel) $userSamples[$rel]
        }
        $resUnknown = Invoke-Uninstaller $instUnknown
        if ($resUnknown.ExitCode -eq 0) { throw 'Case 2b: expected unverified manifest-less uninstall to refuse deletion' }
        foreach ($rel in @('Atrium.exe', 'Settings\Atrium.Settings.exe')) {
            if (!(Test-Path -LiteralPath (Join-Path $instUnknown $rel) -PathType Leaf)) { throw "Case 2b: removed $rel" }
        }
        foreach ($rel in $keptUnknown.Keys) { Assert-SameBytes (Join-Path $instUnknown $rel) $keptUnknown[$rel] 'Case 2b' }
        Populate-Install $instMismatch @{ tag = 'c2m'; host_class = $testHostClass; events_log = $log }
        $bMismatchUser = Write-Utf8Bytes (Join-Path $instMismatch 'Settings\Assets\my-photo.png') $userSamples['Settings\Assets\my-photo.png']
        $mismatchJson = ([ordered]@{ schema = 1; version = '2.8.0'
            files = @([ordered]@{ path = 'Atrium.exe'; sha256 = ('0' * 64) }) } | ConvertTo-Json -Depth 4) -replace "`r`n", "`n"
        $bMismatchManifest = Write-Utf8Bytes (Join-Path $instMismatch 'package-manifest.json') ($mismatchJson + "`n")
        $resMismatch = Invoke-Uninstaller $instMismatch
        if ($resMismatch.ExitCode -eq 0) { throw 'Case 2c: expected zero-verified manifest uninstall to refuse deletion' }
        foreach ($rel in @('Atrium.exe', 'Settings\Atrium.Settings.exe')) {
            if (!(Test-Path -LiteralPath (Join-Path $instMismatch $rel) -PathType Leaf)) { throw "Case 2c: removed $rel" }
        }
        Assert-SameBytes (Join-Path $instMismatch 'package-manifest.json') $bMismatchManifest 'Case 2c'
        Assert-SameBytes (Join-Path $instMismatch 'Settings\Assets\my-photo.png') $bMismatchUser 'Case 2c'
        Populate-Install $instEmpty @{ tag = 'c2e'; host_class = $testHostClass; events_log = $log }
        [void](Remove-Item -LiteralPath (Join-Path $instEmpty 'mode.txt'), (Join-Path $instEmpty 'Settings\mode.txt') -Force)
        Write-TestManifest $instEmpty '2.8.0'
        $res2 = Invoke-Uninstaller $instEmpty
        if ($res2.ExitCode -ne 0) { throw "Case 2d failed: $($res2.Error)" }
        if (Test-Path -LiteralPath $instEmpty) { throw 'Case 2d: empty install root was not removed' }
    } finally { Stop-CaseProcesses $cDir }
    Write-Host 'Uninstall Case 2 passed (exact legacy hash ownership, unverified refusal & empty root removal).'

    # Case 3: Dual-directory coexistence (A running, B idle -> uninstall B does not disturb A)
    $cDir = Join-Path $fixtureRoot 'case3-dual'; $instA = Join-Path $cDir 'Atrium-A'; $instB = Join-Path $cDir 'Atrium-B'
    $log = Join-Path $cDir 'events.log'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-Install $instA @{ tag = 'A'; host_class = $testHostClass; events_log = $log }
        Populate-Install $instB @{ tag = 'B'; host_class = $testHostClass; events_log = $log }
        Write-TestManifest $instA '2.8.0'; Write-TestManifest $instB '2.8.0'
        $coreA = Start-Process -FilePath (Join-Path $instA 'Atrium.exe') -WindowStyle Hidden -PassThru
        $setA = Start-Process -FilePath (Join-Path $instA 'Settings\Atrium.Settings.exe') -WindowStyle Hidden -PassThru
        [void]$tracked.Add($coreA); [void]$tracked.Add($setA)
        Wait-Log $log 'A:core:ready'; Wait-Log $log 'A:settings:ready'
        $res = Invoke-Uninstaller $instB
        if ($res.ExitCode -ne 0) { throw "Case 3 failed (exit $($res.ExitCode)): $($res.Error)" }
        if ($coreA.HasExited -or $setA.HasExited) { throw 'Case 3: uninstalling idle B stopped running A' }
        if ((Test-Path -LiteralPath (Join-Path $instB 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $instA 'Atrium.exe'))) { throw 'Case 3: wrong directory affected' }
    } finally { Stop-CaseProcesses $cDir $tracked }
    Write-Host 'Uninstall Case 3 passed (uninstalling idle B did not affect running A).'

    # Case 4: Unresponsive Settings in B -> uninstall fails and keeps B files
    $cDir = Join-Path $fixtureRoot 'case4-unresponsive'; $instB = Join-Path $cDir 'Atrium-B'
    $log = Join-Path $cDir 'events.log'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-Install $instB @{ tag = 'B'; host_class = $testHostClass; events_log = $log; ignore_close = '1' }
        Write-TestManifest $instB '2.8.0'
        $setB = Start-Process -FilePath (Join-Path $instB 'Settings\Atrium.Settings.exe') -WindowStyle Hidden -PassThru
        [void]$tracked.Add($setB)
        Wait-Log $log 'B:settings:ready'
        $res = Invoke-Uninstaller $instB
        if ($res.ExitCode -eq 0) { throw 'Case 4: expected uninstall to fail when Settings is unresponsive' }
        if (!(Test-Path -LiteralPath (Join-Path $instB 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $instB 'Settings\Atrium.Settings.exe'))) {
            throw 'Case 4: B files were deleted despite failure'
        }
    } finally { Stop-CaseProcesses $cDir $tracked }
    Write-Host 'Uninstall Case 4 passed (unresponsive Settings aborted uninstall and kept B files).'

    # Case 5: Uninstaller holding install mutex rejects concurrent Update.ps1 without spawning Settings, then completes cleanly
    $cDir = Join-Path $fixtureRoot 'case5-uninstall-blocks-updater'; $inst = Join-Path $cDir 'Atrium'
    $cache = Join-Path $cDir 'cache'; $pkg = Join-Path $cache 'package-background'
    $log = Join-Path $cDir 'events.log'; $uErrLog = Join-Path $cDir 'uninstall.err.log'
    $tracked = New-Object System.Collections.ArrayList
    $releaseName = 'Local\Atrium.UninstallTest.Release.' + [Guid]::NewGuid().ToString('N')
    $releaseEv = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $releaseName)
    try {
        Populate-Install $inst @{ tag = 'c5'; host_class = $testHostClass; events_log = $log; unsaved = '1'; hold_close_event = $releaseName }
        Write-TestManifest $inst '2.8.0'
        $bKeep = Write-Utf8Bytes (Join-Path $inst 'user-keep.txt') 'keep-user'
        New-Item -ItemType Directory -Path (Join-Path $pkg 'Settings') -Force | Out-Null
        Copy-Item -LiteralPath $stubExe -Destination (Join-Path $pkg 'Atrium.exe') -Force
        Copy-Item -LiteralPath $stubExe -Destination (Join-Path $pkg 'Settings\Atrium.Settings.exe') -Force
        Write-TestManifest $pkg '2.9.0'
        $updateCopy = Join-Path $cache 'Update.ps1'
        Copy-Item -LiteralPath (Join-Path $project 'tools\Update.ps1') -Destination $updateCopy -Force
        $statePath = Join-Path $cache 'ready.json'; '{}' | Set-Content -LiteralPath $statePath -Encoding UTF8
        $resultPath = Join-Path $cache 'result.json'
        $coreProc = Start-Process -FilePath (Join-Path $inst 'Atrium.exe') -WindowStyle Hidden -PassThru
        $setProc = Start-Process -FilePath (Join-Path $inst 'Settings\Atrium.Settings.exe') -WindowStyle Hidden -PassThru
        [void]$tracked.Add($coreProc); [void]$tracked.Add($setProc)
        Wait-Log $log 'c5:core:ready'; Wait-Log $log 'c5:settings:ready'
        $uScript = Join-Path $inst 'Uninstall.ps1'
        $uProc = Start-Process -FilePath $ps51 -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$uScript`"",
            '-Confirmed', '-Quiet') -RedirectStandardError $uErrLog -WindowStyle Hidden -PassThru
        [void]$tracked.Add($uProc)
        Wait-Log $log 'c5:close-for-uninstall'
        $dummy = Start-Process -FilePath $stubExe -ArgumentList '--noop' -WindowStyle Hidden -PassThru
        $dummyId = $dummy.Id; [void]$dummy.WaitForExit(5000); $dummy.Dispose()
        $updArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$updateCopy`"", '-InstallDirectory', "`"$inst`"",
            '-PackageDirectory', "`"$pkg`"", '-StatePath', "`"$statePath`"", '-WaitForProcessId', $dummyId,
            '-StartCore', '1', '-StartupTimeoutSeconds', '4')
        $updProc = Start-Process -FilePath $ps51 -ArgumentList $updArgs -WindowStyle Hidden -PassThru
        try {
            if (!$updProc.WaitForExit(30000)) {
                Stop-Process -Id $updProc.Id -Force -ErrorAction SilentlyContinue; [void]$updProc.WaitForExit(2000)
                throw 'Case 5: Update.ps1 timed out while uninstaller held mutex'
            }
        } finally { $updProc.Dispose() }
        if (!(Test-Path -LiteralPath $resultPath -PathType Leaf)) { throw 'Case 5: Update.ps1 did not write result.json' }
        $updRes = Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if (!$updRes.error -or $updRes.error -notmatch '另一项更新或卸载') {
            throw "Case 5: expected mutex conflict in result.json, got: $($updRes.error)"
        }
        $logText = Get-Content -LiteralPath $log -Raw
        $settingsStarts = ([regex]::Matches($logText, 'c5:settings:start')).Count
        if ($settingsStarts -ne 1) { throw "Case 5: expected exactly 1 Settings start, saw $settingsStarts" }
        $settingsPath = Join-Path $inst 'Settings\Atrium.Settings.exe'
        $runningSettings = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and [string]::Equals($_.Path, $settingsPath, [StringComparison]::OrdinalIgnoreCase) } catch { $false }
        })
        if ($runningSettings.Count -ne 1 -or $runningSettings[0].Id -ne $setProc.Id) {
            throw "Case 5: unexpected Settings process spawned while uninstaller held mutex"
        }
        [void]$releaseEv.Set()
        if (!$uProc.WaitForExit(30000)) { throw 'Case 5: Uninstall.ps1 timed out after releasing close event' }
        if ($uProc.ExitCode -ne 0) {
            $errText = if (Test-Path -LiteralPath $uErrLog) { Get-Content -LiteralPath $uErrLog -Raw } else { '' }
            throw "Case 5: Uninstall.ps1 failed (exit $($uProc.ExitCode)): $errText"
        }
        Wait-Log $log 'c5:core:wm_close'
        $finalStarts = ([regex]::Matches((Get-Content -LiteralPath $log -Raw), 'c5:settings:start')).Count
        if ($finalStarts -ne 1) { throw "Case 5: Settings started after updater rejection ($finalStarts)" }
        foreach ($gone in @('Atrium.exe', 'Settings\Atrium.Settings.exe', 'Uninstall.ps1', 'package-manifest.json')) {
            if (Test-Path -LiteralPath (Join-Path $inst $gone)) { throw "Case 5: official file remained: $gone" }
        }
        Assert-SameBytes (Join-Path $inst 'user-keep.txt') $bKeep 'Case 5'
    } finally {
        $releaseEv.Dispose()
        Stop-CaseProcesses $cDir $tracked
    }
    Write-Host 'Uninstall Case 5 passed (uninstaller holding mutex rejected updater without spawning Settings).'
    Write-Host 'Uninstaller regressions passed (5 cases).'
} finally {
    Stop-CaseProcesses $fixtureRoot
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue }
}
exit 0
