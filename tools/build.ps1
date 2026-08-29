[CmdletBinding()]
param(
    [string] $ToolchainRoot = $env:ANYGEAR_TOOLCHAIN_ROOT,
    [string] $KinectSdkRoot = $env:ANYGEAR_KINECT_SDK_ROOT,
    [string] $OpenVrRoot = $env:ANYGEAR_OPENVR_SDK_ROOT,
    [string] $LibrealsenseIncludeDir = $env:ANYGEAR_LIBREALSENSE_INCLUDE_DIR,
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Clean,
    [switch] $AllowDifferentToolchain
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepositoryRoot 'build'

if ([string]::IsNullOrWhiteSpace($KinectSdkRoot)) {
    $RepositorySdk = Join-Path $RepositoryRoot '.deps\kinect\v1.8\sdk'
    if (Test-Path -LiteralPath $RepositorySdk -PathType Container) {
        $KinectSdkRoot = $RepositorySdk
    } elseif (-not [string]::IsNullOrWhiteSpace($env:KINECTSDK10_DIR)) {
        $KinectSdkRoot = $env:KINECTSDK10_DIR
    }
}
if ([string]::IsNullOrWhiteSpace($KinectSdkRoot) -or
    -not (Test-Path -LiteralPath $KinectSdkRoot -PathType Container)) {
    throw 'Kinect SDK headers are absent. Run tools/bootstrap-kinect-sdk.ps1 -Download first.'
}
$KinectSdkRoot = (Resolve-Path -LiteralPath $KinectSdkRoot).Path
& (Join-Path $PSScriptRoot 'bootstrap-kinect-sdk.ps1') -SourceRoot $KinectSdkRoot
if (-not $?) {
    throw 'Kinect SDK dependency verification failed.'
}

if ([string]::IsNullOrWhiteSpace($OpenVrRoot)) {
    $OpenVrRoot = Join-Path $RepositoryRoot '.deps\openvr\v2.15.6'
}
if (-not (Test-Path -LiteralPath $OpenVrRoot -PathType Container)) {
    throw 'OpenVR SDK is absent. Run tools/bootstrap-openvr.ps1 -Download first.'
}

if ([string]::IsNullOrWhiteSpace($LibrealsenseIncludeDir)) {
    $LibrealsenseIncludeDir = Join-Path $RepositoryRoot '.deps\librealsense\v2.50.0\include'
}
if (-not (Test-Path -LiteralPath (Join-Path $LibrealsenseIncludeDir 'librealsense2\rs.h') -PathType Leaf)) {
    throw 'librealsense 2.50.0 C headers are absent. Supply -LibrealsenseIncludeDir from the pinned source tree.'
}
$LibrealsenseIncludeDir = (Resolve-Path -LiteralPath $LibrealsenseIncludeDir).Path
$OpenVrRoot = (Resolve-Path -LiteralPath $OpenVrRoot).Path
& (Join-Path $PSScriptRoot 'bootstrap-openvr.ps1') -SourceRoot $OpenVrRoot
if ($LASTEXITCODE -ne 0) {
    throw "OpenVR dependency verification failed with exit code $LASTEXITCODE."
}

if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    throw 'Supply -ToolchainRoot or set ANYGEAR_TOOLCHAIN_ROOT to a WinLibs mingw64 directory.'
}
$ToolchainRoot = (Resolve-Path -LiteralPath $ToolchainRoot).Path
$BinRoot = Join-Path $ToolchainRoot 'bin'
$GccExe = Join-Path $BinRoot 'gcc.exe'
$GxxExe = Join-Path $BinRoot 'g++.exe'
$CmakeExe = Join-Path $BinRoot 'cmake.exe'
$NinjaExe = Join-Path $BinRoot 'ninja.exe'
$ObjdumpExe = Join-Path $BinRoot 'objdump.exe'

foreach ($required in @($GccExe, $GxxExe, $CmakeExe, $NinjaExe, $ObjdumpExe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required tool not found: $required"
    }
}

$CompilerVersion = (& $GxxExe -dumpfullversion).Trim()
if (-not $AllowDifferentToolchain -and $CompilerVersion -ne '16.2.0') {
    throw "Expected GCC 16.2.0, found $CompilerVersion. Use -AllowDifferentToolchain only for exploratory builds."
}

if ($Clean -and (Test-Path -LiteralPath $BuildRoot)) {
    $resolvedBuild = (Resolve-Path -LiteralPath $BuildRoot).Path
    $expectedPrefix = $RepositoryRoot.TrimEnd('\') + '\'
    if (-not $resolvedBuild.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a directory outside the repository: $resolvedBuild"
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null

$ConfigureArguments = @(
    '-S', $RepositoryRoot,
    '-B', $BuildRoot,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
    "-DCMAKE_C_COMPILER=$GccExe",
    "-DCMAKE_CXX_COMPILER=$GxxExe",
    '-DANYGEAR_BUILD_TESTS=ON',
    '-DANYGEAR_BUILD_WEBCAM=ON',
    '-DANYGEAR_BUILD_STEAMVR=ON',
    '-DANYGEAR_BUILD_D4XX=ON',
    "-DANYGEAR_KINECT_SDK_ROOT=$KinectSdkRoot",
    "-DANYGEAR_OPENVR_SDK_ROOT=$OpenVrRoot",
    "-DANYGEAR_LIBREALSENSE_INCLUDE_DIR=$LibrealsenseIncludeDir"
)

Write-Host "[CONFIGURE] GCC $CompilerVersion, $Configuration"
& $CmakeExe @ConfigureArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

Write-Host '[BUILD] Building monolithic Anygear backends and harnesses...'
& $CmakeExe --build $BuildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$BinOutput = Join-Path $BuildRoot 'bin'
$ExpectedOutputs = @(
    (Join-Path $BinOutput 'dance_around_anygear_kinect.dll'),
    (Join-Path $BinOutput 'dance_around_anygear_webcam.dll'),
    (Join-Path $BinOutput 'dance_around_anygear_steamvr.dll'),
    (Join-Path $BinOutput 'dance_around_anygear_d4xx.dll'),
    (Join-Path $BinOutput 'anygear_loader_smoke.exe'),
    (Join-Path $BinOutput 'anygear_vp4u_harness.exe'),
    (Join-Path $BinOutput 'anygear_kinect_probe.exe'),
    (Join-Path $BinOutput 'anygear_webcam_probe.exe'),
    (Join-Path $BinOutput 'anygear_mediapipe_probe.exe'),
    (Join-Path $BinOutput 'anygear_steamvr_pose_test.exe'),
    (Join-Path $BinOutput 'anygear_steamvr_probe.exe'),
    (Join-Path $BinOutput 'anygear_d4xx_probe.exe')
)
foreach ($output in $ExpectedOutputs) {
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "Expected build output is missing: $output"
    }
}

$PluginDlls = @(
    (Join-Path $BinOutput 'dance_around_anygear_kinect.dll'),
    (Join-Path $BinOutput 'dance_around_anygear_webcam.dll'),
    (Join-Path $BinOutput 'dance_around_anygear_steamvr.dll'),
    (Join-Path $BinOutput 'dance_around_anygear_d4xx.dll')
)
foreach ($PluginDll in $PluginDlls) {
    $PluginExports = (& $ObjdumpExe -p $PluginDll | Out-String)
    foreach ($name in @(
        'vp4uGetVersion', 'vp4uPreboot', 'vp4uShutdown', 'vp4uGetApiTable',
        'spice_sdk_entry_point', 'danceAroundAnygearGetBuildInfo')) {
        if (-not $PluginExports.Contains($name)) {
            throw "Required export is missing from $PluginDll`: $name"
        }
    }
    $PluginSections = (& $ObjdumpExe -h $PluginDll | Out-String)
    foreach ($section in @('.debug_info', '.debug_line')) {
        if (-not $PluginSections.Contains($section)) {
            throw "Embedded debug section is missing from $PluginDll`: $section"
        }
    }
}

$GitCommit = 'uncommitted'
$PreviousErrorActionPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'SilentlyContinue'
    $CandidateCommit = (& git -C $RepositoryRoot rev-parse --verify HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($CandidateCommit)) {
        $GitCommit = $CandidateCommit.Trim()
    }
}
finally {
    $ErrorActionPreference = $PreviousErrorActionPreference
}
$GitDirty = [bool](& git -C $RepositoryRoot status --porcelain)
$Outputs = foreach ($output in $ExpectedOutputs) {
    $item = Get-Item -LiteralPath $output
    [ordered]@{
        path = $item.FullName.Substring($RepositoryRoot.Length + 1).Replace('\', '/')
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    }
}
$Manifest = [ordered]@{
    schemaVersion = 1
    configuration = $Configuration
    gitCommit = [string]$GitCommit
    gitDirty = $GitDirty
    compiler = (& $GxxExe --version | Select-Object -First 1)
    cmake = (& $CmakeExe --version | Select-Object -First 1)
    ninja = (& $NinjaExe --version | Select-Object -First 1)
    outputs = @($Outputs)
}
$ManifestPath = Join-Path $BuildRoot 'build-manifest.json'
$Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8

Write-Host "[OK] Build manifest: $ManifestPath"
$Outputs | ForEach-Object { Write-Host ("     {0}  {1}" -f $_.sha256, $_.path) }
