[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CaptureRoot,
    [ValidateRange(0, 30)]
    [int] $MaximumLagFrames = 5,
    [string] $OutputPath,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
$CaptureRoot = (Resolve-Path -LiteralPath $CaptureRoot).Path
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to restore the pinned SPiKE analysis environment.'
}
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$Arguments = @(
    (Join-Path $PSScriptRoot 'analyze-kinect-teacher.py'),
    $CaptureRoot,
    '--maximum-lag-frames', [string]$MaximumLagFrames
)
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $Arguments += @('--output', $OutputPath)
}
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Kinect teacher analysis failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
