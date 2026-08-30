from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import numpy as np

from anygear_spike.teacher_dataset import load_dataset_manifests
from anygear_spike.teacher_data import (
    D4xxStamp,
    capture_integrity,
    load_teacher_capture,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PREPARE_SCRIPT = REPOSITORY_ROOT / "tools" / "prepare-spike-teacher-dataset.py"


def write_json_lines(path: Path, values: list[dict]) -> None:
    path.write_text(
        "".join(
            json.dumps(value, separators=(",", ":")) + "\n"
            for value in values
        ),
        encoding="utf-8",
    )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def create_synthetic_capture(root: Path) -> Path:
    root.mkdir()
    empty_frames = 5
    frame_count = 45
    interval_ns = 33_333_333
    start_ns = 1_000_000_000
    devices = []
    device_times = []
    for device_index in range(2):
        times = [
            start_ns + frame * interval_ns + device_index * 1_000_000
            for frame in range(frame_count)
        ]
        device_times.append(times)
        depth_path = root / f"device-{device_index}.z16"
        timestamp_path = root / f"device-{device_index}-timestamps.jsonl"
        frames = np.full((frame_count, 96, 96), 3500, dtype="<u2")
        frames[empty_frames:, 4:92, 8:88] = 2000
        depth_path.write_bytes(frames.tobytes())
        write_json_lines(
            timestamp_path,
            [
                {
                    "frame": frame,
                    "sequence": 100 + frame,
                    "host_time_ns": times[frame],
                    "phase": (
                        "empty-stage" if frame < empty_frames else "performance"
                    ),
                }
                for frame in range(frame_count)
            ],
        )
        devices.append(
            {
                "index": device_index,
                "serial": f"synthetic-{device_index}",
                "depth_file": depth_path.name,
                "timestamps_file": timestamp_path.name,
                "depth_scale_m": 0.001,
                "frame_bytes": 96 * 96 * 2,
                "frame_count": frame_count,
                "intrinsics": {
                    "width": 96,
                    "height": 96,
                    "principal_x": 47.5,
                    "principal_y": 47.5,
                    "focal_x": 80.0,
                    "focal_y": 80.0,
                    "distortion_model": 0,
                    "distortion": [0.0] * 5,
                },
            }
        )

    teacher_times = [time_ns + 500_000 for time_ns in device_times[0]]
    world = np.zeros((33, 3), dtype=np.float32).tolist()
    visibility = np.ones(33, dtype=np.float32).tolist()
    teacher = [
        {
            "frame": frame,
            "generation": frame + 1,
            "host_time_ns": teacher_times[frame],
            "phase": "empty-stage" if frame < empty_frames else "performance",
            "valid": frame >= empty_frames,
            "confidence": float(frame >= empty_frames),
            "user_index": 1 if frame >= empty_frames else 0,
            "world_m": world,
            "visibility": visibility,
        }
        for frame in range(frame_count)
    ]
    write_json_lines(root / "kinect-teacher.jsonl", teacher)
    pairs = []
    for frame in range(frame_count):
        paired_devices = []
        for device_index in range(2):
            timestamp = device_times[device_index][frame]
            paired_devices.append(
                {
                    "device": device_index,
                    "frame": frame,
                    "sequence": 100 + frame,
                    "host_time_ns": timestamp,
                    "delta_ns": timestamp - teacher_times[frame],
                }
            )
        pairs.append(
            {
                "pair": frame,
                "teacher_frame": frame,
                "teacher_host_time_ns": teacher_times[frame],
                "teacher_valid": frame >= empty_frames,
                "phase": (
                    "empty-stage" if frame < empty_frames else "performance"
                ),
                "devices": paired_devices,
            }
        )
    write_json_lines(root / "synchronized-pairs.jsonl", pairs)
    manifest = {
        "schema": "dance-around-anygear.d4xx-kinect-teacher.v1",
        "d4xx": {"devices": devices},
        "teacher": {
            "file": "kinect-teacher.jsonl",
            "frame_count": frame_count,
            "valid_frame_count": frame_count - empty_frames,
        },
        "synchronization": {
            "file": "synchronized-pairs.jsonl",
            "max_delta_ms": 17,
            "pair_count": frame_count,
            "valid_teacher_pair_count": frame_count - empty_frames,
        },
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    calibration = {
        "schema": "dance-around-anygear.d4xx-kinect-calibration.v2",
        "source_manifest_sha256": sha256(manifest_path),
        "source_capture": capture_integrity(load_teacher_capture(root)),
        "runtime_config_sha256": "0" * 64,
        "baseline_model_sha256": "1" * 64,
        "method": "synthetic identity fixture",
        "kinect_axis_normalization": [1.0, 1.0, 1.0],
        "minimum_teacher_visibility": 0.99,
        "devices": [
            {
                "index": index,
                "serial": f"synthetic-{index}",
                "rotation": np.eye(3, dtype=np.float32).reshape(-1).tolist(),
                "translation": [0.0, 0.0, 0.0],
                "matrix_row_major": np.eye(4).reshape(-1).tolist(),
                "correspondence_count": 32,
                "inlier_count": 32,
                "median_error_m": 0.01,
                "p95_error_m": 0.02,
                "maximum_error_m": 0.03,
            }
            for index in range(2)
        ],
    }
    calibration_path = root / "calibration.json"
    calibration_path.write_text(json.dumps(calibration), encoding="utf-8")
    return calibration_path


def load_prepare_module():
    spec = importlib.util.spec_from_file_location(
        "anygear_prepare_spike_teacher_dataset", PREPARE_SCRIPT
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_prepare(module, capture: Path, calibration: Path, output: Path) -> None:
    previous_arguments = sys.argv
    try:
        sys.argv = [
            str(PREPARE_SCRIPT),
            str(capture),
            "--output",
            str(output),
            "--config",
            str(REPOSITORY_ROOT / "config" / "dance_around_anygear_d4xx_spike.json"),
            "--calibration",
            str(calibration),
            "--shard-size",
            "7",
            "--minimum-samples-per-device",
            "30",
        ]
        assert module.main() == 0
    finally:
        sys.argv = previous_arguments


def output_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): sha256(path)
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def test_clip_builder_rejects_a_dropped_temporal_frame() -> None:
    module = load_prepare_module()
    recording = type("Recording", (), {})()
    recording.stamps = (
        D4xxStamp(0, 10, 100, "performance"),
        D4xxStamp(1, 12, 133, "performance"),
        D4xxStamp(2, 13, 166, "performance"),
    )
    builder = module.ClipBuilder(
        recording, background=None, isolation=None, frames=3, points=4
    )
    assert builder.build(2) is None


def test_clip_builder_rejects_an_overlong_temporal_gap() -> None:
    module = load_prepare_module()
    recording = type("Recording", (), {})()
    recording.stamps = (
        D4xxStamp(0, 10, 1_000_000_000, "performance"),
        D4xxStamp(1, 11, 1_033_333_333, "performance"),
        D4xxStamp(2, 12, 1_109_000_000, "performance"),
    )
    builder = module.ClipBuilder(
        recording, background=None, isolation=None, frames=3, points=4
    )
    assert builder.build(2) is None


def test_checked_capture_to_shards_is_deterministic(tmp_path: Path) -> None:
    capture = tmp_path / "capture"
    calibration = create_synthetic_capture(capture)
    module = load_prepare_module()
    first = tmp_path / "dataset-first"
    second = tmp_path / "dataset-second"
    run_prepare(module, capture, calibration, first)
    run_prepare(module, capture, calibration, second)
    assert output_hashes(first) == output_hashes(second)

    manifest = json.loads(
        (first / "dataset-manifest.json").read_text(encoding="utf-8")
    )
    assert [device["samples"] for device in manifest["devices"]] == [38, 38]
    for device in manifest["devices"]:
        assert sum(shard["samples"] for shard in device["shards"]) == 38
        assert len(device["shards"]) == 6
    shards, identities = load_dataset_manifests(
        [first / "dataset-manifest.json"]
    )
    assert len(shards) == 12
    assert len(identities) == 1
    assert identities[0].source_capture_sha256 == manifest["source_capture"][
        "sha256"
    ]

    manifest["source_capture"]["files"][0]["bytes"] += 1
    (first / "dataset-manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )
    try:
        load_dataset_manifests([first / "dataset-manifest.json"])
    except ValueError as error:
        assert "aggregate hash mismatch" in str(error)
    else:
        raise AssertionError("altered capture inventory was accepted")
