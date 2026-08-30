from __future__ import annotations

import numpy as np

from anygear_spike.depth import (
    DISTORTION_BROWN_CONRADY,
    DISTORTION_INVERSE_BROWN_CONRADY,
    IsolationOptions,
    _deprojection_rays,
    _label_voxels,
    _person_component,
    estimate_background,
    isolate_point_cloud,
    isolate_player,
    sample_points,
)
from anygear_spike.protocol import DepthSnapshot


def depth_snapshot(depth: np.ndarray) -> DepthSnapshot:
    height, width = depth.shape
    return DepthSnapshot(
        camera_index=0,
        frame_sequence=1,
        host_time_ns=0,
        depth_scale_m=0.001,
        width=width,
        height=height,
        principal_x=width / 2,
        principal_y=height / 2,
        focal_x=100.0,
        focal_y=100.0,
        distortion_model=0,
        distortion=(0.0, 0.0, 0.0, 0.0, 0.0),
        device_index=0,
        serial="test",
        depth=depth,
    )


def test_point_sampling_is_deterministic() -> None:
    points = np.arange(30_000, dtype=np.float32).reshape(-1, 3)
    first = sample_points(points, 4096)
    second = sample_points(points, 4096)
    np.testing.assert_array_equal(first, second)


def _brown_reference(
    pixel_x: float,
    pixel_y: float,
    principal_x: float,
    principal_y: float,
    focal_x: float,
    focal_y: float,
    coefficients: tuple[float, ...],
    inverse: bool,
) -> tuple[float, float]:
    original_x = (pixel_x - principal_x) / focal_x
    original_y = (pixel_y - principal_y) / focal_y
    x, y = original_x, original_y
    for _ in range(10):
        radius2 = x * x + y * y
        inverse_radial = 1.0 / (
            1.0 +
            ((coefficients[4] * radius2 + coefficients[1]) * radius2 +
             coefficients[0]) * radius2
        )
        tangent_x = x / inverse_radial if inverse else x
        tangent_y = y / inverse_radial if inverse else y
        delta_x = (
            2.0 * coefficients[2] * tangent_x * tangent_y +
            coefficients[3] * (radius2 + 2.0 * tangent_x * tangent_x)
        )
        delta_y = (
            2.0 * coefficients[3] * tangent_x * tangent_y +
            coefficients[2] * (radius2 + 2.0 * tangent_y * tangent_y)
        )
        x = (original_x - delta_x) * inverse_radial
        y = (original_y - delta_y) * inverse_radial
    return x, -y


def test_brown_deprojection_matches_realsense_coordinate_contract() -> None:
    coefficients = (0.11, -0.04, 0.008, -0.012, 0.003)
    for model, inverse in (
        (DISTORTION_INVERSE_BROWN_CONRADY, True),
        (DISTORTION_BROWN_CONRADY, False),
    ):
        ray_x, ray_y = _deprojection_rays(
            4, 3, 1.4, 1.2, 3.1, 3.2, model, coefficients
        )
        expected = _brown_reference(
            3.0, 0.0, 1.4, 1.2, 3.1, 3.2, coefficients, inverse
        )
        np.testing.assert_allclose(
            (ray_x[0, 3], ray_y[0, 3]), expected, rtol=2e-6, atol=2e-6
        )


def test_deprojection_cache_includes_distortion_calibration() -> None:
    ideal_x, ideal_y = _deprojection_rays(
        4, 3, 1.4, 1.2, 3.1, 3.2, 0, (0.0,) * 5
    )
    distorted_x, distorted_y = _deprojection_rays(
        4, 3, 1.4, 1.2, 3.1, 3.2,
        DISTORTION_INVERSE_BROWN_CONRADY,
        (0.11, -0.04, 0.008, -0.012, 0.003),
    )
    assert not np.allclose(ideal_x, distorted_x)
    assert not np.allclose(ideal_y, distorted_y)


def test_point_sampling_repeats_small_cloud_without_zeros() -> None:
    points = np.asarray(((1, 2, 3), (4, 5, 6)), dtype=np.float32)
    result = sample_points(points, 9)
    assert result.shape == (9, 3)
    assert {tuple(row) for row in result} == {(1, 2, 3), (4, 5, 6)}


def test_background_envelope_ignores_depth_holes_and_outliers() -> None:
    frames = [
        np.asarray(((0, 1000), (2000, 3000)), dtype=np.uint16),
        np.asarray(((0, 1100), (0, 2900)), dtype=np.uint16),
        np.asarray(((0, 900), (2200, 3100)), dtype=np.uint16),
        np.asarray(((0, 1000), (2100, 3000)), dtype=np.uint16),
        np.asarray(((5000, 8000), (2100, 3000)), dtype=np.uint16),
    ]
    np.testing.assert_array_equal(
        estimate_background(frames),
        np.asarray(((0, 1000), (2100, 3000)), dtype=np.uint16),
    )


def test_background_mask_keeps_a_closer_person_only() -> None:
    background = np.full((120, 120), 3000, dtype=np.uint16)
    current = background.copy()
    current[20:100, 40:80] = 2000
    options = IsolationOptions(
        pixel_stride=1,
        minimum_person_points=500,
        minimum_person_height_m=1.0,
        minimum_acquisition_height_m=1.0,
    )
    assert not len(isolate_player(
        depth_snapshot(background), options, background
    ))
    person = isolate_player(depth_snapshot(current), options, background)
    assert len(person) >= 500
    assert np.ptp(person[:, 1]) >= 1.0
    assert np.ptp(person[:, 0]) <= 1.0


def test_vectorized_voxel_labels_match_reference_flood_fill() -> None:
    generator = np.random.default_rng(42)
    coordinates = np.unique(
        generator.integers(0, 8, size=(80, 3), dtype=np.int32), axis=0
    )
    labels, count = _label_voxels(
        coordinates, np.asarray((8, 8, 8), dtype=np.int32)
    )
    remaining = {tuple(value) for value in coordinates}
    reference = {}
    component = 0
    while remaining:
        component += 1
        pending = [remaining.pop()]
        while pending:
            current = pending.pop()
            reference[current] = component
            for x in (-1, 0, 1):
                for y in (-1, 0, 1):
                    for z in (-1, 0, 1):
                        neighbor = (
                            current[0] + x,
                            current[1] + y,
                            current[2] + z,
                        )
                        if neighbor in remaining:
                            remaining.remove(neighbor)
                            pending.append(neighbor)
    reference_labels = np.asarray(
        [reference[tuple(value)] for value in coordinates]
    )
    assert count == component
    np.testing.assert_array_equal(
        labels[:, None] == labels[None, :],
        reference_labels[:, None] == reference_labels[None, :],
    )


def test_voxel_component_selects_a_plausible_person() -> None:
    x, y, z = np.meshgrid(
        np.linspace(-0.20, 0.20, 9),
        np.linspace(-0.75, 0.75, 31),
        np.linspace(1.90, 2.10, 5),
        indexing="ij",
    )
    body = np.column_stack((x.ravel(), y.ravel(), z.ravel())).astype(
        np.float32
    )
    noise = np.asarray(((1.2, 0.0, 3.0), (1.21, 0.0, 3.0)), np.float32)
    selected = _person_component(
        np.concatenate((body, noise)),
        voxel_size_m=0.075,
        minimum_component_points=10,
        cluster_offset_m=0.2,
        minimum_person_height_m=1.15,
        maximum_person_height_m=2.5,
        maximum_person_width_m=2.5,
        maximum_person_depth_m=1.8,
    )
    assert len(selected) == len(body)
    assert np.ptp(selected[:, 1]) >= 1.49


def test_voxel_component_rejects_a_short_object() -> None:
    x, y, z = np.meshgrid(
        np.linspace(-0.20, 0.20, 9),
        np.linspace(-0.20, 0.20, 9),
        np.linspace(1.90, 2.10, 5),
        indexing="ij",
    )
    short = np.column_stack((x.ravel(), y.ravel(), z.ravel())).astype(
        np.float32
    )
    selected = _person_component(
        short,
        voxel_size_m=0.075,
        minimum_component_points=10,
        cluster_offset_m=0.2,
        minimum_person_height_m=1.15,
        maximum_person_height_m=2.5,
        maximum_person_width_m=2.5,
        maximum_person_depth_m=1.8,
    )
    assert not len(selected)


def test_point_cloud_isolation_applies_the_production_contract() -> None:
    x, y, z = np.meshgrid(
        np.linspace(-0.20, 0.20, 9),
        np.linspace(-0.75, 0.75, 31),
        np.linspace(1.90, 2.10, 5),
        indexing="ij",
    )
    body = np.column_stack((x.ravel(), y.ravel(), z.ravel())).astype(
        np.float32
    )
    noise = np.asarray(
        ((5.0, 0.0, 2.0), (np.nan, 0.0, 2.0)), dtype=np.float32
    )
    options = IsolationOptions(
        pixel_stride=1,
        minimum_person_points=500,
        minimum_person_height_m=1.15,
        minimum_acquisition_height_m=1.15,
    )
    selected = isolate_point_cloud(np.concatenate((body, noise)), options)
    assert len(selected) == len(body)
    assert np.all(np.isfinite(selected))
