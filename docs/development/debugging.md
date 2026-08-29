# Debugging

Debug in layers. A full Unity launch is the last test, not the first diagnostic.

## 1. Source and loader checks

```powershell
./tools/verify-source-tree.ps1
./tools/test.ps1 -ToolchainRoot C:\path\to\mingw64
```

This verifies export names, embedded build information, the 29-slot ABI table,
the one-DLL release layout, and Spice library redirection without opening a
sensor or the game.

## 2. Isolated Kinect hardware check

```powershell
./tools/test.ps1 `
  -ToolchainRoot C:\path\to\mingw64 `
  -SkipBuild `
  -HardwareKinect `
  -ConfigPath C:\path\to\vp4u-config.json `
  -Seconds 30
```

The harness must sustain approximately 30 pose callbacks per second and reject
degenerate poses, implausible limbs, and endpoint jumps. It does not start
Unity. The test first runs a low-level skeleton-only stream probe, then the
full VP4U harness. Expected stable markers include:

```text
runtime 3/5: initialized VP4U shim (backend=kinect)
runtime 4/5: Kinect sensor opened (no-color+skeleton)
runtime 5/5: analysis started (kinect NUI skeleton)
first skeleton frame received (NUI skeleton stream active)
PASS: yes
```

## 3. Isolated MediaPipe and webcam checks

Prepare the pinned runtime/model/test image, then exercise the exact C++ pose
adapter repeatedly without Python, a webcam, or Unity:

```powershell
./tools/bootstrap-mediapipe.ps1 -Download
./tools/test.ps1 -SkipBuild -MediaPipe
```

This also stages the release layout and asks the webcam plugin to initialize
MediaPipe from its adjacent dependency directory. Expected markers include:

```text
stage 2/4: MediaPipe USB webcam profile selected
InitVisionPoseRealtime: MediaPipe engine ready
PASS: MediaPipe pose ... valid=60/60
```

With a physical camera connected, add the capture and complete VP4U path:

```powershell
./tools/test.ps1 `
  -SkipBuild `
  -MediaPipe `
  -HardwareWebcam `
  -WebcamIndex 0 `
  -Seconds 30
```

The low-level probe must enumerate the requested camera and receive real
frames before the full harness runs. An absent or out-of-range camera is an
explicit failure; the release path does not substitute synthetic frames.

## 4. Isolated SteamVR check

Prepare and verify the pinned OpenVR client first:

```powershell
./tools/bootstrap-openvr.ps1 -Download
./tools/test.ps1 -SkipBuild
```

The default tests exercise deterministic six-point reconstruction, loader
redirection, and the VP4U table without connecting to SteamVR. With the HMD,
controllers, and three required trackers ready, opt into the hardware path:

```powershell
./tools/test.ps1 -SkipBuild -HardwareSteamVr -Seconds 30
```

Expected markers include the OpenVR version, a role table containing `waist`,
`left-foot`, and `right-foot` device indices, and a `SteamVR body -> TRACKED`
transition. A missing role is an explicit not-tracked result. The command does
not start Unity, but OpenVR initialization may connect to or start SteamVR.

## 5. Isolated RealSense D4xx dependency check

Before testing a D430/D4xx path, install and verify the exact versions in
[RealSense D4xx native runtime setup](../backends/realsense-d4xx.md). In
particular, `nvidia-smi` proves only that the display driver is present; it
does not install CUDA Runtime, cuDNN, or TensorRT.

Do not start Unity until both D430 devices stream Depth Z16 and Infrared Y8 in
RealSense Viewer and the TensorRT 7.2.1.6 directory contains genuine runtime
DLLs. A local placeholder DLL shadows a system installation and must never be
treated as a successful dependency check.

## 6. Full game check

Install and generate the short launcher without running it:

```powershell
./tools/install-local.ps1 -CabinetRoot D:\UDN -GenerateLauncher
```

The generated BAT supplies the Spice-owned arguments:

```text
-ea -cardio -w -keyboardnav -k dance_around_anygear_kinect.dll
```

The reviewed Spice patch auto-detects UDN and assigns its 1 GiB heap default,
so the generated launcher does not repeat `-udn` or `-h`.

To fix the initial window client size and position in the generated BAT, add
`-WindowSize '960,1626' -WindowPosition '120,40'` to `install-local.ps1`.
These become Spice `-windowsize` and `-windowpos` arguments and are applied
only because the launcher also enables `-w`.

Preserve the known short working path; Unity-based games can fail on long
paths. Do not invoke the game executable ad hoc from a terminal. The generated
BAT is intentionally short: stable Kinect configuration and stage logging live
in the DLL. The longer repository diagnostics are for maintainers and can
observe resource use for as long as calibration/asset loading requires.

Normal startup emits these milestones before sensor-specific logs:

```text
stage 1/4: Anygear ... loaded
stage 2/4: stable Kinect profile selected
stage 3/4: embedded VP4U ABI 210 exports validated
stage 4/4: backend ready; waiting for Unity VP4U calls
```

Once Unity enters the VP4U ABI, a second `runtime 1/5` through `runtime 5/5`
sequence identifies the last completed ABI/sensor stage. Release builds log
tracking acquisition and loss only when state changes; periodic joint/resource
telemetry is intentionally left to `tools/diagnostics/`.

## Resource invariants

- Image pool is bounded (default 12 for Kinect; source clamps 4..24).
- Pose analysis is paced by new NUI skeleton generations, not a busy poll.
- Preview is bounded separately and uses a 1x1 sentinel with color disabled.
- Kinect does not load MediaPipe or create a pose-inference worker.
- A short missing-body interval is held as inferred data, then transitions to
  not-tracked instead of emitting unbounded stale data.
- Webcam inference is capped at 30 Hz on an 848x480 BGR frame; preview is
  capped at 15 Hz and shares the same bounded image pool.
- SteamVR polls standing-space poses at 60 Hz, creates no render context, and
  uses the same 1x1 image-token path as skeleton-only Kinect.
