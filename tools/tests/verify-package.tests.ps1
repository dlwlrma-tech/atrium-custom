param(
    [string]$CandidateZipPath = '',
    [switch]$RequireRealPackage
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$verifyScript = Join-Path $project 'tools\verify-package.ps1'
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$pwshExe = (Get-Process -Id $PID).Path
$csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $ps51 -PathType Leaf)) { throw "Windows PowerShell 5.1 not found: $ps51" }
if (!(Test-Path -LiteralPath $csc -PathType Leaf)) { throw "C# compiler not found: $csc" }
$cmake = Get-Content -LiteralPath (Join-Path $project 'native\CMakeLists.txt') -Raw
if ($cmake -notmatch 'VERSION\s+(\d+\.\d+\.\d+)') { throw 'Missing version in CMakeLists.txt.' }
$expectedVersion = $Matches[1]
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('Atrium-verify-tests-' + [Guid]::NewGuid().ToString('N'))

function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($Path)
        try { return [BitConverter]::ToString($sha.ComputeHash($fs)).Replace('-', '').ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Write-Utf8File([string]$Path, [string]$Text, [bool]$Bom = $false) {
    $dir = Split-Path -Parent $Path
    if ($dir -and !(Test-Path -LiteralPath $dir -PathType Container)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding $Bom))
}

function Assert-NoVerifyOrphans([string]$Label, [string]$PipeTag = '') {
    $verifyPrefix = ([IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')) + '\Atrium-verify-'
    $deadline = [DateTime]::UtcNow.AddSeconds(4)
    for (;;) {
        $orphans = [System.Collections.Generic.List[string]]::new()
        foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
            try {
                if ($p.Path -and $p.Path.StartsWith($verifyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                    $orphans.Add("$($p.Name) ($($p.Id)): $($p.Path)")
                }
            } catch { }
        }
        if ($PipeTag) {
            foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue)) {
                if ($cim.CommandLine -and $cim.CommandLine.IndexOf($PipeTag, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
                    [int]$cim.ProcessId -ne $PID) {
                    $orphans.Add("pid=$($cim.ProcessId): $($cim.CommandLine)")
                }
            }
        }
        if ($orphans.Count -eq 0) { return }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Residual verify-package processes after ${Label}:`n$($orphans -join "`n")"
        }
        Start-Sleep -Milliseconds 150
    }
}

$sentinelDir = Join-Path $env:LOCALAPPDATA 'Atrium'
$sentinelPath = Join-Path $sentinelDir 'settings.json'
$hadSentinel = Test-Path -LiteralPath $sentinelPath -PathType Leaf
$sentinelHash = if ($hadSentinel) { Get-FileSha256 $sentinelPath } else { '' }
$sentinelDirExisted = if ($hadSentinel) { $true } else { Test-Path -LiteralPath $sentinelDir -PathType Container }

function Assert-SentinelUnchanged([string]$Step) {
    if ($hadSentinel) {
        if (!(Test-Path -LiteralPath $sentinelPath -PathType Leaf)) {
            throw "Sentinel settings.json disappeared during $Step`: $sentinelPath"
        }
        $actual = Get-FileSha256 $sentinelPath
        if ($actual -cne $sentinelHash) {
            throw "Sentinel settings.json hash changed during $Step`: expected $sentinelHash, got $actual"
        }
    } else {
        if (Test-Path -LiteralPath $sentinelPath -PathType Leaf) {
            throw "Absent sentinel settings.json was created during $Step`: $sentinelPath"
        }
        if (!$sentinelDirExisted -and (Test-Path -LiteralPath $sentinelDir)) {
            throw "Absent sentinel directory was created during $Step`: $sentinelDir"
        }
    }
}

function Start-VerifyChild([string]$ShellExe, [string[]]$ScriptArgs, [string]$StdOutPath, [string]$StdErrPath) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ShellExe
    $argParts = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$verifyScript`"") + $ScriptArgs
    $psi.Arguments = $argParts -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($ShellExe -match 'WindowsPowerShell' -and $env:PSModulePath) {
        $cleanModules = @($env:PSModulePath.Split(';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\(7|Modules)|\\PowerShell\\7\\' })
        $psi.EnvironmentVariables['PSModulePath'] = $cleanModules -join ';'
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    return [pscustomobject]@{ Process = $p; OutTask = $outTask; ErrTask = $errTask; OutPath = $StdOutPath; ErrPath = $StdErrPath }
}

function Invoke-ThreeSentinelSuite([string]$ZipPath, [string]$ShellExe, [bool]$IsStub, [string]$SuiteName) {
    $suiteDir = Join-Path $fixtureRoot $SuiteName
    New-Item -ItemType Directory -Path $suiteDir -Force | Out-Null
    $pipeArg = @()
    $pipeTag = ''
    if ($IsStub) {
        $pipeTag = 'Atrium.Test.Core.' + [Guid]::NewGuid().ToString('N')
        $pipeArg = @('-PipeName', $pipeTag, '-SkipRunningProcessCheck')
    }
    $zipArg = @('-ZipPath', "`"$ZipPath`"", '-SmokeStart') + $pipeArg

    # (2) Force-kill verify-package.ps1 right before SaveButton.Invoke()
    $markerPath = Join-Path $suiteDir 'before-save.marker'
    $resumeName = 'Local\Atrium.VerifyTest.Resume.' + [Guid]::NewGuid().ToString('N')
    $resumeEv = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $resumeName)
    try {
        $ev2 = Join-Path $suiteDir 'ev2'
        $args2 = $zipArg + @('-BeforeSaveMarkerPath', "`"$markerPath`"", '-BeforeSaveResumeEvent', $resumeName,
            '-EvidenceDirectory', "`"$ev2`"")
        $child2 = Start-VerifyChild $ShellExe $args2 (Join-Path $suiteDir 's2.out') (Join-Path $suiteDir 's2.err')
        try {
            $deadline2 = [DateTime]::UtcNow.AddSeconds(45)
            while (!(Test-Path -LiteralPath $markerPath -PathType Leaf)) {
                if ($child2.Process.HasExited) {
                    $o = $child2.OutTask.GetAwaiter().GetResult()
                    $e = $child2.ErrTask.GetAwaiter().GetResult()
                    throw "$SuiteName (2) exited before marker (exit=$($child2.Process.ExitCode)):`n$o`n$e"
                }
                if ([DateTime]::UtcNow -ge $deadline2) {
                    Stop-Process -Id $child2.Process.Id -Force -ErrorAction SilentlyContinue
                    throw "$SuiteName (2) timed out waiting for before-save marker."
                }
                Start-Sleep -Milliseconds 100
            }
            $markerLines = @([IO.File]::ReadAllLines($markerPath))
            if ($markerLines.Count -lt 2 -or [string]::IsNullOrWhiteSpace($markerLines[1])) {
                throw "$SuiteName (2) marker missing temp directory path."
            }
            $killedTemp = $markerLines[1].Trim()
            $killSw = [Diagnostics.Stopwatch]::StartNew()
            Stop-Process -Id $child2.Process.Id -Force
            if (!$child2.Process.WaitForExit(5000)) {
                throw "$SuiteName (2) verify-package process did not exit promptly after Stop-Process -Force."
            }
            $killSw.Stop()
            if ($killSw.ElapsedMilliseconds -ge 5000) {
                throw "$SuiteName (2) kill took too long: $($killSw.ElapsedMilliseconds)ms"
            }
        } finally {
            $child2.Process.Dispose()
        }
    } finally {
        $resumeEv.Dispose()
    }
    Assert-NoVerifyOrphans "$SuiteName (2)" $pipeTag
    Assert-SentinelUnchanged "$SuiteName (2)"
    if (Test-Path -LiteralPath $killedTemp -PathType Container) {
        $rmDeadline = [DateTime]::UtcNow.AddSeconds(5)
        for (;;) {
            try {
                Remove-Item -LiteralPath $killedTemp -Recurse -Force -ErrorAction Stop
                break
            } catch {
                if ([DateTime]::UtcNow -ge $rmDeadline) {
                    throw "$SuiteName (2) failed to remove killed temp dir '$killedTemp' (handle lock remained): $_"
                }
                Start-Sleep -Milliseconds 100
            }
        }
        if (Test-Path -LiteralPath $killedTemp) {
            throw "$SuiteName (2) killed temp dir still exists after cleanup: $killedTemp"
        }
    }

    # (3) UIA hang bounded timeout
    $expectMs = if ($IsStub) { 1500 } else { 4000 }
    $args3 = $zipArg + @('-HangUiaStep', 'first-toggle', '-UiaTimeoutMs', "$expectMs")
    $child3 = Start-VerifyChild $ShellExe $args3 (Join-Path $suiteDir 's3.out') (Join-Path $suiteDir 's3.err')
    try {
        if (!$child3.Process.WaitForExit(90000)) {
            Stop-Process -Id $child3.Process.Id -Force -ErrorAction SilentlyContinue
            throw "$SuiteName (3) hung past 90s upper bound."
        }
        $out3 = $child3.OutTask.GetAwaiter().GetResult()
        $err3 = $child3.ErrTask.GetAwaiter().GetResult()
        $combined3 = "$out3`n$err3"
        if ($child3.Process.ExitCode -eq 0) {
            throw "$SuiteName (3) unexpectedly succeeded under -HangUiaStep first-toggle."
        }
        $elapsedMatch = [regex]::Match($combined3, 'elapsed=(\d+)ms')
        if ($combined3 -notmatch 'first-toggle' -or $combined3 -notmatch 'ClockSecondsSwitch' -or
            $combined3 -notmatch [regex]::Escape("${expectMs}ms") -or !$elapsedMatch.Success) {
            throw "$SuiteName (3) error missing first-toggle / ClockSecondsSwitch / ${expectMs}ms / elapsed:`n$combined3"
        }
        $elapsedMs = [int]$elapsedMatch.Groups[1].Value
        if ($elapsedMs -lt ($expectMs - 200) -or $elapsedMs -gt ($expectMs + 5000)) {
            throw "$SuiteName (3) single-step elapsed ${elapsedMs}ms outside [$($expectMs - 200), $($expectMs + 5000)]ms."
        }
    } finally {
        $child3.Process.Dispose()
    }
    Assert-NoVerifyOrphans "$SuiteName (3)" $pipeTag
    Assert-SentinelUnchanged "$SuiteName (3)"

    # (1) Immediate normal completion
    $ev1 = Join-Path $suiteDir 'ev1'
    $args1 = $zipArg + @('-EvidenceDirectory', "`"$ev1`"")
    $child1 = Start-VerifyChild $ShellExe $args1 (Join-Path $suiteDir 's1.out') (Join-Path $suiteDir 's1.err')
    try {
        if (!$child1.Process.WaitForExit(90000)) {
            Stop-Process -Id $child1.Process.Id -Force -ErrorAction SilentlyContinue
            throw "$SuiteName (1) timed out after 90s."
        }
        $out1 = $child1.OutTask.GetAwaiter().GetResult()
        $err1 = $child1.ErrTask.GetAwaiter().GetResult()
        if ($child1.Process.ExitCode -ne 0) {
            throw "$SuiteName (1) failed (exit=$($child1.Process.ExitCode)):`n$out1`n$err1"
        }
        if (!(Test-Path -LiteralPath (Join-Path $ev1 'core.log') -PathType Leaf)) {
            throw "$SuiteName (1) did not collect core.log into EvidenceDirectory."
        }
    } finally {
        $child1.Process.Dispose()
    }
    Assert-NoVerifyOrphans "$SuiteName (1)" $pipeTag
    Assert-SentinelUnchanged "$SuiteName (1)"
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    $stubDir = Join-Path $fixtureRoot 'stub\Atrium'
    $stubSettingsDir = Join-Path $stubDir 'Settings'
    New-Item -ItemType Directory -Path $stubSettingsDir, (Join-Path $stubDir 'locales') -Force | Out-Null

    $asmVersion = $expectedVersion + '.0'
    $stubCoreCs = Join-Path $fixtureRoot 'StubCore.cs'
    $stubSettingsCs = Join-Path $fixtureRoot 'StubSettings.cs'
    $stubDllCs = Join-Path $fixtureRoot 'StubDll.cs'
    $stubCoreCode = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyVersion("__VER__")]
[assembly: AssemblyFileVersion("__VER__")]

internal static class StubCore
{
    private const int WmClose = 0x0010;
    private const string WindowProp = "Atrium.StubCore.Window";
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr hwnd, string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetProp(IntPtr hwnd, string name, IntPtr val);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr wp, IntPtr lp);

    private sealed class HostForm : Form
    {
        private readonly string _settingsExe;
        private readonly string _settingsArgs;

        public HostForm(string settingsExe, string settingsArgs)
        {
            _settingsExe = settingsExe;
            _settingsArgs = settingsArgs;
            ShowInTaskbar = false;
            FormBorderStyle = FormBorderStyle.FixedToolWindow;
            Size = new System.Drawing.Size(1, 1);
            StartPosition = FormStartPosition.Manual;
            Location = new System.Drawing.Point(-32000, -32000);
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            SetProp(Handle, WindowProp, new IntPtr(1));
            SetProp(Handle, "Atrium.Core.Host", new IntPtr(1));
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == 0x8002 && (m.LParam.ToInt64() & 0xFFFF) == 0x0400)
            {
                using (Process p = Process.Start(new ProcessStartInfo(_settingsExe, _settingsArgs) { UseShellExecute = false })) { }
            }
            base.WndProc(ref m);
        }
    }

    private static bool IsFullyQualified(string p)
    {
        if (string.IsNullOrEmpty(p) || !Path.IsPathRooted(p)) return false;
        if (p.Length >= 3 && char.IsLetter(p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
        return p.StartsWith(@"\\", StringComparison.Ordinal);
    }

    [STAThread]
    private static int Main(string[] args)
    {
        bool doExit = false;
        string guard = "", dataDir = "", pipeArg = "", readyEventName = "";
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            if (string.Equals(a, "--exit", StringComparison.Ordinal)) doExit = true;
            else if (string.Equals(a, "--guard", StringComparison.Ordinal) && i + 1 < args.Length) guard = args[++i];
            else if (string.Equals(a, "--data-dir", StringComparison.Ordinal) && i + 1 < args.Length) dataDir = args[++i];
            else if (string.Equals(a, "--pipe-name", StringComparison.Ordinal) && i + 1 < args.Length) pipeArg = args[++i];
            else if (string.Equals(a, "--ready-event", StringComparison.Ordinal) && i + 1 < args.Length) readyEventName = args[++i];
        }
        if (guard.Length > 0)
        {
            int parentPid = int.Parse(guard);
            try { using (Process p = Process.GetProcessById(parentPid)) { p.WaitForExit(); } } catch { }
            return 0;
        }
        if (doExit)
        {
            string selfExe = Assembly.GetExecutingAssembly().Location;
            int selfPid = Process.GetCurrentProcess().Id;
            IntPtr targetWin = IntPtr.Zero;
            int targetPid = 0;
            EnumWindows(delegate(IntPtr hwnd, IntPtr lp)
            {
                if (GetProp(hwnd, WindowProp) == IntPtr.Zero) return true;
                uint wpid;
                GetWindowThreadProcessId(hwnd, out wpid);
                if ((int)wpid == selfPid) return true;
                try
                {
                    using (Process p = Process.GetProcessById((int)wpid))
                    {
                        if (string.Equals(p.MainModule.FileName, selfExe, StringComparison.OrdinalIgnoreCase))
                        {
                            targetWin = hwnd;
                            targetPid = (int)wpid;
                            return false;
                        }
                    }
                }
                catch { }
                return true;
            }, IntPtr.Zero);
            if (targetWin == IntPtr.Zero) return 0;
            using (Process targetProc = Process.GetProcessById(targetPid))
            {
                PostMessage(targetWin, WmClose, IntPtr.Zero, IntPtr.Zero);
                return targetProc.WaitForExit(10000) ? 0 : 1;
            }
        }
        if (!IsFullyQualified(dataDir)) return 2;
        dataDir = Path.GetFullPath(dataDir);
        Directory.CreateDirectory(dataDir);
        string logDir = Path.Combine(dataDir, "logs");
        Directory.CreateDirectory(logDir);
        string logPath = Path.Combine(logDir, "core.log");
        File.AppendAllText(logPath, "core: starting" + Environment.NewLine, Encoding.UTF8);
        string settingsPath = Path.Combine(dataDir, "settings.json");
        string pipeName = pipeArg.Length > 0 ? pipeArg : "Atrium.Core." + Process.GetCurrentProcess().SessionId;
        string selfPath = Assembly.GetExecutingAssembly().Location;
        Process guardProc = Process.Start(new ProcessStartInfo(selfPath,
            "--guard " + Process.GetCurrentProcess().Id + " --development --data-dir \"" + dataDir + "\"")
        {
            UseShellExecute = false,
            CreateNoWindow = true
        });
        string trayExe = Path.Combine(Path.GetDirectoryName(selfPath), "Settings\\Atrium.Settings.exe");
        string trayArgs = "--development --data-dir \"" + dataDir + "\"" + (pipeArg.Length > 0 ? " --pipe-name \"" + pipeArg + "\"" : "");
        HostForm host = new HostForm(trayExe, trayArgs);
        bool stopping = false;
        double savedHeight = 60;
        bool savedClockSec = false;
        double currentHeight = 60;
        bool currentClockSec = false;
        long revision = 1;
        string instanceId = "stub-1";
        JavaScriptSerializer js = new JavaScriptSerializer();
        if (File.Exists(settingsPath))
        {
            Dictionary<string, object> d = js.DeserializeObject(File.ReadAllText(settingsPath, Encoding.UTF8)) as Dictionary<string, object>;
            if (d != null && d.ContainsKey("showClockSeconds")) savedClockSec = currentClockSec = Convert.ToBoolean(d["showClockSeconds"]);
        }
        object stateLock = new object();
        Thread pipeThread = new Thread(delegate()
        {
            bool signaled = false;
            while (!stopping)
            {
                try
                {
                    using (NamedPipeServerStream pipe = new NamedPipeServerStream(
                        pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Message, PipeOptions.Asynchronous))
                    {
                        if (!signaled && readyEventName.Length > 0)
                        {
                            signaled = true;
                            try { using (EventWaitHandle ev = EventWaitHandle.OpenExisting(readyEventName)) { ev.Set(); } } catch { }
                        }
                        IAsyncResult ar = pipe.BeginWaitForConnection(null, null);
                        while (!stopping && !ar.AsyncWaitHandle.WaitOne(100)) { }
                        if (stopping) break;
                        pipe.EndWaitForConnection(ar);
                        byte[] buf = new byte[4096];
                        for (;;)
                        {
                            MemoryStream ms = new MemoryStream();
                            int read;
                            do
                            {
                                read = pipe.Read(buf, 0, buf.Length);
                                if (read == 0) break;
                                ms.Write(buf, 0, read);
                            } while (!pipe.IsMessageComplete);
                            if (ms.Length == 0) break;
                            string reqStr = Encoding.UTF8.GetString(ms.ToArray());
                            Dictionary<string, object> req = js.DeserializeObject(reqStr) as Dictionary<string, object>;
                            string op = req != null && req.ContainsKey("op") ? Convert.ToString(req["op"]) : "";
                            string reply;
                            lock (stateLock)
                            {
                                if (op == "get")
                                {
                                    reply = js.Serialize(new Dictionary<string, object>
                                    {
                                        { "op", "state" }, { "protocol", 1 }, { "instanceId", instanceId },
                                        { "revision", revision }, { "path", settingsPath },
                                        { "saved", new Dictionary<string, object> { { "height", savedHeight }, { "showClockSeconds", savedClockSec } } },
                                        { "current", new Dictionary<string, object> { { "height", currentHeight }, { "showClockSeconds", currentClockSec } } }
                                    });
                                }
                                else if (op == "preview")
                                {
                                    Dictionary<string, object> sObj = req["settings"] as Dictionary<string, object>;
                                    if (sObj != null && sObj.ContainsKey("height")) currentHeight = Convert.ToDouble(sObj["height"]);
                                    if (sObj != null && sObj.ContainsKey("showClockSeconds")) currentClockSec = Convert.ToBoolean(sObj["showClockSeconds"]);
                                    reply = "{\"op\":\"ok\"}";
                                }
                                else if (op == "commit")
                                {
                                    long exp = req.ContainsKey("expectedRevision") ? Convert.ToInt64(req["expectedRevision"]) : -1;
                                    if (exp != revision)
                                    {
                                        reply = js.Serialize(new Dictionary<string, object>
                                        {
                                            { "op", "conflict" }, { "instanceId", instanceId }, { "revision", revision },
                                            { "saved", new Dictionary<string, object> { { "height", savedHeight }, { "showClockSeconds", savedClockSec } } },
                                            { "current", new Dictionary<string, object> {
                                                { "height", currentHeight }, { "showClockSeconds", currentClockSec } } }
                                        });
                                    }
                                    else
                                    {
                                        Dictionary<string, object> sObj = req["settings"] as Dictionary<string, object>;
                                        if (sObj != null && sObj.ContainsKey("height")) savedHeight = currentHeight = Convert.ToDouble(sObj["height"]);
                                        if (sObj != null && sObj.ContainsKey("showClockSeconds"))
                                        {
                                            savedClockSec = currentClockSec = Convert.ToBoolean(sObj["showClockSeconds"]);
                                        }
                                        File.WriteAllText(settingsPath, js.Serialize(new Dictionary<string, object>
                                        {
                                            { "height", savedHeight }, { "showClockSeconds", savedClockSec }
                                        }), Encoding.UTF8);
                                        revision++;
                                        reply = js.Serialize(new Dictionary<string, object>
                                        {
                                            { "op", "ok" }, { "instanceId", instanceId }, { "revision", revision }
                                        });
                                    }
                                }
                                else { reply = "{\"op\":\"error\"}"; }
                            }
                            byte[] outBytes = Encoding.UTF8.GetBytes(reply);
                            pipe.Write(outBytes, 0, outBytes.Length);
                            pipe.Flush();
                        }
                        lock (stateLock) { currentHeight = savedHeight; currentClockSec = savedClockSec; }
                    }
                }
                catch { if (!stopping) Thread.Sleep(50); }
            }
        });
        pipeThread.IsBackground = true;
        pipeThread.Start();
        host.FormClosed += delegate
        {
            stopping = true;
            try { if (guardProc != null && !guardProc.HasExited) guardProc.Kill(); } catch { }
            File.AppendAllText(logPath, "core: exited" + Environment.NewLine, Encoding.UTF8);
        };
        Application.Run(host);
        if (guardProc != null) guardProc.Dispose();
        return 0;
    }
}
'@
    $stubSettingsCode = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyVersion("__VER__")]
[assembly: AssemblyFileVersion("__VER__")]

internal static class StubSettings
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool SetProp(IntPtr hwnd, string name, IntPtr val);

    private static string GetArg(string[] args, string name)
    {
        for (int i = 0; i + 1 < args.Length; i++)
        {
            if (string.Equals(args[i], name, StringComparison.Ordinal)) return args[i + 1];
        }
        return "";
    }

    private static Dictionary<string, object> SendPipe(string pipeName, string json)
    {
        using (NamedPipeClientStream pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut))
        {
            pipe.Connect(5000);
            pipe.ReadMode = PipeTransmissionMode.Message;
            byte[] req = Encoding.UTF8.GetBytes(json);
            pipe.Write(req, 0, req.Length);
            pipe.Flush();
            MemoryStream ms = new MemoryStream();
            byte[] buf = new byte[4096];
            do
            {
                int n = pipe.Read(buf, 0, buf.Length);
                if (n == 0) break;
                ms.Write(buf, 0, n);
            } while (!pipe.IsMessageComplete);
            return (new JavaScriptSerializer()).DeserializeObject(Encoding.UTF8.GetString(ms.ToArray())) as Dictionary<string, object>;
        }
    }

    [STAThread]
    private static int Main(string[] args)
    {
        string dataDir = GetArg(args, "--data-dir");
        if (string.IsNullOrEmpty(dataDir) || !Path.IsPathRooted(dataDir)) return 2;
        dataDir = Path.GetFullPath(dataDir);
        string logDir = Path.Combine(dataDir, "logs");
        Directory.CreateDirectory(logDir);
        string pipeName = GetArg(args, "--pipe-name");
        if (pipeName.Length == 0) pipeName = "Atrium.Core." + Process.GetCurrentProcess().SessionId;
        string readyEventName = GetArg(args, "--ready-event");
        Dictionary<string, object> st = SendPipe(pipeName, "{\"op\":\"get\"}");
        Dictionary<string, object> saved = st["saved"] as Dictionary<string, object>;
        bool initialSec = saved != null && saved.ContainsKey("showClockSeconds") && Convert.ToBoolean(saved["showClockSeconds"]);
        string instanceId = Convert.ToString(st["instanceId"]);
        long revision = Convert.ToInt64(st["revision"]);
        double height = saved != null && saved.ContainsKey("height") ? Convert.ToDouble(saved["height"]) : 60;

        Application.EnableVisualStyles();
        Form form = new Form();
        form.Text = "Atrium Settings Stub";
        form.ClientSize = new Size(320, 140);
        CheckBox chk = new CheckBox();
        chk.Name = "ClockSecondsSwitch";
        chk.Text = "Show seconds";
        chk.Checked = initialSec;
        chk.Location = new Point(20, 20);
        chk.AutoSize = true;
        Button btn = new Button();
        btn.Name = "SaveButton";
        btn.Text = "Save";
        btn.Enabled = false;
        btn.Visible = false;
        btn.Location = new Point(20, 60);
        chk.CheckedChanged += delegate
        {
            bool dirty = (chk.Checked != initialSec);
            btn.Visible = dirty;
            btn.Enabled = dirty;
        };
        btn.Click += delegate
        {
            JavaScriptSerializer js = new JavaScriptSerializer();
            string commitJson = js.Serialize(new Dictionary<string, object>
            {
                { "op", "commit" }, { "instanceId", instanceId }, { "expectedRevision", revision },
                { "settings", new Dictionary<string, object> { { "height", height }, { "showClockSeconds", chk.Checked } } }
            });
            Dictionary<string, object> rep = SendPipe(pipeName, commitJson);
            if (rep != null && Convert.ToString(rep["op"]) == "ok")
            {
                initialSec = chk.Checked;
                revision = Convert.ToInt64(rep["revision"]);
                btn.Enabled = false;
                btn.Visible = false;
            }
        };
        form.Controls.Add(chk);
        form.Controls.Add(btn);
        form.Shown += delegate
        {
            SetProp(form.Handle, "Atrium.Settings.Window", new IntPtr(1));
            if (readyEventName.Length > 0)
            {
                try { using (EventWaitHandle ev = EventWaitHandle.OpenExisting(readyEventName)) { ev.Set(); } } catch { }
            }
        };
        Application.Run(form);
        return 0;
    }
}
'@
    $stubDllCode = "using System.Reflection;`r`n[assembly: AssemblyVersion(`"$asmVersion`")]`r`n[assembly: AssemblyFileVersion(`"$asmVersion`")]`r`n"
    Write-Utf8File $stubCoreCs ($stubCoreCode.Replace('__VER__', $asmVersion)) $false
    Write-Utf8File $stubSettingsCs ($stubSettingsCode.Replace('__VER__', $asmVersion)) $false
    Write-Utf8File $stubDllCs $stubDllCode $false

    $coreOut = & $csc /nologo /optimize+ /target:winexe `
        /reference:System.dll,System.Drawing.dll,System.Web.Extensions.dll,System.Windows.Forms.dll `
        "/out:$(Join-Path $stubDir 'Atrium.exe')" $stubCoreCs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile StubCore: $($coreOut -join "`n")" }
    $setOut = & $csc /nologo /optimize+ /target:winexe `
        /reference:System.dll,System.Drawing.dll,System.Web.Extensions.dll,System.Windows.Forms.dll `
        "/out:$(Join-Path $stubSettingsDir 'Atrium.Settings.exe')" $stubSettingsCs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile StubSettings: $($setOut -join "`n")" }
    $dllOut = & $csc /nologo /optimize+ /target:library `
        "/out:$(Join-Path $stubSettingsDir 'Atrium.Settings.dll')" $stubDllCs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile StubDll: $($dllOut -join "`n")" }

    Write-Utf8File (Join-Path $stubSettingsDir 'Update.ps1') "# stub`r`n" $true
    Write-Utf8File (Join-Path $stubDir 'Uninstall.ps1') "# stub`r`n" $true
    Write-Utf8File (Join-Path $stubDir 'LICENSE') "MIT`n" $false
    Write-Utf8File (Join-Path $stubDir 'locales\manifest.json') '{"languages":[{"tag":"en"}]}' $false

    $stubFull = [IO.Path]::GetFullPath($stubDir).TrimEnd('\')
    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($f in @(Get-ChildItem -LiteralPath $stubFull -Recurse -File | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($stubFull.Length + 1).Replace('/', '\')
        $entries.Add([ordered]@{ path = $rel; sha256 = (Get-FileSha256 $f.FullName) })
    }
    $manifestJson = ([ordered]@{ schema = 1; version = $expectedVersion; files = $entries } | ConvertTo-Json -Depth 4) -replace "`r`n", "`n"
    Write-Utf8File (Join-Path $stubFull 'package-manifest.json') ($manifestJson + "`n") $false

    $stubZip = Join-Path $fixtureRoot "Atrium-$expectedVersion-win-x64.zip"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stubDir, $stubZip, [IO.Compression.CompressionLevel]::Optimal, $true)

    # Read-only verify-package check on stub zip
    & $verifyScript -ZipPath $stubZip
    if ($LASTEXITCODE) { throw "Read-only verify-package failed on stub zip." }
    Assert-SentinelUnchanged 'read-only stub verify'

    # Invalid --data-dir must exit non-zero without touching sentinel
    $badProc = Start-Process -FilePath (Join-Path $stubDir 'Atrium.exe') -ArgumentList '--development --data-dir relative\bad' `
        -WindowStyle Hidden -PassThru
    try {
        if (!$badProc.WaitForExit(10000) -or $badProc.ExitCode -eq 0) {
            throw "Stub Atrium.exe accepted invalid --data-dir (exit=$($badProc.ExitCode))."
        }
    } finally { $badProc.Dispose() }
    Assert-SentinelUnchanged 'invalid --data-dir rejection'

    $hostRunning = (@(Get-Process -Name Atrium -ErrorAction SilentlyContinue).Count -gt 0) -or
        (@(Get-Process -Name 'Atrium.Settings' -ErrorAction SilentlyContinue).Count -gt 0)
    if ($hostRunning) {
        if ($RequireRealPackage) {
            throw 'Cannot run -RequireRealPackage while Atrium or Atrium.Settings is already running.'
        }
        $guardChild = Start-VerifyChild $pwshExe @('-ZipPath', "`"$stubZip`"", '-SmokeStart') `
            (Join-Path $fixtureRoot 'guard.out') (Join-Path $fixtureRoot 'guard.err')
        try {
            if (!$guardChild.Process.WaitForExit(15000)) {
                Stop-Process -Id $guardChild.Process.Id -Force -ErrorAction SilentlyContinue
                throw 'Running-process guard timed out.'
            }
            $gOut = $guardChild.OutTask.GetAwaiter().GetResult()
            $gErr = $guardChild.ErrTask.GetAwaiter().GetResult()
            if ($guardChild.Process.ExitCode -eq 0 -or "$gOut`n$gErr" -notmatch 'already running') {
                throw "Expected running-process guard failure, got exit=$($guardChild.Process.ExitCode): $gOut`n$gErr"
            }
        } finally { $guardChild.Process.Dispose() }
        Assert-SentinelUnchanged 'running-process guard'
    }

    Invoke-ThreeSentinelSuite $stubZip $ps51 $true 'stub-ps51'
    if (![string]::Equals($pwshExe, $ps51, [StringComparison]::OrdinalIgnoreCase)) {
        Invoke-ThreeSentinelSuite $stubZip $pwshExe $true 'stub-pwsh'
    }

    if ($CandidateZipPath) {
        $candFull = [IO.Path]::GetFullPath($CandidateZipPath)
        if (!(Test-Path -LiteralPath $candFull -PathType Leaf)) { throw "Candidate zip not found: $candFull" }
        & $verifyScript -ZipPath $candFull
        if ($LASTEXITCODE) { throw "Read-only verify-package failed on candidate zip: $candFull" }
        Assert-SentinelUnchanged 'read-only candidate verify'
        if (!$hostRunning) {
            Invoke-ThreeSentinelSuite $candFull $pwshExe $false 'real-pwsh'
        } else {
            Write-Host 'Host Atrium is running; skipped live -SmokeStart on candidate zip (stub + guard verified).'
        }
    } elseif ($RequireRealPackage) {
        throw '-RequireRealPackage requires -CandidateZipPath.'
    }
    Write-Host 'verify-package tests passed.'
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue }
}
