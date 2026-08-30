[CmdletBinding()]
param(
    [ValidateSet('kinect', 'webcam', 'steamvr', 'd4xx', 'd4xx-spike')]
    [string] $Backend = 'kinect',
    [string] $Version = '0.4.0',
    [string] $MediaPipeRoot,
    [string] $OpenVrRoot,
    [string] $SpikeWorkerRoot,
    [string] $SpikeModel,
    [string] $RealSenseRuntime
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepositoryRoot 'build'
$BinRoot = Join-Path $BuildRoot 'bin'
$DistRoot = Join-Path $RepositoryRoot 'dist'
$PackageName = "dance-around-anygear-v$Version-$Backend-win64"
$StageRoot = Join-Path $DistRoot $PackageName
$ZipPath = Join-Path $DistRoot "$PackageName.zip"

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Value
    )
    $Normalized = $Value.TrimEnd("`r", "`n") + "`n"
    [IO.File]::WriteAllText(
        $Path, $Normalized, [Text.UTF8Encoding]::new($false))
}

function New-DeterministicZip {
    param(
        [Parameter(Mandatory)][string] $SourceRoot,
        [Parameter(Mandatory)][string] $Destination
    )

    Add-Type -AssemblyName System.IO.Compression
    $Output = [IO.File]::Open(
        $Destination, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    $Archive = [IO.Compression.ZipArchive]::new(
        $Output, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        $FixedTime = [DateTimeOffset]::new(
            1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $Files = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File |
            Sort-Object FullName
        foreach ($File in $Files) {
            $EntryName = $File.FullName.Substring(
                $DistRoot.Length + 1).Replace('\', '/')
            $Entry = $Archive.CreateEntry(
                $EntryName, [IO.Compression.CompressionLevel]::Optimal)
            $Entry.LastWriteTime = $FixedTime
            $Input = [IO.File]::OpenRead($File.FullName)
            $EntryStream = $Entry.Open()
            try {
                $Input.CopyTo($EntryStream)
            }
            finally {
                $EntryStream.Dispose()
                $Input.Dispose()
            }
        }
    }
    finally {
        $Archive.Dispose()
        $Output.Dispose()
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $BuildRoot 'build-manifest.json') -PathType Leaf)) {
    throw 'Build manifest missing. Run tools/build.ps1 and tools/test.ps1 first.'
}
New-Item -ItemType Directory -Path $DistRoot -Force | Out-Null
foreach ($target in @($StageRoot, $ZipPath)) {
    if (Test-Path -LiteralPath $target) {
        $resolved = (Resolve-Path -LiteralPath $target).Path
        $expectedPrefix = $DistRoot.TrimEnd('\') + '\'
        if (-not $resolved.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to replace path outside dist: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$BackendStem = $Backend.Replace('-', '_')
$PluginName = "dance_around_anygear_$BackendStem.dll"
Copy-Item -LiteralPath (Join-Path $BinRoot $PluginName) -Destination $StageRoot

if ($Backend -eq 'webcam') {
    if ([string]::IsNullOrWhiteSpace($MediaPipeRoot)) {
        $MediaPipeRoot = Join-Path $RepositoryRoot '.deps\mediapipe\v1.0.0\windows-x86_64'
    }
    $MediaPipeRoot = (Resolve-Path -LiteralPath $MediaPipeRoot).Path
    $DependencyHashes = [ordered]@{
        'libmediapipe.dll' = 'A8970C645C8C87C25EC9965CB5C898E803C6C42F7192B7DE9A0541C62AE48CEF'
        'pose_landmarker_lite.task' = '59929E1D1EE95287735DDD833B19CF4AC46D29BC7AFDDBBF6753C459690D574A'
        'LICENSE.mediapipe.txt' = '8707EEF0533987EFC5B155D64761EEB6E20793F50B9BD1A68DAD1CF4719D0ED8'
        'NOTICE.mediapipe.txt' = 'D3B4A80A24A01FD445D4B70A610FD836EC3547C3A62EB835A1041956C38D9F56'
    }
    $DependencyDestination = Join-Path $StageRoot 'dance_around_anygear_webcam'
    New-Item -ItemType Directory -Path $DependencyDestination -Force | Out-Null
    foreach ($Name in $DependencyHashes.Keys) {
        $Source = Join-Path $MediaPipeRoot $Name
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "MediaPipe dependency missing: $Source"
        }
        $ActualHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        if ($ActualHash -ne $DependencyHashes[$Name]) {
            throw "MediaPipe dependency hash mismatch for $Name`: $ActualHash"
        }
        Copy-Item -LiteralPath $Source -Destination $DependencyDestination
    }
    $Readme = @"
dance-around-anygear $Version - MediaPipe USB webcam

Copy the full extracted layout beside Spice and load only:
    -k $PluginName

Spice loads only the Anygear DLL. It locates libmediapipe.dll and the Pose
Landmarker model in .\dance_around_anygear_webcam by absolute path. Python,
OpenCV, a helper process, and network access are not required at runtime.

The MediaPipe runtime and Pose Landmarker model are distributed under
Apache License 2.0. The upstream LICENSE and NOTICE files are included in
the dependency directory.

Logs are compact: stage 1/4..4/4 cover Spice loading/redirection and runtime
1/5..5/5 cover VP4U entry, API table, MediaPipe init, USB camera, and analysis.
"@
} elseif ($Backend -eq 'steamvr') {
    if ([string]::IsNullOrWhiteSpace($OpenVrRoot)) {
        $OpenVrRoot = Join-Path $RepositoryRoot '.deps\openvr\v2.15.6'
    }
    $OpenVrRoot = (Resolve-Path -LiteralPath $OpenVrRoot).Path
    $OpenVrDependencies = [ordered]@{
        'bin\win64\openvr_api.dll' = @{
            Name = 'openvr_api.dll'
            Sha256 = 'BAB8AC6EF64E68A9CA53315B0014D131088584B2EFDFA6DB511D67EC03CFCB4A'
        }
        'LICENSE' = @{
            Name = 'LICENSE.openvr.txt'
            Sha256 = '9E6D1480FB68E86CEAFED312F7E67DADCDC2A99B350B710D624B8F0F0F1A2329'
        }
    }
    $DependencyDestination = Join-Path $StageRoot 'dance_around_anygear_steamvr'
    New-Item -ItemType Directory -Path $DependencyDestination -Force | Out-Null
    foreach ($RelativePath in $OpenVrDependencies.Keys) {
        $Source = Join-Path $OpenVrRoot $RelativePath
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "OpenVR dependency missing: $Source"
        }
        $ActualHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        if ($ActualHash -ne $OpenVrDependencies[$RelativePath].Sha256) {
            throw "OpenVR dependency hash mismatch for $RelativePath`: $ActualHash"
        }
        Copy-Item -LiteralPath $Source -Destination (Join-Path `
            $DependencyDestination $OpenVrDependencies[$RelativePath].Name)
    }
    $Readme = @"
dance-around-anygear $Version - SteamVR full-body tracked poses

Requirements:
- Spice build with DANCE aROUND support
- SteamVR with an HMD and left/right controllers
- Waist, left-foot, and right-foot trackers assigned in SteamVR's
  Manage Trackers screen (strict six-point profile)
- Original, unmodified game files

Copy the full extracted layout beside Spice and load only:
    -k $PluginName

Spice loads only the Anygear DLL. It loads the pinned OpenVR client runtime
from .\dance_around_anygear_steamvr and reads standing-space poses. RGB,
MediaPipe, a render context, a helper process, and runtime downloads are not
used. Chest, shoulder, elbow, and knee tracker roles are consumed when present.

Start SteamVR and confirm every tracker role before starting the game. Logs
show the resolved device-role table and transition between TRACKED and
NOT TRACKED. A missing required role remains NOT TRACKED by design.
"@
} elseif ($Backend -eq 'd4xx-spike') {
    if ([string]::IsNullOrWhiteSpace($SpikeWorkerRoot)) {
        $SpikeWorkerRoot = Join-Path $RepositoryRoot `
            'build\spike-worker\dist\dance_around_anygear_spike_worker'
    }
    if ([string]::IsNullOrWhiteSpace($SpikeModel)) {
        $SpikeModel = Join-Path $RepositoryRoot `
            '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\spike-itop-side-primary-fp16.onnx'
    }
    if ([string]::IsNullOrWhiteSpace($RealSenseRuntime)) {
        $RealSenseRuntime = Join-Path $RepositoryRoot `
            '.deps\librealsense\v2.50.0\windows-x86_64\realsense2.dll'
    }
    $SpikeWorkerRoot = (Resolve-Path -LiteralPath $SpikeWorkerRoot `
        -ErrorAction Stop).Path
    $SpikeModel = (Resolve-Path -LiteralPath $SpikeModel -ErrorAction Stop).Path
    $RealSenseRuntime = (Resolve-Path -LiteralPath $RealSenseRuntime `
        -ErrorAction Stop).Path
    $ConfigSource = Join-Path $RepositoryRoot `
        'config\dance_around_anygear_d4xx_spike.json'
    $LibrealsenseLicense = Join-Path $RepositoryRoot `
        '.deps\librealsense\v2.50.0\LICENSE.librealsense.txt'
    $Worker = Join-Path $SpikeWorkerRoot `
        'dance_around_anygear_spike_worker.exe'
    $WorkerRequirements = @(
        $Worker,
        (Join-Path $SpikeWorkerRoot `
            '_internal\onnxruntime\capi\DirectML.dll'),
        (Join-Path $SpikeWorkerRoot `
            '_internal\onnxruntime\capi\onnxruntime.dll'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.onnxruntime.txt'),
        (Join-Path $SpikeWorkerRoot 'NOTICE.onnxruntime.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.spike.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.python.txt'),
        (Join-Path $SpikeWorkerRoot 'AUTHORS.cc3d.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.cc3d-gpl.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.cc3d-lgpl.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.numpy.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.pyinstaller.txt'),
        (Join-Path $SpikeWorkerRoot 'LICENSE.setuptools.txt')
    )
    foreach ($Required in @(
        $ConfigSource, $SpikeModel, $RealSenseRuntime,
        $LibrealsenseLicense) + $WorkerRequirements) {
        if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
            throw "D4xx/SPiKE package input is absent: $Required"
        }
    }
    $PinnedFiles = [ordered]@{
        $SpikeModel = @{
            Bytes = 143295911
            Sha256 = 'C48C40FF94C8F358762DB296DA0117DCFF78D8C4AD2FA4DB575298979BF2DA0D'
        }
        $RealSenseRuntime = @{
            Bytes = 31955456
            Sha256 = 'C8B83F041C1D92C264A0BFCDA5C9F28197ED212F7FAC40237DF63FFD2D5D1C4A'
        }
        $LibrealsenseLicense = @{
            Bytes = 11352
            Sha256 = 'C7AA1FDF0E38C4827FEF17859DDBFAC800B8995F3EC875A06DD23C79135F956D'
        }
    }
    foreach ($Path in $PinnedFiles.Keys) {
        $Item = Get-Item -LiteralPath $Path
        $ActualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        if ($Item.Length -ne $PinnedFiles[$Path].Bytes -or
            $ActualHash -ne $PinnedFiles[$Path].Sha256) {
            throw "D4xx/SPiKE package input mismatch: $Path"
        }
    }
    $UnexpectedGpuRuntimes = @(Get-ChildItem -LiteralPath $SpikeWorkerRoot `
        -Recurse -File | Where-Object {
            $_.Name -match '^(torch|cudart|cublas|cudnn).*\.dll$'
        })
    if ($UnexpectedGpuRuntimes.Count -gt 0) {
        throw 'The DirectML worker unexpectedly contains a CUDA/PyTorch runtime.'
    }

    $DependencyDestination = Join-Path $StageRoot `
        'dance_around_anygear_d4xx_spike'
    $WorkerDestination = Join-Path $DependencyDestination 'worker'
    New-Item -ItemType Directory -Path $WorkerDestination -Force | Out-Null
    Copy-Item -Path (Join-Path $SpikeWorkerRoot '*') `
        -Destination $WorkerDestination -Recurse
    Copy-Item -LiteralPath $SpikeModel -Destination (Join-Path `
        $DependencyDestination 'spike-itop-side-primary-fp16.onnx')
    Copy-Item -LiteralPath $RealSenseRuntime -Destination (Join-Path `
        $DependencyDestination 'realsense2.dll')
    Copy-Item -LiteralPath $LibrealsenseLicense -Destination (Join-Path `
        $DependencyDestination 'LICENSE.librealsense.txt')
    Copy-Item -LiteralPath $ConfigSource -Destination $StageRoot
    $Readme = @"
dance-around-anygear $Version - depth-only D4xx/SPiKE research backend

Requirements:
- Two Intel RealSense D4xx depth modules connected through USB 3
- A DirectX 12 GPU supported by ONNX Runtime DirectML
- Original, unmodified game files

Copy the full extracted layout beside Spice and load only:
    -k $PluginName

The entry DLL opens both native Z16-only streams and starts the adjacent
worker. Keep the play area empty during the initial 45-frame background
calibration. The worker then isolates the player in metric point clouds and
executes the pinned SPiKE model through DirectML. RGB, host-visible Y8,
MediaPipe, Python, PyTorch, CUDA, TensorRT, runtime downloads, and a user launch
script are not required.

Keep dance_around_anygear_d4xx_spike.json beside the DLL. It contains camera
selection, room placement, person-isolation, cadence, and calibrated-fusion
settings. The default primary mode does not assume an unmeasured transform
between the two cameras; the second unit remains an acquisition fallback.

Startup logs report the four plugin stages, five VP4U stages, worker stages,
camera serial order, player acquisition/loss, and any worker fault.
"@
} elseif ($Backend -eq 'd4xx') {
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot `
        'config\dance_around_anygear_d4xx.json') -Destination $StageRoot
    $Readme = @"
dance-around-anygear $Version - native RealSense D4xx bridge

Requirements:
- Spice build with DANCE aROUND support and library-alias SDK support
- The game's original visionposewrapper.dll, visionpose.dll, models, and
  original VisionPose runtime dependencies
- Official librealsense runtime compatible with the original game

Load only:
    -k $PluginName

This DLL keeps the original VP4U, two-camera body association, stereo
triangulation, and stabilizer. It maps the wrapper's D435 COLOR/BGR8 request
to a D4xx infrared Y8 stream, maps depth alignment to the selected infrared
imager, and exposes a virtual BGR24 frame to the original inference pipeline.

The adjacent JSON is optional. With no JSON, infrared index 1 and stable
percentile contrast are used. OriginalWrapperPath should remain empty for an
unaltered game installation.
"@
} else {
    $Readme = @"
dance-around-anygear $Version - Kinect for Windows v1

Requirements:
- Spice build with DANCE aROUND support
- Kinect for Windows SDK 1.8 runtime (Kinect10.dll)
- Original, unmodified game files

Copy the DLL beside the Spice working directory and add this argument:
    -k $PluginName

The plugin contains the Kinect VP4U backend and redirects Unity at runtime.
It does not overwrite the game's visionposewrapper.dll and needs no project-
specific runtime files besides this DLL.

Startup logging is intentionally compact. "stage 1/4" through "stage 4/4"
cover Spice loading/redirection; "runtime 1/5" through "runtime 5/5" cover
Unity entering VP4U, the ABI table, initialization, Kinect open, and analysis.
When reporting a failure, include the last completed stage and the DLL hash.
"@
}
Write-Utf8NoBom -Path (Join-Path $StageRoot 'README.txt') -Value $Readme

$Files = Get-ChildItem -LiteralPath $StageRoot -Recurse -File | Sort-Object FullName
$Hashes = foreach ($file in $Files) {
    $relative = $file.FullName.Substring($StageRoot.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    "$hash  $relative"
}
[IO.File]::WriteAllText(
    (Join-Path $StageRoot 'SHA256SUMS'),
    (($Hashes -join "`n") + "`n"),
    [Text.Encoding]::ASCII)
New-DeterministicZip -SourceRoot $StageRoot -Destination $ZipPath
Write-Host "[OK] Package: $ZipPath"
Write-Host "     SHA-256: $((Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash)"
