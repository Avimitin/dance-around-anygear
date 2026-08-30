# Paired D430/Kinect teacher recordings

This dataset path exists to measure and improve the depth-only D430 model.
Kinect v1 supplies skeleton labels during maintainer data collection only. It
does not become a dependency of the released D430/SPiKE backend.

## Capture contract

One native recorder process owns all three sensors:

```text
D430 0: 848x480x30 native Z16 --------+
D430 1: 848x480x30 native Z16 --------+--> steady-clock timestamps --> pair index
Kinect v1: 320x240x30 depth/player ---+
Kinect v1: skeleton ------------------+
```

The D430 streams use the high-density preset with both emitters continuously
enabled. Infrared and color frames are not requested. Kinect also runs without
its color stream; the depth stream retains millimetres and the NUI hardware
player index separately. All `host_time_ns` values come from the same process
and the same `std::chrono::steady_clock`, so they can be compared directly.
Kinect depth and skeleton records additionally retain the SDK's native
`sensor_time_ms`, measured in milliseconds since sensor initialization.
The unit and clock origin follow Microsoft's archived
[SkeletonFrame.Timestamp](https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/hh855711(v=ieb.10))
and [ImageFrame.Timestamp](https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/hh855710(v=ieb.10))
contracts.

Build the project first. With the game and SPiKE worker stopped, capture a
session through the PowerShell entry point:

```powershell
./tools/record-d4xx-kinect-teacher.ps1 `
  -RealSenseRuntime C:\cabinet\runtime\realsense2.dll `
  -Seconds 120 `
  -EmptyStageSeconds 5 `
  -WarmupSeconds 2 `
  -MaxSyncDeltaMs 17
```

Leave the complete stage empty for the declared initial segment, then perform
the motion sequence from the research plan. The wrapper creates a new
timestamped directory and never reuses a previous recording directory.

## Files

Each capture has this layout:

```text
manifest.json
device-0.z16
device-0-timestamps.jsonl
device-1.z16
device-1-timestamps.jsonl
kinect-teacher.jsonl
kinect-depth.z16
kinect-player-index.u8
kinect-depth-timestamps.jsonl
synchronized-pairs.jsonl
```

The current manifest schema is `dance-around-anygear.d4xx-kinect-teacher.v2`.
The loader can still inspect v1 captures, but v1 lacks native Kinect depth and
player masks, so it cannot run surface refinement. Calibration schema v2 also
records the corrected shared X-axis convention; an old calibration must be
regenerated instead of being silently reused.

`manifest.json` records the D430 serials, native intrinsics, depth scale,
camera options, Kinect tracking options, phase boundary, clock definition,
file names, frame counts, and synchronization summary. Each Z16 file is a
headerless sequence of fixed-size little-endian frames; its dimensions and
`frame_bytes` are in the corresponding device entry.

Ingest hashes every source file, including all three raw depth streams, the
player-index stream, labels, timestamps, pairs, and manifest. The sorted file
inventory and its aggregate SHA-256 become the capture identity embedded in
both calibration and dataset manifests. A changed raw byte therefore cannot
silently reuse an earlier calibration or appear in both training and held-out
sets as if it were the original session. This full scan is offline and does
not enter the released worker.

`kinect-depth.z16` contains native 320x240 millimetre values with the three
player bits removed. `kinect-player-index.u8` preserves those bits as one byte
per pixel: zero is background and values 1 through 6 identify the NUI user
slot. This mask is produced by Kinect's depth/skeleton runtime and does not add
an image segmentation model to capture or gameplay.

Each D430 timestamp line identifies its zero-based file frame, librealsense
sequence, host timestamp, and capture phase. Each Kinect line contains:

- zero-based teacher frame and Kinect generation;
- host timestamp and `empty-stage` or `performance` phase;
- validity and aggregate confidence;
- the NUI user index associated with the selected skeleton;
- 33 compatibility landmarks in Kinect camera-space meters;
- per-landmark visibility, where fully tracked points are `1.0` and Kinect
  inferred points are below `1.0`.
- a same-frame `prefilter_*` observation captured before both
  `NuiTransformSmooth` and Anygear's gameplay stability filter. It is retained
  for lag/jitter measurement; the checked stabilized observation remains the
  default teacher until a held-out comparison justifies changing it.

`synchronized-pairs.jsonl` contains deterministic nearest-timestamp matches.
Pairs are monotonic and one-to-one for each D430 stream and the Kinect depth
stream. A line is emitted only when all three depth frames pass the configured
host-time gate around the skeleton. The Kinect depth/skeleton pair must also
pass the same limit on their common native sensor clock. Each signed `delta_ns`
is `depth host time - skeleton host time`; `sensor_delta_ms` provides the
independent on-device check. This makes stale-frame and transport skew visible
instead of hiding either in the training loader.

## Ingest and camera geometry

Validate a capture without running the model:

```powershell
./tools/prepare-spike-teacher-dataset.ps1 `
  -CaptureRoot C:\cabinet\captures\20260830-120000 `
  -QualityOnly
```

Measure the displacement and best temporal lag introduced by Kinect's native
smoother plus the gameplay stability filter:

```powershell
./tools/analyze-kinect-teacher.ps1 `
  -CaptureRoot C:\cabinet\captures\20260830-120000 `
  -OutputPath C:\cabinet\experiments\teacher-filter.json
```

The report compares the stabilized and prefilter observations at the same
hardware frame, checks lags from -5 through +5 frames by default, and reports
per-joint displacement. It is evidence for choosing a teacher-label path; it
does not alter a capture or runtime configuration.

To fit both camera transforms and create deterministic training shards, omit
`-QualityOnly`:

```powershell
./tools/prepare-spike-teacher-dataset.ps1 `
  -CaptureRoot C:\cabinet\captures\20260830-120000
```

The ingest tool verifies every depth-file size, monotonically increasing
sequence and host timestamp, teacher shape and visibility, pair identity,
one-to-one frame use, and the declared synchronization gate. It stops before
building a dataset when the capture fails the cadence or label-retention gates.
Its quality report retains median signed, P95 absolute, and maximum absolute
offsets for both host-clock pairing and the independent Kinect sensor clock.

For a new mount setup, the pinned baseline SPiKE model supplies only the
initial D430 core-joint correspondence. A robust fit removes high-residual
points and estimates an initial proper rotation and translation. New v2
captures then refine that transform against synchronized body surfaces:
Kinect points come from the native player mask, D430 points come from the same
production isolation path used at runtime, and deterministic trimmed
multi-frame correspondences update the six-degree camera transform. Limits on
rotation/translation correction and before/after symmetric surface error stop
a bad overlap from being accepted. Initial and refined statistics,
model/config hashes, serials, axis convention, and matrices are saved as
`camera-calibration.json`. A P95 fit residual above 200 mm is rejected by
default, and the stricter symmetric whole-surface P95 gate is 150 mm.

The surface stage is offline only. Its pinned SciPy `cKDTree` search is not
packaged into the DirectML worker and therefore has no gameplay cost.

The resulting `dataset-manifest.json` uses schema
`dance-around-anygear.spike-teacher-dataset.v2` and refers to deterministic
`.npy` shards.
Each sample contains a three-frame 4096-point clip, its clip centroid, centered
15-joint target, per-joint visibility mask, raw point counts, and source frame
identities. Clips are stored as FP16 to match the released inference graph;
labels and centroids remain FP32. Supplying the checked calibration with
`-CalibrationPath` regenerates the dataset without repeating baseline model
inference.

A clip is retained only when its three D430 frame numbers are consecutive and
each adjacent host timestamp is at most 75 ms apart. An occasional transport
drop may pass the session-level cadence gate, but it cannot silently stretch
the model's fixed temporal window. The released worker applies the same rule.

Each camera must retain at least 30 complete labeled clips after isolation by
default. This prevents a technically valid capture with almost no usable body
point clouds from silently becoming a training session. Override the sample
floor only for an explicit diagnostic, not for a retained experiment.

## Quality rules

A session is retained for supervised work only when all of these checks pass:

1. Both D430 streams sustain at least 29 frames/s without a long gap.
2. Kinect depth sustains at least 29 frames/s, and at least 95% of valid
   performance frames contain a synchronized, nonempty depth surface whose
   NUI player slot matches the selected skeleton. A nonzero skeleton user
   index alone does not satisfy this gate.
3. Kinect skeleton delivery also sustains at least 29 frames/s without a long
   gap; at least 95% of performance frames contain a body, and every tracked
   torso, shoulder, hip, knee, and foot label is fully tracked in at least 95%
   of those frames.
4. At least 95% of valid Kinect frames receive a complete three-depth-frame pair
   inside the 17 ms gate.
5. The empty-stage segment contains no person and both D430 views contain the
   complete intended play area.
6. Camera mounts remain fixed for the complete session.

Inferred Kinect joints are preserved in the recording for diagnostics but are
masked from supervised position loss. Raw captures stay outside the public
source tree. Record the source commit and aggregate capture SHA-256 with every
derived experiment result; the manifest-only hash remains as a secondary
reference.

## Geometry and dataset split

The Kinect skeleton and each D430 point cloud start in different camera spaces.
Kinect SDK 1.8's own projection maps positive skeleton X to increasing
depth-image X. The D430 path uses the same image-right X and image-up Y
convention, so the loader preserves both axes and estimates only a proper rigid
transform for the physical mount. Introducing an X reflection here makes a
proper rotation mathematically unable to align left and right surfaces. Store
the derived transform with the experiment; do not silently fold it into model
output or average coordinates from uncalibrated cameras.

The coordinate definition is directly reproducible from
`NuiTransformDepthImageToSkeleton` and the nominal 320x240 focal length in the
[Kinect for Windows SDK 1.8](https://www.microsoft.com/en-us/download/details.aspx?id=40278).

Split training and validation by complete recording session and mount setup.
Adjacent frames from one motion or song are highly correlated and cannot be
placed on opposite sides of the split. Report temporal jitter, endpoint error,
dropout duration, left/right swaps, and cadence in addition to mean joint
position error.

## Fine-tuning

Use at least two independently recorded sessions: one or more complete
sessions for training and a different complete session for validation. The
trainer checks aggregate source-content fingerprints and refuses a session
that appears in both sets.

```powershell
$TrainingSessions = @(
  'C:\cabinet\datasets\session-a\dataset-manifest.json'
  'C:\cabinet\datasets\session-b\dataset-manifest.json'
)
./tools/train-spike-teacher.ps1 `
  -TrainManifest $TrainingSessions `
  -ValidationManifest C:\cabinet\datasets\session-c\dataset-manifest.json `
  -Epochs 20 `
  -BatchSize 1
```

The pinned research environment uses PyTorch CUDA through `uv`; it is separate
from the released DirectML worker. The trainer starts from the checked public
SPiKE checkpoint and masks every Kinect-inferred joint. Its objective combines
tracked-joint position, bone length, consecutive-frame velocity, and explicit
left/right identity margins. Validation reports MPJPE, 10 cm PCK, endpoint
error, left/right swap fraction, and per-frame velocity error.

Interrupted training resumes from `latest.pth` in the same output directory.
The checkpoint retains optimizer, scheduler, gradient-scaler, and shuffled
data-generator state. Resume is rejected if the arguments, session manifests,
or JSONL history differ, so a resumed run follows the same sample order as the
uninterrupted run.

The best PyTorch state is still a research result. Before it can replace the
released model it must be exported to the fixed FP16 ONNX graph, measured on
the held-out D430 sessions and the public ITOP set, and pass the realtime gates
in the main research plan. A lower training loss alone is not an acceptance
criterion.

Export and measure a completed training run without changing the release
bundle:

```powershell
./tools/export-spike-finetuned.ps1 `
  -TrainingSummary C:\cabinet\experiments\spike\summary.json `
  -MaximumP95Ms 25
```

This produces a candidate ONNX model, export metadata, DirectML benchmark, and
`candidate-manifest.json`. The manifest keeps held-out D430, public ITOP, and
live-game acceptance false until those checks are run. The command never
copies the candidate into `.deps`, `dist`, or the package staging tree.
The remaining held-out and public regression commands are documented in
[SPiKE model acceptance](spike-model-evaluation.md).
