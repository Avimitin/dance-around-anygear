[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $TrainingSummary,
    [string] $OutputRoot,
    [ValidateRange(5, 1000)]
    [int] $Iterations = 100,
    [ValidateRange(1, 100)]
    [int] $Warmup = 10,
    [ValidateRange(0, 15)]
    [int] $DirectMlDevice = 0,
    [ValidateRange(1.0, 1000.0)]
    [double] $MaximumP95Ms = 25.0,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
$TrainingSummary = (Resolve-Path -LiteralPath $TrainingSummary).Path
$Summary = Get-Content -LiteralPath $TrainingSummary -Raw | ConvertFrom-Json
if ($Summary.schema -ne 'dance-around-anygear.spike-finetune-summary.v1') {
    throw "Unsupported SPiKE training summary: $TrainingSummary"
}
$TrainingRoot = Split-Path -Parent $TrainingSummary
$SourceState = Join-Path $TrainingRoot ([string]$Summary.best_model_state)
if (-not (Test-Path -LiteralPath $SourceState -PathType Leaf)) {
    throw "Fine-tuned model state is absent: $SourceState"
}
$SourceHash = (Get-FileHash -LiteralPath $SourceState -Algorithm SHA256).Hash
if ($SourceHash -ne ([string]$Summary.best_model_state_sha256).ToUpperInvariant()) {
    throw "Fine-tuned model state hash mismatch: $SourceState"
}
$BestValidationMpjpe = [double]$Summary.best_validation_mpjpe_m
if ([double]::IsNaN($BestValidationMpjpe) -or
    [double]::IsInfinity($BestValidationMpjpe) -or
    $BestValidationMpjpe -lt 0.0) {
    throw 'Fine-tuned training summary has an invalid validation MPJPE.'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $CandidateRoot = Join-Path $RepositoryRoot `
        'build\experiments\spike-candidates'
    New-Item -ItemType Directory -Path $CandidateRoot -Force | Out-Null
    $OutputRoot = Join-Path $CandidateRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) {
    if (-not (Test-Path -LiteralPath $OutputRoot -PathType Container) -or
        (Get-ChildItem -LiteralPath $OutputRoot -Force)) {
        throw "Candidate output exists and is not empty: $OutputRoot"
    }
}
else {
    New-Item -ItemType Directory -Path $OutputRoot | Out-Null
}
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to restore the pinned SPiKE export environment.'
}

Write-Host '[EXPORT] Restoring the pinned CPU ONNX export environment...'
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups --group export
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE export environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$OnnxPath = Join-Path $OutputRoot 'spike-d430-candidate-fp16.onnx'
$OnnxMetadata = Join-Path $OutputRoot 'onnx-metadata.json'
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python (Join-Path $PSScriptRoot 'export-spike-onnx.py') `
        $SourceState $OnnxPath '--metadata' $OnnxMetadata
    if ($LASTEXITCODE -ne 0) {
        throw "Fine-tuned SPiKE ONNX export failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
$OnnxHash = (Get-FileHash -LiteralPath $OnnxPath -Algorithm SHA256).Hash
$ExportMetadata = Get-Content -LiteralPath $OnnxMetadata -Raw |
    ConvertFrom-Json
if ($ExportMetadata.schema -ne 'dance-around-anygear.spike-onnx.v1' -or
    ([string]$ExportMetadata.source_sha256).ToUpperInvariant() -ne $SourceHash -or
    ([string]$ExportMetadata.output_sha256).ToUpperInvariant() -ne $OnnxHash -or
    [int64]$ExportMetadata.output_bytes -ne
        (Get-Item -LiteralPath $OnnxPath).Length) {
    throw 'Fine-tuned SPiKE export metadata does not match its model files.'
}
$BenchmarkPath = Join-Path $OutputRoot 'directml-benchmark.json'
& (Join-Path $PSScriptRoot 'benchmark-spike-directml.ps1') `
    -Model $OnnxPath `
    -ExpectedSha256 $OnnxHash `
    -Iterations $Iterations `
    -Warmup $Warmup `
    -Device $DirectMlDevice `
    -MaximumP95Ms $MaximumP95Ms `
    -OutputJson $BenchmarkPath
if ($LASTEXITCODE -ne 0) {
    throw "Fine-tuned SPiKE DirectML benchmark failed with exit code $LASTEXITCODE."
}

$Benchmark = Get-Content -LiteralPath $BenchmarkPath -Raw | ConvertFrom-Json
$Candidate = [ordered]@{
    schema = 'dance-around-anygear.spike-candidate.v1'
    trainingSummarySha256 = (Get-FileHash -LiteralPath $TrainingSummary `
        -Algorithm SHA256).Hash
    sourceStateSha256 = $SourceHash
    bestValidationMpjpeM = $BestValidationMpjpe
    onnx = [ordered]@{
        file = [IO.Path]::GetFileName($OnnxPath)
        bytes = (Get-Item -LiteralPath $OnnxPath).Length
        sha256 = $OnnxHash
        metadata = [IO.Path]::GetFileName($OnnxMetadata)
    }
    directMl = $Benchmark
    acceptance = [ordered]@{
        maximumDirectMlP95Ms = $MaximumP95Ms
        realtimeP95Passed = ([double]$Benchmark.p95_ms -le $MaximumP95Ms)
        heldOutD430EvaluationPassed = $false
        publicItopEvaluationPassed = $false
        liveGameValidationPassed = $false
    }
}
$CandidatePath = Join-Path $OutputRoot 'candidate-manifest.json'
$CandidateJson = ($Candidate | ConvertTo-Json -Depth 10) + "`n"
[IO.File]::WriteAllText(
    $CandidatePath,
    $CandidateJson,
    [Text.UTF8Encoding]::new($false))
Write-Host '[OK] Fine-tuned SPiKE candidate exported; release model unchanged.'
Write-Host "     Candidate: $CandidatePath"
Write-Host "     ONNX SHA256: $OnnxHash"
Write-Host "     DirectML P95: $([Math]::Round([double]$Benchmark.p95_ms, 3)) ms"
