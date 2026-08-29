[CmdletBinding()]
param(
    [ValidateSet('kinect', 'webcam')]
    [string] $Backend = 'kinect',
    [string] $Version = '0.2.0',
    [string] $MediaPipeRoot,
    [switch] $LocalEvaluation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepositoryRoot 'build'
$BinRoot = Join-Path $BuildRoot 'bin'
$DistRoot = Join-Path $RepositoryRoot 'dist'
if ($Backend -eq 'webcam' -and -not $LocalEvaluation) {
    throw 'Public webcam packaging is gated until the Pose Landmarker model redistribution terms are explicit. Use -LocalEvaluation for a local test bundle.'
}
$PackageName = "dance-around-anygear-v$Version-$Backend-win64"
if ($Backend -eq 'webcam') { $PackageName += '-local-evaluation' }
$StageRoot = Join-Path $DistRoot $PackageName
$ZipPath = Join-Path $DistRoot "$PackageName.zip"

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
$PluginName = "dance_around_anygear_$Backend.dll"
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
dance-around-anygear $Version - MediaPipe USB webcam LOCAL EVALUATION

Copy the full extracted layout beside Spice and load only:
    -k $PluginName

Spice loads only the Anygear DLL. It locates libmediapipe.dll and the Pose
Landmarker model in .\dance_around_anygear_webcam by absolute path. Python,
OpenCV, a helper process, and network access are not required at runtime.

This bundle is marked local-evaluation because Google publishes the model
download but not explicit redistribution terms for the separate .task file.
Do not publish this ZIP until that release gate is resolved.

Logs are compact: stage 1/4..4/4 cover Spice loading/redirection and runtime
1/5..5/5 cover VP4U entry, API table, MediaPipe init, USB camera, and analysis.
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
$Readme | Set-Content -LiteralPath (Join-Path $StageRoot 'README.txt') -Encoding UTF8

$Files = Get-ChildItem -LiteralPath $StageRoot -Recurse -File | Sort-Object FullName
$Hashes = foreach ($file in $Files) {
    $relative = $file.FullName.Substring($StageRoot.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    "$hash  $relative"
}
$Hashes | Set-Content -LiteralPath (Join-Path $StageRoot 'SHA256SUMS') -Encoding ASCII
Compress-Archive -LiteralPath $StageRoot -DestinationPath $ZipPath -CompressionLevel Optimal
Write-Host "[OK] Package: $ZipPath"
Write-Host "     SHA-256: $((Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash)"
