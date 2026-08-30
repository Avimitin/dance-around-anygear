[CmdletBinding()]
param(
    [string] $Python
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($Python)) {
    $Python = Join-Path $RepositoryRoot `
        'runtime\spike\.venv\Scripts\python.exe'
}
$Harness = Join-Path $RepositoryRoot 'build\bin\anygear_spike_ipc_host_test.exe'
foreach ($Required in @($Python, $Harness)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required IPC test file is absent: $Required"
    }
}
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = Join-Path $RepositoryRoot 'runtime\spike'
    & $Harness $Python
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE IPC test failed with exit code $LASTEXITCODE."
    }

    $PidFile = Join-Path $RepositoryRoot 'build\spike-orphan-test.pid'
    if (Test-Path -LiteralPath $PidFile) {
        Remove-Item -LiteralPath $PidFile -Force
    }
    & $Harness $Python --exit-without-cleanup $PidFile
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $PidFile -PathType Leaf)) {
        throw 'SPiKE abrupt-host-exit setup failed.'
    }
    $WorkerProcessId = [int](Get-Content -LiteralPath $PidFile -Raw)
    Remove-Item -LiteralPath $PidFile -Force
    $Deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (Get-Process -Id $WorkerProcessId -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -ge $Deadline) {
            Stop-Process -Id $WorkerProcessId -Force -ErrorAction SilentlyContinue
            throw "SPiKE worker $WorkerProcessId survived its host process."
        }
        Start-Sleep -Milliseconds 50
    }
    Write-Host '[OK] Abrupt host exit left no SPiKE worker process.'
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
Write-Host '[OK] C++ host and Python worker IPC contract passed.'
