# MediaPipe webcam backend

## Scope

`dance_around_anygear_webcam.dll` adapts an ordinary Windows USB camera to the
VP4U ABI expected by DANCE aROUND. Spice loads only that entry DLL. The plugin
then loads the pinned native MediaPipe C runtime and Pose Landmarker model from
an adjacent, backend-owned directory; it does not require Python, OpenCV, a
helper process, or a runtime download.

```text
Spice -k dance_around_anygear_webcam.dll
  -> redirect Unity's visionposewrapper.dll request to the entry DLL
  -> Media Foundation camera capture (RGB32 converted to BGR8)
  -> bounded 848x480 frame
  -> BGR-to-RGB conversion
  -> MediaPipe Pose Landmarker VIDEO mode, CPU, one pose
  -> 33 normalized and world landmarks
  -> VP4U 29-slot API and body/image callbacks
```

## Runtime layout

Keep the directory names intact:

```text
dance_around_anygear_webcam.dll        <- the only Spice -k argument
dance_around_anygear_webcam/
  libmediapipe.dll
  pose_landmarker_lite.task
  LICENSE.mediapipe.txt
  NOTICE.mediapipe.txt
```

The plugin derives this directory from its own module path and loads the
runtime by absolute path. It therefore does not depend on the process working
directory or a global `PATH` edit.

## Build and isolated validation

Prepare only the hash-pinned files in the ignored `.deps/` cache, build both
entry DLLs, and run the no-camera inference test:

```powershell
./tools/bootstrap-mediapipe.ps1 -Download
./tools/build.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/test.ps1 -SkipBuild -MediaPipe
```

`-MediaPipe` first verifies that the staged entry DLL resolves its adjacent
runtime and model. It then decodes Google's pinned pose test image with Windows
Imaging Component and passes it through the same `PoseEngine` compiled into the
plugin for 60 VIDEO-mode iterations.

For a connected device, run the Media Foundation probe and the complete VP4U
harness without starting Unity:

```powershell
./tools/test.ps1 `
  -SkipBuild `
  -MediaPipe `
  -HardwareWebcam `
  -WebcamIndex 0 `
  -Seconds 30
```

The default physical device is index 0. Maintainers can select another index
with `-WebcamIndex` in the test script or `VP4U_CAMERA_INDEX=1` in the launch
environment. An invalid or silent camera fails at `runtime 4/5`; it is never
reported as a synthetic device.

The capture reader explicitly enables Media Foundation video processing so
common camera-native formats can be converted to RGB32. A stopped stream uses
a two-second reconnect delay instead of spinning in a tight open/read loop.

## Mapping and resource profile

MediaPipe image landmarks supply the preview coordinates and visibility.
World landmarks are torso-scale normalized, anchored at stage hip position
`(0.9, 1.0, 0.75)`, Y/depth converted to the existing VP4U convention, and
smoothed with an EMA. The 33 landmarks are reduced to the callback body's
target joint set, with spine/head helper joints synthesized where needed.

Release defaults are intentionally bounded:

- inference and pose callbacks: 30 Hz maximum;
- inference frame: 848x480;
- preview callbacks: 15 Hz maximum;
- outstanding image descriptors: 12 maximum by default, clamped to 4..24;
- one pose, CPU delegate, segmentation masks disabled.

The backend emits compact `stage 1/4` through `stage 4/4` plugin markers,
`runtime 1/5` through `runtime 5/5` Unity/VP4U markers, and pose acquired/lost
state changes. Detailed long-running diagnostics remain in `tools/`.

## Packaging boundary

Create a complete release bundle with:

```powershell
./tools/package.ps1 -Backend webcam
```

The MediaPipe runtime, Pose Landmarker model, upstream license, and notice are
carried together in the archive. Every input is checked against
`dependency-lock.json` before packaging.

## Current validation boundary

The native MediaPipe ABI, plugin-relative loading, model creation, repeated
pose inference, landmark mapping, and packaging layout have been tested. The
current development host enumerates no USB camera, so physical capture,
calibration, face-to-face orientation, sustained resource use, and a complete
game remain required before the webcam backend can be called hardware-validated.
