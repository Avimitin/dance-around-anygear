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

Kinect builds use `NuiApi.h` from an installed Kinect for Windows SDK 1.8.
The resulting plugin resolves `Kinect10.dll` at runtime.

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
never receives the wheel, DLL, model, or test image. The runtime itself is
Apache-2.0 and its `LICENSE`/`NOTICE` accompany local bundles. Public webcam
packaging remains blocked because explicit redistribution terms for Google's
separately hosted `.task` model have not been verified; `-LocalEvaluation` is
an intentional, labelled local-testing option.

The SteamVR backend pins Valve OpenVR `v2.15.6` at commit
`0924064316de3effbcd1acf1e309182a2deb1c05`. The generated C API header,
Windows x86-64 `openvr_api.dll`, and BSD-3-Clause license are each checked by
size and SHA-256. Prepare them in the ignored cache with:

```powershell
./tools/bootstrap-openvr.ps1 -Download
```

The runtime DLL and license are copied into SteamVR release archives; neither
is committed to the source tree.

## Reproducibility check

Run two isolated builds from the same source/toolchain and require an exact DLL
hash match:

```powershell
./tools/check-reproducible.ps1 -ToolchainRoot C:\path\to\mingw64
```

The build maps both source and binary directories out of DWARF, retains the
debug sections, and disables the PE timestamp. Kinect, webcam, and SteamVR entry
DLLs must match byte-for-byte. Package ZIP container timestamps may differ;
the files listed in `SHA256SUMS` are the reproducibility boundary.
