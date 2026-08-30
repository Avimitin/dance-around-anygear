from __future__ import annotations

import numpy as np

from anygear_spike.surface_registration import (
    SurfaceFrame,
    SurfaceRegistrationOptions,
    refine_surface_registration,
)
from anygear_spike.teacher_data import RigidFit


def rotation_y(degrees: float) -> np.ndarray:
    angle = np.deg2rad(degrees)
    return np.asarray(
        (
            (np.cos(angle), 0.0, np.sin(angle)),
            (0.0, 1.0, 0.0),
            (-np.sin(angle), 0.0, np.cos(angle)),
        ),
        dtype=np.float32,
    )


def rigid_fit(rotation: np.ndarray, translation: np.ndarray) -> RigidFit:
    return RigidFit(
        rotation=rotation,
        translation=translation,
        correspondence_count=100,
        inlier_count=100,
        median_error_m=0.05,
        p95_error_m=0.10,
        maximum_error_m=0.15,
    )


def test_surface_refinement_recovers_synchronized_sensor_geometry() -> None:
    generator = np.random.default_rng(20260830)
    true_rotation = rotation_y(8.0)
    true_translation = np.asarray((0.15, -0.05, 0.25), dtype=np.float32)
    frames = []
    for frame_index in range(12):
        direction = generator.normal(size=(800, 3)).astype(np.float32)
        direction /= np.linalg.norm(direction, axis=1, keepdims=True)
        source = direction * np.asarray((0.35, 0.85, 0.22), np.float32)
        source += np.asarray(
            (0.03 * np.sin(frame_index), 0.0, 2.0), dtype=np.float32
        )
        target = source @ true_rotation.T + true_translation
        target += generator.normal(0.0, 0.002, target.shape).astype(np.float32)
        frames.append(SurfaceFrame(source, target))

    initial_rotation = rotation_y(10.0)
    initial_translation = true_translation + np.asarray(
        (0.025, -0.015, 0.020), dtype=np.float32
    )
    result = refine_surface_registration(
        frames,
        rigid_fit(initial_rotation, initial_translation),
        SurfaceRegistrationOptions(
            points_per_cloud=600,
            iterations=10,
            minimum_frames=8,
            minimum_correspondences_per_frame=48,
        ),
    )
    np.testing.assert_allclose(result.fit.rotation, true_rotation, atol=4e-3)
    np.testing.assert_allclose(
        result.fit.translation, true_translation, atol=4e-3
    )
    assert result.final_median_error_m < result.initial_median_error_m
    assert result.final_p95_error_m < result.initial_p95_error_m
    assert result.fit.p95_error_m < 0.01


def test_surface_refinement_rejects_too_few_frames() -> None:
    cloud = np.zeros((100, 3), dtype=np.float32)
    try:
        refine_surface_registration(
            [SurfaceFrame(cloud, cloud)],
            rigid_fit(np.eye(3, dtype=np.float32), np.zeros(3, np.float32)),
        )
    except ValueError as error:
        assert "too few" in str(error)
    else:
        raise AssertionError("undersized surface calibration was accepted")
