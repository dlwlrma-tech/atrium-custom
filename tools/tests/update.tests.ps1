param([string]$CandidateZipPath = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-ownership.fixtures.ps1')
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceUpdate = Join-Path $project 'tools\Update.ps1'
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('Atrium-update-tests-' + [Guid]::NewGuid().ToString('N'))
$stubSourcePath = Join-Path $fixtureRoot 'Stub.cs'
$stubExe = Join-Path $fixtureRoot 'stub.exe'
function Populate-App([string]$Dir, [hashtable]$Mode) {
    $sDir = Join-Path $Dir 'Settings'; New-Item -ItemType Directory -Path $sDir -Force | Out-Null
    Copy-Item -LiteralPath $stubExe -Destination (Join-Path $Dir 'Atrium.exe') -Force
    Copy-Item -LiteralPath $stubExe -Destination (Join-Path $sDir 'Atrium.Settings.exe') -Force
    $lines = foreach ($k in $Mode.Keys) { "$k=$($Mode[$k])" }
    Set-Content -LiteralPath (Join-Path $Dir 'mode.txt') -Value $lines -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $sDir 'mode.txt') -Value $lines -Encoding ASCII
}
function Write-TestManifest([string]$Dir, [string]$Version = '2.8.0') { Write-SharedPackageManifest $Dir $Version }
function Wait-Log([string]$LogPath, [string]$Pattern) {
    $until = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $until) {
        if ((Test-Path -LiteralPath $LogPath) -and ((Get-Content -LiteralPath $LogPath -Raw) -match $Pattern)) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for log pattern: $Pattern"
}
function Stop-CaseProcesses([string]$CaseDir, [System.Diagnostics.Process[]]$Extra = @()) {
    foreach ($p in $Extra) {
        try { if ($p -and !$p.HasExited) { $p.Kill(); [void]$p.WaitForExit(2000) }; if ($p) { $p.Dispose() } } catch { }
    }
    $prefix = $CaseDir.TrimEnd('\') + '\'
    foreach ($p in @(Get-Process -EA SilentlyContinue)) {
        try {
            if ($p.Path -and $p.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $p.Id -Force -EA SilentlyContinue; [void]$p.WaitForExit(2000)
            }
        } catch { }
    }
    try {
        foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)) {
            if ($cim.CommandLine -and $cim.CommandLine.IndexOf($CaseDir, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and [int]$cim.ProcessId -ne $PID) {
                $procId = [int]$cim.ProcessId
                $target = Get-Process -Id $procId -ErrorAction SilentlyContinue
                Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
                if ($target) { [void]$target.WaitForExit(2000); $target.Dispose() }
            }
        }
    } catch { }
}
function New-CaseLayout([string]$Name) {
    $caseDir = Join-Path $fixtureRoot $Name; $install = Join-Path $caseDir 'Atrium'
    $cache = Join-Path $caseDir 'cache'; $pkg = Join-Path $cache 'package-background'
    New-Item -ItemType Directory -Path $install, $pkg | Out-Null
    $scriptCopy = Join-Path $cache 'Update.ps1'; Copy-Item -LiteralPath $sourceUpdate -Destination $scriptCopy -Force
    $statePath = Join-Path $cache 'ready.json'; '{}' | Set-Content -LiteralPath $statePath -Encoding UTF8
    return @{ Dir = $caseDir; Install = $install; Package = $pkg; Script = $scriptCopy; State = $statePath
        Result = (Join-Path $cache 'result.json'); Log = (Join-Path $caseDir 'events.log'); Exit = (Join-Path $caseDir 'exit.sig') }
}
function Invoke-Updater($C, [int]$WaitPid = 0, [string]$LegacyManifestPath = '') {
    if ($WaitPid -le 0) { $d = Start-Process -FilePath $stubExe -ArgumentList '--sleep 10' -WindowStyle Hidden -PassThru; $WaitPid = $d.Id; $d.Dispose() }
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $C.Script, '-InstallDirectory', $C.Install, '-PackageDirectory', $C.Package,
        '-StatePath', $C.State, '-WaitForProcessId', $WaitPid, '-StartCore', '1', '-StartupTimeoutSeconds', '4')
    if ($LegacyManifestPath) { $args += @('-LegacyManifestPath', $LegacyManifestPath) }
    $proc = Start-Process -FilePath $ps51 -ArgumentList $args -WindowStyle Hidden -PassThru
    try {
        if (!$proc.WaitForExit(45000)) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; [void]$proc.WaitForExit(3000)
            throw "Update.ps1 timed out after 45s in $($C.Dir)"
        }
        $script:updaterExitCode = $proc.ExitCode
    } finally { $proc.Dispose() }
}
function Run-RollbackCase($c, [System.Collections.ArrayList]$tracked) {
    [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
    Wait-Log $c.Log 'v1:Atrium\.exe:start'
    Invoke-Updater $c
    return (Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json)
}
function Assert-V1Restarted($c) { Wait-Log $c.Log '(?s)v1:Atrium\.exe:start.*v1:Atrium\.exe:start'; Wait-Log $c.Log 'v1:Atrium\.Settings\.exe:start' }
try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $stubSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
static class Stub {
    static Dictionary<string, string> ReadMode(string dir) {
        var m = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string p = Path.Combine(dir, "mode.txt");
        if (!File.Exists(p)) return m;
        foreach (string line in File.ReadAllLines(p)) { int i = line.IndexOf('='); if (i > 0) m[line.Substring(0, i).Trim()] = line.Substring(i + 1).Trim(); }
        return m;
    }
    static string Get(Dictionary<string, string> m, string k) { string v; return m.TryGetValue(k, out v) ? v : ""; }
    static void Log(Dictionary<string, string> m, string msg) {
        string p = Get(m, "events_log"); if (p.Length == 0) return;
        for (int i = 0; i < 20; i++) { try { File.AppendAllText(p, msg + Environment.NewLine); return; } catch { Thread.Sleep(20); } }
    }
    static bool ShouldExit(bool respectExit, string exitFile, bool exitExisted, DateTime startWrite) {
        return respectExit && exitFile.Length > 0 && File.Exists(exitFile) &&
            (!exitExisted || File.GetLastWriteTimeUtc(exitFile) > startWrite);
    }
    static void WriteReady(string path) {
        if (path.Length == 0) return;
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        File.WriteAllText(path, "1");
    }
    static bool SpawnChildAndWait(Dictionary<string, string> m, string exePath, string args, string tag) {
        string logDir = Path.GetDirectoryName(Get(m, "events_log"));
        string readyDir = string.IsNullOrEmpty(logDir) ? Path.GetTempPath() : logDir;
        string readyPath = Path.Combine(readyDir, tag + "-" + Guid.NewGuid().ToString("N") + ".ready");
        using (Process p = Process.Start(new ProcessStartInfo(exePath, args + " --lock-ready \"" + readyPath + "\"") { UseShellExecute = false })) {
            for (int i = 0; i < 100 && !File.Exists(readyPath); i++) {
                if (p != null && p.HasExited) return false;
                Thread.Sleep(50);
            }
        }
        return File.Exists(readyPath);
    }
    static int Main(string[] args) {
        string dir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), exe = Path.GetFileName(Assembly.GetExecutingAssembly().Location);
        var m = ReadMode(dir);
        string ver = Get(m, "version"), v2m = Get(m, "v2_manifest");
        if (v2m.Length > 0) {
            string rootDir = exe.Equals("Atrium.Settings.exe", StringComparison.OrdinalIgnoreCase) ? Path.GetDirectoryName(dir) : dir;
            string mp = Path.Combine(rootDir, "package-manifest.json");
            if (File.Exists(mp) && File.ReadAllText(mp).Contains("\"" + v2m + "\"")) ver = "v2";
        }
        string readyEv = "", waitEv = "", lockP = "", lockR = ""; int sleepMs = 0;
        bool isExit = false, isGuard = false, isResume = false, lockReadOnly = false;
        for (int i = 0; i < args.Length; i++) {
            if (args[i] == "--ready-event" && i + 1 < args.Length) readyEv = args[++i];
            else if (args[i] == "--wait-event" && i + 1 < args.Length) waitEv = args[++i];
            else if (args[i] == "--lock" && i + 1 < args.Length) lockP = args[++i];
            else if (args[i] == "--lock-read" && i + 1 < args.Length) { lockP = args[++i]; lockReadOnly = true; }
            else if (args[i] == "--lock-ready" && i + 1 < args.Length) lockR = args[++i];
            else if (args[i] == "--sleep" && i + 1 < args.Length) int.TryParse(args[++i], out sleepMs);
            else if (args[i] == "--exit") isExit = true; else if (args[i] == "--guard") isGuard = true; else if (args[i] == "--resume-update") isResume = true;
        }
        if (sleepMs > 0) { Thread.Sleep(sleepMs); return 0; }
        if (waitEv.Length > 0) { using (var ev = EventWaitHandle.OpenExisting(waitEv)) ev.WaitOne(30000); return 0; }
        if (lockP.Length > 0) {
            Directory.CreateDirectory(Path.GetDirectoryName(lockP));
            FileAccess acc = lockReadOnly ? FileAccess.Read : FileAccess.ReadWrite;
            FileShare sh = lockReadOnly ? FileShare.Read : FileShare.None;
            using (new FileStream(lockP, FileMode.OpenOrCreate, acc, sh)) {
                WriteReady(lockR); Thread.Sleep(60000);
            } return 0;
        }
        string exitFile = Get(m, "exit_signal");
        if (isExit) { if (exitFile.Length > 0) File.WriteAllText(exitFile, "1"); Log(m, ver + ":" + exe + ":exit"); Thread.Sleep(150); return 0; }
        bool respectExit = Get(m, "respect_exit") == "1";
        bool exitExisted = exitFile.Length > 0 && File.Exists(exitFile);
        DateTime startWrite = exitExisted ? File.GetLastWriteTimeUtc(exitFile) : DateTime.MinValue;
        if (isGuard || isResume) {
            string rel = Get(m, isGuard ? "guard_lock" : "headless_lock"); FileStream fs = null;
            if (rel.Length > 0) {
                string full = Path.Combine(isResume ? Path.GetDirectoryName(dir) : dir, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(full));
                fs = new FileStream(full, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            }
            try {
                Log(m, ver + ":" + exe + ":" + (isGuard ? "--guard" : "--resume-update") + ":start");
                WriteReady(lockR);
                int holdMs = 0; if (isResume) int.TryParse(Get(m, "headless_hold_ms"), out holdMs);
                for (int e = 0; e < 60000; e += 50) {
                    if ((holdMs > 0 && e >= holdMs) || (holdMs == 0 && ShouldExit(respectExit, exitFile, exitExisted, startWrite))) break;
                    Thread.Sleep(50);
                }
            } finally { if (fs != null) fs.Dispose(); }
            return 0;
        }
        Log(m, ver + ":" + exe + ":start");
        if (exe.Equals("Atrium.Settings.exe", StringComparison.OrdinalIgnoreCase)) {
            string spawnExe = Get(m, "spawn_lock_exe");
            if (spawnExe.Length > 0) {
                string lr = Get(m, "spawn_lock_ready");
                Process.Start(new ProcessStartInfo(spawnExe,
                    "--lock \"" + Get(m, "spawn_lock_target") + "\" --lock-ready \"" + lr + "\"") { UseShellExecute = false });
                for (int i = 0; i < 60 && !File.Exists(lr); i++) Thread.Sleep(50);
            }
            int code = 0; int.TryParse(Get(m, "settings_exit_code"), out code);
            if (code != 0) { Log(m, ver + ":" + exe + ":fail"); return code; }
        }
        if (exe.Equals("Atrium.exe", StringComparison.OrdinalIgnoreCase)) {
            string self = Assembly.GetExecutingAssembly().Location;
            if (Get(m, "spawn_guard") == "1" && !SpawnChildAndWait(m, self, "--guard " + Process.GetCurrentProcess().Id, "guard")) return 2;
            if (Get(m, "spawn_headless") == "1" &&
                !SpawnChildAndWait(m, Path.Combine(dir, "Settings\\Atrium.Settings.exe"), "--resume-update", "headless")) return 2;
        }
        if (readyEv.Length > 0) using (var ev = EventWaitHandle.OpenExisting(readyEv)) ev.Set();
        for (int e = 0; e < 60000; e += 50) { if (ShouldExit(respectExit, exitFile, exitExisted, startWrite)) break; Thread.Sleep(50); }
        return 0;
    }
}
'@
    Set-Content -LiteralPath $stubSourcePath -Value $stubSource -Encoding UTF8
    & $csc /nologo /target:winexe /out:$stubExe $stubSourcePath
    if ($LASTEXITCODE -ne 0) { throw 'Failed to compile stub.exe.' }
    # Case 1: Normal update with headless workers and Hidden+ReadOnly files (manifest-driven)
    $c = New-CaseLayout 'case1-success'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log
            spawn_guard = '1'; spawn_headless = '1'; guard_lock = 'Settings\guard.lock'; headless_lock = 'Settings\headless.lock'; headless_hold_ms = '1800' }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        Set-Content -LiteralPath (Join-Path $c.Install 'development.txt') -Value 'dev' -Encoding UTF8
        $hiddenOld = Join-Path $c.Install 'secret.dat'; Set-Content -LiteralPath $hiddenOld -Value 'v1-secret' -Encoding UTF8
        (Get-Item -LiteralPath $hiddenOld).Attributes = 'Hidden, ReadOnly'
        Set-Content -LiteralPath (Join-Path $c.Package 'secret.dat') -Value 'v2-secret' -Encoding UTF8
        Write-TestManifest $c.Install '2.7.0'; Write-TestManifest $c.Package '2.8.0'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:--guard:start'; Wait-Log $c.Log 'v1:Atrium\.Settings\.exe:--resume-update:start'
        $uiProc = Start-Process -FilePath $stubExe -ArgumentList '--sleep 400' -WindowStyle Hidden -PassThru; [void]$tracked.Add($uiProc)
        Invoke-Updater $c $uiProc.Id
        if ($script:updaterExitCode -ne 0) { throw "Case 1 failed with exit code $script:updaterExitCode" }
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($res.error -ne '' -or (Test-Path -LiteralPath $c.State) -or (Test-Path -LiteralPath $c.Package)) { throw "Case 1 failed: $($res.error)" }
        if (!(Test-Path -LiteralPath (Join-Path $c.Install 'development.txt')) -or
            (Get-Content -LiteralPath $hiddenOld -Raw).Trim() -ne 'v2-secret') { throw 'Case 1: file mismatch' }
        $newM = Get-Content -LiteralPath (Join-Path $c.Install 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($newM.version -ne '2.8.0') { throw 'Case 1: manifest version not updated' }
        Wait-Log $c.Log 'v2:Atrium\.exe:start'; Wait-Log $c.Log 'v2:Atrium\.Settings\.exe:start'
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 2: New Settings fails with --guard and untracked child holding locks (P0-4)
    $c = New-CaseLayout 'case2-rollback-locks'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '0'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log
            spawn_guard = '1'; spawn_headless = '1'; guard_lock = 'Settings\guard.lock'; headless_lock = 'Settings\headless.lock'; settings_exit_code = '1' }
        $hiddenOld = Join-Path $c.Install 'secret.dat'; Set-Content -LiteralPath $hiddenOld -Value 'v1-secret' -Encoding UTF8
        (Get-Item -LiteralPath $hiddenOld).Attributes = 'Hidden, ReadOnly'
        Set-Content -LiteralPath (Join-Path $c.Package 'secret.dat') -Value 'v2-secret' -Encoding UTF8
        Write-TestManifest $c.Install '2.7.0'; Write-TestManifest $c.Package '2.8.0'
        $res = Run-RollbackCase $c $tracked
        if (!$res.error -or !(Test-Path -LiteralPath $c.State) -or
            (Get-Content -LiteralPath $hiddenOld -Raw -ErrorAction SilentlyContinue).Trim() -ne 'v1-secret') {
            throw "Case 2: rollback mismatch (err=$($res.error); state=$(Test-Path -LiteralPath $c.State); exists=$([IO.File]::Exists($hiddenOld)))"
        }
        $restoredM = Get-Content -LiteralPath (Join-Path $c.Install 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($restoredM.version -ne '2.7.0') { throw 'Case 2: manifest not rolled back' }
        if (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.*-*').Count -ne 0) { throw 'Case 2: temp folders remained' }
        Wait-Log $c.Log 'v2:Atrium\.exe:--guard:start'
        Wait-Log $c.Log 'v2:Atrium\.Settings\.exe:--resume-update:start'
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 3: External process locks a file during rollback -> partial rollback resilience
    $c = New-CaseLayout 'case3-partial-rollback'; $tracked = New-Object System.Collections.ArrayList
    try {
        $extra = Join-Path $c.Install 'extra.dll'
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log
            settings_exit_code = '1'; spawn_lock_exe = $stubExe; spawn_lock_target = $extra; spawn_lock_ready = (Join-Path $c.Dir 'lock.ready') }
        Set-Content -LiteralPath $extra -Value 'v1-extra' -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $c.Package 'extra.dll') -Value 'v2-extra' -Encoding UTF8
        Write-TestManifest $c.Install '2.7.0'; Write-TestManifest $c.Package '2.8.0'
        $res = Run-RollbackCase $c $tracked
        $backupDirs = @(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.previous-*')
        if ($backupDirs.Count -ne 1 -or
            !(Test-Path -LiteralPath (Join-Path $backupDirs[0].FullName 'extra.dll'))) { throw 'Case 3: expected extra.dll in backup' }
        if ($res.error -notmatch [regex]::Escape($backupDirs[0].FullName)) { throw "Case 3: error should mention backup path, got: $($res.error)" }
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 4: Stage 1 backup failure on tracked file must not lose user files
    $c = New-CaseLayout 'case4-stage1-fail'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        $lockedFile = Join-Path $c.Install 'locked.dat'; Set-Content -LiteralPath $lockedFile -Value 'v1-locked' -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $c.Package 'locked.dat') -Value 'v2-locked' -Encoding UTF8
        Write-TestManifest $c.Install '2.7.0'; Write-TestManifest $c.Package '2.8.0'
        $userFile = Join-Path $c.Install 'user-file.txt'; $userBytes = Write-Utf8Bytes $userFile 'keep-me'
        $lockReady = Join-Path $c.Dir 'lock.ready'
        $lockProc = Start-Process -FilePath $stubExe -ArgumentList "--lock-read `"$lockedFile`" --lock-ready `"$lockReady`"" -WindowStyle Hidden -PassThru
        [void]$tracked.Add($lockProc)
        $until = [DateTime]::UtcNow.AddSeconds(5)
        while (!(Test-Path -LiteralPath $lockReady) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 30 }
        if (!(Test-Path -LiteralPath $lockReady)) { throw 'Case 4: lock helper did not become ready in time' }
        $res = Run-RollbackCase $c $tracked
        if (!$res.error -or !(Test-Path -LiteralPath $userFile)) { throw 'Case 4: missing error or user-file.txt lost' }
        Stop-Process -Id $lockProc.Id -Force -ErrorAction SilentlyContinue; [void]$lockProc.WaitForExit(2000)
        Assert-SameBytes $userFile $userBytes 'Case 4'
        if (!(Test-Path -LiteralPath (Join-Path $c.Install 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $c.Install 'Settings\Atrium.Settings.exe'))) { throw 'Case 4: v1 binaries missing' }
        if (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.*-*').Count -ne 0) { throw "Case 4: temp folders remained ($($res.error))" }
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    Write-Host 'Updater Cases 1-4 passed.'
    # Case 5a: Real v2.7.0 in-place Update.ps1 baseline reproduction (confirms old script deletes user files)
    $c = New-CaseLayout 'case5a-v270-baseline'; $tracked = New-Object System.Collections.ArrayList
    try {
        $v270Lines = & git -C $project show 'v2.7.0:tools/Update.ps1'
        if ($LASTEXITCODE -ne 0 -or -not $v270Lines) { throw 'Case 5a: failed to read v2.7.0:tools/Update.ps1' }
        [IO.File]::WriteAllText($c.Script, ($v270Lines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding $true))
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        $uRoot = Join-Path $c.Install 'user-root.txt'; Set-Content -LiteralPath $uRoot -Value 'user-root' -Encoding UTF8
        $uSet = Join-Path $c.Install 'Settings\custom.ini'; Set-Content -LiteralPath $uSet -Value 'custom=1' -Encoding UTF8
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        Invoke-Updater $c
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($res.error -ne '') { throw "Case 5a failed: $($res.error)" }
        if ((Test-Path -LiteralPath $uRoot) -or (Test-Path -LiteralPath $uSet)) {
            throw 'Case 5a: expected v2.7.0 Update.ps1 to remove user files in place'
        }
        Write-Host 'Case 5a passed (confirmed v2.7.0 Update.ps1 deletes user-root.txt and Settings\custom.ini in place).'
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 5b: Safe v2.7.0 migration path (record hashes first, extract candidate ZIP to empty dir, re-verify both)
    $case5bDir = Join-Path $fixtureRoot 'case5b-safe-migration'
    $oldDir = Join-Path $case5bDir 'Atrium-2.7.0'
    Populate-App $oldDir @{ version = 'v270' }
    $userSamples = Get-SharedUserFileSamples
    $recordedHashes = @{}; $recordedBytes = @{}
    foreach ($rel in $userSamples.Keys) {
        $full = Join-Path $oldDir $rel
        $recordedBytes[$rel] = Write-Utf8Bytes $full $userSamples[$rel]
        $recordedHashes[$rel] = Get-SharedFileSha256 $full
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipToExtract = $CandidateZipPath
    if (!$zipToExtract) {
        $cmakeText = Get-Content -LiteralPath (Join-Path $project 'native\CMakeLists.txt') -Raw
        if ($cmakeText -match 'VERSION\s+(\d+\.\d+\.\d+)') {
            $distZip = Join-Path $project ("dist\Atrium-{0}-win-x64.zip" -f $Matches[1])
            if (Test-Path -LiteralPath $distZip -PathType Leaf) {
                $za = [IO.Compression.ZipFile]::OpenRead($distZip)
                try {
                    if ($za.Entries | Where-Object { $_.FullName -match '(^|/)package-manifest\.json$' } | Select-Object -First 1) {
                        $zipToExtract = $distZip
                    }
                } finally { $za.Dispose() }
            }
        }
    } elseif (!(Test-Path -LiteralPath $zipToExtract -PathType Leaf)) {
        throw "Case 5b: CandidateZipPath not found: $zipToExtract"
    }
    if (!$zipToExtract) {
        $stageRoot = Join-Path $case5bDir 'stage\Atrium'
        Populate-App $stageRoot @{ version = 'v280' }
        [void](Write-Utf8Bytes (Join-Path $stageRoot 'LICENSE') 'MIT')
        [void](Write-Utf8Bytes (Join-Path $stageRoot 'locales\manifest.json') '{"source":"en"}')
        $modSample = Get-SharedModifiedOfficialSample
        [void](Write-Utf8Bytes (Join-Path $stageRoot $modSample.Path) $modSample.Official)
        Copy-Item -LiteralPath $sourceUpdate -Destination (Join-Path $stageRoot 'Settings\Update.ps1') -Force
        Copy-Item -LiteralPath (Join-Path $project 'tools\Uninstall.ps1') -Destination (Join-Path $stageRoot 'Uninstall.ps1') -Force
        Write-TestManifest $stageRoot '2.8.0'
        $zipToExtract = Join-Path $case5bDir 'Atrium-candidate.zip'
        [IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $zipToExtract, [IO.Compression.CompressionLevel]::Optimal, $true)
    }
    $extractParent = Join-Path $case5bDir 'Atrium-new'
    New-Item -ItemType Directory -Path $extractParent -Force | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($zipToExtract, $extractParent)
    $newDir = Join-Path $extractParent 'Atrium'
    if (!(Test-Path -LiteralPath $newDir -PathType Container)) { $newDir = $extractParent }
    foreach ($rel in $userSamples.Keys) {
        $full = Join-Path $oldDir $rel
        Assert-SameBytes $full $recordedBytes[$rel] 'Case 5b'
        if ((Get-SharedFileSha256 $full) -cne $recordedHashes[$rel]) { throw "Case 5b: hash changed for $rel" }
    }
    $verifiedCount = Assert-VerifiedPackageDirectory $newDir
    Write-Host "Case 5b passed (extracted $zipToExtract; $verifiedCount manifest files & $($userSamples.Count) user hashes intact)."
    # Case 6: Manifest-less upgrade ownership (exact legacy hash catalog required; zero path wildcards)
    $modSample = Get-SharedModifiedOfficialSample
    $c = New-CaseLayout 'case6a-legacy-catalog-preserve'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; v2_manifest = '2.8.0'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        [void](Write-Utf8Bytes (Join-Path $c.Install 'Atrium.Core.exe') 'legacy-core')
        [void](Write-Utf8Bytes (Join-Path $c.Install 'locales\manifest.json') '{"old":1}')
        [void](Write-Utf8Bytes (Join-Path $c.Install $modSample.Path) $modSample.Official)
        [void](Write-Utf8Bytes (Join-Path $c.Install 'Settings\Atrium.Settings.dll') 'old-dll')
        [void](Write-Utf8Bytes (Join-Path $c.Package 'locales\manifest.json') '{"new":2}')
        [void](Write-Utf8Bytes (Join-Path $c.Package $modSample.Path) $modSample.Official)
        [void](Write-Utf8Bytes (Join-Path $c.Package 'Settings\Atrium.Settings.dll') 'new-dll')
        Write-SharedLegacyCatalog $c.Install (Join-Path $c.Package 'Settings\legacy-manifests.json') '2.7.0'
        Write-TestManifest $c.Package '2.8.0'
        $kept = @{}
        foreach ($rel in $userSamples.Keys) { $kept[$rel] = Write-Utf8Bytes (Join-Path $c.Install $rel) $userSamples[$rel] }
        $bSecretPath = Join-Path $c.Install 'Settings\user-secret.dat'
        $bSecret = Write-Utf8Bytes $bSecretPath 'keep-secret'
        (Get-Item -LiteralPath $bSecretPath).Attributes = 'Hidden, ReadOnly'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        Invoke-Updater $c
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($script:updaterExitCode -ne 0 -or $res.error -ne '') { throw "Case 6a failed: $($res.error)" }
        if (Test-Path -LiteralPath (Join-Path $c.Install 'Atrium.Core.exe')) { throw 'Case 6a: Atrium.Core.exe should be retired' }
        if (!(Test-Path -LiteralPath (Join-Path $c.Install 'package-manifest.json'))) { throw 'Case 6a: missing new manifest' }
        foreach ($rel in $userSamples.Keys) { Assert-SameBytes (Join-Path $c.Install $rel) $kept[$rel] 'Case 6a' }
        Assert-SameBytes $bSecretPath $bSecret 'Case 6a'
        $attrs = (Get-Item -LiteralPath $bSecretPath -Force).Attributes
        if (-not ($attrs -band [IO.FileAttributes]::Hidden) -or -not ($attrs -band [IO.FileAttributes]::ReadOnly)) {
            throw "Case 6a: user-secret.dat attributes changed: $attrs"
        }
        Wait-Log $c.Log 'v2:Atrium\.exe:start'
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    $c = New-CaseLayout 'case6b-legacy-conflicts'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        [void](Write-Utf8Bytes (Join-Path $c.Install $modSample.Path) $modSample.Official)
        [void](Write-Utf8Bytes (Join-Path $c.Package $modSample.Path) $modSample.Official)
        Write-SharedLegacyCatalog $c.Install (Join-Path $c.Package 'Settings\legacy-manifests.json') '2.7.0'
        $kept = @{}
        $kept[$modSample.Path] = Write-Utf8Bytes (Join-Path $c.Install $modSample.Path) $modSample.Modified
        $wildcardRisks = @('Settings\Assets\my-photo.png', 'Settings\Fonts\personal.otf', 'Settings\my-extension.dll')
        foreach ($rel in $wildcardRisks) {
            $kept[$rel] = Write-Utf8Bytes (Join-Path $c.Install $rel) $userSamples[$rel]
            [void](Write-Utf8Bytes (Join-Path $c.Package $rel) ('official-v2-' + $rel))
        }
        Write-TestManifest $c.Package '2.8.0'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        Invoke-Updater $c
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($pattern in @('zh-Hans\.json', 'my-photo\.png', 'personal\.otf', 'my-extension\.dll')) {
            if (!$res.error -or $res.error -notmatch $pattern) { throw "Case 6b: expected $pattern in error, got: $($res.error)" }
        }
        if (Test-Path -LiteralPath $c.Exit) { throw 'Case 6b: stopped running Atrium despite conflict' }
        foreach ($rel in $kept.Keys) { Assert-SameBytes (Join-Path $c.Install $rel) $kept[$rel] 'Case 6b' }
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    $c = New-CaseLayout 'case6c-legacy-no-catalog'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        $kept = @{}
        foreach ($rel in @('Settings\Assets\my-photo.png', 'Settings\Fonts\personal.otf', 'Settings\my-extension.dll')) {
            $kept[$rel] = Write-Utf8Bytes (Join-Path $c.Install $rel) $userSamples[$rel]
            [void](Write-Utf8Bytes (Join-Path $c.Package $rel) ('pkg-' + $rel))
        }
        Write-TestManifest $c.Package '2.8.0'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        Invoke-Updater $c
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if (!$res.error -or $res.error -notmatch 'Atrium\.exe' -or $res.error -notmatch 'my-photo\.png') {
            throw "Case 6c: expected unowned conflict without catalog, got: $($res.error)"
        }
        if (Test-Path -LiteralPath $c.Exit) { throw 'Case 6c: stopped Atrium without legacy catalog verification' }
        foreach ($rel in $kept.Keys) { Assert-SameBytes (Join-Path $c.Install $rel) $kept[$rel] 'Case 6c' }
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    Write-Host 'Case 6 passed (exact legacy hash ownership & wildcard-path conflict protection verified).'
    # Case 7: Manifest lifecycle (replace/retire, modified official conflict, rollback user safety, concurrent mutex)
    $c = New-CaseLayout 'case7a-lifecycle'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        [void](Write-Utf8Bytes (Join-Path $c.Install 'Settings\shared.dll') 'shared-v1')
        [void](Write-Utf8Bytes (Join-Path $c.Install 'Settings\old-only.dll') 'retire-me')
        [void](Write-Utf8Bytes (Join-Path $c.Install 'retired-dir\old.txt') 'retire-dir')
        [void](Write-Utf8Bytes (Join-Path $c.Package 'Settings\shared.dll') 'shared-v2')
        [void](Write-Utf8Bytes (Join-Path $c.Package 'Settings\new-only.dll') 'added-v2')
        Write-TestManifest $c.Install '2.8.0'; Write-TestManifest $c.Package '2.9.0'
        $bRoot = Write-Utf8Bytes (Join-Path $c.Install 'user-note.txt') 'keep-user-note'
        $bSet = Write-Utf8Bytes (Join-Path $c.Install 'Settings\custom.ini') 'keep-custom'
        $bLoc = Write-Utf8Bytes (Join-Path $c.Install 'locales\user.json') '{"u":1}'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        Invoke-Updater $c
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($res.error -ne '') { throw "Case 7a failed: $($res.error)" }
        if ((Test-Path -LiteralPath (Join-Path $c.Install 'Settings\old-only.dll')) -or
            (Test-Path -LiteralPath (Join-Path $c.Install 'retired-dir'))) { throw 'Case 7a: retired file/dir remained' }
        if (!(Test-Path -LiteralPath (Join-Path $c.Install 'Settings\new-only.dll'))) { throw 'Case 7a: new file missing' }
        Assert-SameBytes (Join-Path $c.Install 'Settings\shared.dll') ([Text.Encoding]::UTF8.GetBytes('shared-v2')) 'Case 7a'
        Assert-SameBytes (Join-Path $c.Install 'user-note.txt') $bRoot 'Case 7a'
        Assert-SameBytes (Join-Path $c.Install 'Settings\custom.ini') $bSet 'Case 7a'
        Assert-SameBytes (Join-Path $c.Install 'locales\user.json') $bLoc 'Case 7a'
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    $c = New-CaseLayout 'case7b-modified-official'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        $modSample = Get-SharedModifiedOfficialSample
        [void](Write-Utf8Bytes (Join-Path $c.Install $modSample.Path) $modSample.Official)
        [void](Write-Utf8Bytes (Join-Path $c.Package $modSample.Path) $modSample.Official)
        Write-TestManifest $c.Install '2.8.0'; Write-TestManifest $c.Package '2.9.0'
        $modBytes = Write-Utf8Bytes (Join-Path $c.Install $modSample.Path) $modSample.Modified
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        Invoke-Updater $c
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if (!$res.error -or $res.error -notmatch 'zh-Hans\.json') { throw "Case 7b: expected conflict error, got: $($res.error)" }
        if (Test-Path -LiteralPath $c.Exit) { throw 'Case 7b: stopped Atrium despite modified file conflict' }
        Assert-SameBytes (Join-Path $c.Install $modSample.Path) $modBytes 'Case 7b'
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    $c = New-CaseLayout 'case7c-rollback-user-safe'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log
            settings_exit_code = '1' }
        Write-TestManifest $c.Install '2.8.0'; Write-TestManifest $c.Package '2.9.0'
        $bKeep = Write-Utf8Bytes (Join-Path $c.Install 'user-keep.txt') 'never-touch-on-rollback'
        $bSet = Write-Utf8Bytes (Join-Path $c.Install 'Settings\custom.ini') 'never-touch-settings'
        $res = Run-RollbackCase $c $tracked
        if (!$res.error) { throw 'Case 7c: expected rollback error' }
        Assert-SameBytes (Join-Path $c.Install 'user-keep.txt') $bKeep 'Case 7c'
        Assert-SameBytes (Join-Path $c.Install 'Settings\custom.ini') $bSet 'Case 7c'
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    $c = New-CaseLayout 'case7d-concurrent-mutex'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        Write-TestManifest $c.Install '2.8.0'; Write-TestManifest $c.Package '2.9.0'
        $cacheB = Join-Path $c.Dir 'cacheB'; $pkgB = Join-Path $cacheB 'package-background'
        New-Item -ItemType Directory -Path $pkgB | Out-Null
        Copy-Item -Path (Join-Path $c.Package '*') -Destination $pkgB -Recurse -Force
        $stateB = Join-Path $cacheB 'ready.json'; '{}' | Set-Content -LiteralPath $stateB -Encoding UTF8
        $resB = Join-Path $cacheB 'result.json'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        $slowProc = Start-Process -FilePath $stubExe -ArgumentList '--sleep 2500' -WindowStyle Hidden -PassThru
        [void]$tracked.Add($slowProc)
        $argsA = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $c.Script, '-InstallDirectory', $c.Install,
            '-PackageDirectory', $c.Package, '-StatePath', $c.State, '-WaitForProcessId', $slowProc.Id, '-StartCore', '1',
            '-StartupTimeoutSeconds', '4')
        $procA = Start-Process -FilePath $ps51 -ArgumentList $argsA -WindowStyle Hidden -PassThru
        [void]$tracked.Add($procA)
        $until = [DateTime]::UtcNow.AddSeconds(5)
        while (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.update-*').Count -eq 0 -and [DateTime]::UtcNow -lt $until) {
            Start-Sleep -Milliseconds 50
        }
        if (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.update-*').Count -eq 0) {
            throw 'Case 7d: updater A did not start in time'
        }
        $dummy = Start-Process -FilePath $stubExe -ArgumentList '--sleep 10' -WindowStyle Hidden -PassThru
        $dummyId = $dummy.Id; $dummy.Dispose()
        $argsB = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $c.Script, '-InstallDirectory', $c.Install,
            '-PackageDirectory', $pkgB, '-StatePath', $stateB, '-WaitForProcessId', $dummyId, '-StartCore', '1',
            '-StartupTimeoutSeconds', '4')
        $procB = Start-Process -FilePath $ps51 -ArgumentList $argsB -WindowStyle Hidden -PassThru
        try {
            if (!$procB.WaitForExit(15000)) { throw 'Case 7d: updater B timed out' }
            $outB = Get-Content -LiteralPath $resB -Raw -Encoding UTF8 | ConvertFrom-Json
            if (!$outB.error -or $outB.error -notmatch '另一项更新') { throw "Case 7d: expected mutex error, got: $($outB.error)" }
            if (!$procA.WaitForExit(45000)) { throw 'Case 7d: updater A timed out' }
            $outA = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($outA.error -ne '') { throw "Case 7d: updater A failed: $($outA.error)" }
        } finally { $procB.Dispose() }
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    Write-Host 'Case 7 passed (manifest replace/retire, modified official conflict, rollback safety, concurrent mutex).'
    # Case 7e: Updater holding install mutex rejects concurrent uninstaller with zero deletion, then finishes upgrade
    $c = New-CaseLayout 'case7e-updater-blocks-uninstall'; $tracked = New-Object System.Collections.ArrayList
    $releaseName = 'Local\Atrium.UpdateTest.Release.' + [Guid]::NewGuid().ToString('N')
    $releaseEv = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $releaseName)
    try {
        Populate-App $c.Install @{ version = 'v1'; v2_manifest = '2.9.0'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        $installedUninstall = Join-Path $c.Install 'Uninstall.ps1'
        Copy-Item -LiteralPath (Join-Path $project 'tools\Uninstall.ps1') -Destination $installedUninstall -Force
        Copy-Item -LiteralPath (Join-Path $project 'tools\Uninstall.ps1') -Destination (Join-Path $c.Package 'Uninstall.ps1') -Force
        $bOff = Write-Utf8Bytes (Join-Path $c.Install 'official.dat') 'v1-official'
        [void](Write-Utf8Bytes (Join-Path $c.Package 'official.dat') 'v2-official')
        Write-TestManifest $c.Install '2.8.0'; Write-TestManifest $c.Package '2.9.0'
        $bUser = Write-Utf8Bytes (Join-Path $c.Install 'user-note.txt') 'keep-user-note'
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:start'
        $slowProc = Start-Process -FilePath $stubExe -ArgumentList "--wait-event $releaseName" -WindowStyle Hidden -PassThru
        [void]$tracked.Add($slowProc)
        $argsA = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $c.Script, '-InstallDirectory', $c.Install,
            '-PackageDirectory', $c.Package, '-StatePath', $c.State, '-WaitForProcessId', $slowProc.Id, '-StartCore', '1',
            '-StartupTimeoutSeconds', '4')
        $procA = Start-Process -FilePath $ps51 -ArgumentList $argsA -WindowStyle Hidden -PassThru
        [void]$tracked.Add($procA)
        $until = [DateTime]::UtcNow.AddSeconds(10)
        while (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.update-*').Count -eq 0 -and [DateTime]::UtcNow -lt $until) {
            Start-Sleep -Milliseconds 50
        }
        if (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.update-*').Count -eq 0) {
            throw 'Case 7e: updater did not reach temp staging while holding mutex'
        }
        $uCmd = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; & '$($installedUninstall.Replace("'", "''"))' -Confirmed -Quiet"
        $uPsi = New-Object System.Diagnostics.ProcessStartInfo($ps51, "-NoProfile -ExecutionPolicy Bypass -Command `"$uCmd`"")
        $uPsi.UseShellExecute = $false; $uPsi.RedirectStandardError = $true; $uPsi.StandardErrorEncoding = [Text.Encoding]::UTF8
        $uPsi.CreateNoWindow = $true
        $uProc = [System.Diagnostics.Process]::Start($uPsi)
        try {
            $uErr = $uProc.StandardError.ReadToEnd()
            if (!$uProc.WaitForExit(15000)) {
                Stop-Process -Id $uProc.Id -Force -ErrorAction SilentlyContinue; [void]$uProc.WaitForExit(2000)
                throw 'Case 7e: uninstaller timed out while updater held mutex'
            }
            if ($uProc.ExitCode -eq 0) { throw 'Case 7e: expected uninstaller to fail while updater held mutex' }
            if ($uErr -notmatch '正在执行更新或卸载') { throw "Case 7e: expected mutex conflict error on stderr, got: $uErr" }
        } finally { $uProc.Dispose() }
        foreach ($kept in @('Atrium.exe', 'Settings\Atrium.Settings.exe', 'Uninstall.ps1', 'package-manifest.json')) {
            if (!(Test-Path -LiteralPath (Join-Path $c.Install $kept) -PathType Leaf)) {
                throw "Case 7e: official file deleted despite mutex rejection: $kept"
            }
        }
        Assert-SameBytes (Join-Path $c.Install 'official.dat') $bOff 'Case 7e (official intact)'
        Assert-SameBytes (Join-Path $c.Install 'user-note.txt') $bUser 'Case 7e (user intact)'
        [void]$releaseEv.Set()
        if (!$procA.WaitForExit(45000)) { throw 'Case 7e: updater timed out after releasing WaitForProcessId' }
        $outA = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($procA.ExitCode -ne 0 -or $outA.error -ne '') { throw "Case 7e: updater failed after uninstaller rejection: $($outA.error)" }
        $newM = Get-Content -LiteralPath (Join-Path $c.Install 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($newM.version -ne '2.9.0') { throw "Case 7e: expected manifest version 2.9.0, got $($newM.version)" }
        Assert-SameBytes (Join-Path $c.Install 'official.dat') ([Text.Encoding]::UTF8.GetBytes('v2-official')) 'Case 7e (v2)'
        Assert-SameBytes (Join-Path $c.Install 'user-note.txt') $bUser 'Case 7e (user post-upgrade)'
        Wait-Log $c.Log 'v2:Atrium\.exe:start'; Wait-Log $c.Log 'v2:Atrium\.Settings\.exe:start'
    } finally {
        $releaseEv.Dispose()
        Stop-CaseProcesses $c.Dir $tracked.ToArray()
    }
    Write-Host 'Case 7e passed (updater holding mutex rejected uninstaller cleanly and finished v2 upgrade).'
    Write-Host 'Updater regressions passed (all cases).'
} finally {
    Stop-CaseProcesses $fixtureRoot
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue }
}
exit 0
