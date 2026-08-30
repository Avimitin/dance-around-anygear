[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CabinetRoot,
    [ValidateRange(1, 120)]
    [int] $FrameCount = 24,
    [ValidateRange(1, 3600)]
    [int] $FrameStride = 120,
    [string] $OutputDirectory,
    [switch] $Disable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'Stop dancearound.exe before changing D4xx frame diagnostics.'
}
$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
$ConfigPath = Join-Path $CabinetRoot 'dance_around_anygear_d4xx.json'
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
    throw "D4xx configuration not found: $ConfigPath"
}
$Config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
if ($null -eq $Config.D4xxNativeBridge) {
    throw "D4xxNativeBridge section not found in $ConfigPath"
}

if ($Disable) {
    $OutputDirectory = ''
    $FrameCount = 0
    $FrameStride = 1
}
elseif ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $CabinetRoot `
        ('diagnostics\d4xx-frames\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $CabinetRoot $OutputDirectory
}

foreach ($Property in @{
    DiagnosticFrameDirectory = $OutputDirectory
    DiagnosticFrameCount = $FrameCount
    DiagnosticFrameStride = $FrameStride
}.GetEnumerator()) {
    if ($Config.D4xxNativeBridge.PSObject.Properties.Name -contains `
        $Property.Key) {
        $Config.D4xxNativeBridge.($Property.Key) = $Property.Value
    }
    else {
        $Config.D4xxNativeBridge | Add-Member -NotePropertyName `
            $Property.Key -NotePropertyValue $Property.Value
    }
}

$Text = $Config | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText(
    $ConfigPath, $Text + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
if ($Disable) {
    Write-Host '[OK] D4xx frame diagnostics disabled.'
}
else {
    Write-Host "[OK] D4xx frame diagnostics armed for $FrameCount frames (stride $FrameStride)."
    Write-Host "     Output: $OutputDirectory"
}
