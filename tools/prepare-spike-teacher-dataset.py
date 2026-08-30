"""Validate paired captures, fit sensor geometry, and build SPiKE shards."""

from __future__ import annotations

import argparse
from dataclasses import asdict, replace
import hashlib
import json
from pathlib import Path
from typing import Iterable

import numpy as np

from anygear_spike.config import load_config
from anygear_spike.depth import estimate_background, isolate_player, sample_points
from anygear_spike.model_runtime import RealtimeSpikeModel
from anygear_spike.surface_registration import (
    SurfaceFrame,
    SurfaceRegistrationOptions,
    SurfaceRegistrationResult,
    refine_surface_registration,
)
from anygear_spike.teacher_data import (
    CALIBRATION_JOINTS,
    DATASET_SCHEMA,
    JOINT_NAMES,
    RigidFit,
    TeacherCapture,
    capture_integrity,
    capture_quality,
    fit_teacher_to_camera,
    kinect_player_points,
    load_teacher_capture,
    map_kinect_to_itop,
)
from anygear_spike.temporal import recorded_clip_frame_is_contiguous


CALIBRATION_SCHEMA = "dance-around-anygear.d4xx-kinect-calibration.v2"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def write_json(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def prepare_empty_directory(path: Path) -> Path:
    result = path.resolve()
    if result.exists():
        if not result.is_dir() or any(result.iterdir()):
            raise ValueError(f"output directory exists and is not empty: {result}")
    else:
        result.mkdir(parents=True)
    return result


def quality_failures(quality: dict) -> list[str]:
    failures = []
    for device in quality["devices"]:
        if device["rate_hz"] < 29.0:
            failures.append(
                f"D4xx {device['serial']} delivered {device['rate_hz']:.2f} Hz"
            )
        maximum_interval = device["maximum_interval_ms"]
        if maximum_interval is None or maximum_interval > 100.0:
            failures.append(
                f"D4xx {device['serial']} maximum gap is {maximum_interval} ms"
            )
    kinect_depth = quality.get("kinect_depth")
    if kinect_depth is not None:
        if kinect_depth["rate_hz"] < 29.0:
            failures.append(
                "Kinect depth delivered "
                f"{kinect_depth['rate_hz']:.2f} Hz"
            )
        maximum_interval = kinect_depth["maximum_interval_ms"]
        if maximum_interval is None or maximum_interval > 100.0:
            failures.append(
                f"Kinect depth maximum gap is {maximum_interval} ms"
            )
    teacher = quality["teacher"]
    if teacher["rate_hz"] < 29.0:
        failures.append(
            f"Kinect skeleton delivered {teacher['rate_hz']:.2f} Hz"
        )
    teacher_maximum_interval = teacher["maximum_interval_ms"]
    if teacher_maximum_interval is None or teacher_maximum_interval > 100.0:
        failures.append(
            "Kinect skeleton maximum gap is "
            f"{teacher_maximum_interval} ms"
        )
    if quality["empty_stage_tracked_frames"]:
        failures.append(
            "Kinect tracked a body during the declared empty-stage segment"
        )
    if quality["performance_valid_frames"] < 30:
        failures.append("fewer than 30 valid Kinect performance frames")
    if quality["performance_valid_fraction"] < 0.95:
        failures.append(
            "fewer than 95% of Kinect performance frames contain a body"
        )
    weak_joints = [
        name for name, fraction in
        quality["performance_joint_fully_tracked_fraction"].items()
        if fraction < 0.95
    ]
    if weak_joints:
        failures.append(
            "fewer than 95% fully tracked frames for Kinect joints: "
            + ", ".join(weak_joints)
        )
    if quality["performance_valid_pair_fraction"] < 0.95:
        failures.append(
            "fewer than 95% of valid Kinect performance frames are paired"
        )
    if quality.get("kinect_depth") is not None and (
        quality["performance_valid_with_player_mask_fraction"] < 0.95
    ):
        failures.append(
            "fewer than 95% of valid Kinect frames contain their player surface"
        )
    return failures


def select_evenly(values: list, maximum: int) -> list:
    if len(values) <= maximum:
        return values
    indices = np.linspace(0, len(values) - 1, maximum, dtype=np.int64)
    return [values[int(index)] for index in np.unique(indices)]


class ClipBuilder:
    def __init__(self, recording, background, isolation, frames: int, points: int):
        self.recording = recording
        self.background = background
        self.isolation = isolation
        self.frames = frames
        self.points = points
        self.cache: dict[int, np.ndarray] = {}

    def isolated(self, frame_index: int) -> np.ndarray:
        if frame_index not in self.cache:
            snapshot = self.recording.snapshot(frame_index)
            self.cache[frame_index] = isolate_player(
                snapshot, self.isolation, self.background
            )
            if len(self.cache) > 32:
                self.cache.pop(min(self.cache))
        return self.cache[frame_index]

    def build(self, target_frame: int):
        first = target_frame - self.frames + 1
        if first < 0 or target_frame >= len(self.recording.stamps):
            return None
        indices = list(range(first, target_frame + 1))
        if any(
            self.recording.stamps[index].phase != "performance"
            for index in indices
        ):
            return None
        stamps = [self.recording.stamps[index] for index in indices]
        if any(
            not recorded_clip_frame_is_contiguous(
                previous.sequence,
                previous.host_time_ns,
                current.sequence,
                current.host_time_ns,
            )
            for previous, current in zip(stamps, stamps[1:])
        ):
            return None
        point_frames = []
        point_counts = []
        for index in indices:
            points = self.isolated(index)
            point_counts.append(len(points))
            if not len(points):
                return None
            point_frames.append(sample_points(points, self.points))
        clip = np.stack(point_frames).astype(np.float32, copy=False)
        centroid = clip.reshape(-1, 3).mean(axis=0)
        clip = clip - centroid.reshape(1, 1, 3)
        return clip, centroid.astype(np.float32), np.asarray(
            point_counts, dtype=np.int32
        )


def build_backgrounds(capture: TeacherCapture, isolation) -> list[np.ndarray]:
    backgrounds = []
    requested = max(3, isolation.background_calibration_frames)
    for recording in capture.devices:
        indices = [
            stamp.frame
            for stamp in recording.stamps
            if stamp.phase == "empty-stage"
        ]
        if len(indices) < 3:
            raise ValueError(
                f"D4xx {recording.serial} has fewer than three empty-stage frames"
            )
        selected = indices[:requested]
        backgrounds.append(
            estimate_background(
                [np.asarray(recording.frames[index]) for index in selected]
            )
        )
    return backgrounds


def eligible_pairs(capture: TeacherCapture) -> list:
    return [
        pair
        for pair in capture.pairs
        if pair.phase == "performance" and pair.teacher_valid
    ]


def calibrate_device(
    capture: TeacherCapture,
    device_index: int,
    builder: ClipBuilder,
    model: RealtimeSpikeModel,
    maximum_frames: int,
    minimum_visibility: float,
) -> RigidFit:
    candidates = select_evenly(eligible_pairs(capture), maximum_frames)
    teacher_values = []
    prediction_values = []
    visibility_values = []
    dropped_isolation = 0
    dropped_visibility = 0
    for progress, pair in enumerate(candidates, 1):
        teacher = capture.teacher[pair.teacher_frame]
        teacher_joints, visibility = map_kinect_to_itop(teacher)
        if not np.all(visibility[CALIBRATION_JOINTS] >= minimum_visibility):
            dropped_visibility += 1
            continue
        built = builder.build(pair.devices[device_index].frame)
        if built is None:
            dropped_isolation += 1
            continue
        clip, centroid, _ = built
        prediction = model.infer(clip[None, ...])[0] + centroid[None, :]
        teacher_values.append(teacher_joints)
        prediction_values.append(prediction)
        visibility_values.append(visibility)
        if progress % 25 == 0 or progress == len(candidates):
            print(
                f"[CALIBRATE] camera={device_index} examined={progress}/"
                f"{len(candidates)} usable={len(teacher_values)}"
            )
    if not teacher_values:
        raise ValueError(
            f"camera {device_index} has no usable calibration correspondences"
        )
    print(
        f"[CALIBRATE] camera={device_index} dropped-isolation="
        f"{dropped_isolation} dropped-visibility={dropped_visibility}"
    )
    return fit_teacher_to_camera(
        np.stack(teacher_values),
        np.stack(prediction_values),
        np.stack(visibility_values),
        minimum_visibility=minimum_visibility,
    )


def surface_frames_for_device(
    capture: TeacherCapture,
    device_index: int,
    builder: ClipBuilder,
    maximum_frames: int,
) -> list[SurfaceFrame]:
    if capture.kinect_depth is None:
        return []
    candidates = select_evenly(eligible_pairs(capture), maximum_frames)
    result = []
    dropped_mask = 0
    dropped_isolation = 0
    for pair in candidates:
        if pair.kinect_depth is None:
            dropped_mask += 1
            continue
        teacher = capture.teacher[pair.teacher_frame]
        source = kinect_player_points(
            capture.kinect_depth,
            pair.kinect_depth.frame,
            teacher.user_index,
            pixel_stride=1,
        )
        if len(source) < 64:
            dropped_mask += 1
            continue
        target = builder.isolated(pair.devices[device_index].frame)
        if len(target) < 64:
            dropped_isolation += 1
            continue
        result.append(SurfaceFrame(source, target))
    print(
        f"[SURFACE] camera={device_index} usable={len(result)}/"
        f"{len(candidates)} dropped-mask={dropped_mask} "
        f"dropped-isolation={dropped_isolation}"
    )
    return result


def fit_json(
    index: int,
    serial: str,
    fit: RigidFit,
    initial_fit: RigidFit | None = None,
    surface: SurfaceRegistrationResult | None = None,
) -> dict:
    result = {
        "index": index,
        "serial": serial,
        "rotation": fit.rotation.reshape(-1).tolist(),
        "translation": fit.translation.tolist(),
        "matrix_row_major": fit.matrix().reshape(-1).tolist(),
        "correspondence_count": fit.correspondence_count,
        "inlier_count": fit.inlier_count,
        "median_error_m": fit.median_error_m,
        "p95_error_m": fit.p95_error_m,
        "maximum_error_m": fit.maximum_error_m,
    }
    if initial_fit is not None:
        result["initial_core_joint_fit"] = {
            "rotation": initial_fit.rotation.reshape(-1).tolist(),
            "translation": initial_fit.translation.tolist(),
            "correspondence_count": initial_fit.correspondence_count,
            "inlier_count": initial_fit.inlier_count,
            "median_error_m": initial_fit.median_error_m,
            "p95_error_m": initial_fit.p95_error_m,
            "maximum_error_m": initial_fit.maximum_error_m,
        }
    if surface is not None:
        result["surface_refinement"] = surface.as_dict()
    return result


def calibration_json(
    capture: TeacherCapture,
    fits: list[RigidFit],
    source_manifest_sha256: str,
    source_integrity: dict,
    config_sha256: str,
    model_sha256: str,
    minimum_visibility: float,
    initial_fits: list[RigidFit] | None = None,
    surface_results: list[SurfaceRegistrationResult] | None = None,
    surface_options: SurfaceRegistrationOptions | None = None,
    maximum_surface_p95_m: float | None = None,
) -> dict:
    refined = surface_results is not None
    result = {
        "schema": CALIBRATION_SCHEMA,
        "source_manifest_sha256": source_manifest_sha256,
        "source_capture": source_integrity,
        "runtime_config_sha256": config_sha256,
        "baseline_model_sha256": model_sha256,
        "method": (
            "Kinect player-mask surface refinement initialized by a robust "
            "core-joint fit"
            if refined else
            "robust core-joint fit from baseline SPiKE predictions"
        ),
        "kinect_axis_normalization": [1.0, 1.0, 1.0],
        "teacher_observation": "stabilized",
        "minimum_teacher_visibility": minimum_visibility,
        "devices": [
            fit_json(
                index,
                capture.devices[index].serial,
                fit,
                initial_fits[index] if initial_fits is not None else None,
                surface_results[index] if surface_results is not None else None,
            )
            for index, fit in enumerate(fits)
        ],
    }
    if refined:
        if surface_options is None or maximum_surface_p95_m is None:
            raise ValueError("surface calibration metadata is incomplete")
        result["surface_options"] = asdict(surface_options)
        result["maximum_surface_p95_m"] = maximum_surface_p95_m
    return result


def validate_rotation(rotation: np.ndarray) -> None:
    if rotation.shape != (3, 3) or not np.allclose(
        rotation.T @ rotation, np.eye(3), atol=2e-3
    ):
        raise ValueError("calibration rotation is not orthonormal")
    if not np.isclose(np.linalg.det(rotation), 1.0, atol=2e-3):
        raise ValueError("calibration rotation does not preserve handedness")


def load_calibration(
    path: Path,
    capture: TeacherCapture,
    source_manifest_sha256: str,
    source_integrity: dict,
) -> tuple[list[RigidFit], dict]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema") != CALIBRATION_SCHEMA:
        raise ValueError("unsupported D4xx/Kinect calibration schema")
    if value.get("source_manifest_sha256") != source_manifest_sha256:
        raise ValueError("calibration belongs to a different capture manifest")
    if value.get("source_capture") != source_integrity:
        raise ValueError("calibration belongs to different capture content")
    axis = np.asarray(value.get("kinect_axis_normalization"), dtype=np.float32)
    if axis.shape != (3,) or not np.array_equal(
        axis, np.ones(3, dtype=np.float32)
    ):
        raise ValueError("calibration uses an unsupported Kinect axis mapping")
    if not str(value.get("method", "")).strip():
        raise ValueError("calibration method is absent")
    devices = value.get("devices", [])
    if len(devices) != 2:
        raise ValueError("calibration must contain two D4xx devices")
    fits = []
    for index, item in enumerate(devices):
        if int(item["index"]) != index or (
            str(item["serial"]) != capture.devices[index].serial
        ):
            raise ValueError("calibration D4xx identity differs from capture")
        rotation = np.asarray(item["rotation"], dtype=np.float32).reshape(3, 3)
        translation = np.asarray(item["translation"], dtype=np.float32)
        validate_rotation(rotation)
        if translation.shape != (3,) or not np.all(np.isfinite(translation)):
            raise ValueError("calibration translation is invalid")
        correspondence_count = int(item["correspondence_count"])
        inlier_count = int(item["inlier_count"])
        errors = np.asarray(
            (
                float(item["median_error_m"]),
                float(item["p95_error_m"]),
                float(item["maximum_error_m"]),
            ),
            dtype=np.float64,
        )
        if correspondence_count < 3 or not 3 <= inlier_count <= (
            correspondence_count
        ):
            raise ValueError("calibration correspondence counts are invalid")
        if not np.all(np.isfinite(errors)) or np.any(errors < 0.0) or not (
            errors[0] <= errors[1] <= errors[2]
        ):
            raise ValueError("calibration residual metrics are invalid")
        fits.append(
            RigidFit(
                rotation=rotation,
                translation=translation,
                correspondence_count=correspondence_count,
                inlier_count=inlier_count,
                median_error_m=float(errors[0]),
                p95_error_m=float(errors[1]),
                maximum_error_m=float(errors[2]),
            )
        )
    return fits, value


class ShardWriter:
    def __init__(self, root: Path, maximum_samples: int):
        self.root = root
        self.maximum_samples = maximum_samples
        self.buffers: dict[str, list[np.ndarray | int]] = {
            "clips": [],
            "joints_m": [],
            "visibility": [],
            "centroids_m": [],
            "point_counts": [],
            "pair": [],
            "teacher_frame": [],
            "device_frame": [],
            "teacher_time_ns": [],
            "delta_ns": [],
        }
        self.shards = []
        self.sample_count = 0

    def append(
        self,
        clip: np.ndarray,
        joints: np.ndarray,
        visibility: np.ndarray,
        centroid: np.ndarray,
        point_counts: np.ndarray,
        pair,
        device,
    ) -> None:
        self.buffers["clips"].append(clip.astype(np.float16))
        self.buffers["joints_m"].append(joints.astype(np.float32))
        self.buffers["visibility"].append(visibility.astype(np.float32))
        self.buffers["centroids_m"].append(centroid.astype(np.float32))
        self.buffers["point_counts"].append(point_counts.astype(np.int32))
        self.buffers["pair"].append(pair.pair)
        self.buffers["teacher_frame"].append(pair.teacher_frame)
        self.buffers["device_frame"].append(device.frame)
        self.buffers["teacher_time_ns"].append(pair.teacher_host_time_ns)
        self.buffers["delta_ns"].append(device.delta_ns)
        if len(self.buffers["clips"]) >= self.maximum_samples:
            self.flush()

    def flush(self) -> None:
        count = len(self.buffers["clips"])
        if not count:
            return
        shard_index = len(self.shards)
        relative = Path(f"shard-{shard_index:05d}")
        directory = self.root / relative
        directory.mkdir(parents=True)
        files = {}
        for name, values in self.buffers.items():
            path = directory / f"{name}.npy"
            array = np.stack(values) if isinstance(values[0], np.ndarray) else (
                np.asarray(values, dtype=np.int64)
            )
            np.save(path, array, allow_pickle=False)
            files[path.name] = {
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
                "shape": list(array.shape),
                "dtype": str(array.dtype),
            }
            values.clear()
        self.shards.append(
            {"directory": relative.as_posix(), "samples": count, "files": files}
        )
        self.sample_count += count


def generate_dataset(
    capture: TeacherCapture,
    output: Path,
    fits: list[RigidFit],
    backgrounds: list[np.ndarray],
    isolation,
    frames_per_clip: int,
    points_per_frame: int,
    shard_size: int,
    minimum_labeled_joints: int,
    minimum_samples_per_device: int,
) -> list[dict]:
    device_results = []
    pairs = eligible_pairs(capture)
    for device_index, recording in enumerate(capture.devices):
        device_root = output / f"device-{device_index}"
        device_root.mkdir()
        writer = ShardWriter(device_root, shard_size)
        builder = ClipBuilder(
            recording,
            backgrounds[device_index],
            isolation,
            frames_per_clip,
            points_per_frame,
        )
        dropped_isolation = 0
        dropped_labels = 0
        for progress, pair in enumerate(pairs, 1):
            teacher_joints, visibility = map_kinect_to_itop(
                capture.teacher[pair.teacher_frame]
            )
            if np.count_nonzero(visibility >= 0.99) < minimum_labeled_joints:
                dropped_labels += 1
                continue
            device_pair = pair.devices[device_index]
            built = builder.build(device_pair.frame)
            if built is None:
                dropped_isolation += 1
                continue
            clip, centroid, point_counts = built
            camera_joints = fits[device_index].apply(teacher_joints)
            centered_joints = camera_joints - centroid.reshape(1, 3)
            writer.append(
                clip,
                centered_joints,
                visibility,
                centroid,
                point_counts,
                pair,
                device_pair,
            )
            if progress % 250 == 0 or progress == len(pairs):
                print(
                    f"[DATASET] camera={device_index} examined={progress}/"
                    f"{len(pairs)} accepted={writer.sample_count + len(writer.buffers['clips'])}"
                )
        writer.flush()
        if writer.sample_count < minimum_samples_per_device:
            raise ValueError(
                f"camera {device_index} retained only {writer.sample_count} "
                f"samples; at least {minimum_samples_per_device} are required"
            )
        device_results.append(
            {
                "index": device_index,
                "serial": recording.serial,
                "samples": writer.sample_count,
                "dropped_isolation": dropped_isolation,
                "dropped_labels": dropped_labels,
                "shards": writer.shards,
            }
        )
    return device_results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--calibration", type=Path)
    parser.add_argument("--quality-only", action="store_true")
    parser.add_argument("--allow-quality-fail", action="store_true")
    parser.add_argument("--allow-poor-fit", action="store_true")
    parser.add_argument("--maximum-calibration-frames", type=int, default=300)
    parser.add_argument("--maximum-surface-frames", type=int, default=60)
    parser.add_argument("--surface-points-per-cloud", type=int, default=1536)
    parser.add_argument("--maximum-surface-p95-m", type=float, default=0.15)
    parser.add_argument("--disable-surface-refinement", action="store_true")
    parser.add_argument("--minimum-visibility", type=float, default=0.99)
    parser.add_argument("--maximum-fit-p95-m", type=float, default=0.20)
    parser.add_argument("--minimum-labeled-joints", type=int, default=8)
    parser.add_argument("--minimum-samples-per-device", type=int, default=30)
    parser.add_argument("--shard-size", type=int, default=128)
    args = parser.parse_args()

    if args.maximum_calibration_frames < 3 or args.shard_size < 1:
        parser.error("calibration frame count and shard size must be positive")
    if args.maximum_surface_frames < 8:
        parser.error("surface calibration requires at least eight frames")
    if args.surface_points_per_cloud < 64:
        parser.error("surface calibration requires at least 64 points per cloud")
    if not np.isfinite(args.maximum_surface_p95_m) or (
        args.maximum_surface_p95_m <= 0.0
    ):
        parser.error("maximum surface P95 must be positive and finite")
    if not 0.0 <= args.minimum_visibility <= 1.0:
        parser.error("minimum visibility must be inside 0..1")
    if not 1 <= args.minimum_labeled_joints <= len(JOINT_NAMES):
        parser.error("minimum labeled joints is outside 1..15")
    if args.minimum_samples_per_device < 1:
        parser.error("minimum samples per device must be positive")
    if not np.isfinite(args.maximum_fit_p95_m) or args.maximum_fit_p95_m <= 0.0:
        parser.error("maximum fit P95 must be positive and finite")

    capture = load_teacher_capture(args.capture)
    quality = capture_quality(capture)
    print(json.dumps(quality, indent=2))
    failures = quality_failures(quality)
    if failures:
        print("[QUALITY] capture did not meet the research gates:")
        for failure in failures:
            print(f"  - {failure}")
        if not args.allow_quality_fail:
            return 3
    else:
        print("[QUALITY] paired capture meets the ingest gates")
    if args.quality_only:
        return 0
    if args.output is None:
        parser.error("--output is required unless --quality-only is selected")

    output = prepare_empty_directory(args.output)
    source_manifest = capture.root / "manifest.json"
    source_manifest_sha256 = sha256(source_manifest)
    print("[INTEGRITY] hashing every source capture file")
    source_integrity = capture_integrity(capture)
    print(
        f"[INTEGRITY] files={len(source_integrity['files'])} "
        f"capture={source_integrity['sha256']}"
    )
    config_path = args.config.resolve()
    config = load_config(config_path)
    isolation = replace(
        config.isolation,
        # Offline labels already prove that a body is present. Preserve low
        # poses instead of imposing the runtime reacquisition height gate.
        minimum_acquisition_height_m=config.isolation.minimum_person_height_m,
    )
    backgrounds = build_backgrounds(capture, isolation)

    calibration_value = None
    if args.calibration is not None:
        fits, calibration_value = load_calibration(
            args.calibration.resolve(), capture, source_manifest_sha256,
            source_integrity,
        )
        print(f"[CALIBRATE] using checked calibration {args.calibration}")
    else:
        if args.model is None:
            parser.error("--model is required when --calibration is absent")
        model_path = args.model.resolve()
        model = RealtimeSpikeModel(model_path)
        builders = [
            ClipBuilder(
                recording,
                backgrounds[index],
                isolation,
                model.frames_per_clip,
                model.point_count,
            )
            for index, recording in enumerate(capture.devices)
        ]
        initial_fits = [
            calibrate_device(
                capture,
                index,
                builders[index],
                model,
                args.maximum_calibration_frames,
                args.minimum_visibility,
            )
            for index in range(len(capture.devices))
        ]
        surface_results = None
        fits = initial_fits
        if capture.kinect_depth is not None and (
            not args.disable_surface_refinement
        ):
            surface_options = SurfaceRegistrationOptions(
                points_per_cloud=args.surface_points_per_cloud,
                minimum_frames=min(8, args.maximum_surface_frames),
            )
            surface_results = []
            for index in range(len(capture.devices)):
                frames = surface_frames_for_device(
                    capture,
                    index,
                    builders[index],
                    args.maximum_surface_frames,
                )
                result = refine_surface_registration(
                    frames, initial_fits[index], surface_options
                )
                surface_results.append(result)
                print(
                    f"[SURFACE] camera={index} frames={result.frame_count} "
                    f"median={result.initial_median_error_m * 1000:.1f}->"
                    f"{result.final_median_error_m * 1000:.1f}mm "
                    f"p95={result.initial_p95_error_m * 1000:.1f}->"
                    f"{result.final_p95_error_m * 1000:.1f}mm "
                    f"correction={result.rotation_correction_deg:.2f}deg/"
                    f"{result.translation_correction_m * 1000:.1f}mm"
                )
                if result.final_p95_error_m > args.maximum_surface_p95_m and (
                    not args.allow_poor_fit
                ):
                    raise ValueError(
                        f"camera {index} symmetric surface P95 exceeds "
                        f"{args.maximum_surface_p95_m * 1000:.0f} mm"
                    )
            fits = [item.fit for item in surface_results]
        calibration_value = calibration_json(
            capture,
            fits,
            source_manifest_sha256,
            source_integrity,
            sha256(config_path),
            sha256(model_path),
            args.minimum_visibility,
            initial_fits=initial_fits if surface_results is not None else None,
            surface_results=surface_results,
            surface_options=(
                surface_options if surface_results is not None else None
            ),
            maximum_surface_p95_m=(
                args.maximum_surface_p95_m
                if surface_results is not None else None
            ),
        )

    for index, fit in enumerate(fits):
        print(
            f"[CALIBRATE] camera={index} inliers={fit.inlier_count}/"
            f"{fit.correspondence_count} median={fit.median_error_m * 1000:.1f}mm "
            f"p95={fit.p95_error_m * 1000:.1f}mm"
        )
        if fit.p95_error_m > args.maximum_fit_p95_m and (
            not args.allow_poor_fit
        ):
            raise ValueError(
                f"camera {index} fit P95 exceeds "
                f"{args.maximum_fit_p95_m * 1000:.0f} mm"
            )

    assert calibration_value is not None
    write_json(output / "camera-calibration.json", calibration_value)
    devices = generate_dataset(
        capture,
        output,
        fits,
        backgrounds,
        isolation,
        config.runtime.frames_per_clip,
        config.runtime.point_count,
        args.shard_size,
        args.minimum_labeled_joints,
        args.minimum_samples_per_device,
    )
    manifest = {
        "schema": DATASET_SCHEMA,
        "source_manifest_sha256": source_manifest_sha256,
        "source_capture": source_integrity,
        "source_session": capture.root.name,
        "runtime_config_sha256": sha256(config_path),
        "frames_per_clip": config.runtime.frames_per_clip,
        "points_per_frame": config.runtime.point_count,
        "clip_dtype": "float16",
        "joint_dtype": "float32",
        "joint_order": list(JOINT_NAMES),
        "joint_coordinate": "D4xx camera meters, centered by input clip centroid",
        "visibility_semantics": "1.0 fully tracked; lower values masked by training",
        "teacher_observation": "stabilized",
        "calibration": calibration_value,
        "quality": quality,
        "devices": devices,
    }
    write_json(output / "dataset-manifest.json", manifest)
    write_json(output / "capture-quality.json", quality)
    print(
        "[OK] deterministic SPiKE teacher shards: "
        + ", ".join(
            f"camera {item['index']}={item['samples']}" for item in devices
        )
    )
    print(f"     manifest: {output / 'dataset-manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
