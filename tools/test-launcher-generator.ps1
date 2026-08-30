[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Generator = Join-Path $PSScriptRoot 'generate-launcher.ps1'
$TempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$TestRoot = Join-Path $TempBase ("anygear-launcher-test-{0}" -f [Guid]::NewGuid().ToString('N'))
$ResolvedTestRoot = [IO.Path]::GetFullPath($TestRoot)
if (-not $ResolvedTestRoot.StartsWith($TempBase, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($ResolvedTestRoot) -notlike 'anygear-launcher-test-*') {
    throw "Refusing to use unexpected temporary directory: $ResolvedTestRoot"
}

try {
    foreach ($Directory in @(
        $ResolvedTestRoot,
        (Join-Path $ResolvedTestRoot 'game'),
        (Join-Path $ResolvedTestRoot 'modules'))) {
        [void] (New-Item -ItemType Directory -Path $Directory -Force)
    }
    foreach ($File in @(
        (Join-Path $ResolvedTestRoot 'game\dancearound.exe'),
        (Join-Path $ResolvedTestRoot 'modules\kamunity.dll'),
        (Join-Path $ResolvedTestRoot 'modules\execexe.dll'),
        (Join-Path $ResolvedTestRoot 'dance_around_anygear_kinect.dll'))) {
        [IO.File]::WriteAllBytes($File, [byte[]] @())
    }

    & $Generator -CabinetRoot $ResolvedTestRoot -Backend kinect `
        -OutputName 'local.bat'
    $Local = Get-Content -LiteralPath (Join-Path $ResolvedTestRoot 'local.bat') -Raw
    if ($Local -notmatch '(?<!\S)-ea(?!\S)' -or $Local -match '(?<!\S)-url(?!\S)') {
        throw 'Default launcher did not select only the Spice local service.'
    }

    & $Generator -CabinetRoot $ResolvedTestRoot -Backend kinect `
        -EamuseMode external -EamuseUrl 'http://127.0.0.1:8083/' `
        -OutputName 'external.bat'
    $External = Get-Content -LiteralPath (Join-Path $ResolvedTestRoot 'external.bat') -Raw
    if ($External -match '(?<!\S)-ea(?!\S)' -or
        $External -notmatch '-url "http://127\.0\.0\.1:8083/"') {
        throw 'External launcher did not select only the configured service URL.'
    }

    $Rejected = $false
    try {
        & $Generator -CabinetRoot $ResolvedTestRoot -Backend kinect `
            -EamuseMode spice-local -EamuseUrl 'http://127.0.0.1:8083/' `
            -OutputName 'invalid.bat'
    }
    catch {
        $Rejected = $true
    }
    if (-not $Rejected) {
        throw 'Generator accepted an external URL together with Spice local mode.'
    }

    Write-Output 'PASS launcher e-amusement mode selection'
}
finally {
    if (Test-Path -LiteralPath $ResolvedTestRoot -PathType Container) {
        $DeleteTarget = (Resolve-Path -LiteralPath $ResolvedTestRoot).Path
        if (-not $DeleteTarget.StartsWith($TempBase, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($DeleteTarget) -notlike 'anygear-launcher-test-*') {
            throw "Refusing to remove unexpected directory: $DeleteTarget"
        }
        Remove-Item -LiteralPath $DeleteTarget -Recurse -Force
    }
}
