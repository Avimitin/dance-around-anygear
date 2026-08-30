[CmdletBinding()]
param(
    [ValidateRange(1024, 65535)]
    [int] $Port = 8765
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Processes = @(Get-Process -Name 'anygear_d4xx_spike_viewer_host' `
    -ErrorAction SilentlyContinue)
if ($Processes.Count -eq 0) {
    Write-Host '[VIEWER] D4xx/SPiKE live viewer is not running.'
    exit 0
}
foreach ($Process in $Processes) {
    Write-Host "[VIEWER] Stopping host PID $($Process.Id)..."
    try {
        Invoke-RestMethod -Method Post `
            -Uri "http://127.0.0.1:$Port/api/stop" `
            -ContentType 'application/json' `
            -Body '{}' `
            -TimeoutSec 2 | Out-Null
        [void] $Process.WaitForExit(5000)
    } catch {
        # A faulted worker may no longer own the HTTP endpoint. Closing the
        # exact viewer host also closes its worker cleanup job.
    }
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id
        [void] $Process.WaitForExit(3000)
    }
}
Write-Host '[VIEWER] Stopped. The worker process is closed with its host job.'
