[CmdletBinding()]
param(
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to test the SPiKE runtime.'
}
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups `
    --group dev --group calibration
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE test environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python -m pytest $RuntimeRoot
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE Python tests failed with exit code $LASTEXITCODE."
    }
    & $Python -m compileall -q (Join-Path $RuntimeRoot 'anygear_spike')
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE Python bytecode validation failed with exit code $LASTEXITCODE."
    }
    & (Join-Path $PSScriptRoot 'test-spike-ipc.ps1') -Python $Python
    if (-not $?) {
        throw 'SPiKE C++/Python IPC test failed.'
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
Write-Host '[OK] SPiKE runtime source and cross-language IPC tests passed.'
