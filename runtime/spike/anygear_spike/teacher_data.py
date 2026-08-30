"""Validated paired D4xx/Kinect research recordings and geometry helpers."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Iterable

import numpy as np

from .protocol import DepthSnapshot, JOINT_COUNT


TEACHER_SCHEMA_V1 = "dance-around-anygear.d4xx-kinect-teacher.v1"
TEACHER_SCHEMA = "dance-around-anygear.d4xx-kinect-teacher.v2"
SUPPORTED_TEACHER_SCHEMAS = (TEACHER_SCHEMA_V1, TEACHER_SCHEMA)
DATASET_SCHEMA = "dance-around-anygear.spike-teacher-dataset.v2"
MP_LANDMARK_COUNT = 33

# MediaPipe compatibility indices emitted by the Kinect adapter.
MP_NOSE = 0
MP_LEFT_EAR = 7
MP_RIGHT_EAR = 8
MP_LEFT_SHOULDER = 11
MP_RIGHT_SHOULDER = 12
MP_LEFT_ELBOW = 13
MP_RIGHT_ELBOW = 14
MP_LEFT_PINKY = 17
MP_RIGHT_PINKY = 18
MP_LEFT_HIP = 23
MP_RIGHT_HIP = 24
MP_LEFT_KNEE = 25
MP_RIGHT_KNEE = 26
MP_LEFT_FOOT_INDEX = 31
MP_RIGHT_FOOT_INDEX = 32

JOINT_NAMES = (
    "head",
    "neck",
    "right_shoulder",
    "left_shoulder",
    "right_elbow",
    "left_elbow",
    "right_hand",
    "left_hand",
    "torso",
    "right_hip",
    "left_hip",
    "right_knee",
    "left_knee",
    "right_foot",
    "left_foot",
)

# Endpoint predictions carry more model error and do not improve a six-degree
# camera fit. These stable body points define the calibration correspondence.
CALIBRATION_JOINTS = np.asarray((1, 2, 3, 8, 9, 10, 11, 12), dtype=np.int64)


@dataclass(frozen=True)
class Intrinsics:
    width: int
    height: int
    principal_x: float
    principal_y: float
    focal_x: float
    focal_y: float
    distortion_model: int
    distortion: tuple[float, ...]


@dataclass(frozen=True)
class D4xxStamp:
    frame: int
    sequence: int
    host_time_ns: int
    phase: str


@dataclass
class D4xxRecording:
    index: int
    serial: str
    depth_scale_m: float
    intrinsics: Intrinsics
    frame_bytes: int
    stamps: tuple[D4xxStamp, ...]
    frames: np.memmap

    def snapshot(self, frame_index: int) -> DepthSnapshot:
        stamp = self.stamps[frame_index]
        return DepthSnapshot(
            camera_index=self.index,
            frame_sequence=stamp.sequence,
            host_time_ns=stamp.host_time_ns,
            depth_scale_m=self.depth_scale_m,
            width=self.intrinsics.width,
            height=self.intrinsics.height,
            principal_x=self.intrinsics.principal_x,
            principal_y=self.intrinsics.principal_y,
            focal_x=self.intrinsics.focal_x,
            focal_y=self.intrinsics.focal_y,
            distortion_model=self.intrinsics.distortion_model,
            distortion=self.intrinsics.distortion,
            device_index=self.index,
            serial=self.serial,
            depth=np.asarray(self.frames[frame_index]),
        )


@dataclass(frozen=True)
class TeacherSample:
    frame: int
    generation: int
    host_time_ns: int
    sensor_time_ms: int | None
    phase: str
    valid: bool
    confidence: float
    user_index: int
    world_m: np.ndarray
    visibility: np.ndarray
    prefilter_valid: bool
    prefilter_confidence: float
    prefilter_world_m: np.ndarray
    prefilter_visibility: np.ndarray


@dataclass(frozen=True)
class KinectDepthStamp:
    frame: int
    sequence: int
    generation: int
    host_time_ns: int
    sensor_time_ms: int
    phase: str


@dataclass
class KinectDepthRecording:
    depth_scale_m: float
    intrinsics: Intrinsics
    frame_bytes: int
    stamps: tuple[KinectDepthStamp, ...]
    frames: np.memmap
    player_index: np.memmap


@dataclass(frozen=True)
class PairDevice:
    device: int
    frame: int
    sequence: int
    host_time_ns: int
    delta_ns: int


@dataclass(frozen=True)
class PairKinectDepth:
    frame: int
    sequence: int
    host_time_ns: int
    delta_ns: int
    sensor_time_ms: int
    sensor_delta_ms: int


@dataclass(frozen=True)
class SynchronizedPair:
    pair: int
    teacher_frame: int
    teacher_host_time_ns: int
    teacher_valid: bool
    phase: str
    devices: tuple[PairDevice, ...]
    kinect_depth: PairKinectDepth | None = None


@dataclass
class TeacherCapture:
    root: Path
    manifest: dict
    devices: tuple[D4xxRecording, ...]
    teacher: tuple[TeacherSample, ...]
    pairs: tuple[SynchronizedPair, ...]
    kinect_depth: KinectDepthRecording | None = None


@dataclass(frozen=True)
class RigidFit:
    rotation: np.ndarray
    translation: np.ndarray
    correspondence_count: int
    inlier_count: int
    median_error_m: float
    p95_error_m: float
    maximum_error_m: float

    def apply(self, points: np.ndarray) -> np.ndarray:
        value = np.asarray(points, dtype=np.float32)
        return value @ self.rotation.T + self.translation

    def matrix(self) -> np.ndarray:
        result = np.eye(4, dtype=np.float32)
        result[:3, :3] = self.rotation
        result[:3, 3] = self.translation
        return result


def _contained_file(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise ValueError(f"recording file escapes its root: {relative}") from error
    if not path.is_file():
        raise ValueError(f"recording file is absent: {path}")
    return path


def _json_lines(path: Path) -> list[dict]:
    values = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                raise ValueError(f"blank JSONL record in {path}:{line_number}")
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"non-object JSONL record in {path}:{line_number}")
            values.append(value)
    return values


def _require_sequential(values: Iterable[int], label: str) -> None:
    for expected, actual in enumerate(values):
        if actual != expected:
            raise ValueError(
                f"{label} is not sequential at {expected}: got {actual}"
            )


def _require_strictly_increasing(values: Iterable[int], label: str) -> None:
    previous = None
    for value in values:
        if previous is not None and value <= previous:
            raise ValueError(f"{label} is not strictly increasing")
        previous = value


def _load_device(root: Path, value: dict) -> D4xxRecording:
    intrinsics_value = value["intrinsics"]
    intrinsics = Intrinsics(
        width=int(intrinsics_value["width"]),
        height=int(intrinsics_value["height"]),
        principal_x=float(intrinsics_value["principal_x"]),
        principal_y=float(intrinsics_value["principal_y"]),
        focal_x=float(intrinsics_value["focal_x"]),
        focal_y=float(intrinsics_value["focal_y"]),
        distortion_model=int(intrinsics_value["distortion_model"]),
        distortion=tuple(float(item) for item in intrinsics_value["distortion"]),
    )
    if intrinsics.width <= 0 or intrinsics.height <= 0:
        raise ValueError("D4xx intrinsics have invalid dimensions")
    intrinsics_scalars = np.asarray(
        (
            intrinsics.principal_x,
            intrinsics.principal_y,
            intrinsics.focal_x,
            intrinsics.focal_y,
            *intrinsics.distortion,
        ),
        dtype=np.float64,
    )
    if not np.all(np.isfinite(intrinsics_scalars)) or (
        intrinsics.focal_x <= 0.0 or intrinsics.focal_y <= 0.0
    ):
        raise ValueError("D4xx intrinsics contain invalid values")
    frame_bytes = int(value["frame_bytes"])
    expected_frame_bytes = intrinsics.width * intrinsics.height * 2
    if frame_bytes != expected_frame_bytes:
        raise ValueError("D4xx frame_bytes does not match its intrinsics")
    timestamp_path = _contained_file(root, str(value["timestamps_file"]))
    timestamp_values = _json_lines(timestamp_path)
    frame_count = int(value["frame_count"])
    if frame_count <= 0:
        raise ValueError("D4xx recording contains no depth frames")
    if len(timestamp_values) != frame_count:
        raise ValueError("D4xx timestamp count does not match frame_count")
    stamps = tuple(
        D4xxStamp(
            frame=int(item["frame"]),
            sequence=int(item["sequence"]),
            host_time_ns=int(item["host_time_ns"]),
            phase=str(item["phase"]),
        )
        for item in timestamp_values
    )
    _require_sequential((item.frame for item in stamps), "D4xx frame index")
    _require_strictly_increasing(
        (item.sequence for item in stamps), "D4xx sequence"
    )
    _require_strictly_increasing(
        (item.host_time_ns for item in stamps), "D4xx host timestamp"
    )
    if any(item.phase not in ("empty-stage", "performance") for item in stamps):
        raise ValueError("D4xx frame has an unknown capture phase")
    raw_path = _contained_file(root, str(value["depth_file"]))
    if raw_path.stat().st_size != frame_count * frame_bytes:
        raise ValueError(f"D4xx depth stream has the wrong size: {raw_path}")
    frames = np.memmap(
        raw_path,
        mode="r",
        dtype="<u2",
        shape=(frame_count, intrinsics.height, intrinsics.width),
    )
    depth_scale_m = float(value["depth_scale_m"])
    if not np.isfinite(depth_scale_m) or depth_scale_m <= 0.0:
        raise ValueError("D4xx recording has an invalid depth scale")
    return D4xxRecording(
        index=int(value["index"]),
        serial=str(value["serial"]),
        depth_scale_m=depth_scale_m,
        intrinsics=intrinsics,
        frame_bytes=frame_bytes,
        stamps=stamps,
        frames=frames,
    )


def _load_kinect_depth(root: Path, value: dict) -> KinectDepthRecording:
    width = int(value["width"])
    height = int(value["height"])
    intrinsics_value = value["intrinsics"]
    intrinsics = Intrinsics(
        width=width,
        height=height,
        principal_x=float(intrinsics_value["principal_x"]),
        principal_y=float(intrinsics_value["principal_y"]),
        focal_x=float(intrinsics_value["focal_x"]),
        focal_y=float(intrinsics_value["focal_y"]),
        distortion_model=-1,
        distortion=(),
    )
    if width != 320 or height != 240:
        raise ValueError("Kinect v1 depth recording must be 320x240")
    camera_scalars = np.asarray(
        (
            intrinsics.principal_x,
            intrinsics.principal_y,
            intrinsics.focal_x,
            intrinsics.focal_y,
        ),
        dtype=np.float64,
    )
    if not np.all(np.isfinite(camera_scalars)) or (
        intrinsics.focal_x <= 0.0 or intrinsics.focal_y <= 0.0
    ):
        raise ValueError("Kinect depth intrinsics contain invalid values")
    depth_scale_m = float(value["depth_scale_m"])
    if not np.isfinite(depth_scale_m) or depth_scale_m <= 0.0:
        raise ValueError("Kinect depth scale is invalid")
    frame_bytes = int(value["frame_bytes"])
    if frame_bytes != width * height * 2:
        raise ValueError("Kinect depth frame_bytes does not match its dimensions")
    frame_count = int(value["frame_count"])
    if frame_count <= 0:
        raise ValueError("Kinect depth recording contains no frames")
    timestamp_values = _json_lines(
        _contained_file(root, str(value["timestamps_file"]))
    )
    if len(timestamp_values) != frame_count:
        raise ValueError("Kinect depth timestamp count differs from frame_count")
    if any("sensor_time_ms" not in item for item in timestamp_values):
        raise ValueError("Kinect depth frame lacks its sensor timestamp")
    stamps = tuple(
        KinectDepthStamp(
            frame=int(item["frame"]),
            sequence=int(item["sequence"]),
            generation=int(item["generation"]),
            host_time_ns=int(item["host_time_ns"]),
            sensor_time_ms=int(item["sensor_time_ms"]),
            phase=str(item["phase"]),
        )
        for item in timestamp_values
    )
    _require_sequential((item.frame for item in stamps), "Kinect depth frame index")
    _require_strictly_increasing(
        (item.sequence for item in stamps), "Kinect depth sequence"
    )
    _require_strictly_increasing(
        (item.generation for item in stamps), "Kinect depth generation"
    )
    _require_strictly_increasing(
        (item.host_time_ns for item in stamps), "Kinect depth host timestamp"
    )
    _require_strictly_increasing(
        (item.sensor_time_ms for item in stamps),
        "Kinect depth sensor timestamp",
    )
    if any(item.sensor_time_ms < 0 for item in stamps):
        raise ValueError("Kinect depth sensor timestamp is negative")
    if any(item.phase not in ("empty-stage", "performance") for item in stamps):
        raise ValueError("Kinect depth frame has an unknown capture phase")
    raw_path = _contained_file(root, str(value["depth_file"]))
    if raw_path.stat().st_size != frame_count * frame_bytes:
        raise ValueError(f"Kinect depth stream has the wrong size: {raw_path}")
    frames = np.memmap(
        raw_path,
        mode="r",
        dtype="<u2",
        shape=(frame_count, height, width),
    )
    player_index_path = _contained_file(root, str(value["player_index_file"]))
    if player_index_path.stat().st_size != frame_count * width * height:
        raise ValueError(
            f"Kinect player-index stream has the wrong size: {player_index_path}"
        )
    player_index = np.memmap(
        player_index_path,
        mode="r",
        dtype="u1",
        shape=(frame_count, height, width),
    )
    return KinectDepthRecording(
        depth_scale_m=depth_scale_m,
        intrinsics=intrinsics,
        frame_bytes=frame_bytes,
        stamps=stamps,
        frames=frames,
        player_index=player_index,
    )


def _load_teacher(
    root: Path, manifest: dict, require_prefilter: bool
) -> tuple[TeacherSample, ...]:
    values = _json_lines(_contained_file(root, str(manifest["file"])))
    frame_count = int(manifest["frame_count"])
    if frame_count <= 0:
        raise ValueError("Kinect teacher recording contains no frames")
    if len(values) != frame_count:
        raise ValueError("Kinect teacher count does not match its manifest")
    result = []
    for item in values:
        world = np.asarray(item["world_m"], dtype=np.float32)
        visibility = np.asarray(item["visibility"], dtype=np.float32)
        if require_prefilter and "prefilter_world_m" not in item:
            raise ValueError("v2 Kinect teacher frame lacks prefilter landmarks")
        if require_prefilter and "sensor_time_ms" not in item:
            raise ValueError("v2 Kinect teacher frame lacks its sensor timestamp")
        prefilter_world = np.asarray(
            item.get("prefilter_world_m", item["world_m"]), dtype=np.float32
        )
        prefilter_visibility = np.asarray(
            item.get("prefilter_visibility", item["visibility"]),
            dtype=np.float32,
        )
        if world.shape != (MP_LANDMARK_COUNT, 3):
            raise ValueError("Kinect teacher world_m must have shape (33, 3)")
        if visibility.shape != (MP_LANDMARK_COUNT,):
            raise ValueError("Kinect teacher visibility must have shape (33,)")
        if prefilter_world.shape != (MP_LANDMARK_COUNT, 3):
            raise ValueError(
                "Kinect teacher prefilter_world_m must have shape (33, 3)"
            )
        if prefilter_visibility.shape != (MP_LANDMARK_COUNT,):
            raise ValueError(
                "Kinect teacher prefilter_visibility must have shape (33,)"
            )
        if not np.all(np.isfinite(world)) or not np.all(np.isfinite(visibility)):
            raise ValueError("Kinect teacher contains non-finite values")
        if not np.all(np.isfinite(prefilter_world)) or not np.all(
            np.isfinite(prefilter_visibility)
        ):
            raise ValueError("Kinect prefilter teacher contains non-finite values")
        if np.any((visibility < 0.0) | (visibility > 1.0)):
            raise ValueError("Kinect teacher visibility is outside 0..1")
        if np.any((prefilter_visibility < 0.0) | (prefilter_visibility > 1.0)):
            raise ValueError("Kinect prefilter visibility is outside 0..1")
        phase = str(item["phase"])
        confidence = float(item["confidence"])
        prefilter_confidence = float(
            item.get("prefilter_confidence", confidence)
        )
        user_index = int(item.get("user_index", 0))
        if phase not in ("empty-stage", "performance"):
            raise ValueError("Kinect teacher frame has an unknown capture phase")
        if not np.isfinite(confidence) or not 0.0 <= confidence <= 1.0:
            raise ValueError("Kinect teacher confidence is outside 0..1")
        if not np.isfinite(prefilter_confidence) or not (
            0.0 <= prefilter_confidence <= 1.0
        ):
            raise ValueError("Kinect prefilter confidence is outside 0..1")
        if not 0 <= user_index <= 6:
            raise ValueError("Kinect teacher user_index is outside 0..6")
        result.append(
            TeacherSample(
                frame=int(item["frame"]),
                generation=int(item["generation"]),
                host_time_ns=int(item["host_time_ns"]),
                sensor_time_ms=(
                    int(item["sensor_time_ms"]) if require_prefilter else None
                ),
                phase=phase,
                valid=bool(item["valid"]),
                confidence=confidence,
                user_index=user_index,
                world_m=world,
                visibility=visibility,
                prefilter_valid=bool(
                    item.get("prefilter_valid", item["valid"])
                ),
                prefilter_confidence=prefilter_confidence,
                prefilter_world_m=prefilter_world,
                prefilter_visibility=prefilter_visibility,
            )
        )
    samples = tuple(result)
    _require_sequential((item.frame for item in samples), "teacher frame index")
    _require_strictly_increasing(
        (item.generation for item in samples), "teacher generation"
    )
    _require_strictly_increasing(
        (item.host_time_ns for item in samples), "teacher host timestamp"
    )
    if require_prefilter:
        _require_strictly_increasing(
            (int(item.sensor_time_ms) for item in samples),
            "teacher sensor timestamp",
        )
        if any(int(item.sensor_time_ms) < 0 for item in samples):
            raise ValueError("teacher sensor timestamp is negative")
    valid_count = sum(item.valid for item in samples)
    if valid_count != int(manifest["valid_frame_count"]):
        raise ValueError("Kinect valid-frame count does not match its manifest")
    return samples


def _load_pairs(
    root: Path,
    manifest: dict,
    devices: tuple[D4xxRecording, ...],
    teacher: tuple[TeacherSample, ...],
    kinect_depth: KinectDepthRecording | None,
) -> tuple[SynchronizedPair, ...]:
    values = _json_lines(_contained_file(root, str(manifest["file"])))
    if len(values) != int(manifest["pair_count"]):
        raise ValueError("synchronized pair count does not match its manifest")
    if not values:
        raise ValueError("paired recording contains no synchronized frames")
    maximum_delta_ms = float(manifest["max_delta_ms"])
    if not np.isfinite(maximum_delta_ms) or maximum_delta_ms <= 0.0:
        raise ValueError("synchronization gate must be positive and finite")
    maximum_delta_ns = int(maximum_delta_ms * 1_000_000)
    sensor_maximum_delta_ms = None
    if kinect_depth is not None:
        if manifest.get("kinect_sensor_time_unit") != (
            "milliseconds since Kinect initialization"
        ):
            raise ValueError("Kinect sensor timestamp unit is unsupported")
        sensor_maximum_delta_ms = float(
            manifest["kinect_sensor_max_delta_ms"]
        )
        if not np.isfinite(sensor_maximum_delta_ms) or (
            sensor_maximum_delta_ms <= 0.0
        ):
            raise ValueError("Kinect sensor synchronization gate is invalid")
    result = []
    last_device_frame = [-1] * len(devices)
    last_kinect_depth_frame = -1
    last_teacher_frame = -1
    for item in values:
        teacher_frame = int(item["teacher_frame"])
        if teacher_frame < 0 or teacher_frame >= len(teacher):
            raise ValueError("pair refers to an unknown teacher frame")
        if teacher_frame <= last_teacher_frame:
            raise ValueError("pair reuses or reorders a teacher frame")
        last_teacher_frame = teacher_frame
        teacher_sample = teacher[teacher_frame]
        pair_devices = tuple(
            PairDevice(
                device=int(device["device"]),
                frame=int(device["frame"]),
                sequence=int(device["sequence"]),
                host_time_ns=int(device["host_time_ns"]),
                delta_ns=int(device["delta_ns"]),
            )
            for device in item["devices"]
        )
        if tuple(device.device for device in pair_devices) != tuple(
            range(len(devices))
        ):
            raise ValueError("pair does not contain each D4xx device in order")
        for paired in pair_devices:
            recording = devices[paired.device]
            if paired.frame <= last_device_frame[paired.device]:
                raise ValueError("pair reuses or reorders a D4xx frame")
            if paired.frame >= len(recording.stamps):
                raise ValueError("pair refers to an unknown D4xx frame")
            stamp = recording.stamps[paired.frame]
            if paired.sequence != stamp.sequence or (
                paired.host_time_ns != stamp.host_time_ns
            ):
                raise ValueError("pair metadata differs from D4xx timestamp data")
            actual_delta = paired.host_time_ns - teacher_sample.host_time_ns
            if paired.delta_ns != actual_delta:
                raise ValueError("pair has an incorrect timestamp delta")
            if abs(actual_delta) > maximum_delta_ns:
                raise ValueError("pair exceeds the declared synchronization gate")
            last_device_frame[paired.device] = paired.frame
        paired_kinect_depth = None
        if kinect_depth is not None:
            depth_value = item.get("kinect_depth")
            if not isinstance(depth_value, dict):
                raise ValueError("v2 pair is missing its Kinect depth frame")
            paired_kinect_depth = PairKinectDepth(
                frame=int(depth_value["frame"]),
                sequence=int(depth_value["sequence"]),
                host_time_ns=int(depth_value["host_time_ns"]),
                delta_ns=int(depth_value["delta_ns"]),
                sensor_time_ms=int(depth_value["sensor_time_ms"]),
                sensor_delta_ms=int(depth_value["sensor_delta_ms"]),
            )
            if paired_kinect_depth.frame <= last_kinect_depth_frame:
                raise ValueError("pair reuses or reorders a Kinect depth frame")
            if paired_kinect_depth.frame >= len(kinect_depth.stamps):
                raise ValueError("pair refers to an unknown Kinect depth frame")
            depth_stamp = kinect_depth.stamps[paired_kinect_depth.frame]
            if paired_kinect_depth.sequence != depth_stamp.sequence or (
                paired_kinect_depth.host_time_ns != depth_stamp.host_time_ns
            ) or paired_kinect_depth.sensor_time_ms != (
                depth_stamp.sensor_time_ms
            ):
                raise ValueError(
                    "pair metadata differs from Kinect depth timestamp data"
                )
            actual_depth_delta = (
                paired_kinect_depth.host_time_ns - teacher_sample.host_time_ns
            )
            if paired_kinect_depth.delta_ns != actual_depth_delta:
                raise ValueError("pair has an incorrect Kinect depth timestamp delta")
            if abs(actual_depth_delta) > maximum_delta_ns:
                raise ValueError(
                    "Kinect depth pair exceeds the declared synchronization gate"
                )
            if teacher_sample.sensor_time_ms is None:
                raise ValueError("v2 teacher frame lacks its sensor timestamp")
            actual_sensor_delta = (
                paired_kinect_depth.sensor_time_ms
                - teacher_sample.sensor_time_ms
            )
            if paired_kinect_depth.sensor_delta_ms != actual_sensor_delta:
                raise ValueError("pair has an incorrect Kinect sensor-time delta")
            assert sensor_maximum_delta_ms is not None
            if abs(actual_sensor_delta) > sensor_maximum_delta_ms:
                raise ValueError(
                    "Kinect depth pair exceeds the sensor synchronization gate"
                )
            last_kinect_depth_frame = paired_kinect_depth.frame
        elif "kinect_depth" in item:
            raise ValueError("legacy capture unexpectedly contains Kinect depth pairs")
        pair = SynchronizedPair(
            pair=int(item["pair"]),
            teacher_frame=teacher_frame,
            teacher_host_time_ns=int(item["teacher_host_time_ns"]),
            teacher_valid=bool(item["teacher_valid"]),
            phase=str(item["phase"]),
            devices=pair_devices,
            kinect_depth=paired_kinect_depth,
        )
        if pair.teacher_host_time_ns != teacher_sample.host_time_ns or (
            pair.teacher_valid != teacher_sample.valid
        ) or pair.phase != teacher_sample.phase:
            raise ValueError("pair metadata differs from its teacher frame")
        result.append(pair)
    pairs = tuple(result)
    _require_sequential((item.pair for item in pairs), "pair index")
    valid_pair_count = sum(item.teacher_valid for item in pairs)
    if valid_pair_count != int(manifest["valid_teacher_pair_count"]):
        raise ValueError("valid pair count does not match its manifest")
    return pairs


def load_teacher_capture(path: Path) -> TeacherCapture:
    root = path.resolve()
    manifest_path = _contained_file(root, "manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    schema = manifest.get("schema")
    if schema not in SUPPORTED_TEACHER_SCHEMAS:
        raise ValueError("unsupported paired teacher recording schema")
    d4xx_manifest = manifest["d4xx"]
    device_values = d4xx_manifest["devices"]
    if len(device_values) != 2:
        raise ValueError("paired teacher recording must contain two D4xx streams")
    devices = tuple(_load_device(root, item) for item in device_values)
    if tuple(item.index for item in devices) != (0, 1):
        raise ValueError("D4xx device indices must be 0 and 1")
    if len({item.serial for item in devices}) != 2:
        raise ValueError("D4xx serial numbers must be distinct")
    teacher = _load_teacher(
        root, manifest["teacher"], require_prefilter=(schema == TEACHER_SCHEMA)
    )
    kinect_depth = None
    if schema == TEACHER_SCHEMA:
        if "kinect_depth" not in manifest:
            raise ValueError("v2 teacher recording is missing Kinect depth")
        kinect_depth = _load_kinect_depth(root, manifest["kinect_depth"])
    elif "kinect_depth" in manifest:
        raise ValueError("legacy teacher schema unexpectedly contains Kinect depth")
    pairs = _load_pairs(
        root, manifest["synchronization"], devices, teacher, kinect_depth
    )
    return TeacherCapture(root, manifest, devices, teacher, pairs, kinect_depth)


def capture_integrity(capture: TeacherCapture) -> dict:
    """Hash every source file and derive one location-independent identity."""
    manifest = capture.manifest
    relative_files = ["manifest.json"]
    for device in manifest["d4xx"]["devices"]:
        relative_files.extend(
            (str(device["depth_file"]), str(device["timestamps_file"]))
        )
    relative_files.extend(
        (
            str(manifest["teacher"]["file"]),
            str(manifest["synchronization"]["file"]),
        )
    )
    if manifest.get("schema") == TEACHER_SCHEMA:
        depth = manifest["kinect_depth"]
        relative_files.extend(
            (
                str(depth["depth_file"]),
                str(depth["player_index_file"]),
                str(depth["timestamps_file"]),
            )
        )
    if len(relative_files) != len(set(relative_files)):
        raise ValueError("recording manifest reuses a source file")

    files = []
    for relative in sorted(relative_files):
        path = _contained_file(capture.root, relative)
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            while chunk := stream.read(4 * 1024 * 1024):
                digest.update(chunk)
        files.append(
            {
                "path": relative.replace("\\", "/"),
                "bytes": path.stat().st_size,
                "sha256": digest.hexdigest().upper(),
            }
        )
    encoded = json.dumps(
        files, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return {
        "schema": "dance-around-anygear.capture-integrity.v1",
        "sha256": hashlib.sha256(encoded).hexdigest().upper(),
        "files": files,
    }


def capture_quality(capture: TeacherCapture) -> dict:
    def synchronization_summary(values_ms: list[float]) -> dict | None:
        if not values_ms:
            return None
        values = np.asarray(values_ms, dtype=np.float64)
        absolute = np.abs(values)
        return {
            "samples": len(values),
            "median_signed_ms": float(np.median(values)),
            "p95_absolute_ms": float(np.percentile(absolute, 95)),
            "maximum_absolute_ms": float(np.max(absolute)),
        }

    device_metrics = []
    for device in capture.devices:
        timestamps = np.asarray(
            [item.host_time_ns for item in device.stamps], dtype=np.int64
        )
        intervals_ms = np.diff(timestamps).astype(np.float64) / 1_000_000
        duration_s = (
            float(timestamps[-1] - timestamps[0]) / 1_000_000_000
            if len(timestamps) > 1 else 0.0
        )
        rate = (len(timestamps) - 1) / duration_s if duration_s > 0 else 0.0
        device_metrics.append(
            {
                "index": device.index,
                "serial": device.serial,
                "frames": len(device.stamps),
                "rate_hz": rate,
                "median_interval_ms": (
                    float(np.median(intervals_ms)) if len(intervals_ms) else None
                ),
                "p95_interval_ms": (
                    float(np.percentile(intervals_ms, 95))
                    if len(intervals_ms) else None
                ),
                "maximum_interval_ms": (
                    float(np.max(intervals_ms)) if len(intervals_ms) else None
                ),
            }
        )
    teacher_timestamps = np.asarray(
        [item.host_time_ns for item in capture.teacher], dtype=np.int64
    )
    teacher_intervals_ms = (
        np.diff(teacher_timestamps).astype(np.float64) / 1_000_000
    )
    teacher_duration_s = (
        float(teacher_timestamps[-1] - teacher_timestamps[0]) / 1_000_000_000
        if len(teacher_timestamps) > 1 else 0.0
    )
    teacher_metrics = {
        "frames": len(teacher_timestamps),
        "rate_hz": (
            (len(teacher_timestamps) - 1) / teacher_duration_s
            if teacher_duration_s > 0 else 0.0
        ),
        "median_interval_ms": (
            float(np.median(teacher_intervals_ms))
            if len(teacher_intervals_ms) else None
        ),
        "p95_interval_ms": (
            float(np.percentile(teacher_intervals_ms, 95))
            if len(teacher_intervals_ms) else None
        ),
        "maximum_interval_ms": (
            float(np.max(teacher_intervals_ms))
            if len(teacher_intervals_ms) else None
        ),
    }
    performance_samples = tuple(
        sample for sample in capture.teacher
        if sample.phase == "performance"
    )
    performance_valid = sum(sample.valid for sample in performance_samples)
    required_joints = np.asarray(
        (1, 2, 3, 8, 9, 10, 11, 12, 13, 14), dtype=np.int64
    )
    tracked_counts = np.zeros(len(required_joints), dtype=np.int64)
    for sample in performance_samples:
        _, visibility = map_kinect_to_itop(sample)
        tracked_counts += visibility[required_joints] >= 0.99
    joint_tracking = {
        JOINT_NAMES[int(joint)]: (
            int(tracked_counts[index]) / len(performance_samples)
            if performance_samples else 0.0
        )
        for index, joint in enumerate(required_joints)
    }
    performance_valid_paired = sum(
        pair.teacher_valid and pair.phase == "performance"
        for pair in capture.pairs
    )
    empty_stage_tracked = sum(
        sample.valid and sample.phase == "empty-stage"
        for sample in capture.teacher
    )
    performance_valid_with_player_index = sum(
        sample.valid and sample.phase == "performance" and sample.user_index > 0
        for sample in capture.teacher
    )
    performance_valid_with_player_mask = 0
    kinect_depth_metrics = None
    if capture.kinect_depth is not None:
        for pair in capture.pairs:
            if not pair.teacher_valid or pair.phase != "performance" or (
                pair.kinect_depth is None
            ):
                continue
            sample = capture.teacher[pair.teacher_frame]
            if not 1 <= sample.user_index <= 6:
                continue
            frame_index = pair.kinect_depth.frame
            player_index = np.asarray(
                capture.kinect_depth.player_index[frame_index]
            )
            depth = np.asarray(capture.kinect_depth.frames[frame_index])
            # A handful of isolated pixels can be transport noise. The same
            # 64-point floor is required by the geometric refinement stage.
            player_pixels = np.count_nonzero(
                (player_index == sample.user_index) & (depth > 0)
            )
            if player_pixels >= 64:
                performance_valid_with_player_mask += 1
        depth_timestamps = np.asarray(
            [item.host_time_ns for item in capture.kinect_depth.stamps],
            dtype=np.int64,
        )
        depth_intervals_ms = (
            np.diff(depth_timestamps).astype(np.float64) / 1_000_000
        )
        depth_duration_s = (
            float(depth_timestamps[-1] - depth_timestamps[0]) / 1_000_000_000
            if len(depth_timestamps) > 1 else 0.0
        )
        kinect_depth_metrics = {
            "frames": len(depth_timestamps),
            "rate_hz": (
                (len(depth_timestamps) - 1) / depth_duration_s
                if depth_duration_s > 0 else 0.0
            ),
            "median_interval_ms": (
                float(np.median(depth_intervals_ms))
                if len(depth_intervals_ms) else None
            ),
            "p95_interval_ms": (
                float(np.percentile(depth_intervals_ms, 95))
                if len(depth_intervals_ms) else None
            ),
            "maximum_interval_ms": (
                float(np.max(depth_intervals_ms))
                if len(depth_intervals_ms) else None
            ),
        }
    synchronization_metrics = {
        "devices": [
            synchronization_summary(
                [
                    pair.devices[index].delta_ns / 1_000_000
                    for pair in capture.pairs
                ]
            )
            for index in range(len(capture.devices))
        ],
        "kinect_depth_host": synchronization_summary(
            [
                pair.kinect_depth.delta_ns / 1_000_000
                for pair in capture.pairs
                if pair.kinect_depth is not None
            ]
        ),
        "kinect_depth_sensor": synchronization_summary(
            [
                float(pair.kinect_depth.sensor_delta_ms)
                for pair in capture.pairs
                if pair.kinect_depth is not None
            ]
        ),
    }
    return {
        "schema": str(capture.manifest["schema"]),
        "devices": device_metrics,
        "teacher": teacher_metrics,
        "kinect_depth": kinect_depth_metrics,
        "teacher_frames": len(capture.teacher),
        "teacher_valid_frames": sum(item.valid for item in capture.teacher),
        "empty_stage_tracked_frames": empty_stage_tracked,
        "performance_valid_frames": performance_valid,
        "performance_frame_count": len(performance_samples),
        "performance_valid_fraction": (
            performance_valid / len(performance_samples)
            if performance_samples else 0.0
        ),
        "performance_joint_fully_tracked_fraction": joint_tracking,
        "performance_valid_with_player_index_frames": (
            performance_valid_with_player_index
        ),
        "performance_valid_with_player_index_fraction": (
            performance_valid_with_player_index / performance_valid
            if performance_valid else 0.0
        ),
        "performance_valid_with_player_mask_frames": (
            performance_valid_with_player_mask
        ),
        "performance_valid_with_player_mask_fraction": (
            performance_valid_with_player_mask / performance_valid
            if performance_valid else 0.0
        ),
        "performance_valid_paired_frames": performance_valid_paired,
        "performance_valid_pair_fraction": (
            performance_valid_paired / performance_valid
            if performance_valid else 0.0
        ),
        "pair_count": len(capture.pairs),
        "synchronization": synchronization_metrics,
    }


def kinect_player_points(
    recording: KinectDepthRecording,
    frame_index: int,
    user_index: int,
    pixel_stride: int = 1,
) -> np.ndarray:
    """Deproject one NUI player mask into Kinect skeleton coordinates."""
    if frame_index < 0 or frame_index >= len(recording.stamps):
        raise IndexError("Kinect depth frame index is outside the recording")
    if not 1 <= user_index <= 6:
        return np.empty((0, 3), dtype=np.float32)
    stride = max(1, int(pixel_stride))
    depth = np.asarray(recording.frames[frame_index])
    player_index = np.asarray(recording.player_index[frame_index])
    sampled_depth = depth[::stride, ::stride]
    sampled_player = player_index[::stride, ::stride]
    valid = (sampled_player == user_index) & (sampled_depth > 0)
    sampled_rows, sampled_columns = np.nonzero(valid)
    if not len(sampled_rows):
        return np.empty((0, 3), dtype=np.float32)
    rows = sampled_rows * stride
    columns = sampled_columns * stride
    z = depth[rows, columns].astype(np.float32) * recording.depth_scale_m
    intrinsics = recording.intrinsics
    x = (columns.astype(np.float32) - intrinsics.principal_x) * (
        z / intrinsics.focal_x
    )
    y = -(rows.astype(np.float32) - intrinsics.principal_y) * (
        z / intrinsics.focal_y
    )
    points = np.column_stack((x, y, z)).astype(np.float32, copy=False)
    finite = np.all(np.isfinite(points), axis=1)
    return points[finite]


def map_kinect_to_itop(
    sample: TeacherSample, *, prefilter: bool = False
) -> tuple[np.ndarray, np.ndarray]:
    """Map Kinect compatibility landmarks into the 15-joint SPiKE order.

    Kinect SDK 1.8 projects positive skeleton X to increasing depth-image X.
    D4xx deprojection uses the same image-right convention. Keep the raw axis
    so a proper rigid transform can represent only the physical sensor mount,
    without introducing a reflection into teacher labels.
    """
    world = (
        sample.prefilter_world_m if prefilter else sample.world_m
    ).astype(np.float32, copy=True)
    visibility = (
        sample.prefilter_visibility if prefilter else sample.visibility
    )
    joints = np.zeros((JOINT_COUNT, 3), dtype=np.float32)
    confidence = np.zeros(JOINT_COUNT, dtype=np.float32)

    def direct(output: int, source: int) -> None:
        joints[output] = world[source]
        confidence[output] = visibility[source]

    def mean(output: int, sources: tuple[int, ...]) -> None:
        joints[output] = np.mean(world[list(sources)], axis=0)
        confidence[output] = float(np.min(visibility[list(sources)]))

    mean(0, (MP_LEFT_EAR, MP_RIGHT_EAR))
    mean(1, (MP_LEFT_SHOULDER, MP_RIGHT_SHOULDER))
    direct(2, MP_RIGHT_SHOULDER)
    direct(3, MP_LEFT_SHOULDER)
    direct(4, MP_RIGHT_ELBOW)
    direct(5, MP_LEFT_ELBOW)
    direct(6, MP_RIGHT_PINKY)
    direct(7, MP_LEFT_PINKY)
    mean(
        8,
        (
            MP_LEFT_SHOULDER,
            MP_RIGHT_SHOULDER,
            MP_LEFT_HIP,
            MP_RIGHT_HIP,
        ),
    )
    direct(9, MP_RIGHT_HIP)
    direct(10, MP_LEFT_HIP)
    direct(11, MP_RIGHT_KNEE)
    direct(12, MP_LEFT_KNEE)
    direct(13, MP_RIGHT_FOOT_INDEX)
    direct(14, MP_LEFT_FOOT_INDEX)
    valid = sample.prefilter_valid if prefilter else sample.valid
    if not valid:
        confidence.fill(0.0)
    return joints, confidence


def teacher_prefilter_diagnostics(
    capture: TeacherCapture, maximum_lag_frames: int = 5
) -> dict:
    """Measure custom-filter displacement and temporal lag against raw NUI."""
    if maximum_lag_frames < 0 or maximum_lag_frames > 30:
        raise ValueError("teacher lag search is outside 0..30 frames")
    stable = []
    prefilter = []
    stable_visibility = []
    prefilter_visibility = []
    timestamps = []
    for sample in capture.teacher:
        if sample.phase != "performance":
            continue
        stable_joints, stable_confidence = map_kinect_to_itop(sample)
        prefilter_joints, prefilter_confidence = map_kinect_to_itop(
            sample, prefilter=True
        )
        stable.append(stable_joints)
        prefilter.append(prefilter_joints)
        stable_visibility.append(stable_confidence)
        prefilter_visibility.append(prefilter_confidence)
        timestamps.append(sample.host_time_ns)
    if len(stable) < 3:
        raise ValueError("teacher diagnostics require three performance frames")
    stable_value = np.stack(stable)
    prefilter_value = np.stack(prefilter)
    stable_confidence = np.stack(stable_visibility)
    prefilter_confidence = np.stack(prefilter_visibility)
    time_value = np.asarray(timestamps, dtype=np.int64)

    same_mask = (stable_confidence >= 0.99) & (prefilter_confidence >= 0.99)
    same_distance = np.linalg.norm(stable_value - prefilter_value, axis=2)
    tracked_same_distance = same_distance[same_mask]
    if not len(tracked_same_distance):
        raise ValueError("teacher diagnostics have no jointly tracked landmarks")

    lag_values = []
    for lag in range(-maximum_lag_frames, maximum_lag_frames + 1):
        if abs(lag) >= len(stable_value):
            continue
        if lag >= 0:
            stable_slice = slice(lag, None)
            prefilter_slice = slice(None, len(prefilter_value) - lag)
        else:
            stable_slice = slice(None, len(stable_value) + lag)
            prefilter_slice = slice(-lag, None)
        aligned_stable = stable_value[stable_slice]
        aligned_prefilter = prefilter_value[prefilter_slice]
        aligned_mask = (
            stable_confidence[stable_slice] >= 0.99
        ) & (prefilter_confidence[prefilter_slice] >= 0.99)
        distance = np.linalg.norm(
            aligned_stable - aligned_prefilter, axis=2
        )[aligned_mask]
        if not len(distance):
            continue
        lag_values.append(
            {
                "lag_frames": lag,
                "tracked_joint_count": int(len(distance)),
                "median_error_m": float(np.median(distance)),
                "p95_error_m": float(np.percentile(distance, 95)),
            }
        )
    if not lag_values:
        raise ValueError("teacher diagnostics could not test a temporal lag")
    best_lag = min(
        lag_values,
        key=lambda value: (value["median_error_m"], abs(value["lag_frames"])),
    )
    intervals_ms = np.diff(time_value).astype(np.float64) / 1_000_000
    median_interval_ms = float(np.median(intervals_ms))
    per_joint = {}
    for index, name in enumerate(JOINT_NAMES):
        distance = same_distance[:, index][same_mask[:, index]]
        per_joint[name] = {
            "tracked_samples": int(len(distance)),
            "median_displacement_m": (
                float(np.median(distance)) if len(distance) else None
            ),
            "p95_displacement_m": (
                float(np.percentile(distance, 95)) if len(distance) else None
            ),
        }
    return {
        "schema": "dance-around-anygear.kinect-prefilter-diagnostics.v1",
        "source_capture_schema": str(capture.manifest["schema"]),
        "performance_frames": len(stable_value),
        "median_frame_interval_ms": median_interval_ms,
        "same_frame": {
            "tracked_joint_count": int(len(tracked_same_distance)),
            "median_displacement_m": float(np.median(tracked_same_distance)),
            "p95_displacement_m": float(
                np.percentile(tracked_same_distance, 95)
            ),
        },
        "best_stabilized_lag": {
            **best_lag,
            "lag_ms": best_lag["lag_frames"] * median_interval_ms,
        },
        "lag_search": lag_values,
        "per_joint": per_joint,
    }


def estimate_rigid_transform(
    source: np.ndarray, target: np.ndarray
) -> tuple[np.ndarray, np.ndarray]:
    source_value = np.asarray(source, dtype=np.float64)
    target_value = np.asarray(target, dtype=np.float64)
    if source_value.shape != target_value.shape or (
        source_value.ndim != 2 or source_value.shape[1] != 3
    ):
        raise ValueError("rigid-transform points must both have shape (N, 3)")
    if len(source_value) < 3 or not np.all(np.isfinite(source_value)) or (
        not np.all(np.isfinite(target_value))
    ):
        raise ValueError("rigid-transform fit requires three finite pairs")
    source_center = source_value.mean(axis=0)
    target_center = target_value.mean(axis=0)
    centered_source = source_value - source_center
    centered_target = target_value - target_center
    if np.linalg.matrix_rank(centered_source) < 2:
        raise ValueError("rigid-transform source points are degenerate")
    left, _, right_transposed = np.linalg.svd(
        centered_source.T @ centered_target
    )
    rotation = right_transposed.T @ left.T
    if np.linalg.det(rotation) < 0:
        right_transposed[-1] *= -1
        rotation = right_transposed.T @ left.T
    translation = target_center - source_center @ rotation.T
    return rotation.astype(np.float32), translation.astype(np.float32)


def fit_teacher_to_camera(
    teacher_joints: np.ndarray,
    camera_predictions: np.ndarray,
    visibility: np.ndarray,
    minimum_visibility: float = 0.99,
    maximum_inlier_error_m: float = 0.35,
) -> RigidFit:
    source = np.asarray(teacher_joints, dtype=np.float32)
    target = np.asarray(camera_predictions, dtype=np.float32)
    confidence = np.asarray(visibility, dtype=np.float32)
    if source.ndim != 3 or target.ndim != 3 or confidence.ndim != 2:
        raise ValueError("calibration arrays have incompatible shapes")
    sample_count = source.shape[0]
    expected_shape = (sample_count, JOINT_COUNT, 3)
    if source.shape != expected_shape or target.shape != expected_shape or (
        confidence.shape != (sample_count, JOINT_COUNT)
    ):
        raise ValueError("calibration arrays have incompatible shapes")
    stable = CALIBRATION_JOINTS
    mask = confidence[:, stable] >= minimum_visibility
    finite = np.all(np.isfinite(source[:, stable]), axis=2) & np.all(
        np.isfinite(target[:, stable]), axis=2
    )
    mask &= finite
    source_points = source[:, stable][mask]
    target_points = target[:, stable][mask]
    if len(source_points) < 24:
        raise ValueError("camera fit requires at least 24 tracked core pairs")
    active = np.ones(len(source_points), dtype=np.bool_)
    for _ in range(8):
        rotation, translation = estimate_rigid_transform(
            source_points[active], target_points[active]
        )
        predicted = source_points @ rotation.T + translation
        residual = np.linalg.norm(predicted - target_points, axis=1)
        active_residual = residual[active]
        median = float(np.median(active_residual))
        mad = float(np.median(np.abs(active_residual - median)))
        robust_limit = median + max(0.02, 3.0 * 1.4826 * mad)
        limit = min(maximum_inlier_error_m, robust_limit)
        updated = residual <= limit
        if np.count_nonzero(updated) < 24:
            raise ValueError("camera fit rejected too many core correspondences")
        if np.array_equal(updated, active):
            break
        active = updated
    rotation, translation = estimate_rigid_transform(
        source_points[active], target_points[active]
    )
    residual = np.linalg.norm(
        source_points @ rotation.T + translation - target_points, axis=1
    )
    inlier_residual = residual[active]
    return RigidFit(
        rotation=rotation,
        translation=translation,
        correspondence_count=len(source_points),
        inlier_count=int(np.count_nonzero(active)),
        median_error_m=float(np.median(inlier_residual)),
        p95_error_m=float(np.percentile(inlier_residual, 95)),
        maximum_error_m=float(np.max(inlier_residual)),
    )
