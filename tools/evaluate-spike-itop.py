"""Compare final SPiKE ONNX models on the public ITOP side-view test set."""

from __future__ import annotations

import argparse
from dataclasses import replace
import gc
import json
import math
from pathlib import Path
import time

import h5py
import numpy as np
import onnxruntime as ort

from anygear_spike.depth import (
    IsolationOptions,
    isolate_point_cloud,
    sample_points,
)
from anygear_spike.evaluation import PoseMetrics
from anygear_spike.model_runtime import RealtimeSpikeModel
from anygear_spike.teacher_dataset import sha256


ITOP_ISOLATION = replace(
    IsolationOptions(),
    minimum_x_m=-0.85,
    maximum_x_m=0.95,
    minimum_y_m=-1.70,
    maximum_y_m=1.00,
    minimum_z_m=1.85,
    maximum_z_m=3.70,
    voxel_size_m=0.075,
    cluster_offset_m=0.20,
)


def decode_identifier(value) -> str:
    if isinstance(value, bytes):
        return value.decode("ascii")
    if isinstance(value, np.bytes_):
        return bytes(value).decode("ascii")
    array = np.asarray(value)
    if array.dtype == np.uint8:
        return bytes(array.tolist()).decode("ascii").rstrip("\0")
    return str(value)


def dataset_contract(root: Path, include_hashes: bool) -> dict:
    point_path = root / "ITOP_side_test_point_cloud.h5"
    label_path = root / "ITOP_side_test_labels.h5"
    for path in (point_path, label_path):
        if not path.is_file():
            raise ValueError(f"ITOP test file is absent: {path}")
    with h5py.File(point_path, "r") as points, h5py.File(
        label_path, "r"
    ) as labels:
        required_points = {"data", "id"}
        required_labels = {
            "id",
            "is_valid",
            "real_world_coordinates",
        }
        if not required_points.issubset(points) or not (
            required_labels.issubset(labels)
        ):
            raise ValueError("ITOP files do not expose the required datasets")
        count = len(labels["id"])
        expected_shapes = {
            "point data": (count, 76800, 3),
            "label joints": (count, 15, 3),
            "label validity": (count,),
        }
        actual_shapes = {
            "point data": points["data"].shape,
            "label joints": labels["real_world_coordinates"].shape,
            "label validity": labels["is_valid"].shape,
        }
        for name, expected in expected_shapes.items():
            if tuple(actual_shapes[name]) != expected:
                raise ValueError(
                    f"ITOP {name} shape {actual_shapes[name]} != {expected}"
                )
        if len(points["id"]) != count or not np.array_equal(
            points["id"][:], labels["id"][:]
        ):
            raise ValueError("ITOP point-cloud and label identifiers differ")
    files = []
    for path in (point_path, label_path):
        value = {"file": path.name, "bytes": path.stat().st_size}
        if include_hashes:
            value["sha256"] = sha256(path)
        files.append(value)
    return {"frames": count, "files": files}


def candidate_clips(
    labels, maximum_samples: int
) -> list[tuple[int, tuple[int, ...], str, int]]:
    identifiers = [decode_identifier(value) for value in labels["id"]]
    index_by_identifier = {
        identifier: index for index, identifier in enumerate(identifiers)
    }
    result = []
    for target_index, identifier in enumerate(identifiers):
        if not bool(labels["is_valid"][target_index]):
            continue
        person, frame_text = identifier.split("_", maxsplit=1)
        frame = int(frame_text)
        previous = tuple(
            index_by_identifier.get(f"{person}_{value:05d}", -1)
            for value in range(frame - 2, frame + 1)
        )
        if min(previous) >= 0:
            result.append((target_index, previous, person, frame))
    if 0 < maximum_samples < len(result):
        selected = np.linspace(
            0, len(result) - 1, maximum_samples, dtype=np.int64
        )
        result = [result[int(index)] for index in np.unique(selected)]
    if not result:
        raise ValueError("ITOP selection contains no valid three-frame clips")
    return result


def person_points(points) -> np.ndarray:
    cloud = np.asarray(points, dtype=np.float32)
    selected = isolate_point_cloud(cloud, ITOP_ISOLATION)
    if not len(selected):
        raise ValueError("ITOP body isolation contains no valid points")
    return selected


def evaluate_model(
    root: Path,
    selections,
    model_path: Path,
    expected_sha256: str,
    device: int,
    warmup: int,
) -> dict:
    actual_hash = sha256(model_path)
    if actual_hash != expected_sha256.upper():
        raise ValueError(f"SPiKE ITOP model hash mismatch: {model_path}")
    model = RealtimeSpikeModel(
        model_path, device_index=device, warmup_runs=warmup
    )
    metrics = PoseMetrics()
    timings = []
    point_path = root / "ITOP_side_test_point_cloud.h5"
    label_path = root / "ITOP_side_test_labels.h5"
    with h5py.File(point_path, "r") as points, h5py.File(
        label_path, "r"
    ) as labels:
        for sample_index, (target_index, frames, person, frame) in enumerate(
            selections, 1
        ):
            clip = np.stack(
                [
                    sample_points(
                        person_points(points["data"][source_index]),
                        model.point_count,
                    )
                    for source_index in frames
                ]
            ).astype(np.float32, copy=False)
            centroid = clip.reshape(-1, 3).mean(axis=0)
            clip -= centroid.reshape(1, 1, 3)
            started = time.perf_counter_ns()
            prediction = model.infer(clip[None, ...])[0] + centroid[None, :]
            timings.append((time.perf_counter_ns() - started) / 1_000_000)
            target = np.asarray(
                labels["real_world_coordinates"][target_index],
                dtype=np.float32,
            )
            metrics.update(
                prediction,
                target,
                np.ones(15, dtype=np.float32),
                0.99,
                (person, 0),
                frame,
            )
            if sample_index % 250 == 0 or sample_index == len(selections):
                print(
                    f"[ITOP] model={actual_hash[:12]} "
                    f"samples={sample_index}/{len(selections)}"
                )
    return {
        "model_sha256": actual_hash,
        "model_bytes": model_path.stat().st_size,
        "onnxruntime": ort.__version__,
        "providers": model.session.get_providers(),
        "directml_device": device,
        "warmup": warmup,
        "latency": {
            "samples": len(timings),
            "median_ms": float(np.median(timings)),
            "p95_ms": float(np.percentile(timings, 95)),
            "minimum_ms": float(np.min(timings)),
            "maximum_ms": float(np.max(timings)),
        },
        "quality": metrics.result(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--baseline-model", type=Path, required=True)
    parser.add_argument("--baseline-expected-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=0)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--maximum-p95-ms", type=float, default=25.0)
    parser.add_argument("--maximum-pck-drop", type=float, default=0.005)
    parser.add_argument(
        "--maximum-mpjpe-relative-regression", type=float, default=0.02
    )
    parser.add_argument("--skip-dataset-hash", action="store_true")
    parser.add_argument("--require-gates", action="store_true")
    args = parser.parse_args()
    if args.samples < 0 or args.warmup < 0:
        parser.error("sample limit and warmup must be nonnegative")
    if not math.isfinite(args.maximum_p95_ms) or args.maximum_p95_ms <= 0.0:
        parser.error("latency gate must be positive and finite")
    if not 0.0 <= args.maximum_pck_drop <= 1.0 or not (
        0.0 <= args.maximum_mpjpe_relative_regression <= 1.0
    ):
        parser.error("public-regression gates must be inside 0..1")

    root = args.dataset.resolve()
    contract = dataset_contract(root, not args.skip_dataset_hash)
    with h5py.File(root / "ITOP_side_test_labels.h5", "r") as labels:
        selections = candidate_clips(labels, args.samples)
    candidate = evaluate_model(
        root,
        selections,
        args.model.resolve(),
        args.expected_sha256,
        args.device,
        args.warmup,
    )
    gc.collect()
    baseline = evaluate_model(
        root,
        selections,
        args.baseline_model.resolve(),
        args.baseline_expected_sha256,
        args.device,
        args.warmup,
    )
    candidate_pck = float(candidate["quality"]["pck_10cm"])
    baseline_pck = float(baseline["quality"]["pck_10cm"])
    candidate_mpjpe = float(candidate["quality"]["mpjpe_m"])
    baseline_mpjpe = float(baseline["quality"]["mpjpe_m"])
    pck_drop = baseline_pck - candidate_pck
    mpjpe_regression = (
        candidate_mpjpe / baseline_mpjpe - 1.0
        if baseline_mpjpe > 0.0
        else (0.0 if candidate_mpjpe == 0.0 else 1.0)
    )
    checks = {
        "directml_p95": candidate["latency"]["p95_ms"]
        <= args.maximum_p95_ms,
        "pck_regression": pck_drop <= args.maximum_pck_drop,
        "mpjpe_regression": mpjpe_regression
        <= args.maximum_mpjpe_relative_regression,
        "left_right_swaps": candidate["quality"]["left_right_swap_fraction"]
        <= baseline["quality"]["left_right_swap_fraction"],
    }
    checks["passed"] = all(checks.values())
    report = {
        "schema": "dance-around-anygear.spike-itop-side.v1",
        "dataset": contract,
        "preprocessing": {
            "geometry": "official point cloud",
            "foreground": "production 7.5 cm connected-component isolation",
            "frames_per_clip": 3,
            "points_per_frame": 4096,
            "sampling": "production fixed sample",
            "centering": "three-frame point centroid",
        },
        "selected_samples": len(selections),
        "candidate": candidate,
        "baseline": baseline,
        "comparison": {
            "pck_10cm_drop": pck_drop,
            "mpjpe_relative_regression": mpjpe_regression,
        },
        "thresholds": {
            "maximum_p95_ms": args.maximum_p95_ms,
            "maximum_pck_drop": args.maximum_pck_drop,
            "maximum_mpjpe_relative_regression": (
                args.maximum_mpjpe_relative_regression
            ),
        },
        "checks": checks,
        "accepted": checks["passed"],
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    print(rendered, end="")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    return 0 if checks["passed"] or not args.require_gates else 4


if __name__ == "__main__":
    raise SystemExit(main())
