# RealSense D4xx native runtime setup

This page describes the dependency stack used while validating RealSense D4xx
depth modules against the existing native pose path. It is deliberately
step-by-step: do not substitute a newer TensorRT release, and do not copy files
into the game directory until every prerequisite passes the checks below.

For the independent depth-only model that does not use this NVIDIA/native pose
stack, see the
[D4xx/SPiKE research backend](realsense-d4xx-spike.md). The two paths are kept
separate so their evidence and runtime requirements cannot be confused.

The D430 path uses depth and infrared only. A color sensor and RGB frames are
not required. The NVIDIA dependencies run the existing pose model; they are
unrelated to RealSense capture.

## Per-installation placement JSON

The D4xx DLL reads `dance_around_anygear_d4xx.json` from the directory beside
the DLL at startup. This file is the supported place for room- and
mount-specific settings; do not edit the game's original JSON and do not put
these values in a long launch script. Start from
[`config/dance_around_anygear_d4xx.json`](../../config/dance_around_anygear_d4xx.json)
and keep the same filename beside the DLL.

`PrimarySerial` selects the D430 whose infrared image drives pose recognition.
When it is empty, `PrimaryDevice` selects by the librealsense enumeration
index. The serial-number setting wins over the index and is recommended after
the better camera has been identified. Startup logs list both serial numbers.
`InfraredIndex` selects the left (`1`) or right (`2`) infrared imager.
The DLL uses librealsense's alignment block so the depth map and the selected
infrared imager share one pixel grid. It also retains the aligned depth packet
that belongs to each pose input until inference completes; changing to IR 2
does not make the pose sample against a newer or left-imager depth frame.

`AutoCenterZ` samples the first stable hip depth and maps it to `StageHipZ`.
Keep it enabled for normal home setups: it lets a shorter or longer room use
the same game-safe virtual standing distance. `OffsetX`, `OffsetY`, and
`OffsetZ` are additional metre offsets in final stage coordinates.

`PitchDegrees`, `YawDegrees`, and `RollDegrees` compensate for a camera that is
not mounted level. The rotations are applied around the live hip, so changing
an angle does not move the player across the stage. They use a right-handed
coordinate system: X is pitch, Y is yaw, Z is roll, +Y points up, and +Z points
away from the camera. Tune pitch first in small 1-2 degree steps. Configuration
is loaded once; restart the game after editing the file.

Use this tuning order:

1. Pick the device and infrared index that show the complete body with the
   least occlusion. No coordinate parameter can reconstruct hands or feet that
   are outside the selected image.
2. Leave `AutoCenterZ` enabled and `StageHipZ` at `2.3` unless a game-specific
   safe zone requires a different virtual depth.
3. Adjust pitch/yaw/roll until a neutral standing skeleton is upright.
4. Use XYZ offsets only to align the already-correct skeleton to the stage.
5. Lower `Smoothing` for steadier but slower motion; raise it for faster but
   noisier response. `0.65` is the low-latency default; use roughly `0.50`
   through `0.80` for room tuning.
6. Keep `OutputFps` at `30`. Pose inference runs independently and the output
   worker resamples it to the stable VP4U cadence. `PredictionMs` compensates
   for capture/inference age; start at `20` and avoid values over `50` unless
   fast-motion overshoot has been checked in game.
7. `BodyHoldMs` applies only after pose inference is lost. The `250` ms default
   bridges a brief occlusion without keeping an obsolete hand or foot attached
   to another limb for seconds.

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
