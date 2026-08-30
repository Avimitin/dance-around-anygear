# D430 depth-pose research plan

## Objective

The D430 path must improve the game experience, not merely produce a visible
skeleton. The target is stable full-body tracking at the game's 30 Hz cadence
from depth-only input, with no system Python, CUDA, cuDNN, TensorRT, RGB
camera, or network requirement on a downstream machine.

The current implementation is a deliberately measurable baseline:

```text
two native 848x480x30 Z16 streams
  -> calibrated-intrinsics deprojection and empty-stage background envelope
  -> stage bounds, floor rejection, and 3D connected components
  -> three deterministic 4096-point frames
  -> SPiKE ITOP side-view FP16 ONNX through DirectML
  -> primary-view output or explicitly calibrated late fusion
  -> VP4U pose callbacks
```

SPiKE is retained because it consumes a temporal point-cloud clip directly.
It matches the D430's useful signal and avoids reconstructing a color image for
an image-pose model. The published checkpoint remains a starting point rather
than an assumption that ITOP and a home dance space have the same depth
distribution.

## Realtime acceptance gates

Measurements are taken after model warm-up. A change is not eligible for the
default runtime if it improves pose accuracy by missing the frame budget.

| Property | Default-path gate |
|---|---:|
| D430 delivery | at least 29 frames/s from each configured device |
| Player isolation P95 | at most 12 ms per camera |
| Batch-1 DirectML inference P95 | at most 25 ms |
| Primary worker pipeline P95 | at most 33.4 ms |
| Source frame to VP4U callback P95 | at most 50 ms |
| Stale depth retained as current | never; invalid after 150 ms |
| Empty-stage false acquisition | zero during a 10-minute run |
| Visible-player pose availability | at least 99% after acquisition |
| Reacquisition after a real loss | at most 750 ms |
| Left/right identity swaps | zero in the standard motion sequence |
| Static core-joint jitter | at most 20 mm RMS |
| Static hand/foot jitter | at most 35 mm RMS |

The quality target for labeled sessions is an 80 mm or lower mean per-joint
position error after rigid sensor alignment. Endpoint error, temporal jitter,
dropout duration, and left/right swaps are reported separately; a single
average must not hide a bad hand or foot path.

The current 100-run model measurement is 21.29 ms median and 23.06 ms P95 on the
development RTX 2070 SUPER. Running two views in one output exceeded the frame
budget, so the scheduler permits at most one fixed batch-1 inference per
output. The default evaluates the freshest primary view. Opt-in calibrated
fusion periodically gives that one slot to the secondary view and reuses the
recent primary result for a single output.

The pinned CUDA research environment also completed a full forward, backward,
gradient-clipping, and AdamW step for an effective batch of two in 514.5 ms,
with 1,265.2 MiB peak allocated and 1,438.0 MiB peak reserved GPU memory on the
same card. Fine-tuning therefore fits comfortably in the available 8 GiB. It
does not add work to gameplay: the trained state is exported back into the
same fixed ONNX architecture and must still pass the 25 ms inference gate.

The worker measures freshness with its own monotonic clock rather than frame
number alone. A stopped stream reaches a 150 ms invalidation deadline even
though its sequence no longer advances; the no-input loop checks that deadline
at most 20 ms later, publishes an invalid pose, and does not re-infer cached
clips. During normal play a fresh secondary view can replace a stalled primary,
while background calibration reports an explicit camera index if a stream
remains stalled.

The native capture layer separately invalidates a device as soon as
librealsense reports a poll fault, or after 1500 ms without a complete frame.
It then re-enumerates the original serial, reapplies the depth preset and
emitter state, and retries the pipeline every 500 ms. An offline synthetic
runtime covers both explicit transport faults and silent frame stalls. Online
and teacher-data three-frame clips share a stricter rule: sequence must be
consecutive and adjacent capture timestamps must be no more than 75 ms apart.
The runtime log emits 300-output windows for cadence, pose availability,
pipeline time, DirectML inference time, and source-frame age (P50/P95), so the
live gates can be evaluated from the same process used by the game.

## Validation recordings

Raw recordings remain outside the public source tree. Each session keeps the
checked capture manifest, exact camera serials and intrinsics, depth scale,
frame timestamps, runtime configuration, dependency/model hashes, aggregate
raw-capture fingerprint, and source commit. Derived numerical summaries may
be committed when they do not contain captured sensor data.

The minimum validation set is:

1. Empty stage for ten minutes, including normal D430 depth flicker.
2. Five static full-body poses at the near, center, and far stage positions.
3. Lateral steps, forward/back steps, squats, crossed limbs, hands touching,
   feet touching, turns, and short jumps.
4. Partial occlusion from each camera followed by a clean reacquisition.
5. A representative complete song with game capture running concurrently.

The first 45 depth frames of a normal session are reserved for same-session
background calibration. The stage must be empty during that interval. A room
model captured on another day or from another camera angle is not accepted as
the default because small viewpoint changes create large false foregrounds.

## Teacher-data path

If the live baseline misses the quality gates, the next step is paired D430
depth and Kinect v1 skeleton capture. Kinect supplies labels only during data
collection; it is not a dependency of the released D430 backend.

The reproducible recorder, timestamp contract, file format, and retention
checks are documented in
[Paired D430/Kinect teacher recordings](d430-kinect-teacher-data.md).

The collection and training rules are:

- Match frames by monotonic host timestamp with a maximum 17 ms separation.
- Record Kinect's native 320x240 depth and hardware player index beside its
  skeleton, without enabling RGB.
- Keep only Kinect joints reported as fully tracked; inferred joints are
  masked from the supervised loss.
- Map the Kinect 20-joint body to SPiKE's 15-joint order before training.
- Estimate one rigid Kinect-to-D430 transform per physical setup, then refine
  it with synchronized Kinect player-mask and D430 body surfaces. Retain the
  initial fit, surface residuals, and final transform in session metadata.
- Split validation by recording session and camera setup. Neighboring frames
  from one song must never be divided between training and validation.
- Begin from the pinned SPiKE checkpoint and fine-tune on isolated D430 point
  clips. Report both the public ITOP score and the held-out D430 metrics so
  domain improvement cannot silently erase the published baseline.
- Use position, bone-length, velocity, and left/right consistency losses. Do
  not add temporal smoothing that consumes more latency than it removes in
  visible jitter.

If the full model later exceeds the realtime gate on target hardware, distill
to a smaller fixed-shape student and repeat the complete accuracy/cadence
evaluation. Model reduction is a measured fallback, not the first source of
quality loss.

## Improvement order

Work proceeds in this order because each stage has an independent measurement:

1. USB delivery and deterministic Z16 capture.
2. Measured dual-projector mode, same-session foreground isolation, and
   complete-body component selection.
3. Camera-to-stage placement and primary-camera identity by serial number.
4. Single-view SPiKE accuracy and temporal stability.
5. Calibrated secondary-view repair for occluded joints.
6. D430-domain fine-tuning or distillation only when labeled evidence requires
   it.

No multi-view average is accepted without measured rigid transforms. No model
change is accepted from screenshots alone. The release stays experimental
until a live performer session, the standard motion sequence, and a complete
song meet the gates above.

## Reproduction entry points

```powershell
./tools/bootstrap-librealsense.ps1 -Download
./tools/bootstrap-spike-runtime.ps1 -Download
./tools/build-spike-worker.ps1 -Clean
./tools/benchmark-spike-directml.ps1 -MaximumP95Ms 25
./tools/test-spike-runtime.ps1
./tools/record-d4xx-kinect-teacher.ps1 `
  -RealSenseRuntime C:\cabinet\runtime\realsense2.dll -Seconds 120
./tools/prepare-spike-teacher-dataset.ps1 `
  -CaptureRoot C:\cabinet\captures\paired-session
./tools/test-spike-research.ps1
./tools/export-spike-finetuned.ps1 `
  -TrainingSummary C:\cabinet\experiments\spike\summary.json
./tools/assess-spike-candidate.ps1 `
  -CandidateManifest C:\cabinet\candidates\candidate-manifest.json `
  -HeldOutManifest C:\cabinet\datasets\validation\dataset-manifest.json
./tools/prepare-itop-test.ps1 `
  -Destination C:\cabinet\datasets\ITOP\side-test -Download
./tools/assess-spike-itop.ps1 `
  -CandidateManifest C:\cabinet\candidates\candidate-manifest.json `
  -DatasetRoot C:\cabinet\datasets\ITOP\side-test
./tools/check-spike-worker-reproducible.ps1
./tools/check-reproducible.ps1 -ToolchainRoot .deps/toolchain/mingw64
./tools/package.ps1 -Backend d4xx-spike
```

The upstream research basis is the [SPiKE paper](https://arxiv.org/abs/2409.01879)
and its [published implementation](https://github.com/iballester/SPiKE). The
runtime source commit, checkpoint, export graph, Python environment, and
Windows dependency files are independently pinned in `dependency-lock.json`
and `runtime/spike/uv.lock`.
