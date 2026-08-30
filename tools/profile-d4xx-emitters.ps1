[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [ValidateRange(2, 30)]
    [int] $SecondsPerMode = 4,
    [ValidateRange(0, 15)]
    [int] $WarmupSeconds = 2,
    [ValidateRange(0, 10)]
    [int] $PauseSeconds = 2,
    [string] $OutputRoot,
    [string] $SpikeRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepositoryRoot 'build\diagnostics\d4xx-emitter-profile'
}
if ([string]::IsNullOrWhiteSpace($SpikeRoot)) {
    $SpikeRoot = Join-Path (Split-Path -Parent $RepositoryRoot) 'SPiKE'
}
$RecordScript = Join-Path $PSScriptRoot 'record-d4xx-depth.ps1'
$AnalyzeScript = Join-Path $PSScriptRoot 'analyze-d4xx-depth.ps1'
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before profiling D430 emitters.'
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$Session = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $Session | Out-Null

# Leave all emitters enabled at the end so research runs do not silently alter
# the normal camera setup. Every mode explicitly clears stale alternation.
$Modes = @('all-off', 'first-only', 'second-only', 'alternating', 'all-on')
Write-Host '[D4XX] Static-scene emitter profile'
Write-Host '       Keep people and moving objects outside both camera views.'
Write-Host "       Session: $Session"

foreach ($Mode in $Modes) {
    Write-Host "[MODE] $Mode"
    $ModeRoot = Join-Path $Session $Mode
    try {
        & $RecordScript `
            -RealSenseRuntime $RealSenseRuntime `
            -Seconds $SecondsPerMode `
            -RequiredDevices 2 `
            -InfraredIndex 1 `
            -WarmupSeconds $WarmupSeconds `
            -EmitterMode $Mode `
            -OutputRoot $ModeRoot
    } catch {
        Write-Warning "$Mode failed: $($_.Exception.Message)"
    }
    if ($PauseSeconds -gt 0) {
        Start-Sleep -Seconds $PauseSeconds
    }
}

$Report = Join-Path $Session 'emitter-profile.json'
& $AnalyzeScript -CaptureRoot $Session -SpikeRoot $SpikeRoot `
    -OutputJson $Report
Write-Host '[OK] Emitter profile completed.'
Write-Host "     Report: $Report"
