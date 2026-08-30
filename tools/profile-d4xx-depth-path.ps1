[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [ValidateRange(2, 30)]
    [int] $SecondsPerCase = 4,
    [ValidateRange(0, 15)]
    [int] $WarmupSeconds = 2,
    [string] $OutputRoot,
    [string] $SpikeRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepositoryRoot 'build\diagnostics\d4xx-depth-path'
}
if ([string]::IsNullOrWhiteSpace($SpikeRoot)) {
    $SpikeRoot = Join-Path (Split-Path -Parent $RepositoryRoot) 'SPiKE'
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before profiling D430 depth.'
}
$RecordScript = Join-Path $PSScriptRoot 'record-d4xx-depth.ps1'
$AnalyzeScript = Join-Path $PSScriptRoot 'analyze-d4xx-depth.ps1'
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$Session = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $Session | Out-Null

$Cases = @(
    @{ Name = 'aligned-current'; Coordinate = 'infrared'; Preset = 'unchanged' },
    @{ Name = 'native-current'; Coordinate = 'native'; Preset = 'unchanged' },
    @{ Name = 'native-default'; Coordinate = 'native'; Preset = 'default' },
    @{ Name = 'native-high-accuracy'; Coordinate = 'native'; Preset = 'high-accuracy' },
    @{ Name = 'native-high-density'; Coordinate = 'native'; Preset = 'high-density' }
)
Write-Host '[D4XX] Static-scene native-depth and preset profile'
Write-Host '       Keep people and moving objects outside both camera views.'
Write-Host "       Session: $Session"
foreach ($Case in $Cases) {
    Write-Host "[CASE] $($Case.Name)"
    & $RecordScript `
        -RealSenseRuntime $RealSenseRuntime `
        -Seconds $SecondsPerCase `
        -RequiredDevices 2 `
        -InfraredIndex 1 `
        -WarmupSeconds $WarmupSeconds `
        -EmitterMode all-on `
        -DepthCoordinate $Case.Coordinate `
        -VisualPreset $Case.Preset `
        -OutputRoot (Join-Path $Session $Case.Name)
    Start-Sleep -Seconds 2
}

$Report = Join-Path $Session 'depth-path-profile.json'
& $AnalyzeScript -CaptureRoot $Session -SpikeRoot $SpikeRoot `
    -OutputJson $Report
Write-Host '[OK] Depth-path profile completed.'
Write-Host "     Report: $Report"
