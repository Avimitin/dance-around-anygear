# dance-around-anygear

`dance-around-anygear` is a source-only collection of external hardware
compatibility backends for **DANCE aROUND**. Each backend is loaded by Spice
with `-k`; Spice remains responsible for launching the local game installation,
cabinet/platform emulation, input, cards, overlay, and window management.

Release assets are generated from the checked-in source and pinned external
inputs. The Kinect bootstrap reads the verified official SDK bundle without
installing it; MediaPipe runtime files and models are prepared separately for
the webcam target, and the pinned OpenVR SDK supplies the SteamVR header/client
runtime.

## Responsibility boundary

| Setup | What the user loads | Owner |
|---|---|---|
| Intel RealSense D435 | Spice only | Spice/game's native VP4U path |
| Another Intel RealSense D4xx model, native VP4U evaluation | `dance_around_anygear_d4xx.dll` | Anygear stream adapter |
| Another Intel RealSense D4xx model, depth-only research path | `dance_around_anygear_d4xx_spike.dll` plus its pinned DirectML worker/model directory | Anygear SPiKE pose adapter |
| Kinect for Windows v1 | `dance_around_anygear_kinect.dll` | Anygear Kinect/VP4U adapter |
| Ordinary webcam | `dance_around_anygear_webcam.dll` plus its pinned MediaPipe dependency directory | Anygear webcam pose adapter |
| SteamVR full-body setup | `dance_around_anygear_steamvr.dll` plus its pinned OpenVR client directory | Anygear tracked-pose adapter |

If an original D435 setup fails because of virtual screens, key mapping,
card-reader emulation, overlay, windowing, or a game/platform quirk, that is a
Spice compatibility issue. Users should not patch their game assemblies.

## Current status

- Kinect v1 NUI skeleton to VP4U ABI: working and validated in a complete game.
- Kinect color capture: disabled by default; the game only needs a non-null
  image descriptor on the calibration/pose callback path, so a shared
  1x1 sentinel is used.
- Kinect stability: tracking-ID lock, short body hold, inferred-joint repair,
  parent-relative speed rejection, and learned bone-length bounds are enabled.
- Face-to-face behavior: stage-centered yaw reflection is enabled and has been
  validated in game.
- Webcam + MediaPipe: the official v1 C runtime, Pose Landmarker Lite model,
  33-point mapping, loader redirection, and repeated image inference are
  working. Real USB capture and in-game orientation still need hardware tests.
- SteamVR: official OpenVR 2.15.6 standing-space poses, SteamVR tracker-role
  discovery, deterministic 33-landmark reconstruction, loader/ABI tests, and
  a hardware probe are implemented. In-game tracking still needs a physical
  HMD/controller/tracker test.
- RealSense D4xx native bridge: two-camera depth/IR capture and original VP4U
  delegation are implemented, but the D430 calibration result is not yet good
  enough to call this path validated.
- RealSense D4xx + SPiKE: depth-only person isolation, deterministic point
  sampling, a reproducible FP16 ONNX export, DirectML inference, bounded IPC,
  and primary/calibrated fusion are implemented. A dual-D430 field session
  completed calibration, gameplay, results, and a second calibration/gameplay
  cycle. This is now a playable experimental baseline; fine wrist/ankle
  orientation and residual endpoint jitter remain model/topology limits.

## Build

On Windows PowerShell:

```powershell
./tools/bootstrap-openvr.ps1 -Download
./tools/bootstrap-kinect-sdk.ps1 -Download
./tools/bootstrap-librealsense.ps1 -Download
./tools/build.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/test.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/check-reproducible.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/package.ps1 -Backend kinect
```

Prepare and test the webcam dependencies without installing Python:

```powershell
./tools/bootstrap-mediapipe.ps1 -Download
./tools/test.ps1 -SkipBuild -MediaPipe
./tools/package.ps1 -Backend webcam
```

Prepare the pinned OpenVR SDK/runtime and package the SteamVR backend:

```powershell
./tools/bootstrap-openvr.ps1 -Download
./tools/build.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/test.ps1 -SkipBuild
./tools/package.ps1 -Backend steamvr
```

Prepare the depth-only D4xx/SPiKE bundle without installing Python, CUDA, or
TensorRT system-wide:

```powershell
./tools/bootstrap-librealsense.ps1 -Download
./tools/bootstrap-spike-runtime.ps1 -Download
./tools/build-spike-worker.ps1 -Clean
./tools/benchmark-spike-directml.ps1 -MaximumP95Ms 25
./tools/package.ps1 -Backend d4xx-spike
```

The native D4xx evaluation path additionally depends on an exact legacy
NVIDIA inference stack. Follow the complete version, download, installation,
layout, and verification guide in
[RealSense D4xx native runtime setup](docs/backends/realsense-d4xx.md). Do not
substitute a current TensorRT release for TensorRT 7.2.1.6.

The MediaPipe runtime and Pose Landmarker model are Apache-2.0 and pinned by
hash. Their upstream license and notice accompany the webcam archive.

The currently reproduced toolchain is WinLibs GCC 16.2.0, MinGW-w64 14.0.0,
UCRT, POSIX threads, SEH. See [reproducible-builds.md](docs/development/reproducible-builds.md).

## Automated releases

Pushing a tag in exact `vMAJOR.MINOR.PATCH` form starts the Windows release
workflow. The tag version must match `project(... VERSION ...)` in
`CMakeLists.txt`. The workflow prepares every pinned input, builds every entry
DLL, runs the offline regression suite and byte-for-byte reproduction checks,
then creates one GitHub Release containing:

- `dance_around_anygear_kinect.dll` directly, because it has no project runtime
  files;
- `dance_around_anygear_d4xx.dll` directly for native-path evaluation;
- one webcam ZIP containing its MediaPipe runtime, model, and notices;
- one SteamVR ZIP containing its OpenVR runtime and license;
- one D4xx/SPiKE ZIP containing the official librealsense runtime, DirectML
  worker, reproducible ONNX model, configuration, and notices;
- the build manifest and `SHA256SUMS`.

No release is created if any build, test, dependency hash, archive-layout, or
reproducibility check fails. The native D4xx path remains unvalidated. The
SPiKE path is a playable experimental baseline while its fine-joint motion
quality is still being measured and tuned.

Each backend exposes one Spice entry DLL containing the Spice entry point,
VP4U ABI, backend defaults, stage diagnostics, and embedded DWARF information.
For Kinect the entry DLL has no project runtime dependency. The webcam plugin
loads the official MediaPipe runtime and model itself from its adjacent
dependency directory. The SteamVR plugin similarly loads the pinned official
OpenVR client DLL from its own adjacent directory. Spice still receives only
one `-k` argument:

```text
dancearound.exe ... -k dance_around_anygear_kinect.dll
dancearound.exe ... -k dance_around_anygear_webcam.dll
dancearound.exe ... -k dance_around_anygear_steamvr.dll
dancearound.exe ... -k dance_around_anygear_d4xx_spike.dll
```

Do not copy or rename it over the game's original `visionposewrapper.dll`; the
plugin redirects Unity's request at runtime. Maintainer scripts remain verbose
for reproducibility, but they are not downstream runtime dependencies.

For a local cabinet tree, this installs the DLL and writes a short BAT without
starting the game:

```powershell
./tools/install-local.ps1 -CabinetRoot C:\cabinet -GenerateLauncher
./tools/install-local.ps1 -CabinetRoot C:\cabinet -Backend webcam -GenerateLauncher
./tools/install-local.ps1 -CabinetRoot C:\cabinet -Backend steamvr -GenerateLauncher
./tools/install-local.ps1 -CabinetRoot C:\cabinet -Backend d4xx-spike -GenerateLauncher
```

Windowed launchers can pin the initial client size and desktop position using
Spice's existing window arguments:

```powershell
./tools/install-local.ps1 `
  -CabinetRoot C:\cabinet `
  -Backend webcam `
  -GenerateLauncher `
  -WindowSize '960,1626' `
  -WindowPosition '120,40'
```

This emits `-w -windowsize 960,1626 -windowpos 120,40`. Without these optional
values, DANCE aROUND keeps Spice's aspect-correct centered default.

For D4xx/SPiKE placement and model diagnostics, the local live viewer displays
both raw depth streams, production-isolated point clouds, and predicted
skeletons without starting Unity. See the
[D4xx/SPiKE backend guide](docs/backends/realsense-d4xx-spike.md#live-point-cloud-and-skeleton-viewer).

## Documentation

- [Architecture and responsibilities](docs/architecture.md)
- [Support matrix](docs/support-matrix.md)
- [Runtime debugging](docs/development/debugging.md)
- [Release workflow](docs/development/releases.md)
- [MediaPipe webcam backend](docs/backends/mediapipe-webcam.md)
- [RealSense D4xx native dependencies](docs/backends/realsense-d4xx.md)
- [RealSense D4xx depth-only SPiKE backend](docs/backends/realsense-d4xx-spike.md)
- [D430 depth-pose research plan and acceptance gates](docs/research/d430-spike-roadmap.md)
- [Paired D430/Kinect teacher recording format](docs/research/d430-kinect-teacher-data.md)
- [SPiKE candidate evaluation and public ITOP setup](docs/research/spike-model-evaluation.md)
- [SteamVR backend](docs/backends/steamvr.md)
- [Spice integration](integration/spice2x/README.md)

## Usage

Provided for learning and personal experimentation. Source is licensed under
GPL-3.0-or-later; see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
