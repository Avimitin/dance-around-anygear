[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $DatasetRoot,
    [Parameter(Mandatory)]
    [string] $ModelPath,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedSha256,
    [string] $BaselineModelPath,
    [ValidatePattern('^$|^[0-9A-Fa-f]{64}$')]
    [string] $BaselineExpectedSha256,
    [string] $OutputJson,
    [ValidateRange(0, 10501)]
    [int] $Samples = 0,
    [ValidateRange(0, 100)]
    [int] $Warmup = 5,
    [ValidateRange(0, 15)]
    [int] $DirectMlDevice = 0,
    [ValidateRange(1.0, 1000.0)]
    [double] $MaximumP95Ms = 25.0,
    [ValidateRange(0.0, 1.0)]
    [double] $MaximumPckDrop = 0.005,
    [ValidateRange(0.0, 1.0)]
    [double] $MaximumMpjpeRelativeRegression = 0.02,
    [switch] $SkipDatasetHash,
    [switch] $RequireGates,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before ITOP evaluation.'
}
if (Get-Process -Name 'dance_around_anygear_spike_worker' `
        -ErrorAction SilentlyContinue) {
    throw 'The realtime SPiKE worker is running. Stop it before ITOP evaluation.'
}
$DatasetRoot = (Resolve-Path -LiteralPath $DatasetRoot).Path
$ModelPath = (Resolve-Path -LiteralPath $ModelPath).Path
$ModelHash = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash
if ($ModelHash -ne $ExpectedSha256.ToUpperInvariant()) {
    throw "SPiKE ITOP model mismatch: $ModelPath"
}
$Lock = Get-Content -LiteralPath `
    (Join-Path $RepositoryRoot 'dependency-lock.json') -Raw |
    ConvertFrom-Json
$PinnedBaseline = $Lock.spikeD4xx.export
if ([string]::IsNullOrWhiteSpace($BaselineModelPath)) {
    $BaselineModelPath = Join-Path $RepositoryRoot `
        '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\spike-itop-side-primary-fp16.onnx'
}
if ([string]::IsNullOrWhiteSpace($BaselineExpectedSha256)) {
    $BaselineExpectedSha256 = [string]$PinnedBaseline.outputSha256
}
$BaselineModelPath = (Resolve-Path -LiteralPath $BaselineModelPath).Path
$BaselineHash = (Get-FileHash -LiteralPath $BaselineModelPath `
    -Algorithm SHA256).Hash
if ($BaselineHash -ne $BaselineExpectedSha256.ToUpperInvariant()) {
    throw "SPiKE ITOP baseline mismatch: $BaselineModelPath"
}
if ([string]::IsNullOrWhiteSpace($OutputJson)) {
    $EvaluationRoot = Join-Path $RepositoryRoot `
        'build\experiments\spike-evaluations'
    New-Item -ItemType Directory -Path $EvaluationRoot -Force | Out-Null
    $OutputJson = Join-Path $EvaluationRoot `
        ('itop-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
}
$OutputJson = [IO.Path]::GetFullPath($OutputJson)
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to restore the pinned ITOP evaluation environment.'
}
Write-Host '[ENV] Restoring pinned DirectML/ITOP evaluation environment...'
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups --group itop
if ($LASTEXITCODE -ne 0) {
    throw "ITOP evaluation environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$Arguments = @(
    (Join-Path $PSScriptRoot 'evaluate-spike-itop.py'),
    $DatasetRoot,
    '--model', $ModelPath,
    '--expected-sha256', $ExpectedSha256.ToUpperInvariant(),
    '--baseline-model', $BaselineModelPath,
    '--baseline-expected-sha256', `
        $BaselineExpectedSha256.ToUpperInvariant(),
    '--output', $OutputJson,
    '--samples', [string]$Samples,
    '--device', [string]$DirectMlDevice,
    '--warmup', [string]$Warmup,
    '--maximum-p95-ms', $MaximumP95Ms.ToString('R', $Invariant),
    '--maximum-pck-drop', $MaximumPckDrop.ToString('R', $Invariant),
    '--maximum-mpjpe-relative-regression', `
        $MaximumMpjpeRelativeRegression.ToString('R', $Invariant)
)
if ($SkipDatasetHash) {
    $Arguments += '--skip-dataset-hash'
}
if ($RequireGates) {
    $Arguments += '--require-gates'
}
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE ITOP evaluation failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
Write-Host "[OK] Public ITOP evaluation: $OutputJson"
