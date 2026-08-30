[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CaptureRoot,
    [string] $OutputRoot,
    [string] $ConfigPath,
    [string] $ModelPath,
    [string] $CalibrationPath,
    [ValidateRange(3, 10000)]
    [int] $MaximumCalibrationFrames = 300,
    [ValidateRange(8, 10000)]
    [int] $MaximumSurfaceFrames = 60,
    [ValidateRange(64, 10000)]
    [int] $SurfacePointsPerCloud = 1536,
    [ValidateRange(0.001, 1.0)]
    [double] $MaximumSurfaceP95M = 0.15,
    [ValidateRange(1, 4096)]
    [int] $ShardSize = 128,
    [ValidateRange(1, 10000000)]
    [int] $MinimumSamplesPerDevice = 30,
    [switch] $QualityOnly,
    [switch] $AllowQualityFail,
    [switch] $AllowPoorFit,
    [switch] $DisableSurfaceRefinement,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
$CaptureRoot = (Resolve-Path -LiteralPath $CaptureRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $CaptureRoot 'manifest.json') `
        -PathType Leaf)) {
    throw "Paired capture manifest is absent: $CaptureRoot"
}
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $RepositoryRoot `
        'config\dance_around_anygear_d4xx_spike.json'
}
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
if (-not $QualityOnly -and [string]::IsNullOrWhiteSpace($OutputRoot)) {
    $DatasetRoot = Join-Path $RepositoryRoot 'build\datasets\spike-teacher'
    New-Item -ItemType Directory -Path $DatasetRoot -Force | Out-Null
    $OutputRoot = Join-Path $DatasetRoot `
        ((Split-Path -Leaf $CaptureRoot) + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to restore the pinned SPiKE research environment.'
}
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups `
    --group calibration
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'

$Arguments = @(
    (Join-Path $PSScriptRoot 'prepare-spike-teacher-dataset.py'),
    $CaptureRoot,
    '--config', $ConfigPath,
    '--maximum-calibration-frames', [string]$MaximumCalibrationFrames,
    '--maximum-surface-frames', [string]$MaximumSurfaceFrames,
    '--surface-points-per-cloud', [string]$SurfacePointsPerCloud,
    '--maximum-surface-p95-m', $MaximumSurfaceP95M.ToString(
        [Globalization.CultureInfo]::InvariantCulture),
    '--shard-size', [string]$ShardSize,
    '--minimum-samples-per-device', [string]$MinimumSamplesPerDevice
)
if ($QualityOnly) {
    $Arguments += '--quality-only'
}
else {
    $Arguments += @('--output', $OutputRoot)
    if ([string]::IsNullOrWhiteSpace($CalibrationPath)) {
        if ([string]::IsNullOrWhiteSpace($ModelPath)) {
            $ModelPath = Join-Path $RepositoryRoot `
                '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\spike-itop-side-primary-fp16.onnx'
        }
        $ModelPath = (Resolve-Path -LiteralPath $ModelPath).Path
        $Lock = Get-Content -LiteralPath `
            (Join-Path $RepositoryRoot 'dependency-lock.json') -Raw |
            ConvertFrom-Json
        $ExpectedModel = $Lock.spikeD4xx.export
        $ModelItem = Get-Item -LiteralPath $ModelPath
        $ModelHash = (Get-FileHash -LiteralPath $ModelPath `
            -Algorithm SHA256).Hash
        if ($ModelItem.Length -ne [int64]$ExpectedModel.outputBytes -or `
            $ModelHash -ne [string]$ExpectedModel.outputSha256) {
            throw "Pinned SPiKE model mismatch: $ModelPath"
        }
        $Arguments += @('--model', $ModelPath)
    }
    else {
        $CalibrationPath = (Resolve-Path -LiteralPath $CalibrationPath).Path
        $Arguments += @('--calibration', $CalibrationPath)
    }
}
if ($AllowQualityFail) {
    $Arguments += '--allow-quality-fail'
}
if ($AllowPoorFit) {
    $Arguments += '--allow-poor-fit'
}
if ($DisableSurfaceRefinement) {
    $Arguments += '--disable-surface-refinement'
}

$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE teacher preparation failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}

if (-not $QualityOnly) {
    $ManifestPath = Join-Path $OutputRoot 'dataset-manifest.json'
    $ManifestHash = (Get-FileHash -LiteralPath $ManifestPath `
        -Algorithm SHA256).Hash
    Write-Host '[OK] Checked SPiKE teacher dataset is ready.'
    Write-Host "     Manifest: $ManifestPath"
    Write-Host "     SHA256 : $ManifestHash"
}
