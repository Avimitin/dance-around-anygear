[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $CaptureRoot,
    [ValidateRange(1, 10000)][int] $Frames = 60,
    [string] $Python
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
if ([string]::IsNullOrWhiteSpace($Python)) {
    $Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
}
$Config = Join-Path $RepositoryRoot `
    'config\dance_around_anygear_d4xx_spike.json'
foreach ($Required in @(
    $Python, $Config, (Join-Path $CaptureRoot 'manifest.json'))) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "SPiKE isolation benchmark input is absent: $Required"
    }
}
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python (Join-Path $PSScriptRoot 'benchmark-spike-isolation.py') `
        (Resolve-Path -LiteralPath $CaptureRoot).Path $Config `
        --frames $Frames
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE isolation benchmark failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
