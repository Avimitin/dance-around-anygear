# dance-around-anygear

`dance-around-anygear` is a source-only collection of external hardware
compatibility backends for **DANCE aROUND**. Each backend is loaded by Spice
with `-k`; Spice remains responsible for launching the local game installation,
cabinet/platform emulation, input, cards, overlay, and window management.

Release archives are generated from the checked-in source and pinned external
inputs. Install the Kinect for Windows SDK 1.8 when building the Kinect target;
MediaPipe runtime files and models are prepared separately for the webcam target,
and the pinned OpenVR SDK supplies the SteamVR header/client runtime.

## Responsibility boundary

| Setup | What the user loads | Owner |
|---|---|---|
| Intel RealSense D435 | Spice only | Spice/game's native VP4U path |
| Another Intel RealSense D4xx model | `dance_around_anygear_d4xx.dll` (planned) | Anygear hardware adapter |
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
- RealSense D4xx: architecture placeholder only; no release should be published
  until a physical device passes the same harness and in-game checks.

## Build

On Windows PowerShell:

```powershell
./tools/bootstrap-openvr.ps1 -Download
./tools/build.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/test.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/check-reproducible.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/package.ps1 -Backend kinect
```

Prepare and test the webcam dependencies without installing Python:

```powershell
./tools/bootstrap-mediapipe.ps1 -Download
./tools/test.ps1 -SkipBuild -MediaPipe
./tools/package.ps1 -Backend webcam -LocalEvaluation
```

Prepare the pinned OpenVR SDK/runtime and package the SteamVR backend:

```powershell
./tools/bootstrap-openvr.ps1 -Download
./tools/build.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/test.ps1 -SkipBuild
./tools/package.ps1 -Backend steamvr
```

The webcam package is currently labelled local evaluation. The MediaPipe
runtime is Apache-2.0 and pinned, but the separately hosted Google `.task`
model does not yet have explicit redistribution terms suitable for a public
release. This gate does not affect local development or hardware testing.

The currently reproduced toolchain is WinLibs GCC 16.2.0, MinGW-w64 14.0.0,
UCRT, POSIX threads, SEH. See [reproducible-builds.md](docs/development/reproducible-builds.md).

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
```

Do not copy or rename it over the game's original `visionposewrapper.dll`; the
plugin redirects Unity's request at runtime. Maintainer scripts remain verbose
for reproducibility, but they are not downstream runtime dependencies.

For a local cabinet tree, this installs the DLL and writes a short BAT without
starting the game:

```powershell
./tools/install-local.ps1 -CabinetRoot D:\UDN -GenerateLauncher
./tools/install-local.ps1 -CabinetRoot D:\UDN -Backend webcam -GenerateLauncher
./tools/install-local.ps1 -CabinetRoot D:\UDN -Backend steamvr -GenerateLauncher
```

Windowed launchers can pin the initial client size and desktop position using
Spice's existing window arguments:

```powershell
./tools/install-local.ps1 `
  -CabinetRoot D:\UDN `
  -Backend webcam `
  -GenerateLauncher `
  -WindowSize '960,1626' `
  -WindowPosition '120,40'
```

This emits `-w -windowsize 960,1626 -windowpos 120,40`. Without these optional
values, DANCE aROUND keeps Spice's aspect-correct centered default.

## Documentation

- [Architecture and responsibilities](docs/architecture.md)
- [Support matrix](docs/support-matrix.md)
- [Runtime debugging](docs/development/debugging.md)
- [MediaPipe webcam backend](docs/backends/mediapipe-webcam.md)
- [SteamVR backend](docs/backends/steamvr.md)
- [Spice integration](integration/spice2x/README.md)

## Usage

Provided for learning and personal experimentation. Source is licensed under
GPL-3.0-or-later; see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
