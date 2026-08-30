[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CabinetRoot,
    [string] $OriginalWrapper,
    [string] $OfficialRealSense,
    [switch] $Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Close it before changing the local VP4U runtime.'
}

$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
$PluginRoot = Join-Path $CabinetRoot `
    'game\dancearound_data\plugins\x86_64'
$PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path
$ActiveWrapper = Join-Path $PluginRoot 'visionposewrapper.dll'
$ActiveRealSense = Join-Path $PluginRoot 'realsense2.dll'

if ([string]::IsNullOrWhiteSpace($OriginalWrapper)) {
    $OriginalWrapper = Join-Path $PluginRoot 'visionposewrapper.d435.dll'
}
if ([string]::IsNullOrWhiteSpace($OfficialRealSense)) {
    $OfficialRealSense = Join-Path $PluginRoot 'realsense2.dll.orig'
}
$OriginalWrapper = (Resolve-Path -LiteralPath $OriginalWrapper).Path
$OfficialRealSense = (Resolve-Path -LiteralPath $OfficialRealSense).Path

foreach ($Required in @(
    $ActiveWrapper, $ActiveRealSense, $OriginalWrapper, $OfficialRealSense)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required runtime file not found: $Required"
    }
}

$Changes = @(
    [pscustomobject]@{
        Name = 'visionposewrapper.dll'
        Source = $OriginalWrapper
        Destination = $ActiveWrapper
    },
    [pscustomobject]@{
        Name = 'realsense2.dll'
        Source = $OfficialRealSense
        Destination = $ActiveRealSense
    }
) | Where-Object {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_.Source).Hash -ne
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_.Destination).Hash
}

if ($Changes.Count -eq 0) {
    Write-Host '[OK] The local cabinet already uses the selected original VP4U runtime.'
    exit 0
}

Write-Host '[PLAN] Restore the local test cabinet to its original VP4U path:'
foreach ($Change in $Changes) {
    Write-Host "       $($Change.Name) <- $($Change.Source)"
}
if (-not $Apply) {
    Write-Host '[PLAN] No files changed. Re-run with -Apply after reviewing the paths.'
    exit 0
}

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$BackupRoot = Join-Path $CabinetRoot "anygear-backups\original-vp4u-$Stamp"
New-Item -ItemType Directory -Path $BackupRoot -Force | Out-Null

$Manifest = @()
foreach ($Change in $Changes) {
    $BeforeHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath $Change.Destination).Hash
    $SourceHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath $Change.Source).Hash
    $Backup = Join-Path $BackupRoot $Change.Name
    Copy-Item -LiteralPath $Change.Destination -Destination $Backup
    Copy-Item -LiteralPath $Change.Source `
        -Destination $Change.Destination -Force
    $InstalledHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath $Change.Destination).Hash
    if ($InstalledHash -ne $SourceHash) {
        Copy-Item -LiteralPath $Backup `
            -Destination $Change.Destination -Force
        throw "Hash verification failed; restored $($Change.Name) from backup."
    }
    $Manifest += [pscustomobject]@{
        File = $Change.Name
        PreviousSha256 = $BeforeHash
        InstalledSha256 = $InstalledHash
        Source = $Change.Source
    }
}

$ManifestPath = Join-Path $BackupRoot 'manifest.json'
$ManifestText = ($Manifest | ConvertTo-Json -Depth 3).TrimEnd() + "`n"
[IO.File]::WriteAllText(
    $ManifestPath, $ManifestText, [Text.UTF8Encoding]::new($false))

Write-Host '[OK] Original VP4U and official librealsense runtime activated.'
Write-Host "     Previous files are recoverable from: $BackupRoot"
