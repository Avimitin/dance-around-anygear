[CmdletBinding()]
param(
    [switch] $Download,
    [string] $SourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DependencyParent = Join-Path $RepositoryRoot '.deps\openvr'
$DefaultRoot = Join-Path $DependencyParent 'v2.15.6'
$ExpectedCommit = '0924064316de3effbcd1acf1e309182a2deb1c05'
$ExpectedFiles = [ordered]@{
    'headers\openvr_capi.h' = @{
        Bytes = 183736
        Sha256 = '2BA5AC520F344E2359DD25103975E9ACA9C0011A38E84789113DADD51F386A41'
    }
    'bin\win64\openvr_api.dll' = @{
        Bytes = 837272
        Sha256 = 'BAB8AC6EF64E68A9CA53315B0014D131088584B2EFDFA6DB511D67EC03CFCB4A'
    }
    'LICENSE' = @{
        Bytes = 1515
        Sha256 = '9E6D1480FB68E86CEAFED312F7E67DADCDC2A99B350B710D624B8F0F0F1A2329'
    }
}

function Test-OpenVrRoot {
    param([Parameter(Mandatory)][string] $Root)

    $ResolvedRoot = (Resolve-Path -LiteralPath $Root).Path
    foreach ($RelativePath in $ExpectedFiles.Keys) {
        $Path = Join-Path $ResolvedRoot $RelativePath
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "OpenVR SDK file is missing: $Path"
        }
        $Item = Get-Item -LiteralPath $Path
        if ($Item.Length -ne $ExpectedFiles[$RelativePath].Bytes) {
            throw "OpenVR SDK size mismatch: $Path ($($Item.Length))"
        }
        $Hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        if ($Hash -ne $ExpectedFiles[$RelativePath].Sha256) {
            throw "OpenVR SDK hash mismatch: $Path ($Hash)"
        }
    }

    $GitDirectory = Join-Path $ResolvedRoot '.git'
    if (Test-Path -LiteralPath $GitDirectory) {
        $Commit = (& git -C $ResolvedRoot rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $Commit -ne $ExpectedCommit) {
            throw "OpenVR source commit mismatch: $Commit"
        }
    }
    return $ResolvedRoot
}

if (-not [string]::IsNullOrWhiteSpace($SourceRoot)) {
    $VerifiedRoot = Test-OpenVrRoot -Root $SourceRoot
    Write-Host "[OK] Pinned OpenVR SDK verified: $VerifiedRoot"
    return
}

if (Test-Path -LiteralPath $DefaultRoot) {
    $VerifiedRoot = Test-OpenVrRoot -Root $DefaultRoot
    Write-Host "[OK] Pinned OpenVR SDK verified: $VerifiedRoot"
    return
}

if (-not $Download) {
    throw 'OpenVR SDK is absent. Run bootstrap-openvr.ps1 -Download or supply -SourceRoot.'
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git is required to fetch the pinned OpenVR source.'
}
New-Item -ItemType Directory -Path $DependencyParent -Force | Out-Null
$TemporaryRoot = Join-Path $DependencyParent ('.bootstrap-' + [Guid]::NewGuid().ToString('N'))
try {
    & git clone --filter=blob:none --no-checkout `
        'https://github.com/ValveSoftware/openvr.git' $TemporaryRoot
    if ($LASTEXITCODE -ne 0) {
        throw "OpenVR clone failed with exit code $LASTEXITCODE."
    }
    & git -C $TemporaryRoot checkout --detach $ExpectedCommit
    if ($LASTEXITCODE -ne 0) {
        throw "OpenVR checkout failed with exit code $LASTEXITCODE."
    }
    Test-OpenVrRoot -Root $TemporaryRoot | Out-Null
    Move-Item -LiteralPath $TemporaryRoot -Destination $DefaultRoot
}
finally {
    if (Test-Path -LiteralPath $TemporaryRoot) {
        $ResolvedTemporary = (Resolve-Path -LiteralPath $TemporaryRoot).Path
        $ResolvedParent = (Resolve-Path -LiteralPath $DependencyParent).Path
        $ExpectedPrefix = $ResolvedParent.TrimEnd('\') + '\'
        if (-not $ResolvedTemporary.StartsWith(
                $ExpectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove an unexpected temporary path: $ResolvedTemporary"
        }
        Remove-Item -LiteralPath $ResolvedTemporary -Recurse -Force
    }
}

$VerifiedRoot = Test-OpenVrRoot -Root $DefaultRoot
Write-Host "[OK] Pinned OpenVR SDK ready: $VerifiedRoot"
