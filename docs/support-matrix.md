# Support matrix

| Backend | Sensor input | Pose source | RGB required | Status |
|---|---|---|---|---|
| Native D435 | RealSense D435 | Vendor VP4U | Vendor-defined | Not an Anygear target |
| Kinect v1 | Kinect for Windows SDK 1.8 NUI | Hardware skeleton (20 joints) | No | Validated in game |
| D4xx native | librealsense 2.50.0 depth/IR | Original VP4U path | No emulated RGB; IR is adapted to its image input | Two D430s stream; calibration quality is not yet acceptable |
| D4xx + SPiKE | librealsense 2.50.0 depth | SPiKE ITOP side-view model through DirectML | No | Dual-D430 playable baseline validated; fine endpoint motion remains experimental |
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

## D4xx/SPiKE research profile

- Two native `848x480@30` Z16 streams; RGB and MediaPipe are not opened.
- A short far-depth background calibration plus stage-bounded voxel connected
  components isolates a player before a deterministic 4096-point, three-frame
  SPiKE clip is formed.
- The point cloud applies each sensor's librealsense distortion model and
  coefficients; Brown and inverse-Brown vectors match the pinned 2.50.0 C API.
- Projector mode is JSON-selectable and restored after transport recovery, so
  dual-camera interference can be measured per installation rather than
  hidden behind a fixed emitter assumption.
- The published ITOP side-view checkpoint is exported reproducibly to a static
  FP16 ONNX graph and executed with ONNX Runtime DirectML 1.23.0.
- On the RTX 2070 SUPER development host, batch-1 median/P95 inference is
  `21.29/23.06 ms` over 100 runs, below the 25 ms model gate. Batch 2 is not used on the critical
  path because it measured approximately `38-40 ms`.
- Primary mode infers the selected camera at full cadence and treats the other
  as an acquisition fallback. Calibrated mode gives the single batch-1 slot to
  the secondary every third output and fuses recent predictions per joint; no
  output performs two model calls.
- Synthetic librealsense tests cover explicit poll faults, silent 1500 ms frame
  stalls, stale-packet invalidation, serial-bound restart, and restoration of
  camera options. Temporal clips reject sequence skips and adjacent capture
  gaps over 75 ms in both gameplay and teacher-data ingest.
- Empty-stage IPC replay, fault propagation, model load, 90.10% ITOP mAP, and
  complete byte-for-byte worker reproduction pass offline. The 2026-08-30
  field run calibrated twice and completed two gameplay-to-result cycles.
- The game was held near 59--60 rendered FPS for most of that session; worker
  inference was approximately 27--35 ms in the live room. Hand/foot motion is
  playable but remains limited by the 15-joint model's missing wrist, thumb,
  ankle, heel, and toe observations.
- The adapter preserves per-joint untracked/inferred/tracked states and keeps
  missing endpoints out of smoothing history. The worker reports 300-output
  P50/P95 windows for pipeline, inference, and source-frame age.

See [realsense-d4xx-spike.md](backends/realsense-d4xx-spike.md) for the bundle,
configuration, and benchmark procedure.

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
