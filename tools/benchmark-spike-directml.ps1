[CmdletBinding()]
param(
    [string] $Model,
    [ValidateRange(5, 1000)][int] $Iterations = 40,
    [ValidateRange(1, 100)][int] $Warmup = 5,
    [ValidateRange(0, 15)][int] $Device = 0,
    [ValidateRange(0.0, 1000.0)][double] $MaximumP95Ms = 0.0,
    [string] $ExpectedSha256,
    [string] $OutputJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
if ([string]::IsNullOrWhiteSpace($Model)) {
    $Model = Join-Path $RepositoryRoot `
        '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\spike-itop-side-primary-fp16.onnx'
    if ([string]::IsNullOrWhiteSpace($ExpectedSha256)) {
        $ExpectedSha256 = `
            'C48C40FF94C8F358762DB296DA0117DCFF78D8C4AD2FA4DB575298979BF2DA0D'
    }
}
foreach ($Required in @($Python, $Model)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "SPiKE benchmark input is absent: $Required"
    }
}
$Model = (Resolve-Path -LiteralPath $Model).Path
if ([string]::IsNullOrWhiteSpace($ExpectedSha256) -or
    $ExpectedSha256 -notmatch '^[0-9A-Fa-f]{64}$') {
    throw 'Supply -ExpectedSha256 for a non-default research model.'
}
$ExpectedHash = $ExpectedSha256.ToUpperInvariant()
$ActualHash = (Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash
if ($ActualHash -ne $ExpectedHash) {
    throw "SPiKE benchmark model mismatch: $ActualHash"
}
$Arguments = @(
    (Join-Path $PSScriptRoot 'benchmark-spike-directml.py'),
    $Model,
    '--iterations', $Iterations,
    '--warmup', $Warmup,
    '--device', $Device,
    '--maximum-p95-ms', $MaximumP95Ms
)
if (-not [string]::IsNullOrWhiteSpace($OutputJson)) {
    $OutputJson = [IO.Path]::GetFullPath($OutputJson)
    $Arguments += @('--output', $OutputJson)
}
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE DirectML benchmark failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
