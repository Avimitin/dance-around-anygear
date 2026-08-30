[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $RealSenseRuntime,
    [Parameter(Mandatory)]
    [string] $Config,
    [Parameter(Mandatory)]
    [string] $Model,
    [ValidateRange(3, 15)]
    [int] $EmptySeconds = 4,
    [ValidateRange(4, 30)]
    [int] $PerformerSeconds = 8,
    [ValidateRange(0, 30)]
    [int] $CountdownSeconds = 10,
    [string] $OutputRoot,
    [switch] $OpenPreview
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepositoryRoot 'build\diagnostics\d4xx-spike-preview'
}
$RecordScript = Join-Path $PSScriptRoot 'record-d4xx-depth.ps1'
$RenderScript = Join-Path $PSScriptRoot 'render-d4xx-spike-preview.py'
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
foreach ($Path in @($RealSenseRuntime, $Config, $Model, $RecordScript, $RenderScript)) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file not found: $Path"
    }
}
if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound is running. Stop the game before opening both D430 devices.'
}
if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required for the pinned SPiKE diagnostic environment.'
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$Session = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $Session | Out-Null

Write-Host '[PREVIEW] D430 production point-cloud and SPiKE skeleton capture'
Write-Host '          First capture: keep both camera views completely empty.'
for ($Remaining = $CountdownSeconds; $Remaining -gt 0; $Remaining--) {
    Write-Host "          Empty-stage capture starts in $Remaining..."
    Start-Sleep -Seconds 1
}
[System.Media.SystemSounds]::Asterisk.Play()

$BackgroundRoot = Join-Path $Session 'background'
& $RecordScript `
    -RealSenseRuntime $RealSenseRuntime `
    -Seconds $EmptySeconds `
    -RequiredDevices 2 `
    -WarmupSeconds 1 `
    -EmitterMode all-on `
    -DepthCoordinate native `
    -VisualPreset high-density `
    -OutputRoot $BackgroundRoot
if ($LASTEXITCODE -ne 0) {
    throw "Empty-stage capture failed with exit code $LASTEXITCODE."
}
$BackgroundCapture = Get-ChildItem -LiteralPath $BackgroundRoot -Directory |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

Write-Host '[PREVIEW] Empty stage captured.'
Write-Host '          Enter the stage now. Stand centered, then move both hands and feet.'
1..3 | ForEach-Object {
    [System.Media.SystemSounds]::Exclamation.Play()
    Start-Sleep -Milliseconds 350
}
$PerformerRoot = Join-Path $Session 'performer'
& $RecordScript `
    -RealSenseRuntime $RealSenseRuntime `
    -Seconds $PerformerSeconds `
    -RequiredDevices 2 `
    -WarmupSeconds 3 `
    -EmitterMode all-on `
    -DepthCoordinate native `
    -VisualPreset high-density `
    -OutputRoot $PerformerRoot
if ($LASTEXITCODE -ne 0) {
    throw "Performer capture failed with exit code $LASTEXITCODE."
}
$PerformerCapture = Get-ChildItem -LiteralPath $PerformerRoot -Directory |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

$PreviewDirectory = Join-Path $Session 'preview'
Push-Location -LiteralPath $RuntimeRoot
try {
    & uv run --offline --frozen python $RenderScript `
        --background $BackgroundCapture.FullName `
        --performer $PerformerCapture.FullName `
        --config $Config `
        --model $Model `
        --output $PreviewDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE preview renderer failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$Preview = Join-Path $PreviewDirectory 'spike-preview.svg'
Write-Host '[OK] D430/SPiKE preview generated.'
Write-Host "     Preview: $Preview"
Write-Host "     Report : $(Join-Path $PreviewDirectory 'preview-report.json')"
if ($OpenPreview) {
    Start-Process -FilePath $Preview
}
