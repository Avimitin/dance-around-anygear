"""Deterministic multi-frame surface refinement for teacher-camera geometry."""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Iterable

import numpy as np

from .teacher_data import RigidFit, estimate_rigid_transform


@dataclass(frozen=True)
class SurfaceFrame:
    kinect_points: np.ndarray
    camera_points: np.ndarray


@dataclass(frozen=True)
class SurfaceRegistrationOptions:
    points_per_cloud: int = 1536
    iterations: int = 8
    maximum_correspondence_m: float = 0.20
    final_correspondence_m: float = 0.10
    trim_fraction: float = 0.80
    minimum_correspondences_per_frame: int = 64
    minimum_frames: int = 8
    maximum_rotation_correction_deg: float = 12.0
    maximum_translation_correction_m: float = 0.25


@dataclass(frozen=True)
class SurfaceRegistrationResult:
    fit: RigidFit
    frame_count: int
    initial_median_error_m: float
    initial_p95_error_m: float
    final_median_error_m: float
    final_p95_error_m: float
    rotation_correction_deg: float
    translation_correction_m: float

    def as_dict(self) -> dict:
        return {
            "frame_count": self.frame_count,
            "initial_median_error_m": self.initial_median_error_m,
            "initial_p95_error_m": self.initial_p95_error_m,
            "final_median_error_m": self.final_median_error_m,
            "final_p95_error_m": self.final_p95_error_m,
            "rotation_correction_deg": self.rotation_correction_deg,
            "translation_correction_m": self.translation_correction_m,
            "correspondence_count": self.fit.correspondence_count,
            "inlier_count": self.fit.inlier_count,
        }


def _sample_evenly(points: np.ndarray, maximum: int) -> np.ndarray:
    value = np.asarray(points, dtype=np.float32)
    if value.ndim != 2 or value.shape[1] != 3:
        raise ValueError("surface points must have shape (N, 3)")
    value = value[np.all(np.isfinite(value), axis=1)]
    if len(value) <= maximum:
        return value
    indices = np.linspace(0, len(value) - 1, maximum, dtype=np.int64)
    return value[indices]


def _validated_frames(
    frames: Iterable[SurfaceFrame], options: SurfaceRegistrationOptions
) -> tuple[SurfaceFrame, ...]:
    result = []
    for frame in frames:
        source = _sample_evenly(frame.kinect_points, options.points_per_cloud)
        target = _sample_evenly(frame.camera_points, options.points_per_cloud)
        if len(source) < options.minimum_correspondences_per_frame or (
            len(target) < options.minimum_correspondences_per_frame
        ):
            continue
        result.append(SurfaceFrame(source, target))
    if len(result) < options.minimum_frames:
        raise ValueError(
            "surface calibration has too few synchronized player-mask frames"
        )
    return tuple(result)


def _correspondences(
    frames: tuple[SurfaceFrame, ...],
    rotation: np.ndarray,
    translation: np.ndarray,
    maximum_distance_m: float,
    minimum_per_frame: int,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, int]:
    try:
        from scipy.spatial import cKDTree
    except ImportError as error:
        raise RuntimeError(
            "surface calibration requires the pinned scipy dependency group"
        ) from error

    source_values = []
    target_values = []
    distance_values = []
    used_frames = 0
    for frame in frames:
        transformed = frame.kinect_points @ rotation.T + translation
        target_tree = cKDTree(frame.camera_points)
        forward_distance, forward_index = target_tree.query(
            transformed, k=1, workers=1
        )
        source_tree = cKDTree(transformed)
        _, backward_index = source_tree.query(
            frame.camera_points, k=1, workers=1
        )
        mutual = (
            backward_index[forward_index]
            == np.arange(len(transformed), dtype=np.int64)
        )
        accepted = mutual & (forward_distance <= maximum_distance_m)
        if np.count_nonzero(accepted) < minimum_per_frame:
            # Views from different camera positions do not always expose the
            # same body surface. An equally sized two-direction set remains
            # useful when strict mutual pairs are sparse, without letting the
            # denser view dominate the incremental fit.
            forward = np.flatnonzero(forward_distance <= maximum_distance_m)
            backward_source = transformed[backward_index]
            backward_distance = np.linalg.norm(
                backward_source - frame.camera_points, axis=1
            )
            backward = np.flatnonzero(
                backward_distance <= maximum_distance_m
            )
            balanced_count = min(len(forward), len(backward))
            if balanced_count < minimum_per_frame:
                continue
            if len(forward) > balanced_count:
                forward = forward[
                    np.linspace(
                        0, len(forward) - 1, balanced_count, dtype=np.int64
                    )
                ]
            if len(backward) > balanced_count:
                backward = backward[
                    np.linspace(
                        0, len(backward) - 1, balanced_count, dtype=np.int64
                    )
                ]
            source_values.extend(
                (transformed[forward], backward_source[backward])
            )
            target_values.extend(
                (
                    frame.camera_points[forward_index[forward]],
                    frame.camera_points[backward],
                )
            )
            distance_values.extend(
                (forward_distance[forward], backward_distance[backward])
            )
        else:
            source_values.append(transformed[accepted])
            target_values.append(frame.camera_points[forward_index[accepted]])
            distance_values.append(forward_distance[accepted])
        used_frames += 1
    if used_frames < max(3, len(frames) // 2) or not source_values:
        raise ValueError("surface calibration found insufficient geometric overlap")
    return (
        np.concatenate(source_values).astype(np.float32, copy=False),
        np.concatenate(target_values).astype(np.float32, copy=False),
        np.concatenate(distance_values).astype(np.float32, copy=False),
        used_frames,
    )


def _symmetric_distances(
    frames: tuple[SurfaceFrame, ...],
    rotation: np.ndarray,
    translation: np.ndarray,
) -> np.ndarray:
    try:
        from scipy.spatial import cKDTree
    except ImportError as error:
        raise RuntimeError(
            "surface calibration requires the pinned scipy dependency group"
        ) from error
    distances = []
    for frame in frames:
        transformed = frame.kinect_points @ rotation.T + translation
        forward, _ = cKDTree(frame.camera_points).query(
            transformed, k=1, workers=1
        )
        backward, _ = cKDTree(transformed).query(
            frame.camera_points, k=1, workers=1
        )
        distances.extend((forward, backward))
    return np.concatenate(distances).astype(np.float32, copy=False)


def _trimmed_correspondences(
    source: np.ndarray,
    target: np.ndarray,
    distance: np.ndarray,
    trim_fraction: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    if not 0.5 <= trim_fraction <= 1.0:
        raise ValueError("surface trim fraction must be inside 0.5..1.0")
    limit = float(np.quantile(distance, trim_fraction))
    accepted = distance <= limit
    if np.count_nonzero(accepted) < 24:
        raise ValueError("surface calibration rejected too many correspondences")
    return source[accepted], target[accepted], distance[accepted]


def _error_summary(distance: np.ndarray) -> tuple[float, float, float]:
    if not len(distance):
        raise ValueError("surface calibration produced no residuals")
    return (
        float(np.median(distance)),
        float(np.percentile(distance, 95)),
        float(np.max(distance)),
    )


def _rotation_angle_degrees(rotation: np.ndarray) -> float:
    cosine = np.clip((np.trace(rotation) - 1.0) * 0.5, -1.0, 1.0)
    return math.degrees(math.acos(float(cosine)))


def refine_surface_registration(
    frames: Iterable[SurfaceFrame],
    initial_fit: RigidFit,
    options: SurfaceRegistrationOptions = SurfaceRegistrationOptions(),
) -> SurfaceRegistrationResult:
    """Refine a proper Kinect-to-camera transform using synchronized surfaces."""
    if options.points_per_cloud < 64 or options.iterations < 1:
        raise ValueError("surface calibration sampling options are invalid")
    if not 0.0 < options.final_correspondence_m <= (
        options.maximum_correspondence_m
    ):
        raise ValueError("surface correspondence distances are invalid")
    prepared = _validated_frames(frames, options)
    rotation = np.asarray(initial_fit.rotation, dtype=np.float32).copy()
    translation = np.asarray(initial_fit.translation, dtype=np.float32).copy()
    if rotation.shape != (3, 3) or translation.shape != (3,):
        raise ValueError("initial surface transform has an invalid shape")

    initial_distance = _symmetric_distances(prepared, rotation, translation)
    initial_median, initial_p95, _ = _error_summary(initial_distance)

    last_rotation = rotation.copy()
    last_translation = translation.copy()
    for iteration in range(options.iterations):
        fraction = iteration / max(1, options.iterations - 1)
        distance_limit = (
            options.maximum_correspondence_m * (1.0 - fraction)
            + options.final_correspondence_m * fraction
        )
        source, target, distance, _ = _correspondences(
            prepared,
            rotation,
            translation,
            distance_limit,
            options.minimum_correspondences_per_frame,
        )
        source, target, _ = _trimmed_correspondences(
            source, target, distance, options.trim_fraction
        )
        increment_rotation, increment_translation = estimate_rigid_transform(
            source, target
        )
        rotation = increment_rotation @ rotation
        translation = increment_rotation @ translation + increment_translation
        rotation = rotation.astype(np.float32)
        translation = translation.astype(np.float32)
        angular_step = _rotation_angle_degrees(rotation @ last_rotation.T)
        translation_step = float(np.linalg.norm(translation - last_translation))
        last_rotation = rotation.copy()
        last_translation = translation.copy()
        if angular_step < 0.005 and translation_step < 0.0001:
            break

    source, target, distance, used_frames = _correspondences(
        prepared,
        rotation,
        translation,
        options.final_correspondence_m,
        options.minimum_correspondences_per_frame,
    )
    correspondence_count = len(distance)
    source, target, inlier_distance = _trimmed_correspondences(
        source, target, distance, options.trim_fraction
    )
    # One final fit ensures the reported transform and residuals describe the
    # same deterministic inlier set.
    increment_rotation, increment_translation = estimate_rigid_transform(
        source, target
    )
    rotation = (increment_rotation @ rotation).astype(np.float32)
    translation = (
        increment_rotation @ translation + increment_translation
    ).astype(np.float32)
    fitted = source @ increment_rotation.T + increment_translation
    inlier_distance = np.linalg.norm(fitted - target, axis=1)
    inlier_median, inlier_p95, inlier_maximum = _error_summary(inlier_distance)
    final_distance = _symmetric_distances(prepared, rotation, translation)
    final_median, final_p95, _ = _error_summary(final_distance)

    correction_rotation = rotation @ initial_fit.rotation.T
    rotation_correction = _rotation_angle_degrees(correction_rotation)
    translation_correction = float(
        np.linalg.norm(translation - initial_fit.translation)
    )
    if rotation_correction > options.maximum_rotation_correction_deg:
        raise ValueError("surface calibration rotation correction is implausible")
    if translation_correction > options.maximum_translation_correction_m:
        raise ValueError("surface calibration translation correction is implausible")
    if final_median > initial_median + 1.0e-4 or (
        final_p95 > initial_p95 + 1.0e-4
    ):
        raise ValueError("surface calibration did not improve geometric alignment")
    if not np.isclose(np.linalg.det(rotation), 1.0, atol=2.0e-3):
        raise ValueError("surface calibration produced an improper rotation")

    fit = RigidFit(
        rotation=rotation,
        translation=translation,
        correspondence_count=correspondence_count,
        inlier_count=len(inlier_distance),
        median_error_m=inlier_median,
        p95_error_m=inlier_p95,
        maximum_error_m=inlier_maximum,
    )
    return SurfaceRegistrationResult(
        fit=fit,
        frame_count=used_frames,
        initial_median_error_m=initial_median,
        initial_p95_error_m=initial_p95,
        final_median_error_m=final_median,
        final_p95_error_m=final_p95,
        rotation_correction_deg=rotation_correction,
        translation_correction_m=translation_correction,
    )
