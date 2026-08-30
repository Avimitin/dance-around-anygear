[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]] $DatasetManifest,
    [Parameter(Mandatory)]
    [string] $ModelPath,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedSha256,
    [string] $BaselineModelPath,
    [ValidatePattern('^$|^[0-9A-Fa-f]{64}$')]
    [string] $BaselineExpectedSha256,
    [string] $OutputJson,
    [ValidateRange(0, 10000000)]
    [int] $MaximumSamples = 0,
    [ValidateRange(0, 100)]
    [int] $Warmup = 5,
    [ValidateRange(0, 15)]
    [int] $DirectMlDevice = 0,
    [ValidateRange(1.0, 1000.0)]
    [double] $MaximumP95Ms = 25.0,
    [ValidateRange(0.001, 10.0)]
    [double] $MaximumMpjpeM = 0.08,
    [ValidateRange(0.001, 10.0)]
    [double] $MaximumEndpointErrorM = 0.10,
    [ValidateRange(0.0, 1.0)]
    [double] $MaximumSwapFraction = 0.0,
    [ValidateRange(0.0, 0.99)]
    [double] $MinimumRelativeImprovement = 0.02,
    [switch] $RequireGates,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before model evaluation.'
}
if (Get-Process -Name 'dance_around_anygear_spike_worker' `
        -ErrorAction SilentlyContinue) {
    throw 'The realtime SPiKE worker is running. Stop it before evaluation.'
}
$ResolvedDatasets = @($DatasetManifest | ForEach-Object {
    (Resolve-Path -LiteralPath $_).Path
})
$ModelPath = (Resolve-Path -LiteralPath $ModelPath).Path
$ActualHash = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash
if ($ActualHash -ne $ExpectedSha256.ToUpperInvariant()) {
    throw "SPiKE evaluation model mismatch: $ModelPath"
}
$HasBaselinePath = -not [string]::IsNullOrWhiteSpace($BaselineModelPath)
$HasBaselineHash = -not [string]::IsNullOrWhiteSpace(
    $BaselineExpectedSha256)
if ($HasBaselinePath -ne $HasBaselineHash) {
    throw 'Supply both the baseline model and its expected SHA-256.'
}
if ($HasBaselinePath) {
    $BaselineModelPath = (Resolve-Path -LiteralPath $BaselineModelPath).Path
    $BaselineHash = (Get-FileHash -LiteralPath $BaselineModelPath `
        -Algorithm SHA256).Hash
    if ($BaselineHash -ne $BaselineExpectedSha256.ToUpperInvariant()) {
        throw "SPiKE baseline model mismatch: $BaselineModelPath"
    }
}
if ([string]::IsNullOrWhiteSpace($OutputJson)) {
    $EvaluationRoot = Join-Path $RepositoryRoot `
        'build\experiments\spike-evaluations'
    New-Item -ItemType Directory -Path $EvaluationRoot -Force | Out-Null
    $OutputJson = Join-Path $EvaluationRoot `
        ((Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
}
$OutputJson = [IO.Path]::GetFullPath($OutputJson)
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to restore the pinned SPiKE evaluation environment.'
}

Write-Host '[ENV] Restoring pinned DirectML evaluation environment...'
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE evaluation environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$Arguments = @(
    (Join-Path $PSScriptRoot 'evaluate-spike-teacher.py'),
    '--dataset'
) + $ResolvedDatasets + @(
    '--model', $ModelPath,
    '--expected-sha256', $ExpectedSha256.ToUpperInvariant(),
    '--output', $OutputJson,
    '--device', [string]$DirectMlDevice,
    '--warmup', [string]$Warmup,
    '--maximum-samples', [string]$MaximumSamples,
    '--maximum-p95-ms', $MaximumP95Ms.ToString('R', $Invariant),
    '--maximum-mpjpe-m', $MaximumMpjpeM.ToString('R', $Invariant),
    '--maximum-endpoint-error-m', `
        $MaximumEndpointErrorM.ToString('R', $Invariant),
    '--maximum-swap-fraction', `
        $MaximumSwapFraction.ToString('R', $Invariant),
    '--minimum-relative-improvement', `
        $MinimumRelativeImprovement.ToString('R', $Invariant)
)
if ($HasBaselinePath) {
    $Arguments += @(
        '--baseline-model', $BaselineModelPath,
        '--baseline-expected-sha256', `
            $BaselineExpectedSha256.ToUpperInvariant()
    )
}
if ($RequireGates) {
    $Arguments += '--require-gates'
}

$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE held-out evaluation failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
Write-Host "[OK] Held-out D430 evaluation: $OutputJson"
