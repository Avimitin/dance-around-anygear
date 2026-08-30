[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CabinetRoot,
    [Parameter(Mandatory)]
    [string] $SpiceExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'dancearound.exe is running. Stop it before installing Spice.'
}

$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
$SpiceExecutable = (Resolve-Path -LiteralPath $SpiceExecutable).Path
if (-not (Test-Path -LiteralPath $SpiceExecutable -PathType Leaf)) {
    throw "Spice executable not found: $SpiceExecutable"
}

$SourceItem = Get-Item -LiteralPath $SpiceExecutable
if ($SourceItem.Extension -ne '.exe' -or $SourceItem.Length -eq 0) {
    throw "Expected a non-empty Spice executable: $SpiceExecutable"
}

$Targets = @(
    [pscustomobject]@{
        Path = Join-Path $CabinetRoot 'game\dancearound.exe'
        BackupName = 'game-dancearound.exe'
    },
    [pscustomobject]@{
        Path = Join-Path $CabinetRoot 'spice64.exe'
        BackupName = 'root-spice64.exe'
    }
)
foreach ($Target in $Targets) {
    if (-not (Test-Path -LiteralPath $Target.Path -PathType Leaf)) {
        throw "Cabinet Spice target not found: $($Target.Path)"
    }
}

$SourceHash = (Get-FileHash -LiteralPath $SpiceExecutable `
    -Algorithm SHA256).Hash
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$BackupDirectory = Join-Path $CabinetRoot `
    "anygear-backups\spice-$Stamp"
New-Item -ItemType Directory -Path $BackupDirectory -Force | Out-Null

foreach ($Target in $Targets) {
    Copy-Item -LiteralPath $Target.Path -Destination (Join-Path `
        $BackupDirectory $Target.BackupName) -Force
}

try {
    foreach ($Target in $Targets) {
        Copy-Item -LiteralPath $SpiceExecutable -Destination $Target.Path -Force
        $InstalledHash = (Get-FileHash -LiteralPath $Target.Path `
            -Algorithm SHA256).Hash
        if ($InstalledHash -ne $SourceHash) {
            throw "Installed Spice hash mismatch: $($Target.Path)"
        }
    }
}
catch {
    foreach ($Target in $Targets) {
        $BackupPath = Join-Path $BackupDirectory $Target.BackupName
        if (Test-Path -LiteralPath $BackupPath -PathType Leaf) {
            Copy-Item -LiteralPath $BackupPath -Destination $Target.Path -Force
        }
    }
    throw
}

Write-Host '[OK] Spice installed; the game was not started.'
Write-Host "     Source : $SpiceExecutable"
Write-Host "     SHA256 : $SourceHash"
Write-Host "     Backup : $BackupDirectory"
foreach ($Target in $Targets) {
    Write-Host "     Target : $($Target.Path)"
}
