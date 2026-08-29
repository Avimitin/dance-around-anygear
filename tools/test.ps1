[CmdletBinding()]
param(
    [string] $ToolchainRoot = $env:ANYGEAR_TOOLCHAIN_ROOT,
    [switch] $SkipBuild,
    [switch] $HardwareKinect,
    [switch] $MediaPipe,
    [switch] $HardwareWebcam,
    [string] $MediaPipeRoot,
    [string] $MediaPipeImage,
    [string] $ConfigPath,
    [ValidateRange(0, 31)]
    [int] $WebcamIndex = 0,
    [ValidateRange(5, 120)]
    [int] $Seconds = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepositoryRoot 'build'
$BinRoot = Join-Path $BuildRoot 'bin'
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -ToolchainRoot $ToolchainRoot
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed with exit code $LASTEXITCODE."
    }
}

$StageRoot = Join-Path $BuildRoot 'test-runtime'
if (Test-Path -LiteralPath $StageRoot) {
    $resolvedStage = (Resolve-Path -LiteralPath $StageRoot).Path
    $expectedPrefix = $BuildRoot.TrimEnd('\') + '\'
    if (-not $resolvedStage.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace unexpected test directory: $resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null

$KinectPlugin = Join-Path $BinRoot 'dance_around_anygear_kinect.dll'
$WebcamPlugin = Join-Path $BinRoot 'dance_around_anygear_webcam.dll'
$LoaderSmoke = Join-Path $BinRoot 'anygear_loader_smoke.exe'
$Harness = Join-Path $BinRoot 'anygear_vp4u_harness.exe'
$KinectProbe = Join-Path $BinRoot 'anygear_kinect_probe.exe'
$WebcamProbe = Join-Path $BinRoot 'anygear_webcam_probe.exe'
$MediaPipeProbe = Join-Path $BinRoot 'anygear_mediapipe_probe.exe'
foreach ($required in @(
    $KinectPlugin, $WebcamPlugin, $LoaderSmoke, $Harness, $KinectProbe,
    $WebcamProbe, $MediaPipeProbe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required build output not found: $required"
    }
}
Copy-Item -LiteralPath $KinectPlugin -Destination $StageRoot
Copy-Item -LiteralPath $WebcamPlugin -Destination $StageRoot
$StagedKinectPlugin = Join-Path $StageRoot 'dance_around_anygear_kinect.dll'
$StagedWebcamPlugin = Join-Path $StageRoot 'dance_around_anygear_webcam.dll'

foreach ($entry in @(
    @{ Name = 'Kinect'; Path = $StagedKinectPlugin },
    @{ Name = 'MediaPipe webcam'; Path = $StagedWebcamPlugin })) {
    Write-Host "[TEST] $($entry.Name) Spice loader fallback..."
    & $LoaderSmoke $entry.Path
    if ($LASTEXITCODE -ne 0) {
        throw "$($entry.Name) loader smoke test failed with exit code $LASTEXITCODE."
    }
    Write-Host "[TEST] $($entry.Name) Spice SDK library hook..."
    & $LoaderSmoke $entry.Path '--sdk-hook'
    if ($LASTEXITCODE -ne 0) {
        throw "$($entry.Name) SDK loader test failed with exit code $LASTEXITCODE."
    }
    Write-Host "[TEST] $($entry.Name) VP4U ABI table..."
    & $Harness $entry.Path '--abi-only'
    if ($LASTEXITCODE -ne 0) {
        throw "$($entry.Name) VP4U ABI test failed with exit code $LASTEXITCODE."
    }
}

if ($MediaPipe -or $HardwareWebcam) {
    if ([string]::IsNullOrWhiteSpace($MediaPipeRoot)) {
        $MediaPipeRoot = Join-Path $RepositoryRoot '.deps\mediapipe\v1.0.0\windows-x86_64'
    }
    $MediaPipeRoot = (Resolve-Path -LiteralPath $MediaPipeRoot).Path
    $MediaPipeDll = Join-Path $MediaPipeRoot 'libmediapipe.dll'
    $MediaPipeModel = Join-Path $MediaPipeRoot 'pose_landmarker_lite.task'
    $MediaPipeHashes = [ordered]@{
        $MediaPipeDll = 'A8970C645C8C87C25EC9965CB5C898E803C6C42F7192B7DE9A0541C62AE48CEF'
        $MediaPipeModel = '59929E1D1EE95287735DDD833B19CF4AC46D29BC7AFDDBBF6753C459690D574A'
    }
    foreach ($required in $MediaPipeHashes.Keys) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "MediaPipe runtime file not found: $required"
        }
        $ActualHash = (Get-FileHash -LiteralPath $required -Algorithm SHA256).Hash
        if ($ActualHash -ne $MediaPipeHashes[$required]) {
            throw "MediaPipe runtime hash mismatch: $required ($ActualHash)"
        }
    }
    $StagedDependencies = Join-Path $StageRoot 'dance_around_anygear_webcam'
    Copy-Item -LiteralPath $MediaPipeRoot -Destination $StagedDependencies -Recurse
}

if ($MediaPipe) {
    if ([string]::IsNullOrWhiteSpace($MediaPipeImage)) {
        $MediaPipeImage = Join-Path $RepositoryRoot '.deps\mediapipe\testdata\pose.jpg'
    }
    $MediaPipeImage = (Resolve-Path -LiteralPath $MediaPipeImage).Path
    Write-Host '[TEST] Staged webcam plugin resolves its own MediaPipe dependencies...'
    & $Harness $StagedWebcamPlugin '--pose-init-only'
    if ($LASTEXITCODE -ne 0) {
        throw "Staged MediaPipe initialization failed with exit code $LASTEXITCODE."
    }
    Write-Host '[TEST] MediaPipe official pose image through Anygear adapter...'
    & $MediaPipeProbe $MediaPipeRoot $MediaPipeModel $MediaPipeImage
    if ($LASTEXITCODE -ne 0) {
        throw "MediaPipe pose probe failed with exit code $LASTEXITCODE."
    }
}

if ($HardwareKinect) {
    if (-not (Test-Path -LiteralPath "$env:SystemRoot\System32\Kinect10.dll" -PathType Leaf)) {
        throw 'Kinect for Windows SDK 1.8 runtime is not installed.'
    }
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        throw 'Supply -ConfigPath for a hardware Kinect test.'
    }
    $ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
    $saved = @{}
    foreach ($name in @('VP4U_BACKEND', 'VP4U_CAPTURE_FPS', 'VP4U_PREVIEW_FPS', 'VP4U_IMAGE_POOL_MAX', 'VP4U_CAMERA_INDEX')) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        $env:VP4U_BACKEND = 'kinect'
        $env:VP4U_CAPTURE_FPS = '30'
        $env:VP4U_PREVIEW_FPS = '15'
        $env:VP4U_IMAGE_POOL_MAX = '12'
        Write-Host "[TEST] Kinect skeleton stream probe for $Seconds seconds..."
        & $KinectProbe $Seconds
        if ($LASTEXITCODE -ne 0) {
            throw "Kinect stream probe failed with exit code $LASTEXITCODE."
        }
        Write-Host "[TEST] Kinect hardware harness for $Seconds seconds..."
        & $Harness $StagedKinectPlugin $ConfigPath $Seconds
        if ($LASTEXITCODE -ne 0) {
            throw "Kinect hardware test failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        foreach ($name in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
        }
    }
}

if ($HardwareWebcam) {
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        $ConfigPath = Join-Path $StageRoot 'webcam-test-config.json'
        @{ Vp4uWrap = @{ CameraIndex = $WebcamIndex } } |
            ConvertTo-Json -Depth 3 |
            Set-Content -LiteralPath $ConfigPath -Encoding UTF8
    }
    $ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
    $saved = @{}
    foreach ($name in @('VP4U_BACKEND', 'VP4U_CAPTURE_FPS', 'VP4U_PREVIEW_FPS', 'VP4U_IMAGE_POOL_MAX', 'VP4U_CAMERA_INDEX')) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        $env:VP4U_BACKEND = 'webcam'
        $env:VP4U_CAPTURE_FPS = '30'
        $env:VP4U_PREVIEW_FPS = '15'
        $env:VP4U_IMAGE_POOL_MAX = '12'
        $env:VP4U_CAMERA_INDEX = [string]$WebcamIndex
        Write-Host "[TEST] USB webcam capture probe for $Seconds seconds..."
        & $WebcamProbe $WebcamIndex $Seconds
        if ($LASTEXITCODE -ne 0) {
            throw "USB webcam capture probe failed with exit code $LASTEXITCODE."
        }
        Write-Host "[TEST] USB webcam VP4U harness for $Seconds seconds..."
        & $Harness $StagedWebcamPlugin $ConfigPath $Seconds
        if ($LASTEXITCODE -ne 0) {
            throw "USB webcam test failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        foreach ($name in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
        }
    }
}

Write-Host '[OK] All requested tests passed.'
