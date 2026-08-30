# RealSense D4xx depth-only SPiKE backend

This is the research-quality path for D4xx modules that cannot use the game's
native D435 profile. It consumes metric depth only. RGB, an emulated color
stream, MediaPipe, and the native TensorRT pose engine are not part of this
backend.

## Runtime architecture

```text
two librealsense Z16 streams
  -> distortion-corrected camera-space XYZ point clouds
  -> short far-depth background calibration (no Y8/RGB stream)
  -> foreground, stage bounds, floor rejection, and cc3d components
  -> deterministic 4096-point samples across three frames
  -> fixed-batch SPiKE ONNX model through DirectML
  -> per-camera confidence and optional calibrated fusion
  -> bounded shared-memory IPC
  -> VP4U pose callbacks and 1x1 image sentinels
```

Spice loads only `dance_around_anygear_d4xx_spike.dll`. The DLL starts and
stops the adjacent worker, publishes native depth frames through the bounded
IPC region, maps the 15 SPiKE joints to VP4U, and reports worker faults in its
normal log. The worker is frozen with Python 3.10.5, but downstream machines do
not need Python installed.

The production bundle uses ONNX Runtime DirectML 1.23.0. PyTorch is used only
by maintainers to convert the published checkpoint into a fixed FP16 ONNX
graph. CUDA, cuDNN, TensorRT, PyTorch, MediaPipe, and network access are not
runtime requirements. See the official
[DirectML execution-provider documentation](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)
for the DirectX 12 device requirements.

## Current evidence

The following results were measured on the development machine with an RTX
2070 SUPER:

| Check | Result |
|---|---:|
| Fixed batch-1 DirectML inference, median (100 runs) | 21.29 ms |
| Fixed batch-1 DirectML inference, P95 (100 runs) | 23.06 ms |
| Recorded dual-D430 isolation, median/P95 per camera | 7.93/10.20 ms |
| Brown/inverse-Brown vector deprojection vs librealsense 2.50.0 | float32-identical test vectors |
| DirectML versus PyTorch CUDA maximum absolute output difference | 0.0004883 m |
| ITOP side-view 10 cm PCK, 64-sample preprocessing check | 90.10% |
| Frozen worker size, including notices | 107.52 MiB |
| Complete Windows release archive | 168.99 MiB |

The ONNX export is byte-for-byte reproducible at 143,295,911 bytes with
SHA-256 `C48C40FF94C8F358762DB296DA0117DCFF78D8C4AD2FA4DB575298979BF2DA0D`.
Two consecutive frozen-worker builds also reproduce all 77 bundled files; the
112,745,930-byte directory has entry-EXE SHA-256
`7C293A86BB0065326A3606E8F79BE66707835B7907BD0317676B78E11E0FA7C7`.
Two consecutive release packages also reproduce the same 177,203,203-byte
ZIP and SHA-256
`1785145996603936C66E2EC4C7D082272A24986621F51091DCE5FBF60586BF5D`.

Offline IPC, empty-stage acquisition, model loading, failure propagation, and
all shared loader/VP4U tests pass. In the 2026-08-30 dual-D430 field session,
the final DirectML bundle calibrated twice and completed two full
gameplay-to-result cycles. Pose availability reached 100% during stable
segments, live inference was approximately 27--35 ms, and the windowed game
held near 59--60 rendered FPS for most of the session.

Version 0.4.0 records this as a playable experimental baseline. It does not
claim production-grade fine-joint tracking: visible hand/foot motion still has
residual jitter and lag, and the model topology cannot measure wrist roll,
thumb direction, ankle rotation, heel, or toe direction. Further work should
be judged against full-game latency and stability rather than endpoint detail
alone.

The quantitative live-test sequence, realtime gates, and paired Kinect teacher
data fallback are defined in the
[D430 depth-pose research plan](../research/d430-spike-roadmap.md).

## Why inference is fixed batch 1

One DirectML inference fits inside the 30 Hz frame budget on the development
GPU. Two camera inferences in one output measured roughly 38-40 ms, which
would make the avatar visibly late. The default therefore evaluates the
primary camera on normal outputs and keeps the second camera available as a
capture fallback. In calibrated mode, the secondary view takes the single
batch-1 inference slot once every `SecondaryEveryN` outputs; it is never added
as a second inference in the same output. The recent primary result is reused
for that one output and refreshed on the next input frame.

This schedule bounds the model portion of every output to one measured
batch-1 call while still allowing a calibrated second view to repair occluded
joints. It does not pretend that two uncalibrated camera coordinate systems
are interchangeable.

The worker also publishes the fused per-joint 0/1/2 tracking state. The native
adapter preserves that state instead of deriving it again from confidence, so
a missing hand or foot remains untracked and its placeholder coordinate is not
reported to the game as an inferred endpoint. Missing endpoints retain their
last valid coordinate without updating the smoothing history, and a complete
body loss resets that history before reacquisition.

The model still produces one measured pose per completed inference. The VP4U
adapter resamples that pose at 60 Hz without running a second inference. Source
timestamps bound short capture-age prediction; motion below the endpoint noise
radius is never extrapolated, and the synthesized hand/foot landmarks move as
rigid groups. This prevents a small model fluctuation from becoming a much
larger wrist or ankle rotation in the consumer.

ITOP/SPiKE exposes 15 joints and does not observe wrist, thumb, ankle, heel, or
toe orientation separately. The adapter therefore supplies a stable neutral
palm frame from the forearm and body plane and a neutral foot frame from the
lower leg and body-facing direction. Hand and foot position remains measured,
but wrist roll and toe rotation cannot be recovered from this model topology.

For calibrated views, a severe per-joint disagreement is never averaged. A
view with a clear surface, kinematic, and temporal confidence advantage wins
with confidence equal to its margin over the other view; equally plausible
but conflicting predictions become untracked. This lets a clean side repair
an occluded hand or foot without guessing when camera evidence is ambiguous.

## Installation layout

Release archives have this fixed layout:

```text
dance_around_anygear_d4xx_spike.dll       <- only Spice -k argument
dance_around_anygear_d4xx_spike.json      <- room/camera configuration
dance_around_anygear_d4xx_spike/
  realsense2.dll                           <- official librealsense 2.50.0
  LICENSE.librealsense.txt
  spike-itop-side-primary-fp16.onnx
  worker/
    dance_around_anygear_spike_worker.exe
    _internal/...
    AUTHORS.cc3d.txt
    LICENSE.cc3d-*.txt
    LICENSE.*.txt
    NOTICE.onnxruntime.txt
```

Extract the whole archive beside Spice without flattening it, then add:

```text
-k dance_around_anygear_d4xx_spike.dll
```

The loader resolves every dependency relative to the entry DLL. It neither
changes the system `PATH` nor replaces the game's original
`visionposewrapper.dll` or `realsense2.dll`.

## Configuration

Copy the distributed JSON as-is, then make room-specific changes in
`dance_around_anygear_d4xx_spike.json` beside the DLL. It is read once at
startup.

Important fields:

- `D4xxPlacement.PrimarySerial` selects the preferred camera by stable serial;
  it takes precedence over `PrimaryDevice`.
- `FrameStallTimeoutMs` controls when a librealsense pipeline that has stopped
  producing complete frames is rebuilt. `ReconnectDelayMs` controls the
  serial-bound retry interval. A disconnect invalidates the old depth packet
  immediately; the defaults are 1500 ms and 500 ms.
- `EmitterMode` accepts `all-on`, `all-off`, `first-only`, `second-only`, or
  `alternating`. The `all-on` default preserves the validated baseline. Two
  nearby projectors can interfere, so use `profile-d4xx-emitters.ps1` on a
  static scene before selecting a different mode; the DLL reapplies it after
  a USB reconnect.
- `PitchDegrees`, `YawDegrees`, `RollDegrees`, and XYZ offsets describe the
  physical mount and final stage placement.
- `Isolation` bounds must contain the dancer but exclude walls, furniture, and
  the other side of the room. Acquisition requires a taller component than
  short-term tracking, preventing an empty floor from becoming a body.
- `BackgroundCalibrationFrames` defaults to 45. Keep the stage empty for this
  initial window after the camera opens. The worker builds a robust far-depth
  envelope, so a moving performer is less likely to become part of the room
  model. Set it to `0` only when manually tuned stage bounds are sufficient.
- `BackgroundMargin` is the minimum foreground separation from that room
  model. The 0.10 m default rejected every candidate in the recorded static
  high-density validation segment; smaller values retain more thin limbs but
  also admit more D430 depth flicker.
- `Runtime.DirectMlDeviceIndex` selects the DirectML adapter.
- `Runtime.SecondaryEveryN` is used only by calibrated fusion. Lower values
  replace more primary outputs with a secondary refresh. Every output still
  performs at most one DirectML inference.
- `Fusion.Mode` defaults to `primary`. Do not select `calibrated` until both
  camera serials and measured `CameraToStage` rigid transforms are present.

Camera placement should be corrected physically before widening isolation
bounds. A depth module cannot recover a hand or foot outside its field of view.
The SPiKE backend requests depth Z16 only. The default D430 profile uses the
high-density preset with both projectors continuously enabled, while the JSON
can select the measured emitter mode for a particular installation. It does
not transfer or copy a host-visible infrared frame.

The worker rejects a temporal clip if its source sequence skips or if adjacent
capture timestamps are more than 75 ms apart. It rebuilds all three frames
before the next inference, so a USB pause is not presented to SPiKE as ordinary
30 Hz motion.

Every 300 outputs, the adjacent worker log reports output cadence, valid-pose
percentage, processing, DirectML inference, and source-frame age as P50/P95.
These values are collected in fixed windows and make the realtime acceptance
gates observable during an actual song without enabling a separate profiler.

## Live point-cloud and skeleton viewer

Maintainers can inspect the production preprocessing path without starting
Unity. Build `anygear_d4xx_spike_viewer_host`, leave both camera views empty,
then run:

```powershell
./tools/start-d4xx-spike-viewer.ps1 `
  -RealSenseRuntime C:\cabinet\dance_around_anygear_d4xx_spike\realsense2.dll `
  -Config C:\cabinet\dance_around_anygear_d4xx_spike.json `
  -Model C:\cabinet\dance_around_anygear_d4xx_spike\spike-itop-side-primary-fp16.onnx
```

The loopback-only web page shows both downsampled raw depth views, the exact
background/ROI/component-isolated point clouds in camera X/Y and Z/Y, each
SPiKE prediction, capture cadence, inference latency, and skipped source-frame
count. `Camera 0`, `Camera 1`, and `dual` controls select the inference
schedule. Dual mode alternates one fixed-batch inference slot between the two
views, so it compares complementary viewpoints without issuing two DirectML
calls in one iteration.

`Recalibrate` clears the room model and requires an empty stage again. Stop the
viewer from its web page, with Ctrl+C in the host console, or with
`tools/stop-d4xx-spike-viewer.ps1`. The viewer and game are mutually exclusive
because both need exclusive access to the two D4xx streams.

The viewer preserves actual capture timing when the latest-frame IPC skips a
source sequence inside the 75 ms temporal window, while visibly accumulating
the skipped-frame counter. This keeps the diagnostic skeleton responsive and
makes scheduling loss observable. The release worker retains its stricter
sequence contract until the production IPC is converted to a bounded frame
ring.

## Maintainer commands

Prepare every pinned input, freeze the worker, verify deterministic output, and
measure the local GPU without starting the game:

```powershell
./tools/bootstrap-librealsense.ps1 -Download
./tools/bootstrap-spike-runtime.ps1 -Download
./tools/build-spike-worker.ps1 -Clean
./tools/check-spike-worker-reproducible.ps1
./tools/benchmark-spike-directml.ps1 -MaximumP95Ms 25
./tools/package.ps1 -Backend d4xx-spike
```

The benchmark threshold is deliberately opt-in because GPU and driver choices
belong to the test machine. The 25 ms model gate reserves the rest of each
33.4 ms output period for deprojection, isolation, IPC, and fusion.
