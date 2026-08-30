from __future__ import annotations

import numpy as np

from anygear_spike.fusion import (
    FusionOptions,
    JOINT_COUNT,
    LEFT_HAND,
    RigidTransform,
    fuse_views,
    kinematic_confidence,
)


def sample_pose() -> np.ndarray:
    # Compact but anatomically connected synthetic SPiKE skeleton.
    return np.asarray(
        [
            (0.00, 1.75, 2.00), (0.00, 1.52, 2.00),
            (-0.20, 1.48, 2.00), (0.20, 1.48, 2.00),
            (-0.42, 1.22, 2.00), (0.42, 1.22, 2.00),
            (-0.58, 0.98, 2.00), (0.58, 0.98, 2.00),
            (0.00, 1.15, 2.00), (-0.12, 0.98, 2.00),
            (0.12, 0.98, 2.00), (-0.12, 0.53, 2.00),
            (0.12, 0.53, 2.00), (-0.12, 0.08, 2.00),
            (0.12, 0.08, 2.00),
        ],
        dtype=np.float32,
    )


def surface_points(pose: np.ndarray) -> np.ndarray:
    return np.repeat(pose, 32, axis=0)


def test_rigid_transform_applies_rotation_and_translation() -> None:
    transform = RigidTransform.from_matrix(
        (0, 0, 1, 1, 0, 1, 0, 2, -1, 0, 0, 3, 0, 0, 0, 1)
    )
    result = transform.apply(np.asarray(((2, 4, 6),), dtype=np.float32))
    np.testing.assert_allclose(result, ((7, 6, 1),), atol=1e-6)


def test_primary_mode_does_not_mix_uncalibrated_cameras() -> None:
    first = sample_pose()
    second = first + np.asarray((1.0, 0.0, 0.0), dtype=np.float32)
    result = fuse_views(
        np.stack((first, second)),
        (surface_points(first), surface_points(second)),
        (True, True),
        (RigidTransform.identity(), RigidTransform.identity()),
        FusionOptions(mode="primary", primary_camera=0),
    )
    np.testing.assert_allclose(result.joints, first, atol=1e-6)
    assert result.valid


def test_calibrated_views_average_small_disagreement() -> None:
    first = sample_pose()
    second = first + np.asarray((0.02, 0.0, 0.0), dtype=np.float32)
    result = fuse_views(
        np.stack((first, second)),
        (surface_points(first), surface_points(second)),
        (True, True),
        (RigidTransform.identity(), RigidTransform.identity()),
        FusionOptions(mode="calibrated"),
    )
    np.testing.assert_allclose(result.joints[:, 0], first[:, 0] + 0.01,
                               atol=2e-3)
    assert result.valid


def test_disconnected_hand_is_penalized_and_not_averaged() -> None:
    good = sample_pose()
    bad = good.copy()
    bad[LEFT_HAND] += np.asarray((1.3, 0.8, 0.0), dtype=np.float32)
    penalties = kinematic_confidence(bad)
    assert penalties[LEFT_HAND] < 0.1
    result = fuse_views(
        np.stack((good, bad)),
        (surface_points(good), surface_points(good)),
        (True, True),
        (RigidTransform.identity(), RigidTransform.identity()),
        FusionOptions(mode="calibrated"),
        previous_joints=good,
        elapsed_seconds=1.0 / 30.0,
    )
    np.testing.assert_allclose(result.joints[LEFT_HAND], good[LEFT_HAND],
                               atol=1e-6)
    assert result.tracking_state[LEFT_HAND] == 2


def test_equally_supported_severe_disagreement_is_not_guessed() -> None:
    first = sample_pose()
    second = first + np.asarray((0.6, 0.0, 0.0), dtype=np.float32)
    result = fuse_views(
        np.stack((first, second)),
        (surface_points(first), surface_points(second)),
        (True, True),
        (RigidTransform.identity(), RigidTransform.identity()),
        FusionOptions(mode="calibrated"),
    )
    assert not result.valid
    assert np.all(result.tracking_state == 0)


def test_invalid_primary_falls_back_to_secondary() -> None:
    pose = sample_pose()
    result = fuse_views(
        np.stack((np.zeros_like(pose), pose)),
        (np.empty((0, 3), dtype=np.float32), surface_points(pose)),
        (False, True),
        (RigidTransform.identity(), RigidTransform.identity()),
        FusionOptions(mode="primary", primary_camera=0),
    )
    np.testing.assert_allclose(result.joints, pose, atol=1e-6)
    assert result.valid
