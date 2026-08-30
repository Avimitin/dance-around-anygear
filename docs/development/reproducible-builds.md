# Reproducible builds

## Source policy

The Git tree contains source, headers, scripts, documentation, and dependency
metadata only. `tools/verify-source-tree.ps1` rejects native binaries, models,
captured media, crash dumps, and logs outside ignored output directories.

## Pinned toolchain

The pinned build uses a WinLibs archive with SHA-256:

```text
winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip
C1F52294597C0B73786B2A78EB5D176D89226D2F21875EAB75E783A8B1CEFCC4
```

It contains GCC 16.2.0, MinGW-w64 14.0.0 (UCRT/POSIX/SEH), CMake 4.4.2,
Ninja 1.13.2, and Binutils 2.47. The archive is not committed. Verify and
extract a supplied copy with:

```powershell
./tools/bootstrap-toolchain.ps1 -Archive C:\downloads\winlibs.zip
```

Or fetch that exact pinned archive into the ignored `.deps/` cache:

```powershell
./tools/bootstrap-toolchain.ps1 -Download
```

`tools/build.ps1` rejects another GCC version unless explicitly used for an
exploratory build. Every build writes `build/build-manifest.json` with source
state, tool versions, sizes, and output hashes.

Release DLLs retain DWARF line/symbol information (`-g2`) so a distributed file
is itself useful for postmortem debugging. Prefix-map flags remove the local
checkout path, and the PE linker timestamp is disabled; no separate PDB is
required or committed.

## External runtimes

Kinect builds use the four NUI headers from Kinect for Windows SDK 1.8. The
bootstrap downloads Microsoft's official `KinectSDK-v1.8-Setup.exe`, verifies
its pinned SHA-256, and reads the x64 SDK payload with 7-Zip without launching
or installing the bundle. Each extracted header is checked independently. The
resulting plugin resolves the user's installed `Kinect10.dll` at runtime.

```powershell
./tools/bootstrap-kinect-sdk.ps1 -Download
```

The webcam backend pins these external inputs in `dependency-lock.json`:

```text
MediaPipe source tag             v1.0.0
MediaPipe source commit          6d31f1ebc3284db74d211d62bdc4f0a0c29ea120
Windows wheel SHA-256            DA57E6719BBAB05007272C91D6CA2E0E2E370709491CBE344A372F87E25CF604
Extracted libmediapipe SHA-256   A8970C645C8C87C25EC9965CB5C898E803C6C42F7192B7DE9A0541C62AE48CEF
Pose Landmarker Lite SHA-256     59929E1D1EE95287735DDD833B19CF4AC46D29BC7AFDDBBF6753C459690D574A
```

Prepare those inputs in the ignored cache with either the pinned downloads:

```powershell
./tools/bootstrap-mediapipe.ps1 -Download
```

or caller-supplied offline files:

```powershell
./tools/bootstrap-mediapipe.ps1 `
  -Wheel C:\downloads\mediapipe-1.0.0-py3-none-win_amd64.whl `
  -Model C:\downloads\pose_landmarker_lite.task `
  -TestImage C:\downloads\pose.jpg
```

Both paths verify every input and extracted output before use. The source tree
never receives the wheel, DLL, model, or test image. The runtime and Pose
Landmarker model are Apache-2.0; the runtime `LICENSE`/`NOTICE` accompany
release bundles.

The SteamVR backend pins Valve OpenVR `v2.15.6` at commit
`0924064316de3effbcd1acf1e309182a2deb1c05`. The generated C API header,
Windows x86-64 `openvr_api.dll`, and BSD-3-Clause license are each checked by
size and SHA-256. Prepare them in the ignored cache with:

```powershell
./tools/bootstrap-openvr.ps1 -Download
```

The runtime DLL and license are copied into SteamVR release archives; neither
is committed to the source tree.

The D4xx native evaluation stack is pinned separately in
[realsense-d4xx.md](../backends/realsense-d4xx.md): librealsense 2.50.0,
TensorRT 7.2.1.6, CUDA 11.0 Update 1, and cuDNN 8.0.4. NVIDIA download terms
require the user or release maintainer to acquire the NVIDIA archives. No D4xx
release may be published until the exact input and extracted DLL hashes have
been recorded in `dependency-lock.json` and the staging script verifies them.

The independent D4xx/SPiKE path does not use that NVIDIA stack. Its build is
split into these pinned inputs:

```text
librealsense source commit       c94410a420b74e5fb6a414bd12215c05ddd82b69
librealsense C API               25000
SPiKE source commit              57ddaec83dad754aed813afacab4d0591fd387b1
Python                           3.10.5
uv                               0.12.7
PyTorch CPU, export only         2.11.0
ONNX / opset                     1.22.0 / 20
ONNX Runtime DirectML            1.23.0
connected-components-3d          4.0.0
PyInstaller                      6.22.2
```

Maintainer-only model work is pinned separately: PyTorch 2.11.0+cu128 for
fine-tuning, SciPy 1.15.3 for offline synchronized-surface calibration, and
h5py 3.14.0 for public ITOP evaluation. The ITOP version 1.0 archive URLs,
compressed and expanded hashes, and exact Windows h5py/SciPy wheels are
recorded in `dependency-lock.json`. None of these research-only inputs enter a
release archive.

`tools/bootstrap-librealsense.ps1` verifies the pinned source archive and
official Unity package, then extracts only C headers, the Apache-2.0 license,
and the x64 2.50.0 runtime. `tools/bootstrap-spike-runtime.ps1` verifies the
published checkpoint and exports a static FP16 ONNX file. The output must
match the recorded 143,295,911-byte size and SHA-256 before packaging.

The source tree contains the fixed-shape SPiKE inference definition and its
MIT license, not the checkpoint, ONNX file, or research datasets. The two
`uv.lock` files pin the complete export, test, frozen-worker, public-evaluation,
and CUDA fine-tuning environments. The downstream package needs no Python,
PyTorch, CUDA, TensorRT, SciPy, h5py, or network access.

## Reproducibility check

Run two isolated builds from the same source/toolchain and require an exact DLL
hash match:

```powershell
./tools/check-reproducible.ps1 -ToolchainRoot C:\path\to\mingw64
```

The build maps both source and binary directories out of DWARF, retains the
debug sections, and disables the PE timestamp. All entry DLLs must match
byte-for-byte. The frozen DirectML worker is built twice with a fixed source
epoch and Python hash seed; every file in both trees must match:

```powershell
./tools/check-spike-worker-reproducible.ps1
```

Package files are sorted and every ZIP entry receives the fixed DOS epoch
timestamp, so repeated packaging from the same DLLs and pinned dependencies is
byte-for-byte stable as well.
