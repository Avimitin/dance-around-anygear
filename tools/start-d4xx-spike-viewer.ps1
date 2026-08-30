[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [Parameter(Mandatory)]
    [string] $Config,
    [Parameter(Mandatory)]
    [string] $Model,
    [ValidateRange(1024, 65535)]
    [int] $Port = 8765,
    [ValidateRange(0, 30)]
    [int] $CountdownSeconds = 5,
    [string] $Log,
    [switch] $NoOpen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
$ViewerHost = Join-Path $RepositoryRoot 'build\bin\anygear_d4xx_spike_viewer_host.exe'
foreach ($Path in @($RealSenseRuntime, $Config, $Model, $ViewerHost)) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file not found: $Path"
    }
}
if ([IO.Path]::GetFileName($RealSenseRuntime) -notin @(
        'realsense2.dll', 'realsense2.dll.orig')) {
    throw "Expected an official librealsense runtime, got: $RealSenseRuntime"
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before opening both D4xx devices.'
}
if (Get-Process -Name 'anygear_d4xx_spike_viewer_host' -ErrorAction SilentlyContinue) {
    throw 'The D4xx/SPiKE live viewer is already running.'
}
$Listener = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
if ($Listener) {
    throw "TCP port $Port is already in use. Choose another -Port value."
}
$Uv = Get-Command uv -ErrorAction SilentlyContinue
if (-not $Uv) {
    throw 'uv is required for the pinned SPiKE runtime environment.'
}

$RealSenseRuntime = (Resolve-Path -LiteralPath $RealSenseRuntime).Path
$Config = (Resolve-Path -LiteralPath $Config).Path
$Model = (Resolve-Path -LiteralPath $Model).Path
if ([string]::IsNullOrWhiteSpace($Log)) {
    $Session = Join-Path $RepositoryRoot (
        'build\diagnostics\d4xx-spike-live-viewer\' +
        (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Force -Path $Session | Out-Null
    $Log = Join-Path $Session 'worker.log'
} else {
    $LogParent = Split-Path -Parent $Log
    if (-not [string]::IsNullOrWhiteSpace($LogParent)) {
        New-Item -ItemType Directory -Force -Path $LogParent | Out-Null
    }
    $Log = [IO.Path]::GetFullPath($Log)
}

Write-Host '[VIEWER] Preparing the pinned SPiKE environment...'
Push-Location -LiteralPath $RuntimeRoot
try {
    & $Uv.Source sync --offline --frozen
    if ($LASTEXITCODE -ne 0) {
        throw "uv sync failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw "Pinned Python environment is incomplete: $Python"
}

Write-Host '[VIEWER] Keep both camera views completely empty during startup.'
for ($Remaining = $CountdownSeconds; $Remaining -gt 0; $Remaining--) {
    Write-Host "         Capture starts in $Remaining..."
    Start-Sleep -Seconds 1
}
[System.Media.SystemSounds]::Asterisk.Play()
Write-Host "[VIEWER] URL: http://127.0.0.1:$Port/"
Write-Host "         Log: $Log"
Write-Host '         Use the browser Stop button or Ctrl+C in this window to exit.'

$PreviousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = if ([string]::IsNullOrWhiteSpace($PreviousPythonPath)) {
    $RuntimeRoot
} else {
    "$RuntimeRoot;$PreviousPythonPath"
}
Push-Location -LiteralPath $RuntimeRoot
try {
    $Arguments = @(
        '--realsense-runtime', $RealSenseRuntime,
        '--worker', $Python,
        '--model', $Model,
        '--config', $Config,
        '--log', $Log,
        '--port', $Port
    )
    if ($NoOpen) {
        $Arguments += '--no-open'
    }
    & $ViewerHost @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "D4xx/SPiKE live viewer failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
    $env:PYTHONPATH = $PreviousPythonPath
}
