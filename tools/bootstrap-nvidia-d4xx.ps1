[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $TensorRtZip,
    [Parameter(Mandatory)]
    [string] $CuDnnZip,
    [string] $CudaRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.1',
    [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$DependencyRoot = Join-Path $RepositoryRoot '.deps'
$DefaultDestination = Join-Path $DependencyRoot 'nvidia\tensorrt-7.2.1.6-cuda11.1-cudnn8.0.4.30'

$TensorRtSpec = [ordered]@{
    Name = 'TensorRT-7.2.1.6.Windows10.x86_64.cuda-11.1.cudnn8.0.zip'
    Bytes = 589099640
    Sha256 = '901C2385B0CB6C92B024F92F58E931DEE6460308B3754BCDCAEDD192560BD159'
    ArchiveRoot = 'TensorRT-7.2.1.6/'
}
$CuDnnSpec = [ordered]@{
    Name = 'cudnn-11.1-windows-x64-v8.0.4.30.zip'
    Bytes = 758009704
    Sha256 = '58BF1FB324D11088A28C3A9F71A38F5DFE167EFDF723815DCE1867BC03DDAEA2'
    ArchiveRoot = 'cuda/'
}

function Get-VerifiedArchive {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][System.Collections.IDictionary] $Spec
    )

    $Resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $Item = Get-Item -LiteralPath $Resolved
    if ($Item.Name -ne $Spec.Name) {
        throw "Expected archive name '$($Spec.Name)', got '$($Item.Name)'."
    }
    if ($Item.Length -ne $Spec.Bytes) {
        throw "Archive size mismatch for $Resolved. Expected $($Spec.Bytes), got $($Item.Length)."
    }
    $Hash = (Get-FileHash -LiteralPath $Resolved -Algorithm SHA256).Hash
    if ($Hash -ne $Spec.Sha256) {
        throw "Archive hash mismatch for $Resolved. Expected $($Spec.Sha256), got $Hash."
    }
    return $Resolved
}

function Assert-File {
    param([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file not found: $Path"
    }
}

function Expand-SelectedArchiveEntries {
    param(
        [Parameter(Mandatory)][string] $ArchivePath,
        [Parameter(Mandatory)][string] $ArchiveRoot,
        [Parameter(Mandatory)][string] $OutputRoot,
        [Parameter(Mandatory)][scriptblock] $Include
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $Archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $Selected = @($Archive.Entries | Where-Object {
            -not [string]::IsNullOrEmpty($_.Name) -and (& $Include $_.FullName)
        })
        if ($Selected.Count -eq 0) {
            throw "No selected entries found in $ArchivePath."
        }

        $OutputPrefix = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\') + '\'
        foreach ($Entry in $Selected) {
            if (-not $Entry.FullName.StartsWith($ArchiveRoot, [StringComparison]::Ordinal)) {
                throw "Unexpected archive entry outside '$ArchiveRoot': $($Entry.FullName)"
            }
            $RelativePath = $Entry.FullName.Substring($ArchiveRoot.Length).Replace('/', '\')
            $Target = [IO.Path]::GetFullPath((Join-Path $OutputRoot $RelativePath))
            if (-not $Target.StartsWith($OutputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Unsafe archive entry path: $($Entry.FullName)"
            }
            $Parent = Split-Path -Parent $Target
            New-Item -ItemType Directory -Path $Parent -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $Target, $true)
        }
    }
    finally {
        $Archive.Dispose()
    }
}

$TensorRtZip = Get-VerifiedArchive -Path $TensorRtZip -Spec $TensorRtSpec
$CuDnnZip = Get-VerifiedArchive -Path $CuDnnZip -Spec $CuDnnSpec
$CudaRoot = (Resolve-Path -LiteralPath $CudaRoot -ErrorAction Stop).Path
$CudaBin = Join-Path $CudaRoot 'bin'
$CudaFiles = @(
    'cudart64_110.dll',
    'cublas64_11.dll',
    'cublasLt64_11.dll',
    'nvrtc64_111_0.dll',
    'nvrtc-builtins64_111.dll'
)
foreach ($Name in $CudaFiles) {
    Assert-File -Path (Join-Path $CudaBin $Name)
}

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = $DefaultDestination
}
$Destination = [IO.Path]::GetFullPath($Destination)
$DependencyPrefix = [IO.Path]::GetFullPath($DependencyRoot).TrimEnd('\') + '\'
if (-not $Destination.StartsWith($DependencyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'NVIDIA bootstrap destination must be inside this repository .deps directory.'
}
if (Test-Path -LiteralPath $Destination) {
    $ResolvedDestination = (Resolve-Path -LiteralPath $Destination).Path
    if (-not ($ResolvedDestination.TrimEnd('\') + '\').StartsWith(
        $DependencyPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace dependency directory: $ResolvedDestination"
    }
    Remove-Item -LiteralPath $ResolvedDestination -Recurse -Force
}

$TensorRtRoot = Join-Path $Destination 'TensorRT-7.2.1.6'
$CuDnnRoot = Join-Path $Destination 'cudnn-8.0.4.30'
New-Item -ItemType Directory -Path $TensorRtRoot, $CuDnnRoot -Force | Out-Null

Write-Host "[EXTRACT] TensorRT 7.2.1.6 -> $TensorRtRoot"
Expand-SelectedArchiveEntries `
    -ArchivePath $TensorRtZip `
    -ArchiveRoot $TensorRtSpec.ArchiveRoot `
    -OutputRoot $TensorRtRoot `
    -Include {
        param($Name)
        $Name -match '^TensorRT-7\.2\.1\.6/(include/[^/]+\.h|lib/[^/]+\.(dll|lib)|doc/TensorRT-SLA\.pdf)$'
    }

Write-Host "[EXTRACT] cuDNN 8.0.4.30 -> $CuDnnRoot"
Expand-SelectedArchiveEntries `
    -ArchivePath $CuDnnZip `
    -ArchiveRoot $CuDnnSpec.ArchiveRoot `
    -OutputRoot $CuDnnRoot `
    -Include {
        param($Name)
        $Name -match '^cuda/(bin/[^/]+\.dll|include/[^/]+\.h|lib/x64/[^/]+\.lib|NVIDIA_SLA_cuDNN_Support\.txt)$'
    }

$RequiredOutputs = @(
    (Join-Path $TensorRtRoot 'lib\nvinfer.dll'),
    (Join-Path $TensorRtRoot 'lib\nvinfer_plugin.dll'),
    (Join-Path $TensorRtRoot 'lib\nvonnxparser.dll'),
    (Join-Path $TensorRtRoot 'lib\nvparsers.dll'),
    (Join-Path $TensorRtRoot 'include\NvInfer.h'),
    (Join-Path $CuDnnRoot 'bin\cudnn64_8.dll'),
    (Join-Path $CuDnnRoot 'bin\cudnn_adv_infer64_8.dll'),
    (Join-Path $CuDnnRoot 'bin\cudnn_cnn_infer64_8.dll'),
    (Join-Path $CuDnnRoot 'bin\cudnn_ops_infer64_8.dll'),
    (Join-Path $CuDnnRoot 'include\cudnn_version.h')
)
foreach ($Path in $RequiredOutputs) {
    Assert-File -Path $Path
}

$Files = @(
    Get-ChildItem -LiteralPath $TensorRtRoot, $CuDnnRoot -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($Destination.TrimEnd('\').Length + 1).Replace('\', '/')
                bytes = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        }
)
$CudaManifest = @(
    foreach ($Name in $CudaFiles) {
        $Path = Join-Path $CudaBin $Name
        $Item = Get-Item -LiteralPath $Path
        [ordered]@{
            path = $Path
            bytes = $Item.Length
            sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
            file_version = $Item.VersionInfo.FileVersion
        }
    }
)
$Manifest = [ordered]@{
    schema_version = 1
    generated_at = (Get-Date).ToUniversalTime().ToString('o')
    tensor_rt = [ordered]@{
        version = '7.2.1.6'
        cuda_build = '11.1'
        archive = $TensorRtZip
        archive_bytes = $TensorRtSpec.Bytes
        archive_sha256 = $TensorRtSpec.Sha256
        root = $TensorRtRoot
    }
    cudnn = [ordered]@{
        version = '8.0.4.30'
        cuda_build = '11.1'
        archive = $CuDnnZip
        archive_bytes = $CuDnnSpec.Bytes
        archive_sha256 = $CuDnnSpec.Sha256
        root = $CuDnnRoot
    }
    cuda = [ordered]@{
        root = $CudaRoot
        detected_toolkit = '11.1'
        files = $CudaManifest
    }
    extracted_files = $Files
}
$ManifestPath = Join-Path $Destination 'manifest.json'
[IO.File]::WriteAllText(
    $ManifestPath,
    ($Manifest | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false))

Write-Host "[OK] NVIDIA D4xx dependencies prepared: $Destination"
Write-Host "     Manifest: $ManifestPath"
Write-Host '     Run tools\diagnostics\test-nvidia-d4xx.ps1 before staging these files with the game.'
