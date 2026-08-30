[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $VisionPoseDll,
    [Parameter(Mandatory)]
    [string] $ProductKeyFile,
    [Parameter(Mandatory)]
    [string] $ConfigTemplate,
    [Parameter(Mandatory)]
    [string] $ModelDirectory,
    [ValidateSet(320, 512)]
    [int] $ModelSize = 320,
    [string] $RuntimeRoot,
    [string] $CudaRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.1'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue) {
    throw 'Refusing to probe VisionPose while dancearound.exe is running.'
}

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    $RuntimeRoot = Join-Path $RepositoryRoot `
        '.deps\nvidia\tensorrt-7.2.1.6-cuda11.1-cudnn8.0.4.30'
}
$VisionPoseDll = (Resolve-Path -LiteralPath $VisionPoseDll).Path
$ProductKeyFile = (Resolve-Path -LiteralPath $ProductKeyFile).Path
$ConfigTemplate = (Resolve-Path -LiteralPath $ConfigTemplate).Path
$ModelDirectory = (Resolve-Path -LiteralPath $ModelDirectory).Path
$RuntimeRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path
$CudaRoot = (Resolve-Path -LiteralPath $CudaRoot).Path

$ModelBase = Join-Path $ModelDirectory 'visionpose'
$ModelEngine = Join-Path $ModelDirectory "visionpose_${ModelSize}x${ModelSize}_16"
$TensorRtBin = Join-Path $RuntimeRoot 'TensorRT-7.2.1.6\lib'
$CuDnnBin = Join-Path $RuntimeRoot 'cudnn-8.0.4.30\bin'
$CudaBin = Join-Path $CudaRoot 'bin'
$PluginDirectory = Split-Path -Parent $VisionPoseDll

$LoadPlan = @(
    (Join-Path $CudaBin 'cudart64_110.dll'),
    (Join-Path $CudaBin 'cublasLt64_11.dll'),
    (Join-Path $CudaBin 'cublas64_11.dll'),
    (Join-Path $CudaBin 'nvrtc-builtins64_111.dll'),
    (Join-Path $CudaBin 'nvrtc64_111_0.dll'),
    (Join-Path $CuDnnBin 'cudnn_ops_infer64_8.dll'),
    (Join-Path $CuDnnBin 'cudnn_cnn_infer64_8.dll'),
    (Join-Path $CuDnnBin 'cudnn_adv_infer64_8.dll'),
    (Join-Path $CuDnnBin 'cudnn64_8.dll'),
    (Join-Path $TensorRtBin 'myelin64_1.dll'),
    (Join-Path $TensorRtBin 'nvinfer.dll'),
    (Join-Path $TensorRtBin 'nvinfer_plugin.dll'),
    (Join-Path $TensorRtBin 'nvonnxparser.dll'),
    (Join-Path $TensorRtBin 'nvparsers.dll')
)
foreach ($Required in @($ModelBase, $ModelEngine) + $LoadPlan) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required VisionPose runtime file not found: $Required"
    }
}

$ConfigText = [IO.File]::ReadAllText($ConfigTemplate)
$ModelPath = $ModelBase.Replace('\', '/')
$ConfigText = [regex]::Replace(
    $ConfigText, '<ModelPath>.*?</ModelPath>',
    "<ModelPath>$ModelPath</ModelPath>")
$ConfigText = [regex]::Replace(
    $ConfigText, '<ModelWidth>\d+</ModelWidth>',
    "<ModelWidth>$ModelSize</ModelWidth>")
$ConfigText = [regex]::Replace(
    $ConfigText, '<ModelHeight>\d+</ModelHeight>',
    "<ModelHeight>$ModelSize</ModelHeight>")
$ProbeDirectory = Join-Path ([IO.Path]::GetTempPath()) `
    ('anygear-visionpose-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $ProbeDirectory | Out-Null
$ProbeConfig = Join-Path $ProbeDirectory 'VisionPoseConfig.xml'
[IO.File]::WriteAllText(
    $ProbeConfig, $ConfigText, [Text.UTF8Encoding]::new($false))

$env:Path = "$PluginDirectory;$TensorRtBin;$CuDnnBin;$CudaBin;$env:Path"
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class AnygearVisionPoseProbe
{
    private const uint LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100;
    private const uint LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public delegate int InitializeDelegate(
        [MarshalAs(UnmanagedType.LPStr)] string productKey,
        [MarshalAs(UnmanagedType.LPStr)] string configPath);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Open3DDelegate();

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibraryEx(
        string fileName, IntPtr reserved, uint flags);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    public static IntPtr Load(string path)
    {
        IntPtr module = LoadLibraryEx(
            path, IntPtr.Zero,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == IntPtr.Zero)
            throw new Win32Exception(
                Marshal.GetLastWin32Error(), "Could not load " + path);
        return module;
    }

    private static IntPtr Procedure(IntPtr module, string name)
    {
        IntPtr address = GetProcAddress(module, name);
        if (address == IntPtr.Zero)
            throw new EntryPointNotFoundException(name);
        return address;
    }

    public static InitializeDelegate GetInitialize(IntPtr module)
    {
        return (InitializeDelegate)Marshal.GetDelegateForFunctionPointer(
            Procedure(module, "vpVisionPose_InitializeWithConfigPath_chr"),
            typeof(InitializeDelegate));
    }

    public static Open3DDelegate GetOpen3D(IntPtr module)
    {
        return (Open3DDelegate)Marshal.GetDelegateForFunctionPointer(
            Procedure(module, "vpVisionPose3D_Open"),
            typeof(Open3DDelegate));
    }
}
'@

foreach ($RuntimeFile in $LoadPlan) {
    [void][AnygearVisionPoseProbe]::Load($RuntimeFile)
}
$Module = [AnygearVisionPoseProbe]::Load($VisionPoseDll)
$Initialize = [AnygearVisionPoseProbe]::GetInitialize($Module)
$Open3D = [AnygearVisionPoseProbe]::GetOpen3D($Module)
$ProductKey = [IO.File]::ReadAllText($ProductKeyFile).Trim()

Write-Host "[PROBE] dll=$VisionPoseDll"
Write-Host "[PROBE] engine=$ModelEngine"
$InitializeResult = $Initialize.Invoke($ProductKey, $ProbeConfig)
Write-Host "[RESULT] initialize=$InitializeResult"
if ($InitializeResult -ne 1) { exit 20 }
$OpenResult = $Open3D.Invoke()
Write-Host "[RESULT] open3d=$OpenResult"
if ($OpenResult -ne 1) { exit 21 }
Write-Host '[OK] This VisionPose DLL accepted the selected engine.'
