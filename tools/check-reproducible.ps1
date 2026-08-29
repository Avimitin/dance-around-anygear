[CmdletBinding()]
param(
    [string] $ToolchainRoot = $env:ANYGEAR_TOOLCHAIN_ROOT,
    [string] $KinectSdkRoot = $env:ANYGEAR_KINECT_SDK_ROOT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RepositorySdk = Join-Path $RepositoryRoot '.deps\kinect\v1.8\sdk'
if ([string]::IsNullOrWhiteSpace($KinectSdkRoot) -and
    (Test-Path -LiteralPath $RepositorySdk -PathType Container)) {
    $KinectSdkRoot = $RepositorySdk
}
if ([string]::IsNullOrWhiteSpace($KinectSdkRoot)) {
    throw 'Kinect SDK headers are absent. Run tools/bootstrap-kinect-sdk.ps1 -Download first.'
}
$KinectSdkRoot = (Resolve-Path -LiteralPath $KinectSdkRoot).Path
& (Join-Path $PSScriptRoot 'bootstrap-kinect-sdk.ps1') -SourceRoot $KinectSdkRoot
if (-not $?) {
    throw 'Kinect SDK dependency verification failed.'
}
$OpenVrRoot = Join-Path $RepositoryRoot '.deps\openvr\v2.15.6'
if (-not (Test-Path -LiteralPath $OpenVrRoot -PathType Container)) {
    throw 'OpenVR SDK is absent. Run tools/bootstrap-openvr.ps1 -Download first.'
}
& (Join-Path $PSScriptRoot 'bootstrap-openvr.ps1') -SourceRoot $OpenVrRoot
if ($LASTEXITCODE -ne 0) {
    throw "OpenVR dependency verification failed with exit code $LASTEXITCODE."
}
$OpenVrRoot = (Resolve-Path -LiteralPath $OpenVrRoot).Path
if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    throw 'Supply -ToolchainRoot or set ANYGEAR_TOOLCHAIN_ROOT.'
}
$ToolchainRoot = (Resolve-Path -LiteralPath $ToolchainRoot).Path
$BinRoot = Join-Path $ToolchainRoot 'bin'
$CmakeExe = Join-Path $BinRoot 'cmake.exe'
$NinjaExe = Join-Path $BinRoot 'ninja.exe'
$GccExe = Join-Path $BinRoot 'gcc.exe'
$GxxExe = Join-Path $BinRoot 'g++.exe'
foreach ($required in @($CmakeExe, $NinjaExe, $GccExe, $GxxExe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required tool not found: $required"
    }
}
if ((& $GxxExe -dumpfullversion).Trim() -ne '16.2.0') {
    throw 'The reproducibility check requires the pinned GCC 16.2.0 toolchain.'
}

$CheckRoot = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'build\repro-check'))
$ExpectedPrefix = $RepositoryRoot.TrimEnd('\') + '\build\'
if (-not $CheckRoot.StartsWith($ExpectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unexpected reproducibility root: $CheckRoot"
}
if (Test-Path -LiteralPath $CheckRoot) {
    $ResolvedCheckRoot = (Resolve-Path -LiteralPath $CheckRoot).Path
    if (-not $ResolvedCheckRoot.StartsWith($ExpectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace a directory outside build: $ResolvedCheckRoot"
    }
    Remove-Item -LiteralPath $ResolvedCheckRoot -Recurse -Force
}

$HashSets = @()
foreach ($name in @('a', 'b')) {
    $BuildDirectory = Join-Path $CheckRoot $name
    $ConfigureArguments = @(
        '-S', $RepositoryRoot,
        '-B', $BuildDirectory,
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
        "-DCMAKE_C_COMPILER=$GccExe",
        "-DCMAKE_CXX_COMPILER=$GxxExe",
        '-DANYGEAR_BUILD_TESTS=OFF',
        '-DANYGEAR_BUILD_WEBCAM=ON',
        '-DANYGEAR_BUILD_STEAMVR=ON',
        "-DANYGEAR_KINECT_SDK_ROOT=$KinectSdkRoot",
        "-DANYGEAR_OPENVR_SDK_ROOT=$OpenVrRoot"
    )
    Write-Host "[REPRO $name] configure"
    & $CmakeExe @ConfigureArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake configure $name failed." }
    & $CmakeExe --build $BuildDirectory --target `
        anygear_kinect anygear_webcam anygear_steamvr
    if ($LASTEXITCODE -ne 0) { throw "CMake build $name failed." }
    $HashSets += ,([ordered]@{
        kinect = (Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'bin\dance_around_anygear_kinect.dll') -Algorithm SHA256).Hash
        webcam = (Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'bin\dance_around_anygear_webcam.dll') -Algorithm SHA256).Hash
        steamvr = (Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'bin\dance_around_anygear_steamvr.dll') -Algorithm SHA256).Hash
    })
}

foreach ($backend in @('kinect', 'webcam', 'steamvr')) {
    if ($HashSets[0][$backend] -ne $HashSets[1][$backend]) {
        throw "Reproducibility check failed for $backend`: $($HashSets[0][$backend]) != $($HashSets[1][$backend])"
    }
    Write-Host "[OK] $backend reproduced $($HashSets[0][$backend])"
}
