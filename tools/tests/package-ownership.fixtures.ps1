function Get-SharedFileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
        try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}

function Write-Utf8Bytes([string]$Path, [string]$Text) {
    $dir = Split-Path -Parent $Path
    if ($dir -and !(Test-Path -LiteralPath $dir -PathType Container)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    [IO.File]::WriteAllBytes($Path, $bytes)
    return $bytes
}

function Assert-SameBytes([string]$Path, [byte[]]$Expected, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "${Label}: missing $Path" }
    $actual = [IO.File]::ReadAllBytes($Path)
    if ($actual.Length -ne $Expected.Length) { throw "${Label}: byte length mismatch for $Path" }
    for ($i = 0; $i -lt $Expected.Length; $i++) {
        if ($actual[$i] -ne $Expected[$i]) { throw "${Label}: byte content mismatch for $Path" }
    }
}

function Get-SharedUserFileSamples {
    return [ordered]@{
        'user-root.txt'                 = "custom-root`nline2"
        'Settings\custom.ini'           = "[user]`ntheme=1"
        'locales\user.json'             = '{"custom":"yes"}'
        'Settings\Assets\my-photo.png'  = 'user-photo-png-bytes'
        'Settings\Fonts\personal.otf'   = 'user-font-otf-bytes'
        'Settings\my-extension.dll'     = 'user-extension-dll-bytes'
    }
}

function Get-SharedModifiedOfficialSample {
    return @{
        Path     = 'locales\zh-Hans.json'
        Official = '{"title":"Atrium Official"}'
        Modified = '{"title":"Atrium User Modified"}'
    }
}

function Get-DirectoryManifestEntries([string]$Dir, [string[]]$ExcludeRel) {
    $full = [IO.Path]::GetFullPath($Dir).TrimEnd('\')
    $rels = New-Object 'System.Collections.Generic.List[string]'
    foreach ($file in @(Get-ChildItem -LiteralPath $full -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($full.Length + 1).Replace('/', '\')
        if ($ExcludeRel -contains $rel.ToLowerInvariant()) { continue }
        $rels.Add($rel)
    }
    $rels.Sort([StringComparer]::OrdinalIgnoreCase)
    $entries = New-Object 'System.Collections.Generic.List[object]'
    foreach ($rel in $rels) {
        $hash = Get-SharedFileSha256 (Join-Path $full $rel)
        $entries.Add([ordered]@{ path = $rel; sha256 = $hash })
    }
    return $entries
}

function Write-SharedPackageManifest([string]$Dir, [string]$Version = '2.8.0') {
    $full = [IO.Path]::GetFullPath($Dir).TrimEnd('\')
    $entries = Get-DirectoryManifestEntries $full @('package-manifest.json', 'development.txt')
    $json = ([ordered]@{ schema = 1; version = $Version; files = $entries } | ConvertTo-Json -Depth 4) -replace "`r`n", "`n"
    [IO.File]::WriteAllText((Join-Path $full 'package-manifest.json'), $json + "`n", (New-Object System.Text.UTF8Encoding $false))
}

function Write-SharedLegacyCatalog([string]$Dir, [string]$OutPath, [string]$Version = '2.7.0') {
    $exclude = @('package-manifest.json', 'development.txt', 'legacy-manifests.json', 'settings\legacy-manifests.json')
    $entries = Get-DirectoryManifestEntries $Dir $exclude
    $outDir = Split-Path -Parent $OutPath
    if ($outDir -and !(Test-Path -LiteralPath $outDir -PathType Container)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
    $releases = @([ordered]@{ version = $Version; files = $entries })
    $json = ([ordered]@{ schema = 1; releases = $releases } | ConvertTo-Json -Depth 5) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($OutPath, $json + "`n", (New-Object System.Text.UTF8Encoding $false))
}

function Assert-VerifiedPackageDirectory([string]$Dir) {
    $fullRoot = [IO.Path]::GetFullPath($Dir).TrimEnd('\')
    foreach ($req in @('Atrium.exe', 'Settings\Atrium.Settings.exe', 'Settings\Update.ps1',
        'Uninstall.ps1', 'locales\manifest.json', 'package-manifest.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $fullRoot $req) -PathType Leaf)) {
            throw "Verified package missing required file: $req"
        }
    }
    $m = Get-Content -LiteralPath (Join-Path $fullRoot 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $m -or (($m.schema -isnot [int]) -and ($m.schema -isnot [long])) -or
        $m.schema -ne 1 -or [string]::IsNullOrWhiteSpace([string]$m.version)) {
        throw 'Verified package manifest schema or version invalid'
    }
    $entries = @($m.files)
    if ($entries.Count -eq 0) { throw 'Verified package manifest files empty' }
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        $norm = ([string]$entry.path).Replace('/', '\')
        if (!$listed.Add($norm)) { throw "Verified package duplicate path: $norm" }
        $full = Join-Path $fullRoot $norm
        if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "Verified package missing file: $norm" }
        $actual = Get-SharedFileSha256 $full
        if ($actual -cne [string]$entry.sha256) { throw "Verified package hash mismatch: $norm" }
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $fullRoot -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($fullRoot.Length + 1).Replace('/', '\')
        if ($rel -ieq 'package-manifest.json' -or $rel -ieq 'development.txt') { continue }
        if (!$listed.Contains($rel)) { throw "Verified package unlisted file: $rel" }
    }
    return $entries.Count
}
