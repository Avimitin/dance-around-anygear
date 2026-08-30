[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Destination,
    [switch] $Download
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Lock = Get-Content -LiteralPath `
    (Join-Path $RepositoryRoot 'dependency-lock.json') -Raw |
    ConvertFrom-Json
$Itop = $Lock.spikeD4xx.itopEvaluation
if ($null -eq $Itop) {
    throw 'The pinned ITOP evaluation inputs are absent from dependency-lock.json.'
}
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$Destination = (Resolve-Path -LiteralPath $Destination).Path

function Assert-File {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][int64] $Bytes,
        [Parameter(Mandatory)][string] $Sha256,
        [string] $Md5
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required ITOP file is absent: $Path"
    }
    $Item = Get-Item -LiteralPath $Path
    if ($Item.Length -ne $Bytes) {
        throw "ITOP file has the wrong size: $Path"
    }
    $ActualSha = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($ActualSha -ne $Sha256.ToUpperInvariant()) {
        throw "ITOP SHA-256 mismatch: $Path"
    }
    if (-not [string]::IsNullOrWhiteSpace($Md5)) {
        $ActualMd5 = (Get-FileHash -LiteralPath $Path -Algorithm MD5).Hash
        if ($ActualMd5 -ne $Md5.ToUpperInvariant()) {
            throw "ITOP MD5 mismatch: $Path"
        }
    }
}

foreach ($Property in $Itop.requiredArchives.PSObject.Properties) {
    $ArchiveName = $Property.Name
    $Expected = $Property.Value
    $ArchivePath = Join-Path $Destination $ArchiveName
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
        if (-not $Download) {
            throw "Download $($Expected.url) to $ArchivePath, or rerun with -Download."
        }
        $DownloadPath = $ArchivePath + '.' + [Guid]::NewGuid().ToString('N') + `
            '.partial'
        try {
            Write-Host "[DOWNLOAD] $ArchiveName"
            Invoke-WebRequest -UseBasicParsing -Uri ([string]$Expected.url) `
                -OutFile $DownloadPath
            Assert-File -Path $DownloadPath `
                -Bytes ([int64]$Expected.bytes) `
                -Sha256 ([string]$Expected.sha256) `
                -Md5 ([string]$Expected.md5)
            Move-Item -LiteralPath $DownloadPath -Destination $ArchivePath
        }
        finally {
            if (Test-Path -LiteralPath $DownloadPath -PathType Leaf) {
                Remove-Item -LiteralPath $DownloadPath -Force
            }
        }
    }
    Assert-File -Path $ArchivePath `
        -Bytes ([int64]$Expected.bytes) `
        -Sha256 ([string]$Expected.sha256) `
        -Md5 ([string]$Expected.md5)

    $ExpandedName = $ArchiveName.Substring(0, $ArchiveName.Length - 3)
    $ExpandedPath = Join-Path $Destination $ExpandedName
    if (-not (Test-Path -LiteralPath $ExpandedPath -PathType Leaf)) {
        $ExpandedPartial = $ExpandedPath + '.' + `
            [Guid]::NewGuid().ToString('N') + '.partial'
        $Input = $null
        $Gzip = $null
        $Output = $null
        try {
            Write-Host "[EXPAND] $ArchiveName"
            $Input = [IO.File]::OpenRead($ArchivePath)
            $Gzip = [IO.Compression.GZipStream]::new(
                $Input, [IO.Compression.CompressionMode]::Decompress)
            $Output = [IO.File]::Create($ExpandedPartial)
            $Gzip.CopyTo($Output)
            $Output.Dispose()
            $Output = $null
            $Gzip.Dispose()
            $Gzip = $null
            $Input.Dispose()
            $Input = $null
            Assert-File -Path $ExpandedPartial `
                -Bytes ([int64]$Expected.expandedBytes) `
                -Sha256 ([string]$Expected.expandedSha256)
            Move-Item -LiteralPath $ExpandedPartial `
                -Destination $ExpandedPath
        }
        finally {
            if ($null -ne $Output) { $Output.Dispose() }
            if ($null -ne $Gzip) { $Gzip.Dispose() }
            if ($null -ne $Input) { $Input.Dispose() }
            if (Test-Path -LiteralPath $ExpandedPartial -PathType Leaf) {
                Remove-Item -LiteralPath $ExpandedPartial -Force
            }
        }
    }
    Assert-File -Path $ExpandedPath `
        -Bytes ([int64]$Expected.expandedBytes) `
        -Sha256 ([string]$Expected.expandedSha256)
}

Write-Host '[OK] Checked ITOP side-view public evaluation files are ready.'
Write-Host "     Root: $Destination"
