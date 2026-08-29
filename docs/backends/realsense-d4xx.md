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
| NVIDIA display driver | 456.81 or newer | CUDA 11.1 Update 1 minimum on Windows; newer drivers are backward compatible | [NVIDIA driver download](https://www.nvidia.com/download/index.aspx) |
| CUDA Toolkit | 11.1 Update 1 (`11.1.1`) | Provides the exact `nvrtc64_111_0.dll` imported by the selected TensorRT package | [CUDA 11.1 Update 1 archive](https://developer.nvidia.com/cuda-11.1.1-download-archive?target_arch=x86_64&target_os=Windows&target_type=exenetwork&target_version=10) |
| cuDNN | 8.0.4.30, Windows x64, CUDA 11.1 | The version NVIDIA tested with TensorRT 7.2.1 | [cuDNN archive](https://developer.nvidia.com/rdp/cudnn-archive) |
| TensorRT | 7.2.1.6, Windows 10 x64, CUDA 11.1, cuDNN 8.0 | Matches the native model toolchain; serialized plans are version-sensitive | [TensorRT 7.x archive](https://developer.nvidia.com/nvidia-tensorrt-7x-download) |
| librealsense | 2.50.0, C API `25000`, Windows x64 | Matches the validated native runtime API and supports D410/D420/D430/D430i/D450 | [librealsense v2.50.0 release](https://github.com/realsenseai/librealsense/releases/tag/v2.50.0) |

The exact TensorRT archive name is:

```text
TensorRT-7.2.1.6.Windows10.x86_64.cuda-11.1.cudnn8.0.zip
```

The exact cuDNN archive name is:

```text
cudnn-11.1-windows-x64-v8.0.4.30.zip
```

The CUDA network installer is named `cuda_11.1.1_win10_network.exe`. The local
installer from the same archive page is also suitable.

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
CUDA 11.1 as supported, and cuDNN 8.0.4 as the tested release. The CUDA 11.1
Update 1 release notes list Windows driver 456.81 as its minimum. See the
[TensorRT release notes](https://docs.nvidia.com/deeplearning/tensorrt/archives/tensorrt-861/release-notes/index.html)
and [CUDA 11.1 Update 1 release notes](https://docs.nvidia.com/cuda/archive/11.1.1/cuda-toolkit-release-notes/index.html).

## Installation

### 1. Check the display driver

Open PowerShell and run:

```powershell
nvidia-smi
```

The command must show the NVIDIA GPU and a driver version at least `456.81`.
A recent driver is preferable. Do not downgrade a working recent driver to the
old driver bundled in the CUDA installer.

### 2. Install CUDA 11.1 Update 1

On the CUDA archive page select:

```text
Operating System: Windows
Architecture: x86_64
Version: 10
Installer Type: exe (network) or exe (local)
```

Run the downloaded installer and choose **Custom (Advanced)**. Keep the CUDA
Toolkit/runtime and cuBLAS components. Clear the bundled display-driver
component when the machine already has a newer NVIDIA driver. Visual Studio
integration, samples, and documentation are optional for runtime-only use.

The normal destination is:

```text
C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.1
```

Verify the runtime without starting the game:

```powershell
$CudaBin = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.1\bin'
Get-Item "$CudaBin\cudart64_110.dll"
Get-Item "$CudaBin\cublas64_11.dll"
Get-Item "$CudaBin\cublasLt64_11.dll"
Get-Item "$CudaBin\nvrtc64_111_0.dll"
```

All four commands must return a file.

CUDA 11.0 and 11.1 can be installed side by side. A pre-existing `v11.0`
directory does not satisfy this package: TensorRT imports `nvrtc64_111_0.dll`,
whereas CUDA 11.0 installs `nvrtc64_110_0.dll`.

### 3. Verify and extract the two NVIDIA archives

Place the exact TensorRT and cuDNN ZIP files in a download directory. From a
clean checkout, run the repository bootstrap script:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\bootstrap-nvidia-d4xx.ps1 `
  -TensorRtZip 'D:\Downloads\TensorRT-7.2.1.6.Windows10.x86_64.cuda-11.1.cudnn8.0.zip' `
  -CuDnnZip 'D:\Downloads\cudnn-11.1-windows-x64-v8.0.4.30.zip'
```

The script checks the pinned SHA-256 hashes, extracts only headers, import
libraries, runtime DLLs, and license files under the ignored `.deps` directory,
and writes a file-by-file manifest. It never modifies the CUDA installation or
the game directory.

Then run the isolated load test:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\diagnostics\test-nvidia-d4xx.ps1
```

The test must load the following libraries and end with `[OK]`:

```text
nvinfer.dll
nvinfer_plugin.dll
nvonnxparser.dll
nvparsers.dll
```

Files ending in `.lib` are build-time import libraries; they do not replace
the runtime `.dll` files. Do not install TensorRT 8, 10, 11, or TensorRT-RTX
for this path. They do not provide the requested 7.2.1 ABI.

### 4. Install or extract librealsense 2.50.0

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
  cudart64_110.dll                     <- CUDA 11.1 Update 1
  cublas64_11.dll
  cublasLt64_11.dll
  nvrtc64_111_0.dll
  nvrtc-builtins64_111.dll
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
- `nvrtc64_110_0.dll` exists but `nvrtc64_111_0.dll` does not: CUDA 11.0 is
  installed, but the selected TensorRT archive was built for CUDA 11.1. Install
  CUDA 11.1 Update 1 side by side; do not rename either DLL.
- A local `nvinfer.dll` is only a few kilobytes: it is not the official
  TensorRT runtime. Application-local DLLs are searched before global `PATH`
  entries and can hide a correct installation.
- The engine cannot be deserialized after installing a newer TensorRT: restore
  7.2.1.6. TensorRT plan files are not generally portable across TensorRT
  versions.

When reporting a setup failure, include the output of `nvidia-smi`, the four
downloaded archive/installer filenames, and the full path of every DLL Windows
actually loaded.
