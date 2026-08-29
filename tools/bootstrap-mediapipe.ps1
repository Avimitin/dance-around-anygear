[CmdletBinding(DefaultParameterSetName = 'Download')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Download')]
    [switch] $Download,
    [Parameter(Mandatory, ParameterSetName = 'Offline')]
    [string] $Wheel,
    [Parameter(Mandatory, ParameterSetName = 'Offline')]
    [string] $Model,
    [Parameter(Mandatory, ParameterSetName = 'Offline')]
    [string] $TestImage,
    [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DownloadRoot = Join-Path $RepositoryRoot '.deps\downloads'
$DefaultDestination = Join-Path $RepositoryRoot '.deps\mediapipe\v1.0.0\windows-x86_64'
$TestDataDestination = Join-Path $RepositoryRoot '.deps\mediapipe\testdata'

$WheelSpec = [ordered]@{
    Name = 'mediapipe-1.0.0-py3-none-win_amd64.whl'
    Url = 'https://files.pythonhosted.org/packages/68/53/ffb67e668f23130aff197ec49be912be910c128b60658000d8bf263207c9/mediapipe-1.0.0-py3-none-win_amd64.whl'
    Sha256 = 'DA57E6719BBAB05007272C91D6CA2E0E2E370709491CBE344A372F87E25CF604'
}
$ModelSpec = [ordered]@{
    Name = 'pose_landmarker_lite.task'
    Url = 'https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_lite/float16/1/pose_landmarker_lite.task'
    Sha256 = '59929E1D1EE95287735DDD833B19CF4AC46D29BC7AFDDBBF6753C459690D574A'
}
$TestImageSpec = [ordered]@{
    Name = 'pose.jpg'
    Url = 'https://storage.googleapis.com/mediapipe-assets/tasks/testdata/vision/pose.jpg?generation=1782185208547605'
    Sha256 = 'C8A830ED683C0276D713DD5AEDA28F415F10CD6291972084A40D0D8B934ED62B'
}

function Get-VerifiedDownload([System.Collections.IDictionary] $Spec) {
    New-Item -ItemType Directory -Path $DownloadRoot -Force | Out-Null
    $Path = Join-Path $DownloadRoot $Spec.Name
    $CurrentHash = if (Test-Path -LiteralPath $Path -PathType Leaf) {
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    } else { '' }
    if ($CurrentHash -ne $Spec.Sha256) {
        $Temporary = "$Path.$([Guid]::NewGuid().ToString('N')).partial"
        try {
            Write-Host "[DOWNLOAD] $($Spec.Url)"
            Invoke-WebRequest -UseBasicParsing -Uri $Spec.Url -OutFile $Temporary
            $CurrentHash = (Get-FileHash -LiteralPath $Temporary -Algorithm SHA256).Hash
            if ($CurrentHash -ne $Spec.Sha256) {
                throw "Hash mismatch for $($Spec.Name). Expected $($Spec.Sha256), got $CurrentHash."
            }
            Move-Item -LiteralPath $Temporary -Destination $Path -Force
        }
        finally {
            Remove-Item -LiteralPath $Temporary -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "[CACHE] $Path"
    }
    return $Path
}

function Assert-Hash([string] $Path, [string] $Expected) {
    $Resolved = (Resolve-Path -LiteralPath $Path).Path
    $Actual = (Get-FileHash -LiteralPath $Resolved -Algorithm SHA256).Hash
    if ($Actual -ne $Expected) {
        throw "Hash mismatch for $Resolved. Expected $Expected, got $Actual."
    }
    return $Resolved
}

if ($Download) {
    $Wheel = Get-VerifiedDownload $WheelSpec
    $Model = Get-VerifiedDownload $ModelSpec
    $TestImage = Get-VerifiedDownload $TestImageSpec
} else {
    $Wheel = Assert-Hash $Wheel $WheelSpec.Sha256
    $Model = Assert-Hash $Model $ModelSpec.Sha256
    $TestImage = Assert-Hash $TestImage $TestImageSpec.Sha256
}

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = $DefaultDestination
}
$Destination = [IO.Path]::GetFullPath($Destination)
$DependencyPrefix = (Join-Path $RepositoryRoot '.deps').TrimEnd('\') + '\'
if (-not $Destination.StartsWith($DependencyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'MediaPipe bootstrap destination must be inside this repository .deps directory.'
}
if (Test-Path -LiteralPath $Destination) {
    $ResolvedDestination = (Resolve-Path -LiteralPath $Destination).Path
    if (-not $ResolvedDestination.StartsWith($DependencyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace dependency directory: $ResolvedDestination"
    }
    Remove-Item -LiteralPath $ResolvedDestination -Recurse -Force
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
New-Item -ItemType Directory -Path $TestDataDestination -Force | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [IO.Compression.ZipFile]::OpenRead($Wheel)
try {
    $Entries = [ordered]@{
        'mediapipe/tasks/c/libmediapipe.dll' = 'libmediapipe.dll'
        'mediapipe-1.0.0.dist-info/licenses/LICENSE' = 'LICENSE.mediapipe.txt'
        'mediapipe-1.0.0.dist-info/licenses/NOTICE' = 'NOTICE.mediapipe.txt'
    }
    foreach ($EntryName in $Entries.Keys) {
        $Entry = $Archive.GetEntry($EntryName)
        if (-not $Entry) { throw "Pinned wheel entry missing: $EntryName" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile(
            $Entry, (Join-Path $Destination $Entries[$EntryName]), $true)
    }
}
finally {
    $Archive.Dispose()
}
Copy-Item -LiteralPath $Model -Destination (Join-Path $Destination 'pose_landmarker_lite.task')
Copy-Item -LiteralPath $TestImage -Destination (Join-Path $TestDataDestination 'pose.jpg')

$ExpectedOutputs = [ordered]@{
    'libmediapipe.dll' = 'A8970C645C8C87C25EC9965CB5C898E803C6C42F7192B7DE9A0541C62AE48CEF'
    'pose_landmarker_lite.task' = $ModelSpec.Sha256
    'LICENSE.mediapipe.txt' = '8707EEF0533987EFC5B155D64761EEB6E20793F50B9BD1A68DAD1CF4719D0ED8'
    'NOTICE.mediapipe.txt' = 'D3B4A80A24A01FD445D4B70A610FD836EC3547C3A62EB835A1041956C38D9F56'
}
foreach ($Name in $ExpectedOutputs.Keys) {
    Assert-Hash (Join-Path $Destination $Name) $ExpectedOutputs[$Name] | Out-Null
}

Write-Host "[OK] MediaPipe v1.0.0 runtime prepared: $Destination"
Write-Host "     Test image: $(Join-Path $TestDataDestination 'pose.jpg')"
