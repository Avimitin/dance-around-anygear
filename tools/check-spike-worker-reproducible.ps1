[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildScript = Join-Path $PSScriptRoot 'build-spike-worker.ps1'
$WorkerRoot = Join-Path $RepositoryRoot `
    'build\spike-worker\dist\dance_around_anygear_spike_worker'

function Get-WorkerManifest {
    if (-not (Test-Path -LiteralPath $WorkerRoot -PathType Container)) {
        throw "Frozen SPiKE worker is absent: $WorkerRoot"
    }
    return @(Get-ChildItem -LiteralPath $WorkerRoot -Recurse -File |
        Sort-Object FullName | ForEach-Object {
            $Relative = $_.FullName.Substring($WorkerRoot.Length + 1).Replace(
                '\', '/')
            $Hash = (Get-FileHash -LiteralPath $_.FullName `
                -Algorithm SHA256).Hash
            "$Hash  $Relative"
        })
}

Write-Host '[REPRO A] Building frozen DirectML worker...'
& $BuildScript -Clean
if (-not $?) { throw 'First SPiKE worker build failed.' }
$First = Get-WorkerManifest

Write-Host '[REPRO B] Building frozen DirectML worker...'
& $BuildScript -Clean
if (-not $?) { throw 'Second SPiKE worker build failed.' }
$Second = Get-WorkerManifest

$Difference = Compare-Object -ReferenceObject $First -DifferenceObject $Second
if ($Difference) {
    $Difference | Select-Object -First 20 | Format-Table | Out-String |
        Write-Host
    throw 'Frozen SPiKE worker did not reproduce byte-for-byte.'
}
Write-Host "[OK] Frozen SPiKE worker reproduced $($Second.Count) files byte-for-byte."
