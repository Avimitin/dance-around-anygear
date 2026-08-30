[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [Alias('GameRoot')]
    [string] $CabinetRoot,
    [Parameter(Mandatory)]
    [string] $ModelDirectory,
    [switch] $Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound.exe is running. Stop it before changing the model path.'
}

$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
$ModelDirectory = (Resolve-Path -LiteralPath $ModelDirectory).Path
$ConfigPath = Join-Path $CabinetRoot 'vpprops\visionposeforunityconfig.json'
$ModelBase = Join-Path $ModelDirectory 'visionpose'
$RealtimeEngine = Join-Path $ModelDirectory 'visionpose_320x320_16'
$OfflineEngine = Join-Path $ModelDirectory 'visionpose_512x512_16'
foreach ($Required in @($ConfigPath, $ModelBase, $RealtimeEngine, $OfflineEngine)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required VisionPose file not found: $Required"
    }
}

$Text = [IO.File]::ReadAllText($ConfigPath)
$TargetModelPath = $ModelBase.Replace('\', '/')
$TargetMarker = '"ModelPath":    "' + $TargetModelPath + '"'
$CurrentPattern = '"ModelPath"\s*:\s*"[^"]+"'
$Matches = [regex]::Matches($Text, $CurrentPattern)
if ($Matches.Count -ne 2) {
    throw "Expected two VisionPose ModelPath entries in $ConfigPath; found $($Matches.Count)."
}
$Updated = [regex]::Replace($Text, $CurrentPattern, $TargetMarker)

Write-Host '[VISIONPOSE] External GPU-compatible engine selection'
Write-Host "             Config   : $ConfigPath"
Write-Host "             Model    : $TargetModelPath"
Write-Host "             Realtime : $((Get-Item -LiteralPath $RealtimeEngine).Length) bytes"
Write-Host "             Offline  : $((Get-Item -LiteralPath $OfflineEngine).Length) bytes"
if (-not $Apply) {
    Write-Host '[PREVIEW] No files changed. Re-run with -Apply to update the JSON path.'
    exit 0
}

if ($Updated -eq $Text) {
    Write-Host '[OK] VisionPose ModelPath already points to this directory.'
    exit 0
}

$BackupRoot = Join-Path $CabinetRoot 'anygear-backups'
$BackupDirectory = Join-Path $BackupRoot `
    ('visionpose-model-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $BackupDirectory -Force | Out-Null
$BackupConfig = Join-Path $BackupDirectory 'visionposeforunityconfig.json'
Copy-Item -LiteralPath $ConfigPath -Destination $BackupConfig

$Manifest = [ordered]@{
    created_at = (Get-Date).ToString('o')
    config_path = $ConfigPath
    config_backup = $BackupConfig
    original_config_sha256 = (Get-FileHash -LiteralPath $BackupConfig -Algorithm SHA256).Hash
    model_base = $ModelBase
    model_base_sha256 = (Get-FileHash -LiteralPath $ModelBase -Algorithm SHA256).Hash
    realtime_engine_sha256 = (Get-FileHash -LiteralPath $RealtimeEngine -Algorithm SHA256).Hash
    offline_engine_sha256 = (Get-FileHash -LiteralPath $OfflineEngine -Algorithm SHA256).Hash
}
[IO.File]::WriteAllText(
    (Join-Path $BackupDirectory 'manifest.json'),
    ($Manifest | ConvertTo-Json -Depth 4) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText(
    $ConfigPath,
    $Updated,
    [Text.UTF8Encoding]::new($false))

Write-Host '[OK] VisionPose now uses the external GPU-compatible engines.'
Write-Host "     Backup: $BackupDirectory"
