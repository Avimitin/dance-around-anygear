from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from anygear_spike.teacher_data import (
    MP_LANDMARK_COUNT,
    TeacherCapture,
    TeacherSample,
    capture_integrity,
    capture_quality,
    fit_teacher_to_camera,
    kinect_player_points,
    load_teacher_capture,
    map_kinect_to_itop,
    teacher_prefilter_diagnostics,
)


def write_json_lines(path: Path, values: list[dict]) -> None:
    path.write_text(
        "".join(json.dumps(value, separators=(",", ":")) + "\n"
                for value in values),
        encoding="utf-8",
    )


def device_manifest(root: Path, index: int, times: list[int]) -> dict:
    depth_file = f"device-{index}.z16"
    timestamp_file = f"device-{index}-timestamps.jsonl"
    frames = np.arange(len(times) * 4, dtype="<u2").reshape(len(times), 2, 2)
    (root / depth_file).write_bytes(frames.tobytes())
    write_json_lines(
        root / timestamp_file,
        [
            {
                "frame": frame,
                "sequence": frame + 10,
                "host_time_ns": timestamp,
                "phase": "empty-stage" if frame == 0 else "performance",
            }
            for frame, timestamp in enumerate(times)
        ],
    )
    return {
        "index": index,
        "serial": f"serial-{index}",
        "depth_file": depth_file,
        "timestamps_file": timestamp_file,
        "depth_scale_m": 0.001,
        "frame_bytes": 8,
        "frame_count": len(times),
        "intrinsics": {
            "width": 2,
            "height": 2,
            "principal_x": 1.0,
            "principal_y": 1.0,
            "focal_x": 2.0,
            "focal_y": 2.0,
            "distortion_model": 0,
            "distortion": [0.0] * 5,
        },
    }


def create_capture(root: Path) -> None:
    devices = (
        device_manifest(root, 0, [100_000_000, 200_000_000]),
        device_manifest(root, 1, [102_000_000, 202_000_000]),
    )
    world = np.zeros((MP_LANDMARK_COUNT, 3), dtype=np.float32).tolist()
    visibility = np.ones(MP_LANDMARK_COUNT, dtype=np.float32).tolist()
    teachers = [
        {
            "frame": frame,
            "generation": frame + 1,
            "host_time_ns": timestamp,
            "sensor_time_ms": 1000 + frame * 100,
            "phase": "empty-stage" if frame == 0 else "performance",
            "valid": frame == 1,
            "confidence": float(frame == 1),
            "user_index": 1 if frame == 1 else 0,
            "world_m": world,
            "visibility": visibility,
            "prefilter_valid": frame == 1,
            "prefilter_confidence": float(frame == 1),
            "prefilter_world_m": world,
            "prefilter_visibility": visibility,
        }
        for frame, timestamp in enumerate((101_000_000, 201_000_000))
    ]
    write_json_lines(root / "kinect-teacher.jsonl", teachers)
    kinect_depth_times = (101_500_000, 201_500_000)
    kinect_depth = np.stack(
        (
            np.full((240, 320), 2000, dtype="<u2"),
            np.full((240, 320), 2100, dtype="<u2"),
        )
    )
    (root / "kinect-depth.z16").write_bytes(kinect_depth.tobytes())
    kinect_player = np.zeros((2, 240, 320), dtype="u1")
    kinect_player[:, 40:220, 100:220] = 1
    (root / "kinect-player-index.u8").write_bytes(kinect_player.tobytes())
    write_json_lines(
        root / "kinect-depth-timestamps.jsonl",
        [
            {
                "frame": frame,
                "sequence": frame + 20,
                "generation": frame + 1,
                "host_time_ns": timestamp,
                "sensor_time_ms": 1001 + frame * 100,
                "phase": "empty-stage" if frame == 0 else "performance",
            }
            for frame, timestamp in enumerate(kinect_depth_times)
        ],
    )
    pairs = []
    for frame, teacher in enumerate(teachers):
        paired_devices = []
        for index, timestamp in enumerate(
            (100_000_000 + frame * 100_000_000,
             102_000_000 + frame * 100_000_000)
        ):
            paired_devices.append(
                {
                    "device": index,
                    "frame": frame,
                    "sequence": frame + 10,
                    "host_time_ns": timestamp,
                    "delta_ns": timestamp - teacher["host_time_ns"],
                }
            )
        pairs.append(
            {
                "pair": frame,
                "teacher_frame": frame,
                "teacher_host_time_ns": teacher["host_time_ns"],
                "teacher_valid": teacher["valid"],
                "phase": teacher["phase"],
                "kinect_depth": {
                    "frame": frame,
                    "sequence": frame + 20,
                    "host_time_ns": kinect_depth_times[frame],
                    "delta_ns": (
                        kinect_depth_times[frame] - teacher["host_time_ns"]
                    ),
                    "sensor_time_ms": 1001 + frame * 100,
                    "sensor_delta_ms": 1,
                },
                "devices": paired_devices,
            }
        )
    write_json_lines(root / "synchronized-pairs.jsonl", pairs)
    manifest = {
        "schema": "dance-around-anygear.d4xx-kinect-teacher.v2",
        "d4xx": {"devices": list(devices)},
        "teacher": {
            "file": "kinect-teacher.jsonl",
            "frame_count": 2,
            "valid_frame_count": 1,
        },
        "kinect_depth": {
            "depth_file": "kinect-depth.z16",
            "player_index_file": "kinect-player-index.u8",
            "timestamps_file": "kinect-depth-timestamps.jsonl",
            "width": 320,
            "height": 240,
            "frame_bytes": 320 * 240 * 2,
            "frame_count": 2,
            "depth_scale_m": 0.001,
            "intrinsics": {
                "principal_x": 160.0,
                "principal_y": 120.0,
                "focal_x": 285.63,
                "focal_y": 285.63,
            },
        },
        "synchronization": {
            "file": "synchronized-pairs.jsonl",
            "max_delta_ms": 17,
            "kinect_sensor_time_unit": (
                "milliseconds since Kinect initialization"
            ),
            "kinect_sensor_max_delta_ms": 17,
            "pair_count": 2,
            "valid_teacher_pair_count": 1,
        },
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )


def test_capture_loader_checks_and_maps_every_stream(tmp_path: Path) -> None:
    create_capture(tmp_path)
    capture = load_teacher_capture(tmp_path)
    assert len(capture.devices) == 2
    assert capture.devices[0].snapshot(1).frame_sequence == 11
    assert int(capture.devices[1].frames[1, 1, 1]) == 7
    assert len(capture.teacher) == 2
    assert capture.teacher[1].sensor_time_ms == 1100
    assert len(capture.pairs) == 2
    assert capture.kinect_depth is not None
    assert int(capture.kinect_depth.frames[1, 239, 319]) == 2100
    assert int(capture.kinect_depth.player_index[1, 100, 150]) == 1
    player_points = kinect_player_points(capture.kinect_depth, 1, 1)
    assert player_points.shape == (180 * 120, 3)
    np.testing.assert_allclose(player_points[:, 2], 2.1)
    assert player_points[:, 0].min() < 0.0 < player_points[:, 0].max()
    assert capture.pairs[1].kinect_depth is not None
    assert capture.pairs[1].kinect_depth.frame == 1
    assert capture.pairs[1].kinect_depth.sensor_delta_ms == 1
    quality = capture_quality(capture)
    assert quality["empty_stage_tracked_frames"] == 0
    assert quality["performance_valid_pair_fraction"] == 1.0
    assert quality["performance_valid_fraction"] == 1.0
    assert quality["performance_valid_with_player_index_fraction"] == 1.0
    assert quality["performance_valid_with_player_mask_fraction"] == 1.0
    assert quality["performance_joint_fully_tracked_fraction"]["right_foot"] == 1.0
    assert quality["kinect_depth"]["frames"] == 2
    assert quality["synchronization"]["kinect_depth_sensor"][
        "median_signed_ms"
    ] == 1.0


def test_capture_quality_requires_a_real_player_surface(tmp_path: Path) -> None:
    create_capture(tmp_path)
    player_path = tmp_path / "kinect-player-index.u8"
    player_index = np.fromfile(player_path, dtype=np.uint8)
    player_index[:] = 0
    player_index.tofile(player_path)
    quality = capture_quality(load_teacher_capture(tmp_path))
    assert quality["performance_valid_with_player_index_fraction"] == 1.0
    assert quality["performance_valid_with_player_mask_fraction"] == 0.0


def test_capture_identity_includes_raw_depth_content(tmp_path: Path) -> None:
    create_capture(tmp_path)
    capture = load_teacher_capture(tmp_path)
    original = capture_integrity(capture)
    manifest_hash = next(
        item["sha256"] for item in original["files"]
        if item["path"] == "manifest.json"
    )
    depth_path = tmp_path / "device-0.z16"
    with depth_path.open("r+b") as stream:
        first = stream.read(1)
        stream.seek(0)
        stream.write(bytes((first[0] ^ 1,)))
    changed = capture_integrity(capture)
    assert changed["sha256"] != original["sha256"]
    assert next(
        item["sha256"] for item in changed["files"]
        if item["path"] == "manifest.json"
    ) == manifest_hash


def test_loader_rejects_a_reused_camera_frame(tmp_path: Path) -> None:
    create_capture(tmp_path)
    path = tmp_path / "synchronized-pairs.jsonl"
    pairs = [json.loads(line) for line in path.read_text().splitlines()]
    pairs[1]["devices"][0].update(
        frame=0,
        sequence=10,
        host_time_ns=100_000_000,
        delta_ns=-101_000_000,
    )
    write_json_lines(path, pairs)
    try:
        load_teacher_capture(tmp_path)
    except ValueError as error:
        assert "reuses or reorders" in str(error)
    else:
        raise AssertionError("reused D4xx frame was accepted")


def test_loader_rejects_a_reused_teacher_frame(tmp_path: Path) -> None:
    create_capture(tmp_path)
    path = tmp_path / "synchronized-pairs.jsonl"
    pairs = [json.loads(line) for line in path.read_text().splitlines()]
    pairs[1].update(
        teacher_frame=0,
        teacher_host_time_ns=101_000_000,
        teacher_valid=False,
        phase="empty-stage",
    )
    for device in pairs[1]["devices"]:
        device["delta_ns"] = device["host_time_ns"] - 101_000_000
    write_json_lines(path, pairs)
    try:
        load_teacher_capture(tmp_path)
    except ValueError as error:
        assert "reuses or reorders a teacher frame" in str(error)
    else:
        raise AssertionError("reused teacher frame was accepted")


def test_loader_rejects_false_kinect_sensor_synchronization(
    tmp_path: Path,
) -> None:
    create_capture(tmp_path)
    path = tmp_path / "synchronized-pairs.jsonl"
    pairs = [json.loads(line) for line in path.read_text().splitlines()]
    pairs[1]["kinect_depth"]["sensor_delta_ms"] = 0
    write_json_lines(path, pairs)
    try:
        load_teacher_capture(tmp_path)
    except ValueError as error:
        assert "sensor-time delta" in str(error)
    else:
        raise AssertionError("incorrect Kinect sensor delta was accepted")


def test_kinect_mapping_preserves_depth_axis_and_joint_order() -> None:
    world = np.zeros((MP_LANDMARK_COUNT, 3), dtype=np.float32)
    visibility = np.ones(MP_LANDMARK_COUNT, dtype=np.float32)
    world[11] = (0.4, 1.4, 2.0)   # left shoulder
    world[12] = (-0.4, 1.4, 2.0)  # right shoulder
    world[7] = (0.1, 1.8, 2.0)
    world[8] = (-0.1, 1.8, 2.0)
    sample = TeacherSample(
        frame=0,
        generation=1,
        host_time_ns=1,
        sensor_time_ms=1,
        phase="performance",
        valid=True,
        confidence=1.0,
        user_index=1,
        world_m=world,
        visibility=visibility,
        prefilter_valid=True,
        prefilter_confidence=1.0,
        prefilter_world_m=world.copy(),
        prefilter_visibility=visibility.copy(),
    )
    joints, confidence = map_kinect_to_itop(sample)
    np.testing.assert_allclose(joints[0], (0.0, 1.8, 2.0))
    np.testing.assert_allclose(joints[2], (-0.4, 1.4, 2.0))
    np.testing.assert_allclose(joints[3], (0.4, 1.4, 2.0))
    np.testing.assert_array_equal(confidence, np.ones(15, dtype=np.float32))


def test_robust_camera_fit_recovers_proper_transform() -> None:
    generator = np.random.default_rng(7)
    teacher = generator.normal(size=(12, 15, 3)).astype(np.float32)
    angle = np.deg2rad(23.0)
    rotation = np.asarray(
        (
            (np.cos(angle), 0.0, np.sin(angle)),
            (0.0, 1.0, 0.0),
            (-np.sin(angle), 0.0, np.cos(angle)),
        ),
        dtype=np.float32,
    )
    translation = np.asarray((0.25, -0.12, 0.40), dtype=np.float32)
    camera = teacher @ rotation.T + translation
    camera[0, 1] += (2.0, -1.0, 0.5)
    camera[3, 9] += (-1.5, 1.0, 0.8)
    visibility = np.ones((12, 15), dtype=np.float32)
    fit = fit_teacher_to_camera(teacher, camera, visibility)
    assert np.linalg.det(fit.rotation) > 0.999
    np.testing.assert_allclose(fit.rotation, rotation, atol=1e-5)
    np.testing.assert_allclose(fit.translation, translation, atol=1e-5)
    assert fit.inlier_count == fit.correspondence_count - 2
    assert fit.p95_error_m < 1e-5


def test_camera_fit_rejects_malformed_arrays_cleanly() -> None:
    malformed = np.zeros(15, dtype=np.float32)
    try:
        fit_teacher_to_camera(malformed, malformed, malformed)
    except ValueError as error:
        assert "incompatible shapes" in str(error)
    else:
        raise AssertionError("malformed calibration arrays were accepted")


def test_prefilter_diagnostics_find_a_one_frame_stability_lag(
    tmp_path: Path,
) -> None:
    visibility = np.ones(MP_LANDMARK_COUNT, dtype=np.float32)
    samples = []
    for frame in range(8):
        prefilter = np.zeros((MP_LANDMARK_COUNT, 3), dtype=np.float32)
        prefilter[:, 0] = frame * 0.10
        stable = np.zeros_like(prefilter)
        stable[:, 0] = max(0, frame - 1) * 0.10
        samples.append(
            TeacherSample(
                frame=frame,
                generation=frame + 1,
                host_time_ns=frame * 33_333_333,
                sensor_time_ms=frame * 33,
                phase="performance",
                valid=True,
                confidence=1.0,
                user_index=1,
                world_m=stable,
                visibility=visibility,
                prefilter_valid=True,
                prefilter_confidence=1.0,
                prefilter_world_m=prefilter,
                prefilter_visibility=visibility,
            )
        )
    capture = TeacherCapture(
        root=tmp_path,
        manifest={"schema": "synthetic"},
        devices=(),
        teacher=tuple(samples),
        pairs=(),
    )
    diagnostics = teacher_prefilter_diagnostics(capture, maximum_lag_frames=2)
    assert diagnostics["best_stabilized_lag"]["lag_frames"] == 1
    assert diagnostics["best_stabilized_lag"]["median_error_m"] < 1e-6
