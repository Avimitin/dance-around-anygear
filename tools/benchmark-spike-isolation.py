"""Replay recorded Z16 frames through production player isolation."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import time

import numpy as np

from anygear_spike.config import load_config
from anygear_spike.depth import isolate_player
from anygear_spike.protocol import DepthSnapshot


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("--frames", type=int, default=60)
    args = parser.parse_args()
    manifest = json.loads(
        (args.capture / "manifest.json").read_text(encoding="utf-8")
    )
    config = load_config(args.config)
    timings = []
    point_counts = []
    for device in manifest["devices"]:
        intrinsics = device["intrinsics"]
        width = int(intrinsics["width"])
        height = int(intrinsics["height"])
        depth_frames = np.memmap(
            args.capture / device["file"],
            mode="r",
            dtype=np.uint16,
            shape=(int(device["frame_count"]), height, width),
        )
        count = min(args.frames, len(depth_frames))
        for index in range(count):
            metadata = device["frames"][index]
            frame = DepthSnapshot(
                camera_index=int(device["index"]),
                frame_sequence=int(metadata["sequence"]),
                host_time_ns=int(metadata["host_time_ns"]),
                depth_scale_m=float(device["depth_scale_m"]),
                width=width,
                height=height,
                principal_x=float(intrinsics["principal_x"]),
                principal_y=float(intrinsics["principal_y"]),
                focal_x=float(intrinsics["focal_x"]),
                focal_y=float(intrinsics["focal_y"]),
                distortion_model=int(intrinsics["distortion_model"]),
                distortion=tuple(intrinsics["distortion"]),
                device_index=int(device["index"]),
                serial=str(device["serial"]),
                depth=np.asarray(depth_frames[index]),
            )
            started = time.perf_counter_ns()
            points = isolate_player(frame, config.isolation)
            timings.append((time.perf_counter_ns() - started) / 1_000_000)
            point_counts.append(len(points))
    output = {
        "camera_frames": len(timings),
        "median_ms": float(np.median(timings)),
        "p95_ms": float(np.percentile(timings, 95)),
        "maximum_ms": float(np.max(timings)),
        "valid_frames": int(np.count_nonzero(point_counts)),
        "maximum_points": int(np.max(point_counts)),
    }
    print(json.dumps(output, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
