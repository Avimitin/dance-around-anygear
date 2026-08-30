[CmdletBinding()]
param(
    [string] $Uv = 'uv',
    [switch] $FullModel,
    [string] $Checkpoint
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ResearchRoot = Join-Path $RepositoryRoot 'research\spike'
$RuntimeRoot = Join-Path $RepositoryRoot 'runtime\spike'
if (-not (Get-Command $Uv -ErrorAction SilentlyContinue)) {
    throw 'uv is required to test the SPiKE research environment.'
}
& $Uv sync --project $ResearchRoot --frozen
if ($LASTEXITCODE -ne 0) {
    throw "SPiKE research environment restore failed with exit code $LASTEXITCODE."
}
$Python = Join-Path $ResearchRoot '.venv\Scripts\python.exe'
$PreviousPythonPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = $RuntimeRoot
    & $Python (Join-Path $PSScriptRoot 'train-spike-teacher.py') `
        '--self-test'
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE training self-test failed with exit code $LASTEXITCODE."
    }
    & $Python -m py_compile `
        (Join-Path $PSScriptRoot 'train-spike-teacher.py') `
        (Join-Path $PSScriptRoot 'prepare-spike-teacher-dataset.py')
    if ($LASTEXITCODE -ne 0) {
        throw "SPiKE research source validation failed with exit code $LASTEXITCODE."
    }
    if ($FullModel) {
        if ([string]::IsNullOrWhiteSpace($Checkpoint)) {
            $Candidates = @(
                (Join-Path $RepositoryRoot `
                    '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\best_model.pth'),
                (Join-Path (Split-Path -Parent $RepositoryRoot) `
                    'SPiKE\weights\best_model.pth')
            )
            $Checkpoint = $Candidates |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
                Select-Object -First 1
        }
        if ([string]::IsNullOrWhiteSpace($Checkpoint)) {
            throw 'Published SPiKE checkpoint is absent. Run bootstrap-spike-runtime.ps1 first.'
        }
        $Checkpoint = (Resolve-Path -LiteralPath $Checkpoint).Path
        $ExpectedHash = 'FF29500C6747BB535476B7CAA5352F27CA62E97F5A748E59EDF646F7BC778E75'
        if ((Get-FileHash -LiteralPath $Checkpoint -Algorithm SHA256).Hash `
                -ne $ExpectedHash) {
            throw "Published SPiKE checkpoint hash mismatch: $Checkpoint"
        }
        & $Python (Join-Path $PSScriptRoot 'train-spike-teacher.py') `
            '--model-self-test' '--checkpoint' $Checkpoint '--device' 'cuda'
        if ($LASTEXITCODE -ne 0) {
            throw "Full SPiKE training-step test failed with exit code $LASTEXITCODE."
        }
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
Write-Host '[OK] Pinned SPiKE research environment and training losses passed.'
