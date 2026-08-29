[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,
    [string] $MediaPipeRoot,
    [string] $OpenVrRoot,
    [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepositoryRoot 'build'
$BinRoot = Join-Path $BuildRoot 'bin'
$ManifestPath = Join-Path $BuildRoot 'build-manifest.json'
$CmakePath = Join-Path $RepositoryRoot 'CMakeLists.txt'
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $RepositoryRoot 'artifacts\release'
}
$Destination = [IO.Path]::GetFullPath($Destination)
$ArtifactPrefix = (Join-Path $RepositoryRoot 'artifacts').TrimEnd('\') + '\'
if (-not $Destination.StartsWith(
        $ArtifactPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Release staging destination must be inside this repository artifacts directory.'
}

$Cmake = Get-Content -LiteralPath $CmakePath -Raw
if ($Cmake -notmatch 'project\(dance_around_anygear VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Unable to read the project version from CMakeLists.txt.'
}
if ($Matches[1] -ne $Version) {
    throw "Tag version $Version does not match CMake project version $($Matches[1])."
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw 'Build manifest missing. Run tools/build.ps1 and tools/test.ps1 first.'
}
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($Manifest.configuration -ne 'Release') {
    throw "Release staging requires a Release build, found $($Manifest.configuration)."
}
if ([bool]$Manifest.gitDirty) {
    throw 'Release staging requires a build from a clean Git tree.'
}
$HeadCommit = (& git -C $RepositoryRoot rev-parse --verify HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($HeadCommit)) {
    throw 'Unable to resolve the source commit for the release.'
}
if ($Manifest.gitCommit -ne $HeadCommit) {
    throw "Build manifest commit $($Manifest.gitCommit) does not match HEAD $HeadCommit."
}
if ([bool](& git -C $RepositoryRoot status --porcelain)) {
    throw 'Release staging requires a clean Git tree.'
}

$Plugins = [ordered]@{
    kinect = 'dance_around_anygear_kinect.dll'
    webcam = 'dance_around_anygear_webcam.dll'
    steamvr = 'dance_around_anygear_steamvr.dll'
}
$ManifestOutputs = @{}
foreach ($Output in $Manifest.outputs) {
    $ManifestOutputs[[string]$Output.path] = [string]$Output.sha256
}
foreach ($PluginName in $Plugins.Values) {
    $RelativePath = "build/bin/$PluginName"
    $PluginPath = Join-Path $BinRoot $PluginName
    if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) {
        throw "Release DLL missing: $PluginPath"
    }
    if (-not $ManifestOutputs.ContainsKey($RelativePath)) {
        throw "Build manifest does not contain $RelativePath."
    }
    $ActualHash = (Get-FileHash -LiteralPath $PluginPath -Algorithm SHA256).Hash
    if ($ActualHash -ne $ManifestOutputs[$RelativePath]) {
        throw "Release DLL hash does not match build manifest: $PluginName"
    }
}

if (Test-Path -LiteralPath $Destination) {
    $ResolvedDestination = (Resolve-Path -LiteralPath $Destination).Path
    if (-not $ResolvedDestination.StartsWith(
            $ArtifactPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace release directory: $ResolvedDestination"
    }
    Remove-Item -LiteralPath $ResolvedDestination -Recurse -Force
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null

# Kinect has no project runtime dependencies, so it remains a directly loadable
# release asset. Backends with adjacent runtime directories are ZIP archives.
Copy-Item -LiteralPath (Join-Path $BinRoot $Plugins.kinect) `
    -Destination (Join-Path $Destination $Plugins.kinect)

$PackageArguments = @{
    Version = $Version
}
if (-not [string]::IsNullOrWhiteSpace($MediaPipeRoot)) {
    $PackageArguments.MediaPipeRoot = $MediaPipeRoot
}
& (Join-Path $PSScriptRoot 'package.ps1') @PackageArguments -Backend webcam
if (-not $?) {
    throw 'Webcam packaging failed.'
}

$PackageArguments = @{
    Version = $Version
}
if (-not [string]::IsNullOrWhiteSpace($OpenVrRoot)) {
    $PackageArguments.OpenVrRoot = $OpenVrRoot
}
& (Join-Path $PSScriptRoot 'package.ps1') @PackageArguments -Backend steamvr
if (-not $?) {
    throw 'SteamVR packaging failed.'
}

$DistRoot = Join-Path $RepositoryRoot 'dist'
$WebcamZipName = "dance-around-anygear-v$Version-webcam-win64.zip"
$SteamVrZipName = "dance-around-anygear-v$Version-steamvr-win64.zip"
Copy-Item -LiteralPath (Join-Path $DistRoot $WebcamZipName) `
    -Destination (Join-Path $Destination $WebcamZipName)
Copy-Item -LiteralPath (Join-Path $DistRoot $SteamVrZipName) `
    -Destination (Join-Path $Destination $SteamVrZipName)

$ReleaseManifestName = "dance-around-anygear-v$Version-build-manifest.json"
Copy-Item -LiteralPath $ManifestPath `
    -Destination (Join-Path $Destination $ReleaseManifestName)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$ExpectedEntries = [ordered]@{
    $WebcamZipName = @(
        "dance-around-anygear-v$Version-webcam-win64/dance_around_anygear_webcam.dll",
        "dance-around-anygear-v$Version-webcam-win64/dance_around_anygear_webcam/libmediapipe.dll",
        "dance-around-anygear-v$Version-webcam-win64/dance_around_anygear_webcam/pose_landmarker_lite.task",
        "dance-around-anygear-v$Version-webcam-win64/dance_around_anygear_webcam/LICENSE.mediapipe.txt",
        "dance-around-anygear-v$Version-webcam-win64/dance_around_anygear_webcam/NOTICE.mediapipe.txt"
    )
    $SteamVrZipName = @(
        "dance-around-anygear-v$Version-steamvr-win64/dance_around_anygear_steamvr.dll",
        "dance-around-anygear-v$Version-steamvr-win64/dance_around_anygear_steamvr/openvr_api.dll",
        "dance-around-anygear-v$Version-steamvr-win64/dance_around_anygear_steamvr/LICENSE.openvr.txt"
    )
}
foreach ($ZipName in $ExpectedEntries.Keys) {
    $Archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $Destination $ZipName))
    try {
        $EntryNames = @($Archive.Entries | ForEach-Object FullName)
        foreach ($ExpectedEntry in $ExpectedEntries[$ZipName]) {
            if ($ExpectedEntry -notin $EntryNames) {
                throw "Release archive $ZipName is missing $ExpectedEntry."
            }
        }
    }
    finally {
        $Archive.Dispose()
    }
}

$Assets = Get-ChildItem -LiteralPath $Destination -File | Sort-Object Name
$ChecksumLines = foreach ($Asset in $Assets) {
    $Hash = (Get-FileHash -LiteralPath $Asset.FullName -Algorithm SHA256).Hash
    "$Hash  $($Asset.Name)"
}
[IO.File]::WriteAllText(
    (Join-Path $Destination 'SHA256SUMS'),
    (($ChecksumLines -join "`n") + "`n"),
    [Text.Encoding]::ASCII)

Write-Host "[OK] Release assets staged: $Destination"
Get-ChildItem -LiteralPath $Destination -File | Sort-Object Name |
    ForEach-Object {
        Write-Host ("     {0}  {1}" -f
            (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,
            $_.Name)
    }
