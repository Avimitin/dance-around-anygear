[CmdletBinding(DefaultParameterSetName = 'Archive')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Archive')]
    [string] $Archive,
    [Parameter(Mandatory, ParameterSetName = 'Download')]
    [switch] $Download,
    [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ArchiveName = 'winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip'
$ArchiveUrl = 'https://sourceforge.net/projects/winlibs-mingw/files/16.2.0posix-14.0.0-ucrt-r1/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip/download'
$ExpectedHash = 'C1F52294597C0B73786B2A78EB5D176D89226D2F21875EAB75E783A8B1CEFCC4'

if ($Download) {
    $DownloadRoot = Join-Path $RepositoryRoot '.deps\downloads'
    New-Item -ItemType Directory -Path $DownloadRoot -Force | Out-Null
    $Archive = Join-Path $DownloadRoot $ArchiveName
    $CachedHash = if (Test-Path -LiteralPath $Archive -PathType Leaf) {
        (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
    } else { '' }
    if ($CachedHash -ne $ExpectedHash) {
        $TemporaryArchive = "$Archive.$([Guid]::NewGuid().ToString('N')).partial"
        try {
            Write-Host "[DOWNLOAD] $ArchiveUrl"
            Invoke-WebRequest -UseBasicParsing -Uri $ArchiveUrl -OutFile $TemporaryArchive
            $DownloadedHash = (Get-FileHash -LiteralPath $TemporaryArchive -Algorithm SHA256).Hash
            if ($DownloadedHash -ne $ExpectedHash) {
                throw "Downloaded toolchain hash mismatch. Expected $ExpectedHash, got $DownloadedHash."
            }
            Move-Item -LiteralPath $TemporaryArchive -Destination $Archive -Force
        }
        finally {
            Remove-Item -LiteralPath $TemporaryArchive -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "[CACHE] Reusing verified archive: $Archive"
    }
}

$Archive = (Resolve-Path -LiteralPath $Archive).Path
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $RepositoryRoot '.deps\toolchain'
}
$ActualHash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
if ($ActualHash -ne $ExpectedHash) {
    throw "Toolchain archive hash mismatch. Expected $ExpectedHash, got $ActualHash."
}

$DestinationFull = [IO.Path]::GetFullPath($Destination)
$RepositoryPrefix = $RepositoryRoot.TrimEnd('\') + '\'
if (-not $DestinationFull.StartsWith($RepositoryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The default bootstrap script only extracts inside this repository. Pass a repository-local destination.'
}
if (Test-Path -LiteralPath $DestinationFull) {
    Remove-Item -LiteralPath $DestinationFull -Recurse -Force
}
New-Item -ItemType Directory -Path $DestinationFull -Force | Out-Null
Expand-Archive -LiteralPath $Archive -DestinationPath $DestinationFull

$ToolchainRoot = Join-Path $DestinationFull 'mingw64'
$GxxExe = Join-Path $ToolchainRoot 'bin\g++.exe'
if (-not (Test-Path -LiteralPath $GxxExe -PathType Leaf)) {
    throw "Extracted archive does not contain the expected compiler: $GxxExe"
}
Write-Host "[OK] Toolchain verified and extracted: $ToolchainRoot"
Write-Host "     `$env:ANYGEAR_TOOLCHAIN_ROOT = '$ToolchainRoot'"
