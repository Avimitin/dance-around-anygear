[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CaptureRoot,
    [string] $SpikeRoot,
    [string] $OutputJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($SpikeRoot)) {
    $SpikeRoot = Join-Path (Split-Path -Parent $RepositoryRoot) 'SPiKE'
}
$CaptureRoot = (Resolve-Path -LiteralPath $CaptureRoot).Path
$SpikeRoot = (Resolve-Path -LiteralPath $SpikeRoot).Path
$Analyzer = Join-Path $SpikeRoot 'analyze_anygear_capture.py'
if (-not (Test-Path -LiteralPath $Analyzer -PathType Leaf)) {
    throw "SPiKE capture analyzer is absent: $Analyzer"
}
if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to run the pinned SPiKE analysis environment.'
}
if ([string]::IsNullOrWhiteSpace($OutputJson)) {
    $OutputJson = Join-Path $CaptureRoot 'capture-analysis.json'
}

Push-Location -LiteralPath $SpikeRoot
try {
    & uv run python $Analyzer $CaptureRoot --json $OutputJson
    if ($LASTEXITCODE -ne 0) {
        throw "Capture analysis failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}
Write-Host "[OK] Capture analysis: $OutputJson"
