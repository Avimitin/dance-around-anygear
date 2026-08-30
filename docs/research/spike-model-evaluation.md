# SPiKE model acceptance

A fine-tuned checkpoint is not a release model. It becomes a candidate only
after export to the same fixed FP16 ONNX graph used by the frozen DirectML
worker. A candidate then has four independent gates:

1. final-graph DirectML latency;
2. accuracy on complete held-out D430/Kinect sessions;
3. regression against the pinned public checkpoint on ITOP side view;
4. the standard live motion sequence and a complete game.

The candidate manifest starts with the last three gates set to `false`. The
assessment tools attach checked reports but never copy a candidate into
`.deps`, `dist`, or a package staging directory.

## Export

```powershell
./tools/export-spike-finetuned.ps1 `
  -TrainingSummary C:\cabinet\experiments\spike\summary.json `
  -MaximumP95Ms 25
```

This verifies the retained PyTorch state, exports it with the pinned CPU
PyTorch/ONNX environment, checks the ONNX metadata, and benchmarks the final
graph through ONNX Runtime DirectML. The default output is an ignored
timestamped directory under `build/experiments/spike-candidates`.

## Held-out D430 sessions

Use one or more complete sessions that did not appear in training. Aggregate
hashes covering every raw capture file are part of the dataset contract and a
duplicate session is rejected even if a directory or manifest name changed.

```powershell
./tools/assess-spike-candidate.ps1 `
  -CandidateManifest C:\cabinet\candidates\candidate-manifest.json `
  -HeldOutManifest @(
    'C:\cabinet\datasets\validation-a\dataset-manifest.json'
    'C:\cabinet\datasets\validation-b\dataset-manifest.json'
  )
```

The final ONNX candidate and pinned baseline are evaluated on the same clips.
The default gates require:

- DirectML P95 at or below 25 ms;
- MPJPE at or below 80 mm;
- mean hand/foot endpoint error at or below 100 mm;
- no left/right identity swaps;
- at least 2% lower MPJPE than the pinned public model.

The report also retains 10 cm PCK, per-joint error, and consecutive-frame
velocity error. A sample cap exists for quick diagnostics, but an acceptance
run uses every held-out sample.

## Public ITOP regression

The public check requires ITOP Dataset version 1.0's side-view test point
cloud and labels. These are research inputs and are not committed or included
in an Anygear release. The exact URLs, archive hashes, expanded hashes, sizes,
and h5py 3.14.0 Windows wheel are pinned in `dependency-lock.json` and
`runtime/spike/uv.lock`.

Prepare an external directory with at least 7 GiB free:

```powershell
./tools/prepare-itop-test.ps1 `
  -Destination C:\cabinet\datasets\ITOP\side-test `
  -Download
```

The two upstream downloads are:

- [ITOP side test point cloud](https://zenodo.org/records/3932973/files/ITOP_side_test_point_cloud.h5.gz?download=1),
  MD5 `3F5227D6F260011B19F325FFFDE08A65`;
- [ITOP side test labels](https://zenodo.org/records/3932973/files/ITOP_side_test_labels.h5.gz?download=1),
  MD5 `7205B0BA47F76892742DED774754D7A1`.

The dataset's [Zenodo record](https://zenodo.org/records/3932973) documents
10,501 side-view test frames, 15-joint camera-space labels, file schemas, and
the required citation. The preparation script checks both Zenodo's MD5 values
and the repository's stronger SHA-256 pins before and after decompression.

Run the complete public comparison:

```powershell
./tools/assess-spike-itop.ps1 `
  -CandidateManifest C:\cabinet\candidates\candidate-manifest.json `
  -DatasetRoot C:\cabinet\datasets\ITOP\side-test
```

Both models consume the official point cloud through the project's production
7.5 cm connected-component isolation, fixed three-frame/4096-point sampling,
and identical centering. The candidate may lose at most 0.5 percentage points
of 10 cm PCK, regress at most 2% in MPJPE, and may not introduce more
left/right swaps than the baseline. `-Samples` is only for an entry-point
check; public acceptance uses the default complete set.

## Live gate

Public and teacher datasets cannot prove game feel. Keep the candidate outside
the release bundle until the room-specific empty-stage acquisition, standard
motion sequence, reacquisition test, and one complete game satisfy the
realtime gates in the main D430 research plan. The live check must use the
same candidate SHA-256 recorded by the two offline reports.
