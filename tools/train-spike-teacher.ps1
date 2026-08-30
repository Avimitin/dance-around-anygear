[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]] $TrainManifest,
    [Parameter(Mandatory)]
    [string[]] $ValidationManifest,
    [string] $Checkpoint,
    [string] $OutputRoot,
    [string] $Resume,
    [ValidateRange(1, 1000)]
    [int] $Epochs = 20,
    [ValidateRange(1, 64)]
    [int] $BatchSize = 1,
    [ValidateRange(0, 32)]
    [int] $Workers = 0,
    [ValidateRange(1.0e-7, 1.0)]
    [double] $LearningRate = 1.0e-4,
    [ValidateSet('cuda', 'cpu')]
    [string] $Device = 'cuda',
    [switch] $SkipHashCheck,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ResearchRoot = Join-Path $RepositoryRoot 'research\spike'
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before GPU fine-tuning.'
}
if (Get-Process -Name 'dance_around_anygear_spike_worker' `
        -ErrorAction SilentlyContinue) {
    throw 'The realtime SPiKE worker is running. Stop it before fine-tuning.'
}
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to restore the pinned SPiKE training environment.'
}

$ResolvedTrain = @($TrainManifest | ForEach-Object {
    (Resolve-Path -LiteralPath $_).Path
})
$ResolvedValidation = @($ValidationManifest | ForEach-Object {
    (Resolve-Path -LiteralPath $_).Path
})
if ([string]::IsNullOrWhiteSpace($Checkpoint)) {
    $Checkpoint = Join-Path $RepositoryRoot `
        '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\best_model.pth'
}
$Checkpoint = (Resolve-Path -LiteralPath $Checkpoint).Path
$Lock = Get-Content -LiteralPath `
    (Join-Path $RepositoryRoot 'dependency-lock.json') -Raw |
    ConvertFrom-Json
$ExpectedCheckpoint = $Lock.spikeD4xx.checkpoint
$CheckpointItem = Get-Item -LiteralPath $Checkpoint
$CheckpointHash = (Get-FileHash -LiteralPath $Checkpoint `
    -Algorithm SHA256).Hash
if ($CheckpointItem.Length -ne [int64]$ExpectedCheckpoint.bytes -or `
    $CheckpointHash -ne [string]$ExpectedCheckpoint.sha256) {
    throw "Pinned SPiKE checkpoint mismatch: $Checkpoint"
}
if (-not [string]::IsNullOrWhiteSpace($Resume)) {
    $Resume = (Resolve-Path -LiteralPath $Resume).Path
}
if ([string]::IsNullOrWhiteSpace($OutputRoot) -and
    -not [string]::IsNullOrWhiteSpace($Resume)) {
    $OutputRoot = Split-Path -Parent $Resume
}
elseif ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $ExperimentRoot = Join-Path $RepositoryRoot 'build\experiments\spike'
    New-Item -ItemType Directory -Path $ExperimentRoot -Force | Out-Null
    $OutputRoot = Join-Path $ExperimentRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
}
Write-Host '[ENV] Restoring pinned PyTorch CUDA research environment...'
& $Uv sync --project $ResearchRoot --frozen
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE research environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $ResearchRoot '.venv\Scripts\python.exe'
$Arguments = @(
    (Join-Path $PSScriptRoot 'train-spike-teacher.py'),
    '--train'
) + $ResolvedTrain + @('--validation') + $ResolvedValidation + @(
    '--checkpoint', $Checkpoint,
    '--output', $OutputRoot,
    '--epochs', [string]$Epochs,
    '--batch-size', [string]$BatchSize,
    '--workers', [string]$Workers,
    '--learning-rate', $LearningRate.ToString(
        'R', [Globalization.CultureInfo]::InvariantCulture),
    '--device', $Device
)
if (-not [string]::IsNullOrWhiteSpace($Resume)) {
    $Arguments += @('--resume', $Resume)
}
if ($SkipHashCheck) {
    $Arguments += '--skip-hash-check'
}

$PreviousPythonPath = $env:PYTHONPATH
$PreviousCublasWorkspace = $env:CUBLAS_WORKSPACE_CONFIG
try {
    $env:PYTHONPATH = $RuntimeRoot
    $env:CUBLAS_WORKSPACE_CONFIG = ':4096:8'
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE fine-tuning failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
    $env:CUBLAS_WORKSPACE_CONFIG = $PreviousCublasWorkspace
}

$Summary = Join-Path $OutputRoot 'summary.json'
$SummaryHash = (Get-FileHash -LiteralPath $Summary -Algorithm SHA256).Hash
Write-Host '[OK] SPiKE D430-domain fine-tuning completed.'
Write-Host "     Summary: $Summary"
Write-Host "     SHA256: $SummaryHash"
