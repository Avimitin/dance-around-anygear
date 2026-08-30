[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [ValidateRange(1, 300)]
    [int] $Seconds = 10,
    [ValidateRange(1, 8)]
    [int] $RequiredDevices = 2,
    [ValidateRange(1, 2)]
    [int] $InfraredIndex = 1,
    [ValidateRange(0, 30)]
    [int] $CountdownSeconds = 0,
    [ValidateRange(0, 30)]
    [int] $WarmupSeconds = 1,
    [ValidateSet('unchanged', 'all-on', 'all-off', 'first-only',
        'second-only', 'alternating')]
    [string] $EmitterMode = 'unchanged',
    [ValidateSet('infrared', 'native')]
    [string] $DepthCoordinate = 'infrared',
    [ValidateSet('unchanged', 'default', 'high-accuracy', 'high-density',
        'medium-density')]
    [string] $VisualPreset = 'unchanged',
    [string] $OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepositoryRoot 'build\diagnostics\d4xx-depth'
}
$Recorder = Join-Path $RepositoryRoot 'build\bin\anygear_d4xx_depth_record.exe'
if (-not (Test-Path -LiteralPath $Recorder -PathType Leaf)) {
    throw 'D4xx depth recorder is absent. Run tools/build.ps1 first.'
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before recording both D4xx devices.'
}

$RealSenseRuntime = (Resolve-Path -LiteralPath $RealSenseRuntime).Path
if ([IO.Path]::GetFileName($RealSenseRuntime) -notin @(
        'realsense2.dll', 'realsense2.dll.orig')) {
    throw "Expected an official librealsense runtime, got: $RealSenseRuntime"
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$CaptureName = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$CaptureDirectory = Join-Path $OutputRoot $CaptureName
if (Test-Path -LiteralPath $CaptureDirectory) {
    throw "Refusing to reuse capture directory: $CaptureDirectory"
}

Write-Host "[D4XX] Recording raw Z16 from $RequiredDevices device(s)..."
Write-Host "       Duration: $Seconds seconds"
Write-Host "       Emitter : $EmitterMode"
Write-Host "       Warm-up : $WarmupSeconds seconds"
Write-Host "       Depth   : $DepthCoordinate coordinates"
Write-Host "       Preset  : $VisualPreset"
Write-Host "       Output  : $CaptureDirectory"
for ($Remaining = $CountdownSeconds; $Remaining -gt 0; $Remaining--) {
    Write-Host "       Starting in $Remaining..."
    Start-Sleep -Seconds 1
}
& $Recorder $RealSenseRuntime $CaptureDirectory $Seconds `
    $RequiredDevices $InfraredIndex $EmitterMode $WarmupSeconds `
    $DepthCoordinate $VisualPreset
if ($LASTEXITCODE -ne 0) {
    throw "D4xx depth recorder failed with exit code $LASTEXITCODE."
}

$Manifest = Join-Path $CaptureDirectory 'manifest.json'
$ManifestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Manifest).Hash
Write-Host '[OK] Raw depth recording completed.'
Write-Host "     Manifest: $Manifest"
Write-Host "     SHA256 : $ManifestHash"
