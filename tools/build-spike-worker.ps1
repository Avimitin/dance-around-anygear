[CmdletBinding()]
param(
    [string] $Uv = 'uv',
    [switch] $Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
$Python = Join-Path $RuntimeRoot '.venv\Scripts\python.exe'
$BuildRoot = Join-Path $RepositoryRoot 'build\spike-worker'
$Spec = Join-Path $RuntimeRoot 'worker.spec'

if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to build the SPiKE worker.'
}
foreach ($Required in @(
    (Join-Path $RuntimeRoot 'pyproject.toml'),
    (Join-Path $RuntimeRoot 'uv.lock'),
    $Spec)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "SPiKE worker build input is absent: $Required"
    }
}

if ($Clean -and (Test-Path -LiteralPath $BuildRoot)) {
    $Resolved = (Resolve-Path -LiteralPath $BuildRoot).Path
    $ExpectedPrefix = (Join-Path $RepositoryRoot 'build').TrimEnd('\') + '\'
    if (-not $Resolved.StartsWith(
            $ExpectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a directory outside build: $Resolved"
    }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null

Write-Host '[SYNC] Restoring the locked Python/DirectML worker environment...'
& $Uv sync --project $RuntimeRoot --frozen --no-default-groups --group build
if ($LASTEXITCODE -ne 0) {
    throw "uv sync failed with exit code $LASTEXITCODE."
}
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
    throw "Locked SPiKE worker Python is absent: $Python"
}

$PreviousEpoch = $env:SOURCE_DATE_EPOCH
$PreviousHashSeed = $env:PYTHONHASHSEED
try {
    $env:SOURCE_DATE_EPOCH = (& git -C $RepositoryRoot log -1 --format=%ct).Trim()
    $env:PYTHONHASHSEED = '0'
    $DistRoot = Join-Path $BuildRoot 'dist'
    $WorkRoot = Join-Path $BuildRoot 'work'
    Write-Host '[BUILD] Freezing the out-of-process SPiKE worker...'
    & $Uv run --project $RuntimeRoot --frozen `
        --no-default-groups --group build `
        python -m PyInstaller --noconfirm --clean `
        --distpath $DistRoot --workpath $WorkRoot `
        $Spec
    if ($LASTEXITCODE -ne 0) {
        throw "PyInstaller failed with exit code $LASTEXITCODE."
    }
}
finally {
    $env:SOURCE_DATE_EPOCH = $PreviousEpoch
    $env:PYTHONHASHSEED = $PreviousHashSeed
}

$WorkerRoot = Join-Path $BuildRoot `
    'dist\dance_around_anygear_spike_worker'
$Worker = Join-Path $WorkerRoot 'dance_around_anygear_spike_worker.exe'
if (-not (Test-Path -LiteralPath $Worker -PathType Leaf)) {
    throw "Frozen SPiKE worker is absent: $Worker"
}
$Smoke = Start-Process -FilePath $Worker -ArgumentList '--self-test' `
    -PassThru -WindowStyle Hidden
if (-not $Smoke.WaitForExit(10000)) {
    Stop-Process -Id $Smoke.Id -Force -ErrorAction SilentlyContinue
    throw 'Frozen SPiKE worker smoke test timed out.'
}
if ($Smoke.ExitCode -ne 0) {
    throw "Frozen SPiKE worker smoke test failed with exit code $($Smoke.ExitCode)."
}

function Copy-WorkerNotice {
    param(
        [Parameter(Mandatory)][string] $Source,
        [Parameter(Mandatory)][string] $Name
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "SPiKE worker notice is absent: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $WorkerRoot $Name) `
        -Force
}

$SitePackages = Join-Path $RuntimeRoot '.venv\Lib\site-packages'
$PythonBase = (& $Python -c 'import sys; print(sys.base_prefix)').Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($PythonBase)) {
    throw 'Unable to resolve the locked Python base directory.'
}
$DistributionNotices = [ordered]@{
    'AUTHORS.cc3d.txt' = 'connected_components_3d-*.dist-info\licenses\AUTHORS'
    'LICENSE.cc3d-gpl.txt' = 'connected_components_3d-*.dist-info\licenses\COPYING'
    'LICENSE.cc3d-lgpl.txt' = 'connected_components_3d-*.dist-info\licenses\COPYING.LESSER'
    'LICENSE.numpy.txt' = 'numpy-*.dist-info\LICENSE.txt'
    'LICENSE.pyinstaller.txt' = 'pyinstaller-*.dist-info\licenses\COPYING.txt'
    'LICENSE.setuptools.txt' = 'setuptools-*.dist-info\licenses\LICENSE'
}
foreach ($Name in $DistributionNotices.Keys) {
    $Matches = @(Get-ChildItem -Path (Join-Path $SitePackages `
        $DistributionNotices[$Name]) -File -ErrorAction SilentlyContinue)
    if ($Matches.Count -ne 1) {
        throw "Expected one notice for $Name, found $($Matches.Count)."
    }
    Copy-WorkerNotice -Source $Matches[0].FullName -Name $Name
}
Copy-WorkerNotice `
    -Source (Join-Path $SitePackages 'onnxruntime\LICENSE') `
    -Name 'LICENSE.onnxruntime.txt'
Copy-WorkerNotice `
    -Source (Join-Path $SitePackages 'onnxruntime\ThirdPartyNotices.txt') `
    -Name 'NOTICE.onnxruntime.txt'
Copy-WorkerNotice `
    -Source (Join-Path $PythonBase 'LICENSE.txt') `
    -Name 'LICENSE.python.txt'
Copy-WorkerNotice `
    -Source (Join-Path $RepositoryRoot 'third_party\spike\LICENSE.MIT') `
    -Name 'LICENSE.spike.txt'

$Files = @(Get-ChildItem -LiteralPath $WorkerRoot -Recurse -File)
$Bytes = ($Files | Measure-Object -Property Length -Sum).Sum
Write-Host '[OK] Frozen SPiKE worker built.'
Write-Host "     Path : $Worker"
Write-Host "     Files: $($Files.Count)"
Write-Host ('     Size : {0:N1} MiB' -f ($Bytes / 1MB))
Write-Host "     SHA-256: $((Get-FileHash -LiteralPath $Worker -Algorithm SHA256).Hash)"
