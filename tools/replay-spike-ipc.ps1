[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CaptureRoot,
    [string] $Python,
    [string] $Model,
    [int] $Frames = 12,
    [string] $OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($Python)) {
    $Python = Join-Path $RepositoryRoot `
        'runtime\spike\.venv\Scripts\python.exe'
}
if ([string]::IsNullOrWhiteSpace($Model)) {
    $Model = Join-Path $RepositoryRoot `
        '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\spike-itop-side-primary-fp16.onnx'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepositoryRoot 'build\spike-ipc-replay'
}
$Harness = Join-Path $RepositoryRoot 'build\bin\anygear_spike_ipc_replay.exe'
$RuntimeConfig = Join-Path $RepositoryRoot `
    'config\dance_around_anygear_d4xx_spike.json'
foreach ($Required in @(
    $Harness, $Python, $Model, $RuntimeConfig,
    (Join-Path $CaptureRoot 'manifest.json'))) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required SPiKE replay input is absent: $Required"
    }
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$Log = Join-Path $OutputRoot 'worker.log'
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = Join-Path $RepositoryRoot 'runtime\spike'
    & $Harness $Python $Model $RuntimeConfig `
        $CaptureRoot $Log $Frames
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE full IPC replay failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
Write-Host "[OK] SPiKE worker log: $Log"
