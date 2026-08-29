# RealSense D4xx native runtime setup

This page describes the dependency stack used while validating RealSense D4xx
depth modules against the existing native pose path. It is deliberately
step-by-step: do not substitute a newer TensorRT release, and do not copy files
into the game directory until every prerequisite passes the checks below.

The D430 path uses depth and infrared only. A color sensor and RGB frames are
not required. The NVIDIA dependencies run the existing pose model; they are
unrelated to RealSense capture.

## Required versions

| Component | Required version | Why it is pinned | Official download |
|---|---:|---|---|
| NVIDIA display driver | 451.82 or newer | CUDA 11.0 Update 1 minimum on Windows; newer drivers are backward compatible | [NVIDIA driver download](https://www.nvidia.com/download/index.aspx) |
| CUDA Toolkit | 11.0 Update 1 (`11.0.3`) | Provides `cudart64_110.dll`, CUDA 11 cuBLAS, and related runtime DLLs | [CUDA 11.0 Update 1 archive](https://developer.nvidia.com/cuda-11-0-1-download-archive?target_os=Windows) |
| cuDNN | 8.0.4 for CUDA 11.x, Windows x64 | The version NVIDIA tested with TensorRT 7.2.1 | [cuDNN archive](https://developer.nvidia.com/rdp/cudnn-archive) |
| TensorRT | 7.2.1.6, Windows 10 x64, CUDA 11.0, cuDNN 8.0 | Matches the native model toolchain; serialized plans are version-sensitive | [TensorRT 7.x archive](https://developer.nvidia.com/nvidia-tensorrt-7x-download) |
| librealsense | 2.50.0, C API `25000`, Windows x64 | Matches the validated native runtime API and supports D410/D420/D430/D430i/D450 | [librealsense v2.50.0 release](https://github.com/realsenseai/librealsense/releases/tag/v2.50.0) |

The exact TensorRT archive name is:

```text
TensorRT-7.2.1.6.Windows10.x86_64.cuda-11.0.cudnn8.0.zip
```

The exact RealSense SDK installer is:

```text
Intel.RealSense.SDK-WIN10-2.50.0.3785.exe
```

It can also be downloaded directly from the
[v2.50.0 release asset](https://github.com/realsenseai/librealsense/releases/download/v2.50.0/Intel.RealSense.SDK-WIN10-2.50.0.3785.exe).

TensorRT and cuDNN downloads require a free NVIDIA Developer account and
acceptance of NVIDIA's download terms. TensorRT is a ZIP archive, not an
installer. Python, TensorFlow, PyTorch, and TensorRT Python wheels are not
needed.

NVIDIA's TensorRT 7.2.1 release notes list CUDA 10.2, CUDA 11.0 Update 1, and
CUDA 11.1 as supported, and cuDNN 8.0.4 as the tested release. The CUDA 11.0
Update 1 release notes list Windows driver 451.82 as its minimum. See the
[TensorRT release notes](https://docs.nvidia.com/deeplearning/tensorrt/archives/tensorrt-861/release-notes/index.html)
and [CUDA 11.0 release notes](https://docs.nvidia.com/cuda/archive/11.0/cuda-toolkit-release-notes/index.html).

## Installation

### 1. Check the display driver

Open PowerShell and run:

```powershell
nvidia-smi
```

The command must show the NVIDIA GPU and a driver version at least `451.82`.
A recent driver is preferable. Do not downgrade a working recent driver to the
old driver bundled in the CUDA installer.

### 2. Install CUDA 11.0 Update 1

On the CUDA archive page select:

```text
Operating System: Windows
Architecture: x86_64
Version: 10
Installer Type: exe (local)
```

Run the downloaded installer and choose **Custom (Advanced)**. Keep the CUDA
Toolkit/runtime and cuBLAS components. Clear the bundled display-driver
component when the machine already has a newer NVIDIA driver. Visual Studio
integration, samples, and documentation are optional for runtime-only use.

The normal destination is:

```text
C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.0
```

Verify the runtime without starting the game:

```powershell
$CudaBin = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.0\bin'
Get-Item "$CudaBin\cudart64_110.dll"
Get-Item "$CudaBin\cublas64_11.dll"
Get-Item "$CudaBin\cublasLt64_11.dll"
```

All three commands must return a file.

### 3. Extract cuDNN 8.0.4

On the cuDNN archive page select **cuDNN 8.0.4 for CUDA 11.x**, then download
the Windows x64 ZIP. Extract it into a versioned directory such as:

```text
D:\dance-around-runtime\cudnn-8.0.4
```

Do not merge it into a newer CUDA installation. Confirm that its `bin`
directory contains `cudnn64_8.dll` and the accompanying cuDNN 8 inference
DLLs:

```powershell
Get-ChildItem 'D:\dance-around-runtime\cudnn-8.0.4' -Recurse -Filter 'cudnn*64_8.dll'
```

### 4. Extract TensorRT 7.2.1.6

On the TensorRT 7.x page expand **TensorRT 7.2.1 GA** and download the exact
Windows/CUDA 11.0 archive named above. Extract it to:

```text
D:\dance-around-runtime\TensorRT-7.2.1.6
```

The extracted `TensorRT-7.2.1.6\lib` directory must contain at least:

```text
nvinfer.dll
nvinfer_plugin.dll
nvonnxparser.dll
nvparsers.dll
```

Files ending in `.lib` are build-time import libraries; they do not replace
the runtime `.dll` files. Do not install TensorRT 8, 10, 11, or TensorRT-RTX
for this path. They do not provide the requested 7.2.1 ABI.

### 5. Install or extract librealsense 2.50.0

Run `Intel.RealSense.SDK-WIN10-2.50.0.3785.exe`, or build the `v2.50.0` source
tag. Connect each D430 directly to a USB 3 controller when possible. Use the
included RealSense Viewer to confirm that both devices can stream:

```text
Depth: Z16
Infrared: Y8
Color: disabled
```

Do not enable or emulate a color stream for D430.

## Recommended application-local layout

Keep these old AI runtimes isolated from unrelated applications. A maintainer
or packaging script should stage them beside the D4xx entry DLL rather than
modifying the global `PATH` permanently:

```text
dance_around_anygear_d4xx.dll          <- the only Spice -k argument
dance_around_anygear_d4xx\
  realsense2.dll                       <- librealsense 2.50.0, x64
  nvinfer.dll                          <- TensorRT 7.2.1.6
  nvinfer_plugin.dll
  nvonnxparser.dll
  nvparsers.dll
  cudart64_110.dll                     <- CUDA 11.0 Update 1
  cublas64_11.dll
  cublasLt64_11.dll
  cudnn64_8.dll                        <- cuDNN 8.0.4 family
  cudnn_adv_infer64_8.dll
  cudnn_cnn_infer64_8.dll
  cudnn_ops_infer64_8.dll
```

Additional transitive DLLs reported by the official TensorRT build should be
copied from the same CUDA/cuDNN versions. Never mix similarly named DLLs from
another major version.

The D4xx release is not published yet. Until its staging script and dependency
hashes are complete, keep the original archives and tell the maintainer their
paths; do not overwrite game files manually.

## Troubleshooting

- TensorRT builder initialization fails: TensorRT 7.2.1 or one of its
  CUDA/cuDNN dependencies is missing, shadowed, or the wrong architecture.
- A DLL exists but Windows reports “module not found”: one of that DLL's own
  dependencies is absent. Check the CUDA, cuDNN, and TensorRT directories as a
  set.
- `nvidia-smi` works but `cudart64_110.dll` is absent: the display driver is
  installed, but the CUDA 11 runtime is not. They are separate packages.
- A local `nvinfer.dll` is only a few kilobytes: it is not the official
  TensorRT runtime. Application-local DLLs are searched before global `PATH`
  entries and can hide a correct installation.
- The engine cannot be deserialized after installing a newer TensorRT: restore
  7.2.1.6. TensorRT plan files are not generally portable across TensorRT
  versions.

When reporting a setup failure, include the output of `nvidia-smi`, the four
downloaded archive/installer filenames, and the full path of every DLL Windows
actually loaded.
