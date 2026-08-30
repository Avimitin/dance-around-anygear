[CmdletBinding()]
# Experimental full replacement retained for A/B comparison. The default D4xx
# release uses tools/test-d4xx-native.ps1 and the original VisionPose pipeline.
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [string] $MediaPipeRoot,
    [ValidateRange(5, 120)]
    [int] $Seconds = 20,
    [ValidateRange(0, 31)]
    [int] $PrimaryDevice = 0,
    [ValidateRange(1, 2)]
    [int] $InfraredIndex = 1,
    [string] $OutputPath,
    [string] $Vp4uConfig
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Probe = Join-Path $RepositoryRoot 'build\bin\anygear_d4xx_probe.exe'
if (-not (Test-Path -LiteralPath $Probe -PathType Leaf)) {
    throw 'D4xx probe is absent. Run tools/build.ps1 first.'
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before taking exclusive access to both D4xx devices.'
}

$RealSenseRuntime = (Resolve-Path -LiteralPath $RealSenseRuntime).Path
if ([IO.Path]::GetFileName($RealSenseRuntime) -notin @('realsense2.dll', 'realsense2.dll.orig')) {
    throw "Expected an official librealsense runtime, got: $RealSenseRuntime"
}
if ([string]::IsNullOrWhiteSpace($MediaPipeRoot)) {
    $MediaPipeRoot = Join-Path $RepositoryRoot '.deps\mediapipe\v1.0.0\windows-x86_64'
}
$MediaPipeRoot = (Resolve-Path -LiteralPath $MediaPipeRoot).Path
$MediaPipeDll = Join-Path $MediaPipeRoot 'libmediapipe.dll'
$MediaPipeModel = Join-Path $MediaPipeRoot 'pose_landmarker_lite.task'
foreach ($Required in @($MediaPipeDll, $MediaPipeModel)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "MediaPipe dependency is absent: $Required"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputDirectory = Join-Path $RepositoryRoot 'build\d4xx-probe'
    $OutputPath = Join-Path $OutputDirectory 'infrared-preview.bmp'
} else {
    $OutputDirectory = Split-Path -Parent $OutputPath
}
if (-not [string]::IsNullOrWhiteSpace($OutputDirectory)) {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
}

Write-Host "[D4XX] Runtime: $RealSenseRuntime"
Write-Host "[D4XX] MediaPipe: $MediaPipeRoot"
Write-Host "[D4XX] Capturing IR + depth from two devices for $Seconds seconds..."
& $Probe $RealSenseRuntime $OutputPath $Seconds $MediaPipeRoot `
    $MediaPipeModel $PrimaryDevice $InfraredIndex
if ($LASTEXITCODE -ne 0) {
    throw "D4xx probe failed with exit code $LASTEXITCODE."
}

$OutputPath = (Resolve-Path -LiteralPath $OutputPath).Path
$OutputHash = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash
Write-Host "[OK] Preview: $OutputPath"
Write-Host "     SHA256: $OutputHash"

if (-not [string]::IsNullOrWhiteSpace($Vp4uConfig)) {
    $Vp4uConfig = (Resolve-Path -LiteralPath $Vp4uConfig).Path
    $StageRoot = Join-Path $RepositoryRoot 'build\d4xx-config-probe'
    $StageDependencies = Join-Path $StageRoot `
        'dance_around_anygear_d4xx_mediapipe_experimental'
    New-Item -ItemType Directory -Path $StageDependencies -Force | Out-Null
    $StagedPlugin = Join-Path $StageRoot `
        'dance_around_anygear_d4xx_mediapipe_experimental.dll'
    Copy-Item -LiteralPath `
        (Join-Path $RepositoryRoot `
            'build\bin\dance_around_anygear_d4xx_mediapipe_experimental.dll') `
        -Destination $StagedPlugin -Force
    Copy-Item -LiteralPath $RealSenseRuntime `
        -Destination (Join-Path $StageDependencies 'realsense2.dll') -Force
    Copy-Item -LiteralPath $MediaPipeDll -Destination $StageDependencies -Force
    Copy-Item -LiteralPath $MediaPipeModel -Destination $StageDependencies -Force
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot `
        'config\dance_around_anygear_d4xx_mediapipe_experimental.json') `
        -Destination (Join-Path $StageRoot `
            'dance_around_anygear_d4xx_mediapipe_experimental.json') -Force
    $Harness = Join-Path $RepositoryRoot 'build\bin\anygear_vp4u_harness.exe'
    Write-Host "[D4XX] Loading the cabinet JSON through the staged VP4U plugin..."
    & $Harness $StagedPlugin '--pose-init-config' $Vp4uConfig
    if ($LASTEXITCODE -ne 0) {
        throw "D4xx VP4U config probe failed with exit code $LASTEXITCODE."
    }
}
