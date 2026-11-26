# Launched only after the user chooses to restart. Runs outside the install folder.
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$StatePath,
    [Parameter(Mandatory)][int]$WaitForProcessId,
    [ValidateSet('0', '1')][string]$StartCore = '1',
    [ValidateRange(1, 60)][int]$StartupTimeoutSeconds = 20,
    [string]$LegacyManifestPath = ''
)
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$StatePath = [IO.Path]::GetFullPath($StatePath)
$cache = Split-Path -Parent $StatePath
$parent = Split-Path -Parent $install
$name = Split-Path -Leaf $install
$suffix = [Guid]::NewGuid().ToString('N')
$incoming = Join-Path $parent "$name.update-$suffix"
$backup = Join-Path $parent "$name.previous-$suffix"
$result = Join-Path $cache 'result.json'
$coreStopped = $false
$backupStarted = $false
$incomingStarted = $false
$confirmed = $false
$restored = $false
$installMutex = $null
$installMutexAcquired = $false
$started = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
# Never the install folder: what starts there would hold it, and the next update could not replace it.
$workingDirectory = [Environment]::GetFolderPath('UserProfile')

function Assert-TemporaryPath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ((Split-Path -Parent $absolute) -ne $parent -or
        ((Split-Path -Leaf $absolute) -ne "$name.update-$suffix" -and
         (Split-Path -Leaf $absolute) -ne "$name.previous-$suffix")) { throw '更新临时目录无效。' }
}

function Get-StringSha256Prefix([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').Substring(0, 16)
    } finally { $sha.Dispose() }
}

function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
        try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}

function Normalize-ManifestRelPath([string]$Rel, [string]$RootDir) {
    if ([string]::IsNullOrWhiteSpace($Rel) -or [IO.Path]::IsPathRooted($Rel) -or
        $Rel.StartsWith('\') -or $Rel.StartsWith('/') -or $Rel.EndsWith('\') -or $Rel.EndsWith('/')) {
        throw "安装包清单路径无效：$Rel"
    }
    $norm = $Rel.Replace('/', '\')
    if ($norm.Split('\') | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }) {
        throw "安装包清单路径越界：$Rel"
    }
    $fullRoot = $RootDir.TrimEnd('\') + '\'
    $full = [IO.Path]::GetFullPath((Join-Path $RootDir $norm))
    if (!$full.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "安装包清单路径越界：$Rel"
    }
    return $norm
}

function Add-ExpectedHash($Map, [string]$Rel, [string]$Sha) {
    if ($Sha -cnotmatch '^[0-9a-f]{64}$') { throw "安装包清单摘要格式无效：$Rel" }
    if (!$Map.ContainsKey($Rel)) {
        $Map[$Rel] = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    }
    [void]$Map[$Rel].Add($Sha)
}

function Read-PackageManifest([string]$RootDir, [string]$Label, [switch]$VerifyHashes) {
    $manifestPath = Join-Path $RootDir 'package-manifest.json'
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "${Label}缺少 package-manifest.json。"
    }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "${Label}的 package-manifest.json 已损坏：$($_.Exception.Message)"
    }
    if ($null -eq $manifest -or (($manifest.schema -isnot [int]) -and ($manifest.schema -isnot [long])) -or
        $manifest.schema -ne 1 -or [string]::IsNullOrWhiteSpace([string]$manifest.version)) {
        throw "${Label}的 package-manifest.json 格式或 schema 不受支持。"
    }
    $entries = @($manifest.files)
    if ($entries.Count -eq 0) { throw "${Label}的 package-manifest.json 文件列表为空。" }
    $map = New-Object 'System.Collections.Generic.Dictionary[string, System.Collections.Generic.HashSet[string]]' (
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        $norm = Normalize-ManifestRelPath ([string]$entry.path) $RootDir
        if ($map.ContainsKey($norm)) { throw "${Label}的 package-manifest.json 包含重复路径：$norm" }
        $expected = [string]$entry.sha256
        Add-ExpectedHash $map $norm $expected
        if ($VerifyHashes) {
            $full = Join-Path $RootDir $norm
            if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "${Label}缺少清单文件：$norm" }
            $actual = Get-FileSha256 $full
            if ($actual -cne $expected) { throw "${Label}文件校验失败：$norm" }
        }
    }
    return $map
}

function Read-LegacyHashCatalog([string]$CatalogPath, [string]$RootDir) {
    try {
        $catalog = Get-Content -LiteralPath $CatalogPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "已知旧版清单已损坏：$($_.Exception.Message)"
    }
    if ($null -eq $catalog -or (($catalog.schema -isnot [int]) -and ($catalog.schema -isnot [long])) -or
        $catalog.schema -ne 1 -or $null -eq $catalog.releases) {
        throw '已知旧版清单格式或 schema 不受支持。'
    }
    $map = New-Object 'System.Collections.Generic.Dictionary[string, System.Collections.Generic.HashSet[string]]' (
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($release in @($catalog.releases)) {
        if ($null -eq $release -or $null -eq $release.files) { continue }
        foreach ($entry in @($release.files)) {
            $norm = Normalize-ManifestRelPath ([string]$entry.path) $RootDir
            Add-ExpectedHash $map $norm ([string]$entry.sha256)
        }
    }
    return $map
}

function Resolve-LegacyHashCatalog([string]$ExplicitPath, [string[]]$DefaultCandidates, [string]$RootDir) {
    if (![string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (!(Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "找不到指定的旧版清单：$ExplicitPath"
        }
        return Read-LegacyHashCatalog $ExplicitPath $RootDir
    }
    foreach ($candidate in $DefaultCandidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return Read-LegacyHashCatalog $candidate $RootDir
        }
    }
    return New-Object 'System.Collections.Generic.Dictionary[string, System.Collections.Generic.HashSet[string]]' (
        [StringComparer]::OrdinalIgnoreCase)
}

function Get-PackageFileOwnership([string]$Rel, [string]$FullPath, $ExpectedHashes) {
    if ($null -eq $ExpectedHashes -or !$ExpectedHashes.ContainsKey($Rel)) { return 'Unowned' }
    $actualSha = Get-FileSha256 $FullPath
    if ($ExpectedHashes[$Rel].Contains($actualSha)) { return 'Owned' }
    return 'Modified'
}

function Remove-EmptyDirectoryIfEmpty([string]$DirPath) {
    if (!(Test-Path -LiteralPath $DirPath -PathType Container)) { return }
    if (Get-ChildItem -LiteralPath $DirPath -Force | Select-Object -First 1) { return }
    try {
        [IO.Directory]::Delete($DirPath, $false)
    } catch [UnauthorizedAccessException] {
        try {
            [IO.File]::SetAttributes($DirPath, ([IO.File]::GetAttributes($DirPath) -band (-bnot [IO.FileAttributes]::ReadOnly)))
            [IO.Directory]::Delete($DirPath, $false)
        } catch { }
    } catch { }
}

function Move-SingleFile(
    [string]$Source,
    [string]$Destination,
    [switch]$BestEffort,
    [System.Collections.Generic.List[string]]$Errors = $null
) {
    $destDir = Split-Path -Parent $Destination
    if ($destDir -and !(Test-Path -LiteralPath $destDir -PathType Container)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    # A file just written may be open for a moment (a virus scanner reading it).
    $maxTries = if ($BestEffort) { 3 } else { 6 }
    for ($try = 1; ; $try++) {
        try {
            Move-Item -LiteralPath $Source -Destination $Destination -Force
            break
        } catch [UnauthorizedAccessException] {
            try {
                if (Test-Path -LiteralPath $Destination -PathType Leaf) {
                    $attrs = [IO.File]::GetAttributes($Destination)
                    if ($attrs -band [IO.FileAttributes]::ReadOnly) {
                        [IO.File]::SetAttributes($Destination, ($attrs -band (-bnot [IO.FileAttributes]::ReadOnly)))
                    }
                }
                Move-Item -LiteralPath $Source -Destination $Destination -Force
                break
            } catch {
                if ($try -lt $maxTries) { Start-Sleep -Milliseconds 300; continue }
                $message = "无法替换 $Source（$($_.Exception.Message.TrimEnd('。', '.'))），请关闭正在使用它的程序后重试。"
                if ($BestEffort) { if ($Errors) { $Errors.Add($message) }; break }
                throw $message
            }
        } catch {
            if ($try -lt $maxTries) { Start-Sleep -Milliseconds 300; continue }
            $message = "无法替换 $Source（$($_.Exception.Message.TrimEnd('。', '.'))），请关闭正在使用它的程序后重试。"
            if ($BestEffort) { if ($Errors) { $Errors.Add($message) }; break }
            throw $message
        }
    }
}

# What settings wrote to its crash log since $Since (its exception's first line), for the message.
function Get-CrashCause([datetime]$Since) {
    $log = Join-Path $env:LOCALAPPDATA 'Atrium\logs\settings.log'
    try {
        if (!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).LastWriteTime -lt $Since) { return '' }
        $last = Get-Content -LiteralPath $log -Tail 60 -Encoding UTF8 |
            Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ' } | Select-Object -Last 1
        if (!$last) { return '' }
        $cause = $last.Substring(20).Trim()
        if ($cause.Length -gt 200) { $cause = $cause.Substring(0, 200) + '...' }
        return "：$cause"
    } catch { return '' }
}

# Smart App Control or an App Control (WDAC) policy refused a file: ERROR_SYSTEM_INTEGRITY_* 4550-4559 and
# 4580-4582. Windows PowerShell's Start-Process keeps only the system message, not the code, so compare that.
function Test-AppControlBlock([string]$Message) {
    foreach ($code in @(4550..4559) + @(4580..4582)) {
        $text = ([ComponentModel.Win32Exception]::new($code)).Message.Trim().TrimEnd('。', '.')
        if ($text -and $Message.Contains($text)) { return $true }
    }
    return $false
}

function Start-Verified([string]$Executable, [string]$Kind, [string]$Arguments) {
    $eventName = "Local\Atrium.Update.$Kind.$suffix"
    $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
    try {
        $startedAt = (Get-Date).AddSeconds(-1)
        try {
            $process = Start-Process -FilePath $Executable -ArgumentList "$Arguments --ready-event $eventName" -WorkingDirectory $workingDirectory `
                -WindowStyle Hidden -PassThru
        } catch {
            if (!(Test-AppControlBlock $_.Exception.Message)) { throw }
            # The release is not code-signed; each version's new files have no reputation yet.
            throw ("Windows 应用程序控制阻止了新版 $Kind 运行，已保留旧版本。新版文件没有代码签名，可能被智能应用控制或单位的应用控制策略拦截：" +
                   "可在 Windows 安全中心 › 应用和浏览器控制 › 智能应用控制中查看；单位电脑请联系管理员放行 Atrium 所在目录。（$($_.Exception.Message.Trim())）")
        }
        $started.Add($process)
        $until = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
        do {
            if ($process.HasExited) {
                $cause = if ($Kind -eq 'Settings') { Get-CrashCause $startedAt } else { '' }
                throw ("$Kind 启动失败（退出码 0x{0:X8}）{1}。" -f $process.ExitCode, $cause)
            }
            if ($ready.WaitOne(100)) {
                if ($process.HasExited) { throw "$Kind 在就绪后退出。" }
                return
            }
        } while ([DateTime]::UtcNow -lt $until)
        throw "$Kind 未确认启动就绪，已保留旧版本。"
    } finally { $ready.Dispose() }
}

function Get-InstallProcesses {
    $prefix = $install + '\'
    $byId = @{}
    foreach ($process in @(Get-Process -ErrorAction SilentlyContinue)) {
        try {
            if ($process.Id -ne $PID -and $process.Path -and
                $process.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $byId[$process.Id] = [pscustomobject]@{ Process = $process; Path = $process.Path }
            }
        } catch { }
    }
    try {
        foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)) {
            $procId = [int]$cim.ProcessId
            if ($procId -ne $PID -and $cim.ExecutablePath -and
                $cim.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and !$byId.ContainsKey($procId)) {
                $process = Get-Process -Id $procId -ErrorAction SilentlyContinue
                if ($process) { $byId[$procId] = [pscustomobject]@{ Process = $process; Path = $cim.ExecutablePath } }
            }
        }
    } catch { }
    return @($byId.Values)
}

function Stop-InstallProcesses([int]$WaitMilliseconds, [System.Diagnostics.Process[]]$Tracked = @()) {
    for ($pass = 1; $pass -le 2; $pass++) {
        $byId = @{}
        foreach ($process in @($Tracked) + @(Get-InstallProcesses | ForEach-Object { $_.Process })) {
            try {
                if ($process -and $process.Id -ne $PID -and !$process.HasExited -and !$byId.ContainsKey($process.Id)) { $byId[$process.Id] = $process }
            } catch { }
        }
        $processes = @($byId.Values)
        if ($processes.Count -eq 0) { return }
        if ($pass -eq 1 -and $WaitMilliseconds -gt 0) {
            $until = [DateTime]::UtcNow.AddMilliseconds($WaitMilliseconds)
            while ([DateTime]::UtcNow -lt $until) {
                $alive = $false; foreach ($process in $processes) { try { if (!$process.HasExited) { $alive = $true; break } } catch { } }
                if (!$alive) { break }; Start-Sleep -Milliseconds 100
            }
        }
        foreach ($process in $processes) {
            try { if (!$process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; [void]$process.WaitForExit(3000) } } catch { }
        }
    }
}

function Write-Result([string]$ErrorText) {
    $temporary = "$result.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    @{ error = $ErrorText } | ConvertTo-Json | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $result -Force
}

try {
    if (!$parent -or !$name -or $install -eq [IO.Path]::GetPathRoot($install).TrimEnd('\') -or
        !$package.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '更新目录无效。' }
    Assert-TemporaryPath $incoming
    Assert-TemporaryPath $backup
    $installHash = Get-StringSha256Prefix ($install.ToUpperInvariant())
    $installMutex = New-Object System.Threading.Mutex($false, "Local\Atrium.Update.Install.$installHash")
    try {
        $installMutexAcquired = $installMutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        $installMutexAcquired = $true
    }
    if (!$installMutexAcquired) { throw '当前安装目录正在执行另一项更新或卸载，请等待其完成后重试。' }
    foreach ($root in @($install, $package)) {
        if (!(Test-Path -LiteralPath (Join-Path $root 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $root 'Settings\Atrium.Settings.exe'))) { throw '找不到完整的 Atrium 程序文件。' }
    }
    $newFiles = Read-PackageManifest $package '新安装包' -VerifyHashes
    $oldManifestPath = Join-Path $install 'package-manifest.json'
    $hasOldManifest = Test-Path -LiteralPath $oldManifestPath -PathType Leaf
    if ($hasOldManifest) {
        $ownedHashes = Read-PackageManifest $install '当前安装目录'
    } else {
        $catalogCandidates = @((Join-Path $package 'Settings\legacy-manifests.json'), (Join-Path $PSScriptRoot 'legacy-manifests.json'))
        $ownedHashes = Resolve-LegacyHashCatalog $LegacyManifestPath $catalogCandidates $install
    }
    $toBackup = New-Object 'System.Collections.Generic.List[string]'
    $toBackupSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $retired = New-Object 'System.Collections.Generic.List[string]'
    $conflicts = New-Object 'System.Collections.Generic.List[string]'
    $existingFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @(Get-ChildItem -LiteralPath $install -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($install.Length + 1).Replace('/', '\')
        [void]$existingFiles.Add($rel)
        if ($rel -ieq 'development.txt') { continue }
        if ($hasOldManifest -and $rel -ieq 'package-manifest.json') {
            $toBackup.Add($rel); [void]$toBackupSet.Add($rel)
            continue
        }
        $ownership = Get-PackageFileOwnership $rel $file.FullName $ownedHashes
        if ($ownership -eq 'Owned') {
            $toBackup.Add($rel); [void]$toBackupSet.Add($rel)
            if (!$newFiles.ContainsKey($rel)) { $retired.Add($rel) }
        } elseif ($ownership -eq 'Modified') {
            $conflicts.Add("$rel (已修改的官方文件)")
        } elseif ($newFiles.ContainsKey($rel)) {
            $conflicts.Add("$rel (未声明归属的同名文件)")
        }
    }
    foreach ($rel in $newFiles.Keys) {
        $parts = $rel.Split('\')
        for ($i = 0; $i -lt $parts.Length - 1; $i++) {
            $anc = ($parts[0..$i]) -join '\'
            if ($existingFiles.Contains($anc) -and !$toBackupSet.Contains($anc)) {
                $conflicts.Add("$rel (与已有文件 $anc 冲突)")
                break
            }
        }
        $targetPath = Join-Path $install $rel
        if (Test-Path -LiteralPath $targetPath -PathType Container) {
            $keptInside = @(Get-ChildItem -LiteralPath $targetPath -Recurse -File -Force | Where-Object {
                $subRel = $_.FullName.Substring($install.Length + 1).Replace('/', '\')
                $subRel -ieq 'development.txt' -or !$toBackupSet.Contains($subRel)
            })
            if ($keptInside.Count -gt 0) { $conflicts.Add("$rel (与已有非空目录冲突)") }
        }
    }
    if ($conflicts.Count -gt 0) {
        throw ("检测到文件冲突，已取消更新并保留原文件：" + (($conflicts | Select-Object -Unique) -join '、'))
    }
    # Copy only verified manifest files before stopping the app; disk/access failures leave the old app running.
    New-Item -ItemType Directory -Path $incoming | Out-Null
    $incomingFiles = New-Object 'System.Collections.Generic.List[string]'
    foreach ($rel in @($newFiles.Keys) + @('package-manifest.json')) {
        $src = Join-Path $package $rel
        $dst = Join-Path $incoming $rel
        $dstDir = Split-Path -Parent $dst
        if ($dstDir -and !(Test-Path -LiteralPath $dstDir -PathType Container)) {
            New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $incomingFiles.Add($rel)
    }
    $settingsProcess = Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue
    if ($settingsProcess -and !$settingsProcess.WaitForExit(30000)) { throw '设置中心尚未退出，更新未安装。' }
    $coreExe = Join-Path $install 'Atrium.exe'
    $coreRunning = @(Get-InstallProcesses | Where-Object { $_.Path -and [string]::Equals($_.Path, $coreExe, [StringComparison]::OrdinalIgnoreCase) })
    if ($StartCore -eq '1' -or $coreRunning.Count -gt 0) {
        $quit = Start-Process -FilePath $coreExe -ArgumentList '--exit' -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru
        try {
            if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Atrium 尚未正常退出，更新未安装。' }
        } finally { $quit.Dispose() }
        $coreStopped = $true
    }
    Stop-InstallProcesses -WaitMilliseconds 5000
    Assert-TemporaryPath $backup
    Assert-TemporaryPath $incoming
    $backupStarted = $true
    New-Item -ItemType Directory -Path $backup | Out-Null
    $backedUp = New-Object 'System.Collections.Generic.List[string]'
    foreach ($rel in $toBackup) {
        Move-SingleFile (Join-Path $install $rel) (Join-Path $backup $rel)
        $backedUp.Add($rel)
    }
    $incomingStarted = $true
    $placed = New-Object 'System.Collections.Generic.List[string]'
    $createdDirs = New-Object 'System.Collections.Generic.List[string]'
    foreach ($rel in $incomingFiles) {
        $target = Join-Path $install $rel
        if (Test-Path -LiteralPath $target -PathType Container) {
            foreach ($sub in @(Get-ChildItem -LiteralPath $target -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending)) {
                Remove-EmptyDirectoryIfEmpty $sub.FullName
            }
            Remove-EmptyDirectoryIfEmpty $target
        }
        $parentRel = Split-Path -Parent $rel
        if ($parentRel) {
            $curRel = ''
            foreach ($seg in $parentRel.Split('\')) {
                $curRel = if ($curRel) { "$curRel\$seg" } else { $seg }
                $curFull = Join-Path $install $curRel
                if (!(Test-Path -LiteralPath $curFull -PathType Container)) {
                    New-Item -ItemType Directory -Path $curFull -Force | Out-Null
                    $createdDirs.Add($curRel)
                }
            }
        }
        Move-SingleFile (Join-Path $incoming $rel) $target
        $placed.Add($rel)
    }
    if ($StartCore -eq '1') { Start-Verified (Join-Path $install 'Atrium.exe') 'Core' '' }
    $settingsArguments = if ($StartCore -eq '1') { '--page about --require-core-ready' } else { '--page about' }
    Start-Verified (Join-Path $install 'Settings\Atrium.Settings.exe') 'Settings' $settingsArguments
    foreach ($process in $started) {
        if ($process.HasExited) { throw '新版程序在启动确认期间退出。' }
    }
    # With the core intentionally off, only settings is launched and verified.
    $confirmed = $true
    foreach ($rel in $retired) {
        $dirRel = Split-Path -Parent $rel
        while ($dirRel) {
            Remove-EmptyDirectoryIfEmpty (Join-Path $install $dirRel)
            $dirRel = Split-Path -Parent $dirRel
        }
    }
    Remove-Item -LiteralPath $StatePath -Force
    Write-Result ''
} catch {
    $failure = $_.Exception.Message
    if ($backupStarted -and !$confirmed) {
        $rollbackErrors = New-Object 'System.Collections.Generic.List[string]'
        if ($incomingStarted) {
            try {
                $newCore = Join-Path $install 'Atrium.exe'
                if ($StartCore -eq '1' -and (Test-Path -LiteralPath $newCore)) {
                    $quit = Start-Process -FilePath $newCore -ArgumentList '--exit' -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru
                    try { [void]$quit.WaitForExit(8000) } finally { $quit.Dispose() }
                }
            } catch { }
            try { Stop-InstallProcesses -WaitMilliseconds 3000 -Tracked @($started) } catch { }
            try {
                Assert-TemporaryPath $incoming
                foreach ($rel in $placed) {
                    $src = Join-Path $install $rel
                    if ([IO.File]::Exists($src)) {
                        Move-SingleFile $src (Join-Path $incoming $rel) -BestEffort -Errors $rollbackErrors
                    }
                }
                for ($i = $createdDirs.Count - 1; $i -ge 0; $i--) {
                    Remove-EmptyDirectoryIfEmpty (Join-Path $install $createdDirs[$i])
                }
            } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        try {
            Assert-TemporaryPath $backup
            foreach ($rel in $backedUp) {
                $src = Join-Path $backup $rel
                if ([IO.File]::Exists($src)) {
                    Move-SingleFile $src (Join-Path $install $rel) -BestEffort -Errors $rollbackErrors
                }
            }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
        $restored = $true
        $remainingBackupFiles = if (Test-Path -LiteralPath $backup) {
            @(Get-ChildItem -LiteralPath $backup -Recurse -File -Force -ErrorAction SilentlyContinue)
        } else { @() }
        if ($rollbackErrors.Count -gt 0 -or $remainingBackupFiles.Count -gt 0) {
            $detail = (($rollbackErrors | Select-Object -Unique) -join ' ')
            if ($detail) { $failure += " 恢复旧包未完成：$detail" }
            $failure += "；恢复副本保留在 $backup。"
        }
    }
    Write-Result $failure
} finally {
    # Never delete an unconfirmed recovery point, even when rollback itself failed.
    $cleanup = @($incoming)
    $backupEmpty = !(Test-Path -LiteralPath $backup) -or
        (@(Get-ChildItem -LiteralPath $backup -Recurse -File -Force -ErrorAction SilentlyContinue).Count -eq 0)
    if ($confirmed -or $backupEmpty) { $cleanup += $backup }
    foreach ($temporary in $cleanup) {
        Assert-TemporaryPath $temporary
        if (Test-Path -LiteralPath $temporary) {
            try { Remove-Item -LiteralPath $temporary -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    if ($installMutexAcquired -and !$confirmed -and (!$backupStarted -or $restored)) {
        $restoredCore = Join-Path $install 'Atrium.exe'; $backupCore = Join-Path $backup 'Atrium.exe'
        if ($coreStopped -and $StartCore -eq '1' -and (Test-Path -LiteralPath $restoredCore) -and !(Test-Path -LiteralPath $backupCore)) {
            try { Start-Process -FilePath $restoredCore -WorkingDirectory $workingDirectory -WindowStyle Hidden } catch { Write-Warning $_.Exception.Message }
        }
        $restoredSettings = Join-Path $install 'Settings\Atrium.Settings.exe'
        $backupSettings = Join-Path $backup 'Settings\Atrium.Settings.exe'
        $backupSettingsDll = Join-Path $backup 'Settings\Atrium.Settings.dll'
        if ((Test-Path -LiteralPath $restoredSettings) -and !(Test-Path -LiteralPath $backupSettings) -and !(Test-Path -LiteralPath $backupSettingsDll)) {
            try { Start-Process -FilePath $restoredSettings -ArgumentList '--page about' -WorkingDirectory $workingDirectory -WindowStyle Hidden }
            catch { Write-Warning $_.Exception.Message }
        }
    }
    if ($confirmed) {
        $work = $package
        while ($work.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Parent $work) -ne $cache) {
            $work = Split-Path -Parent $work
        }
        $absoluteWork = [IO.Path]::GetFullPath($work)
        if ((Split-Path -Parent $absoluteWork) -eq $cache -and (Split-Path -Leaf $absoluteWork) -match '^package-(background|[0-9a-f]{32})$') {
            try { Remove-Item -LiteralPath $absoluteWork -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    foreach ($process in $started) { $process.Dispose() }
    if ($installMutexAcquired) { try { $installMutex.ReleaseMutex() } catch { } }
    if ($installMutex) { $installMutex.Dispose() }
}
