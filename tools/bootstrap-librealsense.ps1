[CmdletBinding()]
param(
    [string] $SourceArchive,
    [string] $UnityPackage,
    [switch] $Download
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DependencyRoot = Join-Path $RepositoryRoot '.deps\librealsense\v2.50.0'
$SourceCommit = 'c94410a420b74e5fb6a414bd12215c05ddd82b69'
$SourceName = "librealsense-$SourceCommit.tar.gz"
$SourceUrl = "https://codeload.github.com/IntelRealSense/librealsense/tar.gz/$SourceCommit"
$SourceBytes = 69300452
$SourceHash = '863B0491D8E72AF960E742F32A24F0E9BE9CF6BB735C435612F75FE6C6E5BE35'
$UnityName = 'Intel.RealSense.unitypackage'
$UnityUrl = 'https://github.com/realsenseai/librealsense/releases/download/v2.50.0/Intel.RealSense.unitypackage'
$UnityBytes = 11507297
$UnityHash = 'F94A6047A6DB845FE0C994591E28F73533A4E84A082507BB5D1FD79284CB1073'
$RuntimeAsset = '8a66d578b28d8a8478c91cc1c9aa34df/asset'
$RuntimeBytes = 31955456
$RuntimeHash = 'C8B83F041C1D92C264A0BFCDA5C9F28197ED212F7FAC40237DF63FFD2D5D1C4A'
$HeaderHash = '40AF91EB43922151C03CA2B191C012E8A0002B4C96433344F1B4AC25B7CEC453'
$LicenseHash = 'C7AA1FDF0E38C4827FEF17859DDBFAC800B8995F3EC875A06DD23C79135F956D'

function Assert-File {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][long] $Bytes,
        [Parameter(Mandatory)][string] $Sha256
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Pinned librealsense input is absent: $Path"
    }
    $Item = Get-Item -LiteralPath $Path
    $ActualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($Item.Length -ne $Bytes -or $ActualHash -ne $Sha256) {
        throw "librealsense input mismatch: $Path ($($Item.Length) bytes, $ActualHash)"
    }
}

function Get-PinnedFile {
    param(
        [string] $Provided,
        [string] $DefaultPath,
        [string] $Url,
        [long] $Bytes,
        [string] $Sha256
    )
    $Path = if ([string]::IsNullOrWhiteSpace($Provided)) {
        $DefaultPath
    } else {
        $Provided
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if (-not $Download -or -not [string]::IsNullOrWhiteSpace($Provided)) {
            throw "Pinned librealsense archive is absent: $Path"
        }
        $Partial = "$Path.partial"
        if (Test-Path -LiteralPath $Partial) {
            Remove-Item -LiteralPath $Partial -Force
        }
        Write-Host "[DOWNLOAD] $([IO.Path]::GetFileName($Path))"
        & curl.exe --fail --location --retry 3 --output $Partial $Url
        if ($LASTEXITCODE -ne 0) {
            throw "librealsense download failed with exit code $LASTEXITCODE."
        }
        Move-Item -LiteralPath $Partial -Destination $Path
    }
    $Path = (Resolve-Path -LiteralPath $Path).Path
    Assert-File -Path $Path -Bytes $Bytes -Sha256 $Sha256
    return $Path
}

if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) {
    throw 'Windows tar.exe is required to prepare librealsense.'
}
New-Item -ItemType Directory -Path $DependencyRoot -Force | Out-Null
$SourceArchive = Get-PinnedFile -Provided $SourceArchive `
    -DefaultPath (Join-Path $DependencyRoot $SourceName) `
    -Url $SourceUrl -Bytes $SourceBytes -Sha256 $SourceHash
$UnityPackage = Get-PinnedFile -Provided $UnityPackage `
    -DefaultPath (Join-Path $DependencyRoot $UnityName) `
    -Url $UnityUrl -Bytes $UnityBytes -Sha256 $UnityHash

$Staging = Join-Path $DependencyRoot '.extracting'
if (Test-Path -LiteralPath $Staging) {
    $Resolved = (Resolve-Path -LiteralPath $Staging).Path
    $Expected = $DependencyRoot.TrimEnd('\') + '\'
    if (-not $Resolved.StartsWith(
            $Expected, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace librealsense staging directory: $Resolved"
    }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}
New-Item -ItemType Directory -Path $Staging -Force | Out-Null
try {
    $SourceRoot = "librealsense-$SourceCommit"
    & tar.exe -xf $SourceArchive -C $Staging `
        "$SourceRoot/include" "$SourceRoot/LICENSE"
    if ($LASTEXITCODE -ne 0) {
        throw "librealsense source extraction failed with exit code $LASTEXITCODE."
    }
    & tar.exe -xf $UnityPackage -C $Staging $RuntimeAsset
    if ($LASTEXITCODE -ne 0) {
        throw "librealsense runtime extraction failed with exit code $LASTEXITCODE."
    }

    $Header = Join-Path $Staging "$SourceRoot\include\librealsense2\rs.h"
    $License = Join-Path $Staging "$SourceRoot\LICENSE"
    $Runtime = Join-Path $Staging $RuntimeAsset.Replace('/', '\')
    Assert-File -Path $Header -Bytes 5664 -Sha256 $HeaderHash
    Assert-File -Path $License -Bytes 11352 -Sha256 $LicenseHash
    Assert-File -Path $Runtime -Bytes $RuntimeBytes -Sha256 $RuntimeHash

    $IncludeDestination = Join-Path $DependencyRoot 'include'
    $RuntimeDestination = Join-Path $DependencyRoot 'windows-x86_64'
    foreach ($Target in @($IncludeDestination, $RuntimeDestination)) {
        if (Test-Path -LiteralPath $Target) {
            $Resolved = (Resolve-Path -LiteralPath $Target).Path
            $Expected = $DependencyRoot.TrimEnd('\') + '\'
            if (-not $Resolved.StartsWith(
                    $Expected, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to replace librealsense output: $Resolved"
            }
            Remove-Item -LiteralPath $Resolved -Recurse -Force
        }
    }
    Move-Item -LiteralPath (Join-Path $Staging "$SourceRoot\include") `
        -Destination $IncludeDestination
    New-Item -ItemType Directory -Path $RuntimeDestination -Force | Out-Null
    Move-Item -LiteralPath $Runtime `
        -Destination (Join-Path $RuntimeDestination 'realsense2.dll')
    Copy-Item -LiteralPath $License `
        -Destination (Join-Path $DependencyRoot 'LICENSE.librealsense.txt') -Force
}
finally {
    if (Test-Path -LiteralPath $Staging) {
        Remove-Item -LiteralPath $Staging -Recurse -Force
    }
}

$RuntimeOutput = Join-Path $DependencyRoot 'windows-x86_64\realsense2.dll'
Assert-File -Path $RuntimeOutput -Bytes $RuntimeBytes -Sha256 $RuntimeHash
Write-Host '[OK] librealsense 2.50.0 headers and official Windows runtime prepared.'
Write-Host "     Include: $(Join-Path $DependencyRoot 'include')"
Write-Host "     Runtime: $RuntimeOutput"
