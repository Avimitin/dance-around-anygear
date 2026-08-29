[CmdletBinding(DefaultParameterSetName = 'Download')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Download')]
    [switch] $Download,
    [Parameter(Mandatory, ParameterSetName = 'Offline')]
    [string] $Installer,
    [Parameter(Mandatory, ParameterSetName = 'Verify')]
    [string] $SourceRoot,
    [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DownloadRoot = Join-Path $RepositoryRoot '.deps\downloads'
$DependencyRoot = Join-Path $RepositoryRoot '.deps\kinect\v1.8'
$InstallerSpec = [ordered]@{
    Name = 'KinectSDK-v1.8-Setup.exe'
    Url = 'https://download.microsoft.com/download/E/1/D/E1DEC243-0389-4A23-87BF-F47DE869FC1A/KinectSDK-v1.8-Setup.exe'
    Bytes = 233219096
    Sha256 = '0574651D5576EBD332307DF57C44922F85C569416CD0D75B59D1F99BBE8D1B53'
}
$SdkMsiSpec = [ordered]@{
    Name = 'KinectSDK-v1.8-x64.msi'
    Bytes = 1376256
    Sha256 = 'EE14D090A326298E4FA68CF309092B2AA78FEB6E908AE3132547F2B85A5E6E9D'
}
$AttachedContainerSpec = [ordered]@{
    Offset = 705168
    Bytes = 232504829
    Sha256 = '569F2B5A7239AD020544C33C3DA97BB940CD37783EAFA857921A7D582290EBB7'
}
$HeaderHashes = [ordered]@{
    'NuiApi.h' = '9664880CC330CD2CFA23DEC9BFF823789543098F8631F5F70F0924D7B3F17997'
    'NuiImageCamera.h' = 'E0D60CC5A67075767C9D433B3048E85600E7581C2C8FC2ECE40894AC45DE2E5D'
    'NuiSensor.h' = '162766BC54A6DB7BE9E13B9DFDE22625A73B1B8D4160A7D434BEA45E9338CFFA'
    'NuiSkeleton.h' = '108A9A5118752FBB186ECAB92058D9374ACFA75B7E3AAE9EAAF4DD1203AFFFB2'
}

function Assert-File {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][long] $Bytes,
        [Parameter(Mandatory)][string] $Hash,
        [ValidateSet('SHA1', 'SHA256')][string] $Algorithm = 'SHA256'
    )

    $Resolved = (Resolve-Path -LiteralPath $Path).Path
    $Item = Get-Item -LiteralPath $Resolved
    if ($Item.Length -ne $Bytes) {
        throw "File size mismatch for $Resolved. Expected $Bytes, got $($Item.Length)."
    }
    $ActualHash = (Get-FileHash -LiteralPath $Resolved -Algorithm $Algorithm).Hash
    if ($ActualHash -ne $Hash) {
        throw "$Algorithm mismatch for $Resolved. Expected $Hash, got $ActualHash."
    }
    return $Resolved
}

function Assert-SdkHeaders {
    param([Parameter(Mandatory)][string] $SdkRoot)

    $IncludeRoot = Join-Path $SdkRoot 'inc'
    foreach ($Name in $HeaderHashes.Keys) {
        $Path = Join-Path $IncludeRoot $Name
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "Kinect SDK header is missing: $Path"
        }
        $ActualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        if ($ActualHash -ne $HeaderHashes[$Name]) {
            throw "Kinect SDK header hash mismatch for $Name`: $ActualHash"
        }
    }
    return (Resolve-Path -LiteralPath $SdkRoot).Path
}

function Copy-VerifiedSlice {
    param(
        [Parameter(Mandatory)][string] $Source,
        [Parameter(Mandatory)][string] $Destination,
        [Parameter(Mandatory)][long] $Offset,
        [Parameter(Mandatory)][long] $Bytes,
        [Parameter(Mandatory)][string] $Sha256
    )

    $InputStream = [IO.File]::OpenRead($Source)
    $OutputStream = [IO.File]::Create($Destination)
    try {
        $InputStream.Position = $Offset
        $Remaining = $Bytes
        $Buffer = New-Object byte[] 1048576
        while ($Remaining -gt 0) {
            $Requested = [Math]::Min([long]$Buffer.Length, $Remaining)
            $Read = $InputStream.Read($Buffer, 0, [int]$Requested)
            if ($Read -le 0) {
                throw 'Unexpected end of the verified Kinect SDK bundle.'
            }
            $OutputStream.Write($Buffer, 0, $Read)
            $Remaining -= $Read
        }
    }
    finally {
        $OutputStream.Dispose()
        $InputStream.Dispose()
    }
    Assert-File -Path $Destination -Bytes $Bytes -Hash $Sha256 | Out-Null
}

if (-not [string]::IsNullOrWhiteSpace($SourceRoot)) {
    $VerifiedRoot = Assert-SdkHeaders -SdkRoot $SourceRoot
    Write-Host "[OK] Pinned Kinect SDK 1.8 headers verified: $VerifiedRoot"
    return
}

if ($Download) {
    New-Item -ItemType Directory -Path $DownloadRoot -Force | Out-Null
    $Installer = Join-Path $DownloadRoot $InstallerSpec.Name
    $CurrentHash = if (Test-Path -LiteralPath $Installer -PathType Leaf) {
        (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash
    } else { '' }
    if ($CurrentHash -ne $InstallerSpec.Sha256) {
        $TemporaryInstaller = "$Installer.$([Guid]::NewGuid().ToString('N')).partial"
        try {
            Write-Host "[DOWNLOAD] $($InstallerSpec.Url)"
            Invoke-WebRequest -UseBasicParsing -Uri $InstallerSpec.Url `
                -OutFile $TemporaryInstaller
            Assert-File -Path $TemporaryInstaller -Bytes $InstallerSpec.Bytes `
                -Hash $InstallerSpec.Sha256 | Out-Null
            Move-Item -LiteralPath $TemporaryInstaller -Destination $Installer -Force
        }
        finally {
            Remove-Item -LiteralPath $TemporaryInstaller -Force `
                -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "[CACHE] $Installer"
    }
}

$Installer = Assert-File -Path $Installer -Bytes $InstallerSpec.Bytes `
    -Hash $InstallerSpec.Sha256
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $DependencyRoot 'sdk'
}
$Destination = [IO.Path]::GetFullPath($Destination)
$DependencyPrefix = (Join-Path $RepositoryRoot '.deps').TrimEnd('\') + '\'
if (-not $Destination.StartsWith(
        $DependencyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Kinect SDK bootstrap destination must be inside this repository .deps directory.'
}

if (Test-Path -LiteralPath $Destination -PathType Container) {
    try {
        $VerifiedRoot = Assert-SdkHeaders -SdkRoot $Destination
        Write-Host "[OK] Pinned Kinect SDK 1.8 headers verified: $VerifiedRoot"
        return
    }
    catch {
        Write-Host "[REBUILD] Existing Kinect SDK cache is incomplete: $($_.Exception.Message)"
    }
}

New-Item -ItemType Directory -Path $DependencyRoot -Force | Out-Null
$SevenZip = @(
    (Join-Path $env:ProgramFiles '7-Zip\7z.exe'),
    (Join-Path ${env:ProgramFiles(x86)} '7-Zip\7z.exe')
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and
    (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if (-not $SevenZip) {
    throw '7-Zip is required to read the verified Kinect SDK bundle without installing it.'
}
$TemporaryRoot = Join-Path $DependencyRoot `
    ('.bootstrap-' + [Guid]::NewGuid().ToString('N'))
$AttachedContainer = Join-Path $TemporaryRoot 'attached.cab'
$PayloadRoot = Join-Path $TemporaryRoot 'payload'
$MsiRoot = Join-Path $TemporaryRoot 'msi'
$PreparedSdkRoot = Join-Path $TemporaryRoot 'sdk'
try {
    New-Item -ItemType Directory -Path $PayloadRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $MsiRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $PreparedSdkRoot 'inc') `
        -Force | Out-Null

    Write-Host '[EXTRACT] Reading the verified Microsoft SDK bundle...'
    Copy-VerifiedSlice -Source $Installer -Destination $AttachedContainer `
        -Offset $AttachedContainerSpec.Offset -Bytes $AttachedContainerSpec.Bytes `
        -Sha256 $AttachedContainerSpec.Sha256
    & $SevenZip x $AttachedContainer "-o$PayloadRoot" a2 -y | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Kinect SDK payload extraction failed with exit code $LASTEXITCODE."
    }
    $SdkMsi = Join-Path $PayloadRoot 'a2'
    Assert-File -Path $SdkMsi -Bytes $SdkMsiSpec.Bytes `
        -Hash $SdkMsiSpec.Sha256 | Out-Null
    & $SevenZip x $SdkMsi "-o$MsiRoot" -y | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Kinect SDK header extraction failed with exit code $LASTEXITCODE."
    }

    $ExtractedFiles = @(Get-ChildItem -LiteralPath $MsiRoot -Recurse -File)
    foreach ($Name in $HeaderHashes.Keys) {
        $Matches = @($ExtractedFiles | Where-Object {
            (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq
                $HeaderHashes[$Name]
        })
        if ($Matches.Count -ne 1) {
            throw "Expected exactly one verified $Name payload; found $($Matches.Count)."
        }
        Copy-Item -LiteralPath $Matches[0].FullName -Destination `
            (Join-Path (Join-Path $PreparedSdkRoot 'inc') $Name)
    }
    Assert-SdkHeaders -SdkRoot $PreparedSdkRoot | Out-Null

    if (Test-Path -LiteralPath $Destination) {
        $ResolvedDestination = (Resolve-Path -LiteralPath $Destination).Path
        if (-not $ResolvedDestination.StartsWith(
                $DependencyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to replace dependency directory: $ResolvedDestination"
        }
        Remove-Item -LiteralPath $ResolvedDestination -Recurse -Force
    }
    Move-Item -LiteralPath $PreparedSdkRoot -Destination $Destination
}
finally {
    if (Test-Path -LiteralPath $TemporaryRoot) {
        $ResolvedTemporary = (Resolve-Path -LiteralPath $TemporaryRoot).Path
        $ExpectedPrefix = $DependencyRoot.TrimEnd('\') + '\'
        if (-not $ResolvedTemporary.StartsWith(
                $ExpectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove an unexpected temporary path: $ResolvedTemporary"
        }
        Remove-Item -LiteralPath $ResolvedTemporary -Recurse -Force
    }
}

$VerifiedRoot = Assert-SdkHeaders -SdkRoot $Destination
Write-Host "[OK] Pinned Kinect SDK 1.8 headers prepared: $VerifiedRoot"
Write-Host "     `$env:ANYGEAR_KINECT_SDK_ROOT = '$VerifiedRoot'"
