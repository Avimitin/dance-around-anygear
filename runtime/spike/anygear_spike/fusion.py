"""Coordinate transforms and low-latency fusion for two depth views."""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Iterable, Sequence

import numpy as np

from .protocol import CAMERA_COUNT, JOINT_COUNT


# SPiKE/ITOP order: head, neck, right/left shoulder, right/left elbow,
# right/left hand, torso, right/left hip, right/left knee, right/left foot.
HEAD = 0
NECK = 1
RIGHT_SHOULDER = 2
LEFT_SHOULDER = 3
RIGHT_ELBOW = 4
LEFT_ELBOW = 5
RIGHT_HAND = 6
LEFT_HAND = 7
TORSO = 8
RIGHT_HIP = 9
LEFT_HIP = 10
RIGHT_KNEE = 11
LEFT_KNEE = 12
RIGHT_FOOT = 13
LEFT_FOOT = 14

CORE_JOINTS = np.asarray((NECK, TORSO, RIGHT_HIP, LEFT_HIP))

# A surface point is naturally farther from a predicted internal joint such as
# torso or hip than from a hand. These radii are used as relative view-quality
# evidence, not as a pose acceptance threshold.
SUPPORT_RADIUS_M = np.asarray(
    (
        0.18, 0.22, 0.16, 0.16, 0.14, 0.14, 0.12, 0.12,
        0.28, 0.22, 0.22, 0.16, 0.16, 0.14, 0.14,
    ),
    dtype=np.float32,
)

MAXIMUM_SPEED_MPS = np.asarray(
    (
        4.0, 4.0, 5.0, 5.0, 7.0, 7.0, 9.0, 9.0,
        4.0, 5.0, 5.0, 7.0, 7.0, 9.0, 9.0,
    ),
    dtype=np.float32,
)

# (first joint, second joint, broad adult range in meters). Ranges are wide on
# purpose: they suppress disconnected predictions without constraining dance
# poses or assuming one body shape.
BONE_RANGES = (
    (HEAD, NECK, 0.07, 0.38),
    (NECK, RIGHT_SHOULDER, 0.06, 0.40),
    (NECK, LEFT_SHOULDER, 0.06, 0.40),
    (RIGHT_SHOULDER, RIGHT_ELBOW, 0.14, 0.52),
    (LEFT_SHOULDER, LEFT_ELBOW, 0.14, 0.52),
    (RIGHT_ELBOW, RIGHT_HAND, 0.12, 0.55),
    (LEFT_ELBOW, LEFT_HAND, 0.12, 0.55),
    (NECK, TORSO, 0.14, 0.65),
    (TORSO, RIGHT_HIP, 0.05, 0.42),
    (TORSO, LEFT_HIP, 0.05, 0.42),
    (RIGHT_HIP, RIGHT_KNEE, 0.22, 0.72),
    (LEFT_HIP, LEFT_KNEE, 0.22, 0.72),
    (RIGHT_KNEE, RIGHT_FOOT, 0.20, 0.78),
    (LEFT_KNEE, LEFT_FOOT, 0.20, 0.78),
)


@dataclass(frozen=True)
class RigidTransform:
    rotation: np.ndarray
    translation: np.ndarray

    @classmethod
    def identity(cls) -> "RigidTransform":
        return cls(np.eye(3, dtype=np.float32), np.zeros(3, dtype=np.float32))

    @classmethod
    def from_matrix(cls, values: Iterable[float]) -> "RigidTransform":
        matrix = np.asarray(tuple(values), dtype=np.float64)
        if matrix.size != 16:
            raise ValueError("camera transform must contain 16 numbers")
        matrix = matrix.reshape(4, 4)
        if not np.allclose(matrix[3], (0.0, 0.0, 0.0, 1.0), atol=1e-5):
            raise ValueError("camera transform has an invalid last row")
        rotation = matrix[:3, :3]
        if not np.allclose(rotation.T @ rotation, np.eye(3), atol=2e-3):
            raise ValueError("camera transform rotation is not orthonormal")
        if not math.isclose(np.linalg.det(rotation), 1.0, abs_tol=2e-3):
            raise ValueError("camera transform rotation must preserve handedness")
        return cls(
            rotation.astype(np.float32),
            matrix[:3, 3].astype(np.float32),
        )

    def apply(self, points: np.ndarray) -> np.ndarray:
        points = np.asarray(points, dtype=np.float32)
        return points @ self.rotation.T + self.translation


@dataclass(frozen=True)
class FusionOptions:
    mode: str = "primary"
    primary_camera: int = 0
    agreement_distance_m: float = 0.22
    severe_disagreement_m: float = 0.45
    tracked_confidence: float = 0.55
    inferred_confidence: float = 0.15


@dataclass(frozen=True)
class FusionResult:
    valid: bool
    joints: np.ndarray
    confidence: np.ndarray
    tracking_state: np.ndarray
    camera_joints: np.ndarray
    camera_confidence: np.ndarray


def point_support_confidence(
    joints: np.ndarray, points: np.ndarray
) -> np.ndarray:
    """Estimate relative view quality from distance to observed body points."""
    joints = np.asarray(joints, dtype=np.float32)
    points = np.asarray(points, dtype=np.float32)
    if joints.shape != (JOINT_COUNT, 3) or not len(points):
        return np.zeros(JOINT_COUNT, dtype=np.float32)
    # 4096 x 15 distances are smaller and faster than building a tree here.
    squared = np.sum(
        (joints[:, None, :] - points[None, :, :]) ** 2, axis=2
    )
    distance = np.sqrt(np.min(squared, axis=1))
    confidence = np.exp(
        -0.5 * (distance / SUPPORT_RADIUS_M) ** 2
    )
    return np.clip(confidence, 0.05, 1.0).astype(np.float32)


def kinematic_confidence(joints: np.ndarray) -> np.ndarray:
    """Return per-joint confidence penalties for disconnected limbs."""
    joints = np.asarray(joints, dtype=np.float32)
    result = np.ones(JOINT_COUNT, dtype=np.float32)
    if joints.shape != (JOINT_COUNT, 3) or not np.all(np.isfinite(joints)):
        return np.zeros(JOINT_COUNT, dtype=np.float32)
    for first, second, minimum, maximum in BONE_RANGES:
        length = float(np.linalg.norm(joints[first] - joints[second]))
        if length < minimum:
            scale = max(0.05, math.exp(-(minimum - length) / 0.06))
        elif length > maximum:
            scale = max(0.05, math.exp(-(length - maximum) / 0.10))
        else:
            scale = 1.0
        result[first] = min(result[first], scale)
        result[second] = min(result[second], scale)
    return result


def temporal_confidence(
    joints: np.ndarray,
    previous_joints: np.ndarray | None,
    elapsed_seconds: float | None,
) -> np.ndarray:
    if previous_joints is None or elapsed_seconds is None or elapsed_seconds <= 0:
        return np.ones(JOINT_COUNT, dtype=np.float32)
    previous = np.asarray(previous_joints, dtype=np.float32)
    if previous.shape != (JOINT_COUNT, 3):
        return np.ones(JOINT_COUNT, dtype=np.float32)
    speed = np.linalg.norm(joints - previous, axis=1) / elapsed_seconds
    ratio = np.maximum(speed / MAXIMUM_SPEED_MPS - 1.0, 0.0)
    return np.exp(-2.0 * ratio * ratio).astype(np.float32)


def fuse_views(
    camera_joints: np.ndarray,
    camera_points: Sequence[np.ndarray],
    camera_valid: Sequence[bool],
    transforms: Sequence[RigidTransform],
    options: FusionOptions = FusionOptions(),
    previous_joints: np.ndarray | None = None,
    elapsed_seconds: float | None = None,
) -> FusionResult:
    poses = np.asarray(camera_joints, dtype=np.float32)
    if poses.shape != (CAMERA_COUNT, JOINT_COUNT, 3):
        raise ValueError("camera_joints must have shape (2, 15, 3)")
    if len(camera_points) != CAMERA_COUNT or len(camera_valid) != CAMERA_COUNT:
        raise ValueError("two camera inputs are required")
    if len(transforms) != CAMERA_COUNT:
        raise ValueError("two camera transforms are required")
    if options.mode not in ("primary", "calibrated"):
        raise ValueError("fusion mode must be 'primary' or 'calibrated'")
    if options.primary_camera not in range(CAMERA_COUNT):
        raise ValueError("primary camera must be 0 or 1")

    transformed = np.zeros_like(poses)
    transformed_points: list[np.ndarray] = []
    confidence = np.zeros((CAMERA_COUNT, JOINT_COUNT), dtype=np.float32)
    valid = np.asarray(camera_valid, dtype=np.bool_)
    for index in range(CAMERA_COUNT):
        transformed[index] = transforms[index].apply(poses[index])
        points = transforms[index].apply(camera_points[index])
        transformed_points.append(points)
        if not valid[index] or not np.all(np.isfinite(transformed[index])):
            valid[index] = False
            continue
        confidence[index] = (
            point_support_confidence(transformed[index], points)
            * kinematic_confidence(transformed[index])
            * temporal_confidence(
                transformed[index], previous_joints, elapsed_seconds
            )
        )

    primary = options.primary_camera
    secondary = 1 - primary
    fused = np.zeros((JOINT_COUNT, 3), dtype=np.float32)
    fused_confidence = np.zeros(JOINT_COUNT, dtype=np.float32)

    if options.mode == "primary":
        selected = primary if valid[primary] else secondary
        if valid[selected]:
            fused[:] = transformed[selected]
            fused_confidence[:] = confidence[selected]
            if selected != primary:
                fused_confidence *= 0.75
    elif valid[0] and valid[1]:
        disagreement = np.linalg.norm(transformed[0] - transformed[1], axis=1)
        agreement = np.exp(
            -0.5 * (disagreement / options.agreement_distance_m) ** 2
        ).astype(np.float32)
        for joint in range(JOINT_COUNT):
            first_weight = float(confidence[0, joint])
            second_weight = float(confidence[1, joint])
            if disagreement[joint] >= options.severe_disagreement_m:
                # A large disagreement is usually an occluded limb in one view.
                # Select the better-supported view instead of averaging a hand
                # into empty space between two predictions. Confidence is the
                # winning view's margin over the other view: a clear winner can
                # repair an occlusion, while equally plausible disagreement is
                # reported as untracked instead of choosing arbitrarily.
                selected = 0 if first_weight >= second_weight else 1
                other = 1 - selected
                fused[joint] = transformed[selected, joint]
                fused_confidence[joint] = max(
                    float(confidence[selected, joint]) -
                    float(confidence[other, joint]),
                    0.0,
                )
                continue
            weights = np.asarray((first_weight, second_weight), dtype=np.float32)
            weight_sum = float(weights.sum())
            if weight_sum <= 1e-6:
                continue
            fused[joint] = np.average(
                transformed[:, joint], axis=0, weights=weights
            )
            fused_confidence[joint] = (
                max(first_weight, second_weight) * agreement[joint]
            )
    else:
        selected = 0 if valid[0] else 1
        if valid[selected]:
            fused[:] = transformed[selected]
            confidence_scale = 0.70 if options.mode == "calibrated" else 1.0
            fused_confidence[:] = confidence[selected] * confidence_scale

    tracking_state = np.zeros(JOINT_COUNT, dtype=np.uint8)
    tracking_state[fused_confidence >= options.inferred_confidence] = 1
    tracking_state[fused_confidence >= options.tracked_confidence] = 2
    pose_valid = bool(
        np.all(tracking_state[CORE_JOINTS] > 0)
        and np.count_nonzero(tracking_state) >= 10
    )
    return FusionResult(
        valid=pose_valid,
        joints=fused,
        confidence=fused_confidence,
        tracking_state=tracking_state,
        camera_joints=transformed,
        camera_confidence=confidence,
    )
