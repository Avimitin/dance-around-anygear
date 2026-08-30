[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [ValidateRange(1, 1800)]
    [int] $Seconds = 60,
    [ValidateRange(0, 300)]
    [int] $EmptyStageSeconds = 5,
    [ValidateRange(0, 30)]
    [int] $WarmupSeconds = 2,
    [ValidateRange(1, 50)]
    [int] $MaxSyncDeltaMs = 17,
    [ValidateRange(0, 30)]
    [int] $CountdownSeconds = 5,
    [string] $OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($EmptyStageSeconds -gt $Seconds) {
    throw 'EmptyStageSeconds cannot exceed the complete capture duration.'
}

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepositoryRoot `
        'build\diagnostics\d4xx-kinect-teacher'
}
$Recorder = Join-Path $RepositoryRoot `
    'build\bin\anygear_d4xx_kinect_teacher_record.exe'
if (-not (Test-Path -LiteralPath $Recorder -PathType Leaf)) {
    throw 'The paired teacher recorder is absent. Run tools/build.ps1 first.'
}
if (-not (Test-Path -LiteralPath `
        "$env:SystemRoot\System32\Kinect10.dll" -PathType Leaf)) {
    throw 'Kinect for Windows SDK 1.8 runtime is not installed.'
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before recording the sensors.'
}
if (Get-Process -Name 'dance_around_anygear_spike_worker' `
        -ErrorAction SilentlyContinue) {
    throw 'The SPiKE worker is running. Stop it before recording the sensors.'
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

Write-Host '[TEACHER] Preparing synchronized depth/skeleton capture...'
Write-Host '          D4xx     : 2 x native 848x480 Z16, high-density, emitters on'
Write-Host '          Kinect   : native 320x240 depth + skeleton; no color frames'
Write-Host "          Duration : $Seconds seconds"
Write-Host "          Empty    : first $EmptyStageSeconds seconds"
Write-Host "          Sync gate: +/-$MaxSyncDeltaMs ms"
Write-Host "          Output   : $CaptureDirectory"
Write-Host '          Leave both cameras and the complete play area unobstructed.'
for ($Remaining = $CountdownSeconds; $Remaining -gt 0; $Remaining--) {
    Write-Host "          Opening sensors in $Remaining..."
    Start-Sleep -Seconds 1
}

& $Recorder $RealSenseRuntime $CaptureDirectory $Seconds `
    $EmptyStageSeconds $WarmupSeconds $MaxSyncDeltaMs
if ($LASTEXITCODE -ne 0) {
    throw "Paired teacher recorder failed with exit code $LASTEXITCODE."
}

$ManifestPath = Join-Path $CaptureDirectory 'manifest.json'
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw |
    ConvertFrom-Json
$ManifestHash = (Get-FileHash -Algorithm SHA256 `
    -LiteralPath $ManifestPath).Hash
$ValidPairs = [int64]$Manifest.synchronization.valid_teacher_pair_count
$AllPairs = [int64]$Manifest.synchronization.pair_count
$TeacherValid = [int64]$Manifest.teacher.valid_frame_count
$TeacherFrames = [int64]$Manifest.teacher.frame_count
$KinectDepthFrames = [int64]$Manifest.kinect_depth.frame_count

Write-Host '[OK] Paired research capture completed.'
Write-Host "     Teacher : $TeacherValid / $TeacherFrames valid skeleton frames"
Write-Host "     K-depth : $KinectDepthFrames native depth frames"
Write-Host "     Sync    : $ValidPairs / $AllPairs paired frames have a valid teacher"
Write-Host "     Manifest: $ManifestPath"
Write-Host "     SHA256  : $ManifestHash"
