[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CabinetRoot,
    [Parameter(Mandatory)]
    [string] $LauncherName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
if ([IO.Path]::GetFileName($LauncherName) -ne $LauncherName) {
    throw '-LauncherName must be a file name generated inside CabinetRoot.'
}
$LauncherPath = Join-Path $CabinetRoot $LauncherName
if (-not (Test-Path -LiteralPath $LauncherPath -PathType Leaf)) {
    throw "Generated launcher is absent: $LauncherPath"
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound.exe is already running.'
}

Write-Host "[START] $LauncherPath"
Write-Host '[START] This script invokes the generated launcher; it does not rewrite game files.'
Push-Location $CabinetRoot
try {
    & $LauncherPath
    $ExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
Write-Host "[EXIT] dancearound returned $ExitCode"
exit $ExitCode
