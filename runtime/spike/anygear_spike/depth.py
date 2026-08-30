"""Fast depth deprojection and player isolation for the SPiKE worker."""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache

import numpy as np
import cc3d

from .protocol import DepthSnapshot


DISTORTION_NONE = 0
DISTORTION_MODIFIED_BROWN_CONRADY = 1
DISTORTION_INVERSE_BROWN_CONRADY = 2
DISTORTION_FTHETA = 3
DISTORTION_BROWN_CONRADY = 4
DISTORTION_KANNALA_BRANDT4 = 5


@dataclass(frozen=True)
class IsolationOptions:
    minimum_x_m: float = -1.4
    maximum_x_m: float = 1.4
    minimum_y_m: float = -1.8
    maximum_y_m: float = 1.3
    minimum_z_m: float = 0.45
    maximum_z_m: float = 4.5
    voxel_size_m: float = 0.075
    pixel_stride: int = 3
    minimum_component_points: int = 10
    minimum_person_points: int = 500
    cluster_offset_m: float = 0.2
    minimum_person_height_m: float = 0.65
    minimum_acquisition_height_m: float = 1.15
    maximum_person_height_m: float = 2.50
    maximum_person_width_m: float = 2.50
    maximum_person_depth_m: float = 1.80
    background_calibration_frames: int = 45
    background_margin_m: float = 0.10
    dropout_hold_ms: int = 100


@lru_cache(maxsize=16)
def _deprojection_rays(
    width: int,
    height: int,
    principal_x: float,
    principal_y: float,
    focal_x: float,
    focal_y: float,
    distortion_model: int,
    distortion: tuple[float, ...],
) -> tuple[np.ndarray, np.ndarray]:
    rows, columns = np.indices((height, width), dtype=np.float32)
    x = (columns - principal_x) / focal_x
    y = (rows - principal_y) / focal_y
    coefficients = np.asarray(distortion, dtype=np.float32)
    if coefficients.shape != (5,):
        raise ValueError("depth intrinsics require five distortion values")
    if distortion_model not in range(6):
        raise ValueError(f"unsupported depth distortion model {distortion_model}")
    if distortion_model == DISTORTION_MODIFIED_BROWN_CONRADY:
        raise ValueError(
            "modified Brown-Conrady depth cannot be deprojected directly"
        )

    if np.any(np.abs(coefficients) >= np.finfo(np.float32).eps):
        original_x = x.copy()
        original_y = y.copy()
        if distortion_model in (
            DISTORTION_INVERSE_BROWN_CONRADY,
            DISTORTION_BROWN_CONRADY,
        ):
            for _ in range(10):
                radius2 = x * x + y * y
                inverse_radial = 1.0 / (
                    1.0 +
                    ((coefficients[4] * radius2 + coefficients[1]) *
                     radius2 + coefficients[0]) * radius2
                )
                if distortion_model == DISTORTION_INVERSE_BROWN_CONRADY:
                    tangent_x = x / inverse_radial
                    tangent_y = y / inverse_radial
                else:
                    tangent_x = x
                    tangent_y = y
                delta_x = (
                    2.0 * coefficients[2] * tangent_x * tangent_y +
                    coefficients[3] *
                    (radius2 + 2.0 * tangent_x * tangent_x)
                )
                delta_y = (
                    2.0 * coefficients[3] * tangent_x * tangent_y +
                    coefficients[2] *
                    (radius2 + 2.0 * tangent_y * tangent_y)
                )
                x = (original_x - delta_x) * inverse_radial
                y = (original_y - delta_y) * inverse_radial
        elif distortion_model == DISTORTION_KANNALA_BRANDT4:
            radius_distorted = np.maximum(
                np.sqrt(x * x + y * y), np.finfo(np.float32).eps
            )
            theta = radius_distorted.copy()
            for _ in range(4):
                theta2 = theta * theta
                residual = theta * (
                    1.0 + theta2 * (
                        coefficients[0] + theta2 * (
                            coefficients[1] + theta2 * (
                                coefficients[2] + theta2 * coefficients[3]
                            )
                        )
                    )
                ) - radius_distorted
                derivative = 1.0 + theta2 * (
                    3.0 * coefficients[0] + theta2 * (
                        5.0 * coefficients[1] + theta2 * (
                            7.0 * coefficients[2] +
                            9.0 * theta2 * coefficients[3]
                        )
                    )
                )
                theta -= residual / derivative
            radius = np.tan(theta)
            x *= radius / radius_distorted
            y *= radius / radius_distorted
        elif distortion_model == DISTORTION_FTHETA:
            radius_distorted = np.maximum(
                np.sqrt(x * x + y * y), np.finfo(np.float32).eps
            )
            radius = np.tan(coefficients[0] * radius_distorted) / np.arctan(
                2.0 * np.tan(coefficients[0] / 2.0)
            )
            x *= radius / radius_distorted
            y *= radius / radius_distorted

    # Sensor image Y grows downward; the runtime stage convention grows up.
    ray_x = x.astype(np.float32, copy=False)
    ray_y_up = (-y).astype(np.float32, copy=False)
    ray_x.setflags(write=False)
    ray_y_up.setflags(write=False)
    return ray_x, ray_y_up


def _remove_floor(points: np.ndarray) -> np.ndarray:
    if len(points) < 100:
        return points
    counts, edges = np.histogram(points[:, 1], bins=100)
    low_bins = counts[:10]
    if low_bins.sum() <= 10 * counts.mean():
        return points
    peak = int(low_bins.argmax())
    below_mean = low_bins[peak:] < low_bins[peak:].mean()
    cut = int(below_mean.argmax()) if np.any(below_mean) else 0
    return points[points[:, 1] >= edges[peak + cut]]


def _label_voxels(
    shifted: np.ndarray, span: np.ndarray
) -> tuple[np.ndarray, int]:
    """Label occupied voxels with the pinned linear-time C++ runtime."""
    occupancy = np.zeros(tuple(int(value) for value in span), dtype=np.uint8)
    occupancy[tuple(shifted.T)] = 1
    labels, label_count = cc3d.connected_components(
        occupancy,
        connectivity=26,
        return_N=True,
        out_dtype=np.uint32,
    )
    return labels[tuple(shifted.T)], int(label_count)


def _person_component(
    points: np.ndarray,
    voxel_size_m: float,
    minimum_component_points: int,
    cluster_offset_m: float,
    minimum_person_height_m: float,
    maximum_person_height_m: float,
    maximum_person_width_m: float,
    maximum_person_depth_m: float,
) -> np.ndarray:
    if not len(points):
        return points
    coordinates = np.floor(points / voxel_size_m).astype(np.int32)
    minimum_coordinate = coordinates.min(axis=0)
    shifted = coordinates - minimum_coordinate
    span = shifted.max(axis=0) + 1
    if np.prod(span, dtype=np.int64) > 8_000_000:
        return np.empty((0, 3), dtype=np.float32)
    point_labels, label_count = _label_voxels(shifted, span)
    counts = np.bincount(point_labels, minlength=label_count + 1)
    counts[0] = 0
    counts[counts < minimum_component_points] = 0
    selected_label = 0
    selected_count = 0
    for label_id in range(1, label_count + 1):
        if counts[label_id] == 0:
            continue
        component = points[point_labels == label_id]
        extent = component.max(axis=0) - component.min(axis=0)
        plausible = (
            extent[0] <= maximum_person_width_m
            and minimum_person_height_m <= extent[1] <= maximum_person_height_m
            and extent[2] <= maximum_person_depth_m
        )
        if plausible and counts[label_id] > selected_count:
            selected_label = label_id
            selected_count = int(counts[label_id])
    if selected_label == 0:
        return np.empty((0, 3), dtype=np.float32)

    largest = points[point_labels == selected_label]
    offset = cluster_offset_m
    minimum = largest.min(axis=0) - np.asarray((offset, offset, 0.0))
    maximum = largest.max(axis=0) + np.asarray(
        (offset, 2 * offset, offset)
    )
    selected = [largest]
    for label_id in range(1, label_count + 1):
        if label_id == selected_label or counts[label_id] == 0:
            continue
        component = points[point_labels == label_id]
        component_minimum = component.min(axis=0)
        component_maximum = component.max(axis=0)
        inside = np.all(component_minimum >= minimum) and np.all(
            component_maximum <= maximum
        )
        in_front = (
            component_minimum[0] >= minimum[0] + offset
            and component_maximum[0] <= maximum[0] - offset
            and component_maximum[1] <= maximum[1]
            and component_minimum[2] >= minimum[2]
            and component_maximum[2] <= maximum[2]
        )
        if inside or in_front:
            selected.append(component)
    return np.concatenate(selected).astype(np.float32, copy=False)


def estimate_background(frames: list[np.ndarray]) -> np.ndarray:
    """Build a robust far-depth envelope from a calibration window."""
    if not frames:
        raise ValueError("background calibration requires depth frames")
    shape = np.asarray(frames[0]).shape
    if len(shape) != 2 or any(
        np.asarray(frame).shape != shape for frame in frames
    ):
        raise ValueError("background calibration frame dimensions differ")
    values = np.stack(frames).astype(np.uint16, copy=False)
    # The background is normally farther than a performer. Select roughly the
    # 80th percentile so a moving performer is not baked into the stage model,
    # while requiring at least three repeated far samples to reject outliers.
    rank_from_farthest = max(3, int(np.ceil(len(frames) * 0.20)))
    rank_from_farthest = min(rank_from_farthest, len(frames))
    kth = len(frames) - rank_from_farthest
    background = np.partition(values, kth, axis=0)[kth]
    valid_count = np.count_nonzero(values, axis=0)
    return np.where(
        valid_count >= rank_from_farthest, background, 0
    ).astype(np.uint16, copy=False)


def isolate_player(
    frame: DepthSnapshot,
    options: IsolationOptions = IsolationOptions(),
    background: np.ndarray | None = None,
) -> np.ndarray:
    depth = frame.depth
    if depth.shape != (frame.height, frame.width):
        raise ValueError("depth dimensions do not match IPC metadata")
    stride = max(1, options.pixel_stride)
    sampled = depth[::stride, ::stride]
    minimum_units = options.minimum_z_m / frame.depth_scale_m
    maximum_units = options.maximum_z_m / frame.depth_scale_m
    valid = (sampled > minimum_units) & (sampled < maximum_units)
    if background is not None:
        background = np.asarray(background)
        if background.shape != depth.shape:
            raise ValueError("background dimensions do not match depth")
        sampled_background = background[::stride, ::stride]
        margin_units = options.background_margin_m / frame.depth_scale_m
        valid &= (
            (sampled_background > 0)
            & (sampled.astype(np.int32) + int(round(margin_units))
               < sampled_background.astype(np.int32))
        )
    rows, columns = np.nonzero(valid)
    rows *= stride
    columns *= stride
    z = depth[rows, columns].astype(np.float32) * frame.depth_scale_m
    ray_x, ray_y = _deprojection_rays(
        frame.width,
        frame.height,
        frame.principal_x,
        frame.principal_y,
        frame.focal_x,
        frame.focal_y,
        frame.distortion_model,
        frame.distortion,
    )
    points = np.column_stack(
        (ray_x[rows, columns] * z, ray_y[rows, columns] * z, z)
    ).astype(np.float32, copy=False)
    return isolate_point_cloud(points, options)


def isolate_point_cloud(
    points: np.ndarray,
    options: IsolationOptions = IsolationOptions(),
) -> np.ndarray:
    """Apply the production ROI and connected-component body selection."""
    points = np.asarray(points, dtype=np.float32)
    if points.ndim != 2 or points.shape[1:] != (3,):
        raise ValueError("point cloud must have shape (N, 3)")
    if not len(points):
        return np.empty((0, 3), dtype=np.float32)
    points = points[np.all(np.isfinite(points), axis=1)]
    if not len(points):
        return np.empty((0, 3), dtype=np.float32)
    roi = (
        (points[:, 0] >= options.minimum_x_m)
        & (points[:, 0] <= options.maximum_x_m)
        & (points[:, 1] >= options.minimum_y_m)
        & (points[:, 1] <= options.maximum_y_m)
        & (points[:, 2] >= options.minimum_z_m)
        & (points[:, 2] <= options.maximum_z_m)
    )
    points = _remove_floor(points[roi])
    points = _person_component(
        points,
        options.voxel_size_m,
        options.minimum_component_points,
        options.cluster_offset_m,
        options.minimum_person_height_m,
        options.maximum_person_height_m,
        options.maximum_person_width_m,
        options.maximum_person_depth_m,
    )
    if len(points) < options.minimum_person_points:
        return np.empty((0, 3), dtype=np.float32)
    return points


def sample_points(points: np.ndarray, count: int) -> np.ndarray:
    """Use a fixed training-compatible sample to avoid frame RNG noise."""
    if not len(points):
        raise ValueError("cannot sample an empty point cloud")
    random = np.random.default_rng(0)
    if len(points) >= count:
        indices = random.choice(len(points), count, replace=False)
    else:
        repeats, residue = divmod(count, len(points))
        residue_indices = random.choice(
            len(points), residue, replace=False
        ) if residue else np.empty(0, dtype=np.int64)
        indices = np.concatenate(
            (np.tile(np.arange(len(points)), repeats), residue_indices)
        )
        random.shuffle(indices)
    return points[indices].astype(np.float32, copy=False)
