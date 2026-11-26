# Removes this distribution only. Personal settings and widget layouts are retained.
param(
    [switch]$Confirmed,
    [switch]$Quiet,
    [string]$LegacyManifestPath = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AtriumUninstallNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class AtriumUninstallNative
{
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint msg, UIntPtr wparam, IntPtr lparam);

    public static void PostCloseForUninstall(int pid)
    {
        var handles = new List<IntPtr>();
        EnumWindows((hwnd, _) =>
        {
            uint owner = 0;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner == (uint)pid) handles.Add(hwnd);
            return true;
        }, IntPtr.Zero);
        foreach (IntPtr hwnd in handles)
            PostMessage(hwnd, 0x8000 + 32, UIntPtr.Zero, IntPtr.Zero);
    }
}
'@
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

function Read-PackageManifest([string]$RootDir, [string]$Label) {
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
        Add-ExpectedHash $map $norm ([string]$entry.sha256)
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

function Remove-PackageFile([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    try {
        Remove-Item -LiteralPath $Path -Force
    } catch [UnauthorizedAccessException] {
        $attrs = [IO.File]::GetAttributes($Path)
        if ($attrs -band [IO.FileAttributes]::ReadOnly) {
            [IO.File]::SetAttributes($Path, ($attrs -band (-bnot [IO.FileAttributes]::ReadOnly)))
        }
        Remove-Item -LiteralPath $Path -Force
    }
}

$installMutex = $null
$installMutexAcquired = $false
$failed = $false
try {
    $installRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $coreExe = Join-Path $installRoot 'Atrium.exe'
    $settingsExe = Join-Path $installRoot 'Settings\Atrium.Settings.exe'
    if (!$installRoot -or $installRoot -eq [IO.Path]::GetPathRoot($installRoot).TrimEnd('\')) {
        throw '请从 Atrium 安装目录运行卸载程序。'
    }

    if (!$Confirmed) {
        if (!(Test-Path -LiteralPath $coreExe -PathType Leaf) -or
            !(Test-Path -LiteralPath $settingsExe -PathType Leaf)) {
            throw '请从 Atrium 安装目录运行卸载程序。'
        }
        $answer = [Windows.Forms.MessageBox]::Show(
            '卸载 Atrium？个人设置和桌面小组件布局将会保留。',
            'Atrium', 'YesNo', 'Question', 'Button2')
        if ($answer -ne [Windows.Forms.DialogResult]::Yes) { exit 0 }
    }

    $installHash = Get-StringSha256Prefix ($installRoot.ToUpperInvariant())
    $installMutex = New-Object System.Threading.Mutex($false, "Local\Atrium.Update.Install.$installHash")
    try {
        $installMutexAcquired = $installMutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        $installMutexAcquired = $true
    }
    if (!$installMutexAcquired) { throw '当前安装目录正在执行更新或卸载，请等待其完成后重试。' }

    if (!(Test-Path -LiteralPath $coreExe -PathType Leaf) -or
        !(Test-Path -LiteralPath $settingsExe -PathType Leaf)) {
        throw '请从 Atrium 安装目录运行卸载程序。'
    }

    # Resolve and verify ownership by path + SHA-256 before stopping running apps or deleting files.
    $filesToDelete = New-Object 'System.Collections.Generic.List[string]'
    $seenFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $manifestPath = Join-Path $installRoot 'package-manifest.json'
    $hasManifest = Test-Path -LiteralPath $manifestPath -PathType Leaf
    if ($hasManifest) {
        $ownedHashes = Read-PackageManifest $installRoot '当前安装目录'
    } else {
        $catalogCandidates = @((Join-Path $installRoot 'Settings\legacy-manifests.json'), (Join-Path $PSScriptRoot 'legacy-manifests.json'))
        $ownedHashes = Resolve-LegacyHashCatalog $LegacyManifestPath $catalogCandidates $installRoot
    }
    $verifiedOwnedCount = 0
    foreach ($file in @(Get-ChildItem -LiteralPath $installRoot -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($installRoot.Length + 1).Replace('/', '\')
        if ($rel -ieq 'package-manifest.json' -or $rel -ieq 'development.txt') { continue }
        $ownership = Get-PackageFileOwnership $rel $file.FullName $ownedHashes
        if ($ownership -eq 'Owned') {
            $verifiedOwnedCount++
            if ($seenFiles.Add($file.FullName)) { $filesToDelete.Add($file.FullName) }
        }
    }
    if ($verifiedOwnedCount -eq 0) {
        $reason = if ($hasManifest) {
            'package-manifest.json 中没有任何未修改的官方文件与当前目录匹配，已取消卸载并保留全部文件。'
        } else {
            '缺少 package-manifest.json 且未匹配到已知旧版安装包文件，已取消卸载并保留全部文件。'
        }
        throw $reason
    }
    if ($hasManifest) {
        foreach ($extra in @('package-manifest.json', 'development.txt')) {
            $norm = Normalize-ManifestRelPath $extra $installRoot
            $full = Join-Path $installRoot $norm
            if ((Test-Path -LiteralPath $full -PathType Leaf) -and $seenFiles.Add($full)) {
                $filesToDelete.Add($full)
            }
        }
    }

    $settings = @(Get-Process -Name 'Atrium.Settings' -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and [string]::Equals($_.Path, $settingsExe, [StringComparison]::OrdinalIgnoreCase) } catch { $false }
    })
    foreach ($process in $settings) {
        try {
            if ($Confirmed) {
                [AtriumUninstallNative]::PostCloseForUninstall($process.Id)
                if (!$process.WaitForExit(1000)) {
                    [void]$process.CloseMainWindow()
                    if (!$process.WaitForExit(9000)) { throw '请关闭 Atrium 设置中心后重试。' }
                }
            } else {
                [void]$process.CloseMainWindow()
                if (!$process.WaitForExit(10000)) { throw '请关闭 Atrium 设置中心后重试。' }
            }
        } finally { $process.Dispose() }
    }
    $quit = Start-Process -FilePath $coreExe -ArgumentList '--exit' -WindowStyle Hidden -PassThru
    try {
        if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Atrium 未能退出，请关闭后重试。' }
    } finally { $quit.Dispose() }

    $shell = New-Object -ComObject WScript.Shell
    $programs = [Environment]::GetFolderPath('Programs')
    foreach ($name in @('Atrium.lnk', 'Atrium.Core.lnk')) {
        $shortcut = Join-Path $programs $name
        if (Test-Path -LiteralPath $shortcut) {
            $target = $shell.CreateShortcut($shortcut).TargetPath
            if ($target -eq $coreExe -or $target -eq (Join-Path $installRoot 'Atrium.Core.exe')) {
                Remove-Item -LiteralPath $shortcut -Force
            }
        }
    }
    $taskName = 'Atrium Startup-' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $task = Get-ScheduledTask -TaskPath '\' -TaskName $taskName -ErrorAction SilentlyContinue
    if ($task -and $task.Actions[0].Execute.Trim('"') -eq $coreExe) {
        Unregister-ScheduledTask -TaskPath '\' -TaskName $taskName -Confirm:$false
    }
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $startup = (Get-ItemProperty -LiteralPath $runKey -Name Atrium -ErrorAction SilentlyContinue).Atrium
    if ($startup -eq ('"' + $coreExe + '"') -or $startup -eq ('"' + (Join-Path $installRoot 'Atrium.Core.exe') + '"')) {
        Remove-ItemProperty -LiteralPath $runKey -Name Atrium
    }

    foreach ($file in $filesToDelete) {
        Remove-PackageFile $file
    }
    foreach ($dir in @(Get-ChildItem -LiteralPath $installRoot -Recurse -Directory -Force |
        Sort-Object { $_.FullName.Length } -Descending)) {
        if (!(Get-ChildItem -LiteralPath $dir.FullName -Force | Select-Object -First 1)) {
            try { [IO.Directory]::Delete($dir.FullName, $false) } catch { }
        }
    }
    $appKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Atrium'
    if ((Get-ItemProperty -LiteralPath $appKey -ErrorAction SilentlyContinue).InstallLocation -eq $installRoot) {
        Remove-Item -LiteralPath $appKey -Recurse -Force
    }
    # Leave any files the user added to the application folder.
    # An app some process started there may still be in it: the empty folder then stays.
    if (!(Get-ChildItem -LiteralPath $installRoot -Force | Select-Object -First 1)) {
        try { Remove-Item -LiteralPath $installRoot } catch { }
    }
}
catch {
    $failed = $true
    if ($Quiet) {
        [Console]::Error.WriteLine($_.Exception.Message)
    } else {
        [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Atrium', 'OK', 'Error')
    }
}
finally {
    if ($installMutexAcquired) { try { $installMutex.ReleaseMutex() } catch { } }
    if ($installMutex) { $installMutex.Dispose() }
}
if ($failed) { exit 1 }
