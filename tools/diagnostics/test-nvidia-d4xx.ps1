[CmdletBinding()]
param(
    [string] $RuntimeRoot,
    [string] $CudaRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.1'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    $RuntimeRoot = Join-Path $RepositoryRoot '.deps\nvidia\tensorrt-7.2.1.6-cuda11.1-cudnn8.0.4.30'
}
$RuntimeRoot = (Resolve-Path -LiteralPath $RuntimeRoot -ErrorAction Stop).Path
$CudaRoot = (Resolve-Path -LiteralPath $CudaRoot -ErrorAction Stop).Path
$TensorRtBin = Join-Path $RuntimeRoot 'TensorRT-7.2.1.6\lib'
$CuDnnBin = Join-Path $RuntimeRoot 'cudnn-8.0.4.30\bin'
$CudaBin = Join-Path $CudaRoot 'bin'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class NativeLibraryProbe
{
    private const uint LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100;
    private const uint LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibraryEx(string fileName, IntPtr reserved, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    public static IntPtr Load(string path)
    {
        IntPtr module = LoadLibraryEx(
            path,
            IntPtr.Zero,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not load " + path);
        return module;
    }

    public static void RequireExport(IntPtr module, string name)
    {
        if (GetProcAddress(module, name) == IntPtr.Zero)
            throw new EntryPointNotFoundException(name);
    }

    public static void Release(IntPtr module)
    {
        if (module != IntPtr.Zero)
            FreeLibrary(module);
    }
}
'@

$LoadPlan = @(
    [pscustomobject]@{ Path = Join-Path $CudaBin 'cudart64_110.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CudaBin 'cublasLt64_11.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CudaBin 'cublas64_11.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CudaBin 'nvrtc-builtins64_111.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CudaBin 'nvrtc64_111_0.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CuDnnBin 'cudnn_ops_infer64_8.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CuDnnBin 'cudnn_cnn_infer64_8.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CuDnnBin 'cudnn_adv_infer64_8.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $CuDnnBin 'cudnn64_8.dll'; Exports = @('cudnnGetVersion') },
    [pscustomobject]@{ Path = Join-Path $TensorRtBin 'myelin64_1.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $TensorRtBin 'nvinfer.dll'; Exports = @('createInferBuilder_INTERNAL', 'createInferRuntime_INTERNAL') },
    [pscustomobject]@{ Path = Join-Path $TensorRtBin 'nvinfer_plugin.dll'; Exports = @('initLibNvInferPlugins') },
    [pscustomobject]@{ Path = Join-Path $TensorRtBin 'nvonnxparser.dll'; Exports = @() },
    [pscustomobject]@{ Path = Join-Path $TensorRtBin 'nvparsers.dll'; Exports = @() }
)

$Loaded = [Collections.Generic.List[object]]::new()
try {
    foreach ($Item in $LoadPlan) {
        if (-not (Test-Path -LiteralPath $Item.Path -PathType Leaf)) {
            throw "Required runtime file not found: $($Item.Path)"
        }
        $Handle = [NativeLibraryProbe]::Load($Item.Path)
        $Loaded.Add([pscustomobject]@{ Path = $Item.Path; Handle = $Handle })
        foreach ($Export in $Item.Exports) {
            [NativeLibraryProbe]::RequireExport($Handle, $Export)
        }
        $File = Get-Item -LiteralPath $Item.Path
        Write-Host ('[LOAD] {0} ({1:N0} bytes, {2})' -f `
            $File.Name, $File.Length, $File.VersionInfo.FileVersion)
    }
}
finally {
    for ($Index = $Loaded.Count - 1; $Index -ge 0; $Index--) {
        [NativeLibraryProbe]::Release($Loaded[$Index].Handle)
    }
}

Write-Host '[OK] TensorRT 7.2.1.6, cuDNN 8.0.4.30, and CUDA 11.1 DLLs loaded in one isolated process.'
Write-Host '     No game files were changed and the game was not started.'
