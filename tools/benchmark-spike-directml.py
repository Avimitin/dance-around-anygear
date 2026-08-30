"""Measure fixed-batch SPiKE latency through the production runtime."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort

from anygear_spike.model_runtime import RealtimeSpikeModel


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("--iterations", type=int, default=40)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--maximum-p95-ms", type=float, default=0.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.iterations < 5:
        parser.error("--iterations must be at least 5")

    model = RealtimeSpikeModel(
        args.model, device_index=args.device, warmup_runs=args.warmup
    )
    generator = np.random.default_rng(20260830)
    clip = generator.normal(
        0.0, 0.45, (1, model.frames_per_clip, model.point_count, 3)
    ).astype(np.float32)
    clip[..., 2] = np.abs(clip[..., 2])
    timings = []
    for _ in range(args.iterations):
        started = time.perf_counter_ns()
        output = model.infer(clip)
        timings.append((time.perf_counter_ns() - started) / 1_000_000)
    if output.shape != (1, 15, 3) or not np.isfinite(output).all():
        raise RuntimeError("SPiKE produced an invalid output tensor")
    result = {
        "schema": "dance-around-anygear.spike-directml-benchmark.v1",
        "model_sha256": sha256(args.model),
        "model_bytes": args.model.stat().st_size,
        "onnxruntime": ort.__version__,
        "providers": model.session.get_providers(),
        "input": [1, model.frames_per_clip, model.point_count, 3],
        "input_dtype": "float16",
        "iterations": args.iterations,
        "warmup": args.warmup,
        "device": args.device,
        "median_ms": float(np.median(timings)),
        "p95_ms": float(np.percentile(timings, 95)),
        "minimum_ms": float(np.min(timings)),
        "maximum_ms": float(np.max(timings)),
    }
    rendered = json.dumps(result, indent=2) + "\n"
    print(rendered, end="")
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    if args.maximum_p95_ms > 0 and result["p95_ms"] > args.maximum_p95_ms:
        raise RuntimeError(
            f"P95 {result['p95_ms']:.3f} ms exceeds "
            f"{args.maximum_p95_ms:.3f} ms"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
