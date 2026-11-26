<#
.SYNOPSIS
  Builds Atrium and assembles a portable folder: dist/Atrium/.

.DESCRIPTION
  native (CMake, MSVC) -> settings (dotnet publish, self-contained WinUI 3) -> dist.
  Layout:
    dist/Atrium/Atrium.exe
    dist/Atrium/Settings/Fonts/ (MiSans, D40; the core reads it from there)
    dist/Atrium/Settings/Atrium.Settings.exe (+ runtime)
  -Zip also writes dist/Atrium-<version>-win-x64.zip; -Tests also builds the native tests (ctest).
  -Ninja builds the native core with Ninja (exporting compile_commands.json).
#>
param(
    [string]$Configuration = "Release",
    [switch]$Zip,
    [switch]$Development,
    [switch]$Tests,
    [switch]$Ninja
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root $(if ($Development) { "dist/Development/Atrium" } else { "dist/Atrium" })
$buildDirectory = if ($Development) { 'native/build/development' } else { 'native/build/win-x64' }
$developmentFlag = if ($Development) { 'ON' } else { 'OFF' }
$testsFlag = if ($Tests) { 'ON' } else { 'OFF' }

Push-Location $root
try {
    if ($Ninja) {
        $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
        if (-not ($cl -and $cl.Source -match '[\\/]x64[\\/]cl\.exe$')) {
            $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
            $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath 2>$null } else { $null }
            $vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' } else { $null }
            if (-not $vcvars -or -not (Test-Path $vcvars)) {
                throw 'build: x64 cl.exe not on PATH and Visual Studio vcvars64.bat not found'
            }
            $envLines = & cmd /c "`"$vcvars`" >nul && set"
            if ($LASTEXITCODE) { throw "build: vcvars64.bat failed (exit $LASTEXITCODE)" }
            foreach ($line in $envLines) {
                if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
            }
            $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
            if (-not ($cl -and $cl.Source -match '[\\/]x64[\\/]cl\.exe$')) {
                throw 'build: x64 cl.exe not found after loading vcvars64.bat'
            }
        }
    }
    $cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        $genLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' | Select-Object -First 1
        $cachedGen = if ($genLine) { $genLine.Matches[0].Groups[1].Value.Trim() } else { '' }
        if ($cachedGen -and (($Ninja -and $cachedGen -ne 'Ninja') -or (-not $Ninja -and $cachedGen -notlike 'Visual Studio *'))) {
            Write-Host "Cleaning $buildDirectory (generator changed: $cachedGen)"
            Remove-Item -LiteralPath $buildDirectory -Recurse -Force
        }
    }
    if ($Ninja) {
        cmake --preset win-x64-ninja -S native -B $buildDirectory "-DCMAKE_BUILD_TYPE=$Configuration" `
            "-DATRIUM_DEVELOPMENT=$developmentFlag" "-DATRIUM_BUILD_TESTS=$testsFlag"
        if ($LASTEXITCODE) { throw "cmake configure failed" }
        cmake --build $buildDirectory
        if ($LASTEXITCODE) { throw "native build failed" }
    } else {
        cmake --preset win-x64 -S native -B $buildDirectory `
            "-DATRIUM_DEVELOPMENT=$developmentFlag" "-DATRIUM_BUILD_TESTS=$testsFlag"
        if ($LASTEXITCODE) { throw "cmake configure failed" }
        cmake --build $buildDirectory --config $Configuration
        if ($LASTEXITCODE) { throw "native build failed" }
    }

    if (Test-Path $dist) {
        # A running Atrium (or its settings window) locks files: stop before deleting anything.
        $full = (Resolve-Path $dist).Path
        $expected = [IO.Path]::GetFullPath((Join-Path $root $(if ($Development) { 'dist/Development/Atrium' } else { 'dist/Atrium' })))
        if (![string]::Equals($full, $expected, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected distribution path' }
        $busy = @(Get-Process -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and $_.Path.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) })
        if ($busy) { throw "dist/Atrium is in use by $(($busy | ForEach-Object { "$($_.Name) ($($_.Id))" }) -join ', '); close it first" }
        Remove-Item -Recurse -Force $dist
    }
    New-Item -ItemType Directory -Force $dist | Out-Null
    Copy-Item (Join-Path $root "locales") $dist -Recurse
    $developmentBuild = $Development.IsPresent.ToString().ToLowerInvariant()
    dotnet publish settings/Atrium.Settings/Atrium.Settings.csproj -c $Configuration -p:AtriumDevelopment=$developmentBuild -o (Join-Path $dist "Settings")
    if ($LASTEXITCODE) { throw "settings publish failed" }

    $exe = if ($Ninja) { Join-Path $buildDirectory 'Atrium.exe' } else { Join-Path "$buildDirectory/$Configuration" 'Atrium.exe' }
    Copy-Item $exe $dist
    if ($Development) {
        Set-Content -LiteralPath (Join-Path $dist 'development.txt') -Value 'Development distribution: does not register or change Windows startup.'
    }
    Copy-Item (Join-Path $PSScriptRoot "Uninstall.ps1") $dist
    Copy-Item LICENSE $dist
    $version = (Select-String -Path native/CMakeLists.txt -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
    $distFull = [IO.Path]::GetFullPath($dist).TrimEnd('\')
    $manifestEntries = [System.Collections.Generic.List[object]]::new()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $relPaths = [System.Collections.Generic.List[string]]::new()
        foreach ($file in @(Get-ChildItem -LiteralPath $distFull -Recurse -File -Force)) {
            $rel = $file.FullName.Substring($distFull.Length + 1).Replace('/', '\')
            if ($rel -ieq 'package-manifest.json' -or $rel -ieq 'development.txt') { continue }
            $relPaths.Add($rel)
        }
        $relPaths.Sort([StringComparer]::OrdinalIgnoreCase)
        foreach ($rel in $relPaths) {
            $stream = [IO.File]::OpenRead((Join-Path $distFull $rel))
            try {
                $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
            } finally {
                $stream.Dispose()
            }
            $manifestEntries.Add([ordered]@{ path = $rel; sha256 = $hash })
        }
    } finally {
        $sha.Dispose()
    }
    $manifestJson = ([ordered]@{ schema = 1; version = $version; files = $manifestEntries } | ConvertTo-Json -Depth 4) -replace "`r`n", "`n"
    [IO.File]::WriteAllText((Join-Path $distFull 'package-manifest.json'), $manifestJson + "`n", (New-Object System.Text.UTF8Encoding $false))

    if ($Zip) {
        $zipPath = if ($Development) { Join-Path $root "dist/Development/Atrium-$version-development-win-x64.zip" }
                   else { Join-Path $root "dist/Atrium-$version-win-x64.zip" }
        if (Test-Path $zipPath) { Remove-Item $zipPath }
        Compress-Archive -Path $dist -DestinationPath $zipPath
        Write-Host "wrote $zipPath"
    }
    Write-Host "dist ready: $dist"
}
finally {
    Pop-Location
}
