[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $OriginalWrapper,
    [string] $BridgeDll,
    [string] $ProbeExe,
    [switch] $LegacyHost
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before validating the native bridge.'
}
if ([string]::IsNullOrWhiteSpace($BridgeDll)) {
    $BridgeDll = Join-Path $RepositoryRoot `
        'build\bin\dance_around_anygear_d4xx.dll'
}
if ([string]::IsNullOrWhiteSpace($ProbeExe)) {
    $ProbeExe = Join-Path $RepositoryRoot `
        'build\bin\anygear_d4xx_native_bridge_smoke.exe'
}

$OriginalWrapper = (Resolve-Path -LiteralPath $OriginalWrapper).Path
$BridgeDll = (Resolve-Path -LiteralPath $BridgeDll).Path
$ProbeExe = (Resolve-Path -LiteralPath $ProbeExe).Path
if ([IO.Path]::GetExtension($OriginalWrapper) -ne '.dll') {
    throw "Expected an original VP4U DLL, got: $OriginalWrapper"
}

Write-Host '[D4XX] Loading the original VP4U without starting Unity...'
Write-Host "       Wrapper: $OriginalWrapper"
Write-Host "       Bridge : $BridgeDll"
$ProbeArguments = @($BridgeDll, $OriginalWrapper, '--load-only')
if ($LegacyHost) {
    $ProbeArguments += '--legacy-host'
    Write-Host '       Host   : Spice SDK without module aliases'
}
& $ProbeExe @ProbeArguments
if ($LASTEXITCODE -ne 0) {
    throw "Native D4xx bridge validation failed with exit code $LASTEXITCODE."
}

Write-Host '[OK] Original VP4U exports and required librealsense imports were found.'
Write-Host '     The game was not started and no camera stream was opened.'
