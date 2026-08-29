[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [Alias('GameRoot')]
    [string] $CabinetRoot,
    [ValidateSet('kinect', 'webcam')]
    [string] $Backend = 'kinect',
    [string] $MediaPipeRoot,
    [string] $WindowSize,
    [string] $WindowPosition,
    [switch] $GenerateLauncher
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (@(Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'dancearound.exe is running. Stop it before installing Anygear files.'
}
$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
$SpiceExe = Join-Path $CabinetRoot 'game\dancearound.exe'
$VendorVp4u = Join-Path $CabinetRoot 'game\dancearound_data\plugins\x86_64\visionposewrapper.dll'
foreach ($required in @($SpiceExe, $VendorVp4u)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Expected game file not found: $required"
    }
}

$BinRoot = Join-Path $RepositoryRoot 'build\bin'
$PluginName = "dance_around_anygear_$Backend.dll"
$PluginSource = Join-Path $BinRoot $PluginName
foreach ($required in @($PluginSource)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Build output not found: $required"
    }
}

$MediaPipeSources = [ordered]@{}
if ($Backend -eq 'webcam') {
    if ([string]::IsNullOrWhiteSpace($MediaPipeRoot)) {
        $MediaPipeRoot = Join-Path $RepositoryRoot '.deps\mediapipe\v1.0.0\windows-x86_64'
    }
    $MediaPipeRoot = (Resolve-Path -LiteralPath $MediaPipeRoot).Path
    $DependencyHashes = [ordered]@{
        'libmediapipe.dll' = 'A8970C645C8C87C25EC9965CB5C898E803C6C42F7192B7DE9A0541C62AE48CEF'
        'pose_landmarker_lite.task' = '59929E1D1EE95287735DDD833B19CF4AC46D29BC7AFDDBBF6753C459690D574A'
        'LICENSE.mediapipe.txt' = '8707EEF0533987EFC5B155D64761EEB6E20793F50B9BD1A68DAD1CF4719D0ED8'
        'NOTICE.mediapipe.txt' = 'D3B4A80A24A01FD445D4B70A610FD836EC3547C3A62EB835A1041956C38D9F56'
    }
    foreach ($Name in $DependencyHashes.Keys) {
        $Source = Join-Path $MediaPipeRoot $Name
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "MediaPipe dependency not found: $Source"
        }
        $ActualHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        if ($ActualHash -ne $DependencyHashes[$Name]) {
            throw "MediaPipe dependency hash mismatch for $Name`: $ActualHash"
        }
        $MediaPipeSources[$Name] = $Source
    }
}

# All inputs have now been validated. Only start changing the cabinet tree
# after a missing or mismatched dependency can no longer leave a partial install.
Copy-Item -LiteralPath $PluginSource -Destination (Join-Path $CabinetRoot $PluginName) -Force
if ($Backend -eq 'webcam') {
    $DependencyDestination = Join-Path $CabinetRoot 'dance_around_anygear_webcam'
    New-Item -ItemType Directory -Path $DependencyDestination -Force | Out-Null
    foreach ($Name in $MediaPipeSources.Keys) {
        Copy-Item -LiteralPath $MediaPipeSources[$Name] `
            -Destination $DependencyDestination -Force
    }
}

Write-Host '[OK] Anygear installed without changing the vendor VP4U DLL.'
Write-Host "     Plugin : $(Join-Path $CabinetRoot $PluginName)"
Write-Host "     Spice argument: -k $PluginName"

if ($GenerateLauncher) {
    $LauncherArguments = @{
        CabinetRoot = $CabinetRoot
        Backend = $Backend
    }
    if (-not [string]::IsNullOrWhiteSpace($WindowSize)) {
        $LauncherArguments.WindowSize = $WindowSize
    }
    if (-not [string]::IsNullOrWhiteSpace($WindowPosition)) {
        $LauncherArguments.WindowPosition = $WindowPosition
    }
    & (Join-Path $PSScriptRoot 'generate-launcher.ps1') @LauncherArguments
}
