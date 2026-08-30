"""Evaluate a final fixed SPiKE ONNX model on held-out D430 sessions."""

from __future__ import annotations

import argparse
import gc
import json
import math
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort

from anygear_spike.evaluation import PoseMetrics
from anygear_spike.model_runtime import RealtimeSpikeModel
from anygear_spike.teacher_dataset import (
    load_dataset_manifests,
    load_shard_arrays,
    sha256,
)


def selected_positions(total: int, maximum: int) -> set[int] | None:
    if maximum <= 0 or total <= maximum:
        return None
    positions = np.linspace(0, total - 1, maximum, dtype=np.int64)
    return {int(position) for position in positions}


def evaluate_model(
    path: Path,
    expected_sha256: str,
    shards,
    minimum_visibility: float,
    maximum_samples: int,
    device: int,
    warmup: int,
) -> dict:
    actual_hash = sha256(path)
    if actual_hash != expected_sha256.upper():
        raise ValueError(f"SPiKE evaluation model hash mismatch: {path}")
    model = RealtimeSpikeModel(path, device_index=device, warmup_runs=warmup)
    total = sum(shard.samples for shard in shards)
    selected = selected_positions(total, maximum_samples)
    metrics = PoseMetrics()
    timings = []
    global_index = 0
    for shard in shards:
        arrays = load_shard_arrays(str(shard.root))
        for local_index in range(shard.samples):
            include = selected is None or global_index in selected
            global_index += 1
            if not include:
                continue
            clip = np.asarray(arrays["clips"][local_index])
            started = time.perf_counter_ns()
            prediction = model.infer(clip[None, ...])[0]
            timings.append((time.perf_counter_ns() - started) / 1_000_000)
            metrics.update(
                prediction,
                arrays["joints_m"][local_index],
                arrays["visibility"][local_index],
                minimum_visibility,
                (shard.session_hash, shard.device_index),
                int(arrays["device_frame"][local_index]),
            )
    if not timings:
        raise ValueError("held-out evaluation selected no samples")
    quality = metrics.result()
    return {
        "model_sha256": actual_hash,
        "model_bytes": path.stat().st_size,
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
        "quality": quality,
    }


def finite_metric(value) -> bool:
    return value is not None and math.isfinite(float(value))


def model_gates(
    result: dict,
    maximum_p95_ms: float,
    maximum_mpjpe_m: float,
    maximum_endpoint_error_m: float,
    maximum_swap_fraction: float,
) -> dict:
    quality = result["quality"]
    values = {
        "directml_p95": result["latency"]["p95_ms"] <= maximum_p95_ms,
        "mpjpe": finite_metric(quality["mpjpe_m"])
        and quality["mpjpe_m"] <= maximum_mpjpe_m,
        "endpoint_error": finite_metric(quality["endpoint_error_m"])
        and quality["endpoint_error_m"] <= maximum_endpoint_error_m,
        "left_right_swaps": finite_metric(quality["left_right_swap_fraction"])
        and quality["left_right_swap_fraction"] <= maximum_swap_fraction,
    }
    values["passed"] = all(values.values())
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", type=Path, nargs="+")
    parser.add_argument("--model", type=Path)
    parser.add_argument("--expected-sha256")
    parser.add_argument("--baseline-model", type=Path)
    parser.add_argument("--baseline-expected-sha256")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--minimum-visibility", type=float, default=0.99)
    parser.add_argument("--maximum-samples", type=int, default=0)
    parser.add_argument("--maximum-p95-ms", type=float, default=25.0)
    parser.add_argument("--maximum-mpjpe-m", type=float, default=0.08)
    parser.add_argument("--maximum-endpoint-error-m", type=float, default=0.10)
    parser.add_argument("--maximum-swap-fraction", type=float, default=0.0)
    parser.add_argument("--minimum-relative-improvement", type=float, default=0.0)
    parser.add_argument("--require-gates", action="store_true")
    args = parser.parse_args()
    required = (args.dataset, args.model, args.expected_sha256, args.output)
    if any(value is None for value in required):
        parser.error("evaluation requires dataset, model, expected hash, and output")
    if (args.baseline_model is None) != (
        args.baseline_expected_sha256 is None
    ):
        parser.error("baseline model and expected hash must be supplied together")
    if not 0.0 <= args.minimum_visibility <= 1.0:
        parser.error("minimum visibility must be inside 0..1")
    if args.maximum_samples < 0 or args.warmup < 0:
        parser.error("sample limit and warmup must be nonnegative")
    positive_gates = (
        args.maximum_p95_ms,
        args.maximum_mpjpe_m,
        args.maximum_endpoint_error_m,
    )
    if any(not math.isfinite(value) or value <= 0.0 for value in positive_gates):
        parser.error("latency and error gates must be positive and finite")
    if not 0.0 <= args.maximum_swap_fraction <= 1.0 or not (
        0.0 <= args.minimum_relative_improvement < 1.0
    ):
        parser.error("fraction gates must be inside their valid range")

    shards, identities = load_dataset_manifests(args.dataset)
    candidate = evaluate_model(
        args.model.resolve(),
        args.expected_sha256,
        shards,
        args.minimum_visibility,
        args.maximum_samples,
        args.device,
        args.warmup,
    )
    candidate["gates"] = model_gates(
        candidate,
        args.maximum_p95_ms,
        args.maximum_mpjpe_m,
        args.maximum_endpoint_error_m,
        args.maximum_swap_fraction,
    )
    baseline = None
    comparison = None
    if args.baseline_model is not None:
        gc.collect()
        baseline = evaluate_model(
            args.baseline_model.resolve(),
            args.baseline_expected_sha256,
            shards,
            args.minimum_visibility,
            args.maximum_samples,
            args.device,
            args.warmup,
        )
        baseline["gates"] = model_gates(
            baseline,
            args.maximum_p95_ms,
            args.maximum_mpjpe_m,
            args.maximum_endpoint_error_m,
            args.maximum_swap_fraction,
        )
        candidate_mpjpe = float(candidate["quality"]["mpjpe_m"])
        baseline_mpjpe = float(baseline["quality"]["mpjpe_m"])
        if baseline_mpjpe == 0.0:
            relative = 0.0 if candidate_mpjpe == 0.0 else -1.0
        else:
            relative = 1.0 - candidate_mpjpe / baseline_mpjpe
        comparison = {
            "mpjpe_relative_improvement": relative,
            "minimum_relative_improvement": args.minimum_relative_improvement,
            "passed": relative >= args.minimum_relative_improvement,
        }

    accepted = candidate["gates"]["passed"] and (
        comparison is None or comparison["passed"]
    )
    report = {
        "schema": "dance-around-anygear.spike-heldout-d430.v1",
        "datasets": [
            {
                "manifest": identity.manifest.name,
                "manifest_sha256": identity.manifest_sha256,
                "source_manifest_sha256": identity.source_manifest_sha256,
                "source_capture_sha256": identity.source_capture_sha256,
                "session": identity.session,
            }
            for identity in identities
        ],
        "minimum_visibility": args.minimum_visibility,
        "maximum_samples": args.maximum_samples,
        "thresholds": {
            "maximum_p95_ms": args.maximum_p95_ms,
            "maximum_mpjpe_m": args.maximum_mpjpe_m,
            "maximum_endpoint_error_m": args.maximum_endpoint_error_m,
            "maximum_swap_fraction": args.maximum_swap_fraction,
        },
        "candidate": candidate,
        "baseline": baseline,
        "comparison": comparison,
        "accepted": accepted,
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    print(rendered, end="")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    return 0 if accepted or not args.require_gates else 4


if __name__ == "__main__":
    raise SystemExit(main())
