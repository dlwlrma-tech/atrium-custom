# Clang-tidy pipeline helper dot-sourced by tools/lint.ps1; never execute standalone.

function Get-VsPath {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath 2>$null } else { $null }
}

function Find-Ninja {
    $cmd = Get-Command ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    $vs = Get-VsPath
    if ($vs) {
        $cand = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (Test-Path $cand) { return $cand }
    }
    $null
}

function Import-MsvcEnv {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cl -and $cl.Source -match '[\\/]x64[\\/]cl\.exe$') { return }
    $vs = Get-VsPath
    $vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' } else { $null }
    if (-not $vcvars -or -not (Test-Path $vcvars)) { throw 'lint: cl.exe not on PATH and Visual Studio vcvars64.bat not found' }
    foreach ($line in (Invoke-Tool 'cmd' @('/c', "`"$vcvars`" >nul && set")).Lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
}

function Resolve-BuildPath([string]$BaseDir, [string]$RelOrAbs) {
    [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($RelOrAbs)) { $RelOrAbs } else { Join-Path $BaseDir $RelOrAbs }))
}

function Get-TouchedShaderHeaders([string]$Build, [System.Collections.Generic.HashSet[string]]$TouchedSet) {
    $genHeaders = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $hasHlsl = [bool]($TouchedSet | Where-Object { $_ -match '^native/core/render/shaders/.*\.hlsl$' } | Select-Object -First 1)
    $buildNinja = Join-Path $Build 'build.ninja'
    if (-not $hasHlsl -or -not (Test-Path -LiteralPath $buildNinja -PathType Leaf)) { return ,$genHeaders }
    foreach ($line in [IO.File]::ReadLines($buildNinja)) {
        if ($line -match '^build\s+([^\s:|]+[\\/]shaders[\\/][^:|]+\.h)\b(?:[^:$]|\$.)*:\s+\S+\s+(.+)$') {
            $genRel = $Matches[1].Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
            foreach ($tok in ($Matches[2] -split '(?<!\$)\s+')) {
                if (-not $tok -or $tok -eq '|' -or $tok -eq '||') { continue }
                $dep = $tok.Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
                if ($TouchedSet.Contains((Rel (Resolve-BuildPath $Build $dep)))) { [void]$genHeaders.Add((Resolve-BuildPath $Build $genRel)); break }
            }
        }
    }
    return ,$genHeaders
}

function Get-StaticHeaders([string]$StartFile, [string]$Root, [hashtable]$DirectCache) {
    $coreDir = Join-Path $Root 'native/core'
    $visited = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($StartFile)
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        if (-not $DirectCache.ContainsKey($cur)) {
            $incs = [System.Collections.Generic.List[string]]::new()
            $curDir = Split-Path $cur
            if (Test-Path -LiteralPath $cur -PathType Leaf) {
                foreach ($line in [IO.File]::ReadLines($cur)) {
                    if ($line -match '^\s*#\s*include\s*[<"]([^>"]+)[>"]') {
                        $cand = [IO.Path]::GetFullPath((Join-Path $curDir $Matches[1]))
                        if (-not (Test-Path -LiteralPath $cand -PathType Leaf)) { $cand = [IO.Path]::GetFullPath((Join-Path $coreDir $Matches[1])) }
                        if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Rel $cand) -match '^native/(?:core|tests)/.*\.h$') { $incs.Add($cand) }
                    }
                }
            }
            $DirectCache[$cur] = $incs
        }
        foreach ($next in $DirectCache[$cur]) { if ($visited.Add($next)) { $queue.Enqueue($next) } }
    }
    return ,$visited
}

function Test-HasDevBranch([string]$Unit, $Headers, [hashtable]$DevFileCache) {
    $hasDev = {
        param([string]$path)
        if (-not $DevFileCache.ContainsKey($path)) {
            $DevFileCache[$path] = (Test-Path -LiteralPath $path -PathType Leaf) -and
                ([IO.File]::ReadAllText($path) -match '(?m)^\s*#\s*(?:el)?if(?:n?def)?\b.*\bATRIUM_DEVELOPMENT\b')
        }
        $DevFileCache[$path]
    }
    if (& $hasDev $Unit) { return $true }
    foreach ($h in $Headers) { if (& $hasDev $h) { return $true } }
    $false
}

$script:TidyTypeKeywords = [System.Collections.Generic.HashSet[string]]::new([string[]]@('int', 'long', 'short', 'char', 'wchar_t',
    'char8_t', 'char16_t', 'char32_t', 'bool', 'float', 'double', 'void', 'signed', 'unsigned', 'const', 'volatile', 'noexcept', 'auto',
    'size_t', 'ptrdiff_t', 'intptr_t', 'uintptr_t', 'int8_t', 'int16_t', 'int32_t', 'int64_t', 'uint8_t', 'uint16_t', 'uint32_t',
    'uint64_t'), [StringComparer]::Ordinal)
$script:TidyQualOnly = [System.Collections.Generic.HashSet[string]]::new([string[]]@('const', 'volatile', 'struct', 'class', 'enum',
    'union', 'typename'), [StringComparer]::Ordinal)
$script:TidyControlKeywords = [System.Collections.Generic.HashSet[string]]::new([string[]]@('if', 'for', 'while', 'switch', 'catch',
    'return', 'else', 'do', 'try', 'case', 'default', 'sizeof', 'alignof', 'decltype', 'static_assert', 'requires', 'using', 'typedef',
    'namespace', 'class', 'struct', 'enum', 'union'), [StringComparer]::Ordinal)

function Find-ParamBounds([string]$sig) {
    $ang = 0; $tp = 0; $par = 0; $open = -1; $attr = 0
    for ($i = 0; $i -lt $sig.Length; $i++) {
        $c = $sig[$i]
        if ($open -ge 0) { if ($c -eq '(') { $par++ } elseif ($c -eq ')' -and --$par -eq 0) { return @($open, $i) }; continue }
        if ($attr -gt 0) { if ($c -eq '[') { $attr++ } elseif ($c -eq ']') { $attr-- }; continue }
        if ($c -eq '[' -and $i + 1 -lt $sig.Length -and $sig[$i + 1] -eq '[') { $attr = 1; continue }
        if ($c -eq 'o' -and ($i -eq 0 -or $sig[$i - 1] -notmatch '\w') -and
            $sig.Substring($i) -match '^operator\s*(?:\(\s*\)|\[\s*\]|(?:new|delete)(?:\s*\[\s*\])?|[+\-*/%^&|~!=<>,]+)') {
            $i += $Matches[0].Length - 1; continue
        }
        if ($ang -gt 0) {
            if ($c -eq '(') { $tp++ } elseif ($c -eq ')' -and $tp -gt 0) { $tp-- }
            elseif ($tp -eq 0 -and $c -eq '<') { $ang++ } elseif ($tp -eq 0 -and $c -eq '>') { $ang-- }; continue
        }
        if ($c -eq '<') { $ang++ }
        elseif ($c -eq '(') { $open = $i; $par = 1 }
        elseif ($c -in @('=', ';', ',', '[', ']', '{', '}', '.') -or ($c -eq '-' -and $i + 1 -lt $sig.Length -and $sig[$i + 1] -eq '>')) { return $null }
        elseif ($c -eq ':') { if ($i + 1 -lt $sig.Length -and $sig[$i + 1] -eq ':') { $i++ } else { return $null } }
    }
    $null
}

function Get-QualName([string]$before) {
    $b = ($before -replace '\s*::\s*', '::' -replace '\s+', ' ').Trim()
    $rx = '(?:(?:[A-Za-z_]\w*(?:<(?:[^<>]|(?<o><)|(?<-o>>))*(?(o)(?!))>)?::)*)' +
        '(?:~[A-Za-z_]\w*|operator\s*(?:\(\s*\)|\[\s*\]|(?:new|delete)(?:\s*\[\s*\])?|[+\-*/%^&|~!=<>,]+|[A-Za-z_]\w*)|[A-Za-z_]\w*)$'
    if ($b -notmatch $rx) { return $null }
    $q = $Matches[0] -replace 'operator\s*\(\s*\)', 'operator()' -replace 'operator\s*\[\s*\]', 'operator[]' `
        -replace 'operator\s+(new|delete)\s*\[\s*\]', 'operator $1[]' -replace 'operator\s+([+\-*/%^&|~!=<>,]+)$', 'operator$1'
    if ($script:TidyControlKeywords.Contains(($q -split '::')[-1])) { return $null }
    $q
}

function Format-ParamList([string]$rawParams) {
    $trimmed = $rawParams.Trim()
    if (-not $trimmed) { return '' }
    $parts = [System.Collections.Generic.List[string]]::new(); $sb = [System.Text.StringBuilder]::new()
    $ang = 0; $par = 0; $brc = 0; $brk = 0; $inDef = $false
    foreach ($c in $trimmed.ToCharArray()) {
        if ($inDef) {
            if ($c -eq '(') { $par++ } elseif ($c -eq ')' -and $par -gt 0) { $par-- }
            elseif ($c -eq '{') { $brc++ } elseif ($c -eq '}' -and $brc -gt 0) { $brc-- }
            elseif ($c -eq '[') { $brk++ } elseif ($c -eq ']' -and $brk -gt 0) { $brk-- }
            elseif ($c -eq ',' -and ($par + $brc + $brk) -eq 0) { $parts.Add($sb.ToString()); $null = $sb.Clear(); $inDef = $false }
            continue
        }
        $top = ($ang + $par + $brc + $brk) -eq 0
        if ($c -eq '<' -and ($par + $brc + $brk) -eq 0) { $ang++ }
        elseif ($c -eq '>' -and ($par + $brc + $brk) -eq 0 -and $ang -gt 0) { $ang-- }
        elseif ($c -eq '(') { $par++ } elseif ($c -eq ')' -and $par -gt 0) { $par-- }
        elseif ($c -eq '{') { $brc++ } elseif ($c -eq '}' -and $brc -gt 0) { $brc-- }
        elseif ($c -eq '[') { $brk++ } elseif ($c -eq ']' -and $brk -gt 0) { $brk-- }
        elseif ($c -eq '=' -and $top) { $inDef = $true; continue }
        elseif ($c -eq ',' -and $top) { $parts.Add($sb.ToString()); $null = $sb.Clear(); continue }
        [void]$sb.Append($c)
    }
    $parts.Add($sb.ToString())
    $out = foreach ($part in $parts) {
        $p = ($part -replace '\s+', ' ').Trim(); if (-not $p) { continue }
        if ($p -match '^(.+[*&>\]])\s*([A-Za-z_]\w*)$' -and -not $script:TidyTypeKeywords.Contains($Matches[2])) { $p = $Matches[1] }
        elseif ($p -match '^(.+?)\s+([A-Za-z_]\w*)$') {
            $head = $Matches[1]; $tail = $Matches[2]; $words = $head -split '\s+'
            $allQual = $true; foreach ($w in $words) { if (-not $script:TidyQualOnly.Contains($w)) { $allQual = $false; break } }
            if (-not $script:TidyTypeKeywords.Contains($tail) -and -not $allQual -and
                $words[-1] -notin @('struct', 'class', 'enum', 'union', 'typename')) { $p = $head }
        }
        (($p -replace '\s+', ' ').Trim() -replace '\s+([*&>)])', '$1' -replace '([<(])\s+', '$1' -replace '\s*,\s*', ',')
    }
    $out -join ','
}

function Get-BlockHeader([string[]]$raw, [string[]]$code, [int]$braceLine, [int]$braceCol) {
    $chunks = [System.Collections.Generic.List[string]]::new(); $p = 0; $b = 0; $c = 0; $start = $braceLine
    for ($s = $braceLine; $s -ge [Math]::Max(0, $braceLine - 7); $s--) {
        if ($raw[$s] -match '^\s*(?:#|(?:public|private|protected|default)\s*:|case\b[^:]*:\s*$)') { break }
        $ln = if ($s -eq $braceLine) { $code[$s].Substring(0, $braceCol) } else { $code[$s] }
        $cut = -1; $hit = $false
        for ($k = $ln.Length - 1; $k -ge 0; $k--) {
            $ch = $ln[$k]
            if ($ch -eq ')') { $p++ } elseif ($ch -eq '(') { $p-- } elseif ($ch -eq ']') { $b++ } elseif ($ch -eq '[') { $b-- }
            elseif ($ch -eq '}') {
                if ($p -eq 0 -and $b -eq 0 -and $c -eq 0 -and $ln.Substring(0, $k) -notmatch '(?:^|[:,])\s*[A-Za-z_]\w*\s*\{[^;{}]*$') {
                    $cut = $k; $hit = $true; break
                }
                $c++
            } elseif ($ch -eq '{') { $c-- }
            if ($p -lt 0 -or $b -lt 0 -or $c -lt 0 -or ($p -eq 0 -and $b -eq 0 -and $c -eq 0 -and $ch -eq ';')) { $cut = $k; $hit = $true; break }
        }
        $seg = if ($cut -ge 0) { $ln.Substring($cut + 1) } else { $ln }
        if ($seg.Trim()) { $chunks.Insert(0, $seg); $start = $s }
        if ($hit) { break }
    }
    [pscustomobject]@{ Header = (($chunks -join ' ') -replace '\s+', ' ').Trim(); Start = $start }
}

function Get-EnclosingNames([string[]]$raw, [string[]]$code, [int]$fromLine, [bool]$wantFn) {
    $names = [System.Collections.Generic.List[string]]::new(); $brace = 0
    for ($j = $fromLine - 1; $j -ge 0; $j--) {
        $cl = $code[$j]
        for ($k = $cl.Length - 1; $k -ge 0; $k--) {
            $ch = $cl[$k]
            if ($ch -eq '}') { $brace++ }
            elseif ($ch -eq '{' -and --$brace -lt 0) {
                $brace = 0; $blk = Get-BlockHeader $raw $code $j $k
                $h = $blk.Header -replace '^\s*template\s*<(?:[^<>]|(?<o><)|(?<-o>>))*(?(o)(?!))>\s*', ''
                if ($h -match '^\s*namespace\b') { return ,$names }
                if ($h -match '^\s*(?:class|struct)\s+(?:(?:__declspec|alignas)\s*\([^)]*\)\s*)*([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\b') {
                    if ($wantFn) { return ,$names }
                    $names.Insert(0, $Matches[1]); continue
                }
                if ($h -match '^\s*(?:enum|union|case|default)\b' -or $h -match ':\s*$') { continue }
                $bounds = Find-ParamBounds $h; if (-not $bounds) { continue }
                $qn = Get-QualName ($h.Substring(0, $bounds[0])); if (-not $qn) { continue }
                $names.Insert(0, $qn)
                if ($qn.Contains('::') -or $raw[$blk.Start] -notmatch '^\s+') { return ,$names }
                $wantFn = $false
            }
        }
    }
    return ,$names
}

function Get-TidySymbolKey([string]$absPath, [int]$row, [string]$bare, [bool]$isLambda) {
    if (-not (Get-Variable -Name fileLinesCache -ErrorAction SilentlyContinue)) { $script:fileLinesCache = @{} }
    $fallback = if ($bare) { $bare } else { '(lambda)' }
    if (-not $absPath) { return $fallback }
    $norm = [IO.Path]::GetFullPath($absPath)
    if (-not $fileLinesCache.ContainsKey($norm)) {
        if (-not (Test-Path -LiteralPath $norm -PathType Leaf)) { return $fallback }
        $rawLines = [IO.File]::ReadAllLines($norm)
        $codeLines = if (Get-Command Get-NativeCodeLines -ErrorAction SilentlyContinue) { Get-NativeCodeLines $rawLines }
            else { @($rawLines | ForEach-Object { $_ -replace '/\*.*?\*/', ' ' -replace '//.*$', '' }) }
        $fileLinesCache[$norm] = @{ Raw = $rawLines; Code = $codeLines }
    }
    $entry = $fileLinesCache[$norm]; $raw = $entry.Raw; $code = $entry.Code; $idx = $row - 1
    if ($idx -lt 0 -or $idx -ge $raw.Count) { return $fallback }
    if ($isLambda -or $bare -eq 'operator()') {
        $cur = $code[$idx]; $lambdaName = '(lambda)'
        if ($cur -match '([A-Za-z_]\w*)\s*=\s*\[') { $lambdaName = $Matches[1] }
        elseif ($idx -gt 0) {
            $prev = $code[$idx - 1]
            if (($cur -match '^\s*\[' -and $prev -match '([A-Za-z_]\w*)\s*=\s*$') -or
                ($cur -match '^\s*=\s*\[' -and $prev -match '([A-Za-z_]\w*)\s*$')) { $lambdaName = $Matches[1] }
        }
        $enclosing = (Get-EnclosingNames $raw $code $idx $true) -join '::'
        return $(if ($enclosing) { "${enclosing}::$lambdaName" } else { $lambdaName })
    }
    $sb = [System.Text.StringBuilder]::new(); $bounds = $null; $sig = ''
    for ($j = $idx; $j -le [Math]::Min($idx + 7, $code.Count - 1); $j++) {
        [void]$sb.Append($code[$j]).Append(' '); $cand = ($sb.ToString() -replace '\s+', ' ').Trim()
        $b = Find-ParamBounds $cand; if ($b) { $bounds = $b; $sig = $cand; break }
    }
    if (-not $bounds) { return $fallback }
    $qual = Get-QualName ($sig.Substring(0, $bounds[0])); if (-not $qual) { return $fallback }
    if (-not $qual.Contains('::') -and $raw[$idx] -match '^\s+') {
        $cls = Get-EnclosingNames $raw $code $idx $false
        if ($cls.Count) { $qual = ($cls + @($qual)) -join '::' }
    }
    "${qual}($(Format-ParamList ($sig.Substring($bounds[0] + 1, $bounds[1] - $bounds[0] - 1))))"
}

function Set-UniqueCount([System.Collections.IDictionary]$map, [string]$baseKey, [int]$value) {
    if ($baseKey -match '^(.*)(::|\|)\(lambda\)$') {
        $pfx = "$($Matches[1])$($Matches[2])"; $n = 1
        while ($map.Contains("${pfx}(lambda#$n)")) { $n++ }
        $key = "${pfx}(lambda#$n)"
    } else {
        $key = $baseKey
        if ($map.Contains($key)) { $n = 2; while ($map.Contains("${baseKey}#$n")) { $n++ }; $key = "${baseKey}#$n" }
    }
    $map[$key] = $value; $key
}

function Invoke-TidyCheck(
    [string]$Root, [string]$Rules, [string]$BuildDir, [string]$Base, [bool]$FullRun, [string[]]$TouchedFiles,
    [System.Collections.Generic.HashSet[string]]$Analysed, [System.Collections.Generic.List[string]]$Errors,
    [System.Collections.IDictionary]$Counts, [int]$LlvmMajor
) {
    Import-MsvcEnv
    $ninja = Find-Ninja
    if ($BuildDir) {
        $build = Resolve-BuildPath $Root $BuildDir
        if (-not (Test-Path -LiteralPath (Join-Path $build 'compile_commands.json') -PathType Leaf)) {
            throw "lint: -BuildDir '$BuildDir' has no compile_commands.json"
        }
    } else {
        $build = Join-Path $Root 'native/build/tidy'
        $cmakeArgs = @('-S', (Join-Path $Root 'native'), '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DATRIUM_DEVELOPMENT=OFF', '-DATRIUM_BUILD_TESTS=ON')
        if ($ninja) { $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$ninja" }
        Invoke-Tool 'cmake' $cmakeArgs | Out-Null
        Invoke-Tool 'cmake' @('--build', $build) | Out-Null # the generated shader headers
    }

    $ccEntries = @((Get-Content -LiteralPath (Join-Path $build 'compile_commands.json') -Raw | ConvertFrom-Json))
    $unitSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $objToUnit = @{}; $unitDevDefined = @{}
    foreach ($entry in $ccEntries) {
        $f = [string]$entry.file
        if ($f -notmatch '[\\/]native[\\/](core|tests)[\\/].*\.cpp$') { continue }
        $u = [IO.Path]::GetFullPath($f)
        [void]$unitSet.Add($u)
        if ($entry.output) { $objToUnit[(([string]$entry.output).Replace('\', '/') -replace '^\./', '')] = $u }
        $cmd = if ($entry.command) { [string]$entry.command } elseif ($entry.arguments) { $entry.arguments -join ' ' } else { '' }
        $dev = $false
        foreach ($m in [regex]::Matches($cmd, '(?:^|[\s"])[-/](D|U)ATRIUM_DEVELOPMENT(?:=([^\s"]+))?(?=[\s"]|$)')) {
            $dev = ($m.Groups[1].Value -ne 'U') -and (-not $m.Groups[2].Success -or $m.Groups[2].Value -ne '0')
        }
        $unitDevDefined[$u] = $dev
    }
    $units = @($unitSet | Sort-Object)

    $touchedSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($tf in $TouchedFiles) { if ($tf) { [void]$touchedSet.Add(($tf.Replace('\', '/') -replace '^\./', '')) } }
    $unitHeaders = @{}
    $selectedSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $seenUnits = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if ($ninja -and (Test-Path -LiteralPath (Join-Path $build '.ninja_deps') -PathType Leaf)) {
        $touchedGen = if ($Base -and -not $FullRun) { Get-TouchedShaderHeaders $build $touchedSet } else { $null }
        $curUnit = $null
        foreach ($line in (Invoke-Tool $ninja @('-C', $build, '-t', 'deps')).Lines) {
            if ($line -match '^(\S.*?): #deps ') {
                $curUnit = $objToUnit[(($Matches[1].Replace('\', '/') -replace '^\./', '').Trim())]
                if ($curUnit -and $line -match ': #deps [1-9]\d*') { [void]$seenUnits.Add($curUnit) } else { $curUnit = $null }
                continue
            }
            if (-not $line.StartsWith(' ') -or -not $curUnit) { if (-not $line.StartsWith(' ')) { $curUnit = $null }; continue }
            $dep = $line.Trim(); if (-not $dep) { continue }
            $depAbs = Resolve-BuildPath $build $dep; $depRel = Rel $depAbs
            if ($depRel -match '^native/(?:core|tests)/.*\.h$') {
                if (-not $unitHeaders.ContainsKey($curUnit)) {
                    $unitHeaders[$curUnit] = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
                }
                [void]$unitHeaders[$curUnit].Add($depAbs)
            }
            if ($Base -and -not $FullRun -and ($touchedSet.Contains($depRel) -or ($touchedGen -and $touchedGen.Contains($depAbs)))) {
                [void]$selectedSet.Add($curUnit)
            }
        }
        if ($Base -and -not $FullRun) {
            $units = @($units | Where-Object { $touchedSet.Contains((Rel $_)) -or $selectedSet.Contains($_) -or -not $seenUnits.Contains($_) })
        }
    } else {
        $directCache = @{}
        $hasHlsl = [bool]($touchedSet | Where-Object { $_ -match '^native/core/render/shaders/.*\.hlsl$' } | Select-Object -First 1)
        foreach ($u in $units) { $unitHeaders[$u] = Get-StaticHeaders $u $Root $directCache }
        if ($Base -and -not $FullRun) {
            $units = @($units | Where-Object {
                $uRel = Rel $_
                $touchedSet.Contains($uRel) -or ($hasHlsl -and $uRel -eq 'native/core/render/glass_renderer.cpp') -or
                    [bool]($unitHeaders[$_] | Where-Object { $touchedSet.Contains((Rel $_)) } | Select-Object -First 1)
            })
        }
    }

    foreach ($u in $units) {
        $r = Rel $u; if ($r.StartsWith('native/')) { [void]$Analysed.Add($r.Substring(7)) }
        if ($unitHeaders.ContainsKey($u)) {
            foreach ($h in $unitHeaders[$u]) { $hr = Rel $h; if ($hr.StartsWith('native/')) { [void]$Analysed.Add($hr.Substring(7)) } }
        }
    }
    $clangTidy = Llvm 'clang-tidy.exe'
    if (-not $clangTidy) { throw "lint: clang-tidy $LlvmMajor not found" }
    $tidyConfig = Join-Path $Rules '.clang-tidy'; $devFileCache = @{}
    # One clang-tidy per processor, or ATRIUM_LINT_JOBS of them (several lints sharing a machine).
    $limit = 0
    $throttle = if ([int]::TryParse($env:ATRIUM_LINT_JOBS, [ref]$limit) -and $limit -gt 0) { $limit } else { [Environment]::ProcessorCount }
    $jobs = @(foreach ($u in $units) {
        $hdrs = if ($unitHeaders.ContainsKey($u)) { $unitHeaders[$u] } else { @() }
        $hasBranch = Test-HasDevBranch $u $hdrs $devFileCache; $devDef = [bool]$unitDevDefined[$u]
        [pscustomobject]@{ Unit = $u; ExtraArg = ''; RecordRatchets = (-not $hasBranch) -or (-not $devDef) }
        if ($hasBranch) {
            [pscustomobject]@{ Unit = $u; ExtraArg = $(if ($devDef) { '-UATRIUM_DEVELOPMENT' } else { '-DATRIUM_DEVELOPMENT' }); RecordRatchets = $devDef }
        }
    })
    $results = if ($jobs.Count) {
        @($jobs | ForEach-Object -ThrottleLimit $throttle -Parallel {
            $job = $_; $errFile = [IO.Path]::GetTempFileName()
            try {
                $argsList = @('-p', $using:build, "--config-file=$using:tidyConfig", '--quiet')
                if ($job.ExtraArg) { $argsList += "--extra-arg=$($job.ExtraArg)" }
                $stdout = @(& $using:clangTidy @argsList $job.Unit 2>$errFile | ForEach-Object { "$_" })
                $exit = $LASTEXITCODE
                $stderr = if (Test-Path $errFile) { [IO.File]::ReadAllLines($errFile) } else { @() }
            } finally { Remove-Item $errFile -ErrorAction SilentlyContinue }
            [pscustomobject]@{ Unit = $job.Unit; ExtraArg = $job.ExtraArg; RecordRatchets = $job.RecordRatchets
                Exit = $exit; Stdout = $stdout; Stderr = $stderr }
        })
    } else { @() }

    $seen = @{}; $fileLinesCache = @{}
    foreach ($r in $results) {
        $unitRel = Rel $r.Unit; $tag = if ($r.ExtraArg) { "$unitRel ($($r.ExtraArg))" } else { $unitRel }
        $allLines = @($r.Stdout) + @($r.Stderr)
        if ($r.Exit -ne 0) { $Errors.Add("tool-error: clang-tidy failed on $tag (exit $($r.Exit)): $(Tail $allLines)") }
        foreach ($line in $allLines) {
            if ($line -match '^(?:(.+?):(\d+):\d+: )?(fatal error: .*)$') {
                Fail $(if ($Matches[1]) { "$(Rel $Matches[1]):$($Matches[2])" } else { $unitRel }) 'clang-diagnostic-error' "$($Matches[3]) (in $tag)"
            } elseif ($line -match '^(.+?):(\d+):\d+: error: (.*) \[(clang-diagnostic-[\w-]+)\]$') {
                Fail "$(Rel $Matches[1]):$($Matches[2])" $Matches[4] "$($Matches[3]) (in $tag)"
            }
        }
        $current = $null
        foreach ($line in $r.Stdout) {
            if ($line -match '^(.+?):(\d+):(\d+): (?:warning|error): (.*) \[([\w.,-]+)\]$') {
                $file, $row, $col, $text, $check = $Matches[1..5]; $rel = Rel $file; $current = $null
                if ($check -like 'clang-diagnostic-*' -and $line -match ': error: ') { continue }
                if (-not $rel.StartsWith('native/core/') -and -not $rel.StartsWith('native/tests/')) { continue }
                $where = "${rel}:$row"
                $isRatchet = $check -in @('readability-function-cognitive-complexity', 'readability-function-size')
                if (($isRatchet -and -not $r.RecordRatchets) -or $seen.ContainsKey("$where|$col|$check")) { continue }
                $seen["$where|$col|$check"] = $true
                if ($check -eq 'readability-function-cognitive-complexity' -and $text -match "function '(.+)' has cognitive complexity of (\d+)") {
                    $sym = Get-TidySymbolKey $file ([int]$row) $Matches[1] $false
                    [void](Set-UniqueCount $Counts "$($rel.Substring(7))|complexity|$sym" ([int]$Matches[2]))
                } elseif ($check -eq 'readability-function-cognitive-complexity' -and $text -match 'lambda has cognitive complexity of (\d+)') {
                    $sym = Get-TidySymbolKey $file ([int]$row) '' $true
                    [void](Set-UniqueCount $Counts "$($rel.Substring(7))|complexity|$sym" ([int]$Matches[1]))
                } elseif ($check -eq 'readability-function-size' -and $text -match "function '(.+)' exceeds") {
                    $sym = Get-TidySymbolKey $file ([int]$row) $Matches[1] ($Matches[1] -eq 'operator()')
                    $current = Set-UniqueCount $Counts "$($rel.Substring(7))|function-lines|$sym" 0
                } else { Fail $where $check $text }
            } elseif ($current -and $line -match 'note: (\d+) lines including') {
                $Counts[$current] = [int]$Matches[1]; $current = $null
            }
        }
    }
}
