[CmdletBinding()]
param(
    [string] $SourceCheckpoint,
    [string] $Python,
    [string] $Uv = 'uv',
    [switch] $Download
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DependencyRoot = Join-Path $RepositoryRoot `
    '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1'
$ExpectedSourceBytes = 521315225
$ExpectedSourceHash =
    'FF29500C6747BB535476B7CAA5352F27CA62E97F5A748E59EDF646F7BC778E75'
$ExpectedRuntimeBytes = 143295911
$ExpectedRuntimeHash =
    'C48C40FF94C8F358762DB296DA0117DCFF78D8C4AD2FA4DB575298979BF2DA0D'
$DownloadUrl =
    'https://cloud.cvl.tuwien.ac.at/public.php/dav/files/ATCBp34rH3fGJ23'

New-Item -ItemType Directory -Path $DependencyRoot -Force | Out-Null
if ([string]::IsNullOrWhiteSpace($SourceCheckpoint)) {
    $SourceCheckpoint = Join-Path $DependencyRoot 'best_model.pth'
}
if (-not (Test-Path -LiteralPath $SourceCheckpoint -PathType Leaf)) {
    if (-not $Download) {
        throw 'SPiKE checkpoint is absent. Supply -SourceCheckpoint or use -Download.'
    }
    $Partial = "$SourceCheckpoint.partial"
    if (Test-Path -LiteralPath $Partial) {
        Remove-Item -LiteralPath $Partial -Force
    }
    Write-Host '[DOWNLOAD] Published SPiKE ITOP-SIDE checkpoint (497 MiB)...'
    & curl.exe --fail --location --retry 3 `
        --header 'X-Requested-With: XMLHttpRequest' `
        --output $Partial $DownloadUrl
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE checkpoint download failed with exit code $LASTEXITCODE."
    }
    Move-Item -LiteralPath $Partial -Destination $SourceCheckpoint
}
$SourceCheckpoint = (Resolve-Path -LiteralPath $SourceCheckpoint).Path
$Source = Get-Item -LiteralPath $SourceCheckpoint
$SourceHash = (Get-FileHash -LiteralPath $SourceCheckpoint `
    -Algorithm SHA256).Hash
if ($Source.Length -ne $ExpectedSourceBytes -or
    $SourceHash -ne $ExpectedSourceHash) {
    throw "SPiKE checkpoint mismatch: $($Source.Length) bytes, $SourceHash"
}

if ([string]::IsNullOrWhiteSpace($Python)) {
    if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
        throw 'uv is required to restore the locked SPiKE export environment.'
    }
    & $Uv sync --project (Join-Path $RepositoryRoot 'runtime\spike') `
        --frozen --no-default-groups --group export
    if ($LASTEXITCODE -ne 0) {
        throw "uv sync failed with exit code $LASTEXITCODE."
    }
    $Python = Join-Path $RepositoryRoot 'runtime\spike\.venv\Scripts\python.exe'
}
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw "SPiKE export Python is absent: $Python"
}
$RuntimeModel = Join-Path $DependencyRoot `
    'spike-itop-side-primary-fp16.onnx'
$Metadata = Join-Path $DependencyRoot `
    'spike-itop-side-primary-fp16.json'
& $Python (Join-Path $PSScriptRoot 'export-spike-onnx.py') `
    $SourceCheckpoint $RuntimeModel --metadata $Metadata
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE ONNX export failed with exit code $LASTEXITCODE."
}
$Runtime = Get-Item -LiteralPath $RuntimeModel
$RuntimeHash = (Get-FileHash -LiteralPath $RuntimeModel `
    -Algorithm SHA256).Hash
if ($Runtime.Length -ne $ExpectedRuntimeBytes -or
    $RuntimeHash -ne $ExpectedRuntimeHash) {
    throw "SPiKE runtime model mismatch: $($Runtime.Length) bytes, $RuntimeHash"
}
Write-Host '[OK] Published checkpoint verified and exported to fixed FP16 ONNX.'
Write-Host "     Runtime: $RuntimeModel"
Write-Host "     SHA-256: $RuntimeHash"
