[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [Alias('GameRoot')]
    [string] $CabinetRoot,
    [ValidateSet('kinect', 'webcam', 'steamvr', 'd4xx', 'd4xx-spike')]
    [string] $Backend = 'kinect',
    [string] $MediaPipeRoot,
    [string] $OpenVrRoot,
    [string] $SpikeWorkerRoot,
    [string] $SpikeModel,
    [string] $RealSenseRuntime,
    [string] $NvidiaRuntimeRoot,
    [string] $CudaRoot,
    [string] $WindowSize,
    [string] $WindowPosition,
    [ValidateSet('spice-local', 'external')]
    [string] $EamuseMode = 'spice-local',
    [string] $EamuseUrl,
    [switch] $GenerateLauncher
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (@(Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'dancearound.exe is running. Stop it before installing Anygear files.'
}
$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$CabinetRoot = (Resolve-Path -LiteralPath $CabinetRoot).Path
$SpiceExe = Join-Path $CabinetRoot 'game\dancearound.exe'
$VendorVp4u = Join-Path $CabinetRoot 'game\dancearound_data\plugins\x86_64\visionposewrapper.dll'
foreach ($required in @($SpiceExe, $VendorVp4u)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Expected game file not found: $required"
    }
}

$BinRoot = Join-Path $RepositoryRoot 'build\bin'
$BackendStem = $Backend.Replace('-', '_')
$PluginName = "dance_around_anygear_$BackendStem.dll"
$PluginSource = Join-Path $BinRoot $PluginName
foreach ($required in @($PluginSource)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Build output not found: $required"
    }
}

$MediaPipeSources = [ordered]@{}
$OpenVrSources = [ordered]@{}
$SpikeSources = [ordered]@{}
$D4xxConfigJson = $null
if ($Backend -eq 'd4xx') {
    $ConfigSource = Join-Path $RepositoryRoot `
        'config\dance_around_anygear_d4xx.json'
    $D4xxConfig = Get-Content -LiteralPath $ConfigSource -Raw |
        ConvertFrom-Json
    $HasNvidiaRuntime = -not [string]::IsNullOrWhiteSpace($NvidiaRuntimeRoot)
    $HasCuda = -not [string]::IsNullOrWhiteSpace($CudaRoot)
    if ($HasNvidiaRuntime -xor $HasCuda) {
        throw 'NvidiaRuntimeRoot and CudaRoot must be supplied together.'
    }
    if ($HasNvidiaRuntime) {
        $NvidiaRuntimeRoot = (Resolve-Path -LiteralPath `
            $NvidiaRuntimeRoot -ErrorAction Stop).Path
        $CudaRoot = (Resolve-Path -LiteralPath $CudaRoot -ErrorAction Stop).Path
        foreach ($Required in @(
            (Join-Path $NvidiaRuntimeRoot 'TensorRT-7.2.1.6\lib\nvinfer.dll'),
            (Join-Path $NvidiaRuntimeRoot 'cudnn-8.0.4.30\bin\cudnn64_8.dll'),
            (Join-Path $CudaRoot 'bin\cudart64_110.dll')
        )) {
            if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
                throw "D4xx NVIDIA runtime file not found: $Required"
            }
        }
        $D4xxConfig.D4xxNativeBridge.NvidiaRuntimeRoot = $NvidiaRuntimeRoot
        $D4xxConfig.D4xxNativeBridge.CudaRoot = $CudaRoot
    }
    $D4xxConfigJson = $D4xxConfig | ConvertTo-Json -Depth 8
}
if ($Backend -eq 'd4xx-spike') {
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
    $SpikeConfigSource = Join-Path $RepositoryRoot `
        'config\dance_around_anygear_d4xx_spike.json'
    $LibrealsenseLicense = Join-Path $RepositoryRoot `
        '.deps\librealsense\v2.50.0\LICENSE.librealsense.txt'
    $SpikeSources['worker'] = Join-Path $SpikeWorkerRoot `
        'dance_around_anygear_spike_worker.exe'
    $SpikeSources['model'] = $SpikeModel
    $SpikeSources['realsense'] = $RealSenseRuntime
    $SpikeSources['config'] = $SpikeConfigSource
    $SpikeSources['license'] = $LibrealsenseLicense
    foreach ($Source in $SpikeSources.Values) {
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "D4xx/SPiKE dependency not found: $Source"
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
            throw "D4xx/SPiKE dependency mismatch: $Path"
        }
    }
}
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
    foreach ($Name in $DependencyHashes.Keys) {
        $Source = Join-Path $MediaPipeRoot $Name
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "MediaPipe dependency not found: $Source"
        }
        $ActualHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        if ($ActualHash -ne $DependencyHashes[$Name]) {
            throw "MediaPipe dependency hash mismatch for $Name`: $ActualHash"
        }
        $MediaPipeSources[$Name] = $Source
    }
}
if ($Backend -eq 'steamvr') {
    if ([string]::IsNullOrWhiteSpace($OpenVrRoot)) {
        $OpenVrRoot = Join-Path $RepositoryRoot '.deps\openvr\v2.15.6'
    }
    $OpenVrRoot = (Resolve-Path -LiteralPath $OpenVrRoot).Path
    $DependencyHashes = [ordered]@{
        'bin\win64\openvr_api.dll' = @{
            Name = 'openvr_api.dll'
            Sha256 = 'BAB8AC6EF64E68A9CA53315B0014D131088584B2EFDFA6DB511D67EC03CFCB4A'
        }
        'LICENSE' = @{
            Name = 'LICENSE.openvr.txt'
            Sha256 = '9E6D1480FB68E86CEAFED312F7E67DADCDC2A99B350B710D624B8F0F0F1A2329'
        }
    }
    foreach ($RelativePath in $DependencyHashes.Keys) {
        $Source = Join-Path $OpenVrRoot $RelativePath
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "OpenVR dependency not found: $Source"
        }
        $ActualHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        if ($ActualHash -ne $DependencyHashes[$RelativePath].Sha256) {
            throw "OpenVR dependency hash mismatch for $RelativePath`: $ActualHash"
        }
        $OpenVrSources[$DependencyHashes[$RelativePath].Name] = $Source
    }
}
if ($Backend -eq 'd4xx-spike') {
    $DependencyDestination = Join-Path $CabinetRoot `
        'dance_around_anygear_d4xx_spike'
    if (Test-Path -LiteralPath $DependencyDestination) {
        $Resolved = (Resolve-Path -LiteralPath $DependencyDestination).Path
        $ExpectedPrefix = $CabinetRoot.TrimEnd('\') + '\'
        if (-not $Resolved.StartsWith(
                $ExpectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to replace dependency directory: $Resolved"
        }
        Remove-Item -LiteralPath $Resolved -Recurse -Force
    }
    $WorkerDestination = Join-Path $DependencyDestination 'worker'
    New-Item -ItemType Directory -Path $WorkerDestination -Force | Out-Null
    Copy-Item -Path (Join-Path $SpikeWorkerRoot '*') `
        -Destination $WorkerDestination -Recurse -Force
    Copy-Item -LiteralPath $SpikeModel -Destination (Join-Path `
        $DependencyDestination 'spike-itop-side-primary-fp16.onnx') -Force
    Copy-Item -LiteralPath $RealSenseRuntime -Destination (Join-Path `
        $DependencyDestination 'realsense2.dll') -Force
    Copy-Item -LiteralPath $LibrealsenseLicense -Destination (Join-Path `
        $DependencyDestination 'LICENSE.librealsense.txt') -Force
    Copy-Item -LiteralPath $SpikeConfigSource -Destination (Join-Path `
        $CabinetRoot 'dance_around_anygear_d4xx_spike.json') -Force
}
# All inputs have now been validated. Only start changing the cabinet tree
# after a missing or mismatched dependency can no longer leave a partial install.
Copy-Item -LiteralPath $PluginSource -Destination (Join-Path $CabinetRoot $PluginName) -Force
if ($Backend -eq 'd4xx') {
    $ConfigDestination = Join-Path $CabinetRoot `
        'dance_around_anygear_d4xx.json'
    [IO.File]::WriteAllText(
        $ConfigDestination,
        $D4xxConfigJson + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}
if ($Backend -eq 'webcam') {
    $DependencyDestination = Join-Path $CabinetRoot 'dance_around_anygear_webcam'
    New-Item -ItemType Directory -Path $DependencyDestination -Force | Out-Null
    foreach ($Name in $MediaPipeSources.Keys) {
        Copy-Item -LiteralPath $MediaPipeSources[$Name] `
            -Destination $DependencyDestination -Force
    }
}
if ($Backend -eq 'steamvr') {
    $DependencyDestination = Join-Path $CabinetRoot 'dance_around_anygear_steamvr'
    New-Item -ItemType Directory -Path $DependencyDestination -Force | Out-Null
    foreach ($Name in $OpenVrSources.Keys) {
        Copy-Item -LiteralPath $OpenVrSources[$Name] `
            -Destination (Join-Path $DependencyDestination $Name) -Force
    }
}

Write-Host '[OK] Anygear installed without changing the vendor VP4U DLL.'
Write-Host "     Plugin : $(Join-Path $CabinetRoot $PluginName)"
Write-Host "     Spice argument: -k $PluginName"

if ($GenerateLauncher) {
    $LauncherArguments = @{
        CabinetRoot = $CabinetRoot
        Backend = $Backend
        EamuseMode = $EamuseMode
    }
    if (-not [string]::IsNullOrWhiteSpace($WindowSize)) {
        $LauncherArguments.WindowSize = $WindowSize
    }
    if (-not [string]::IsNullOrWhiteSpace($WindowPosition)) {
        $LauncherArguments.WindowPosition = $WindowPosition
    }
    if (-not [string]::IsNullOrWhiteSpace($EamuseUrl)) {
        $LauncherArguments.EamuseUrl = $EamuseUrl
    }
    & (Join-Path $PSScriptRoot 'generate-launcher.ps1') @LauncherArguments
}
