# Support matrix

| Backend | Sensor input | Pose source | RGB required | Status |
|---|---|---|---|---|
| Native D435 | RealSense D435 | Vendor VP4U | Vendor-defined | Not an Anygear target |
| Kinect v1 | Kinect for Windows SDK 1.8 NUI | Hardware skeleton (20 joints) | No | Validated in game |
| D4xx | librealsense 2.50.0 depth/IR | Native pose path under evaluation | No | Dependency stack identified; hardware validation in progress |
| Webcam | Media Foundation | MediaPipe v1 Pose Landmarker Lite (33 joints) | Yes | Model/ABI validated; USB and in-game validation pending |
| SteamVR | OpenVR standing-space tracked devices | HMD/controllers plus role-assigned body trackers | No | Mapping/ABI validated; hardware and in-game validation pending |

## Kinect validated profile

- Skeleton stream: 30 Hz NUI.
- Preview: 15 Hz default, backed by a shared 1x1 sentinel when color is off.
- Outstanding image descriptors: hard cap of 12; excess frames are dropped.
- NUI smoothing: `0.35 / 0.65 / 0.00 / 0.05 / 0.04`.
- Tracking ID lock: 400 ms.
- Last complete body hold: 500 ms.
- Parent-relative joint speed bound: 8 m/s.
- Learned bone-length tolerance: 35%.
- Stage reflection: `x' = 1.10 - x`, `z' = 2 * hipZ - z`.

The profile above is the last configuration reported as stable through a full
game. Changes require both isolated harness evidence and an in-game run.

## Webcam validated profile

- Official MediaPipe `v1.0.0` Windows x86-64 C runtime, dynamically loaded.
- Pose Landmarker VIDEO mode, CPU delegate, one person, segmentation disabled.
- USB frames captured through Windows Media Foundation as BGR8 and bounded to
  848x480 before inference.
- Pose callback: 30 Hz target; preview: 15 Hz; image pool cap: 12.
- Official test image: 60/60 valid results at approximately 75 FPS on the
  development host.
- Python, OpenCV, helper processes, and runtime downloads are not required.
- Current development host enumerated zero USB cameras, so real capture,
  performer/avatar orientation, and full game calibration remain unvalidated.

## D4xx evaluation profile

- Two physical D430 depth modules are visible to Windows.
- RGB is disabled; the intended streams are Depth Z16 and Infrared Y8.
- The matching native RealSense C API reports `25000` (librealsense 2.50.0).
- Native pose inference requires TensorRT 7.2.1.6, CUDA 11.0 Update 1, and
  cuDNN 8.0.4. These are application dependencies, not display-driver files.
- The complete download and installation procedure is in
  [realsense-d4xx.md](backends/realsense-d4xx.md).
- Full in-game validation is pending restoration of the genuine NVIDIA
  runtime; placeholder DLLs are not accepted as evidence.

## SteamVR validated profile

- Official OpenVR SDK/client runtime `v2.15.6`, dynamically loaded.
- Background pose client at 60 Hz; no compositor, render target, or RGB path.
- Strict minimum: HMD, left/right controllers, waist, left foot, right foot.
- Optional SteamVR roles: chest, left/right shoulder, elbow, and knee.
- Standing-space origin is sampled once from the waist; subsequent room-scale
  movement is retained and mapped around `(0.55, 0.00, 2.30)` meters.
- Preview and pose-image callbacks use shared 1x1 sentinels.
- Synthetic six-point mapping, all three loader paths, and VP4U ABI pass
  offline. No SteamVR installation was present on the development host, so
  device-role discovery and in-game orientation remain hardware-test items.
