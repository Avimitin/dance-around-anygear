[CmdletBinding()]
param(
    [ValidateRange(1, 60)]
    [int] $GraceSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Processes = @(Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue)
if ($Processes.Count -eq 0) {
    Write-Host '[STOP] dancearound.exe is not running.'
    exit 0
}

foreach ($Process in $Processes) {
    Write-Host "[STOP] Requesting a normal window close for PID $($Process.Id)..."
    if (-not $Process.CloseMainWindow()) {
        throw "dancearound PID $($Process.Id) has no closable main window."
    }
}

$Deadline = (Get-Date).AddSeconds($GraceSeconds)
do {
    Start-Sleep -Milliseconds 250
    $Remaining = @(Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue)
} while ($Remaining.Count -gt 0 -and (Get-Date) -lt $Deadline)

if ($Remaining.Count -gt 0) {
    throw 'The game did not exit normally; no process was terminated.'
}

Write-Host '[OK] dancearound.exe exited normally.'
