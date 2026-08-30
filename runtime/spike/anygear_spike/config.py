"""Validated JSON configuration for the D4xx SPiKE runtime."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path

from .depth import IsolationOptions
from .fusion import FusionOptions, RigidTransform


@dataclass(frozen=True)
class RuntimeOptions:
    inference_fps: int = 30
    point_count: int = 4096
    frames_per_clip: int = 3
    directml_device_index: int = 0
    warmup_runs: int = 3
    secondary_every_n: int = 3


@dataclass(frozen=True)
class WorkerConfig:
    runtime: RuntimeOptions
    isolation: IsolationOptions
    fusion: FusionOptions
    transforms: tuple[RigidTransform, RigidTransform]
    camera_serials: tuple[str, str]
    primary_serial: str


def _number(value: dict, name: str, default, minimum, maximum):
    result = value.get(name, default)
    if isinstance(default, int):
        if isinstance(result, bool) or not isinstance(result, int):
            raise ValueError(f"{name} must be an integer")
    elif isinstance(result, bool) or not isinstance(result, (int, float)):
        raise ValueError(f"{name} must be a number")
    if not minimum <= result <= maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return result


def load_config(path: Path | None) -> WorkerConfig:
    document = {}
    if path is not None and path.is_file():
        document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("SPiKE configuration root must be an object")
    if document.get("SchemaVersion", 1) != 1:
        raise ValueError("unsupported SPiKE configuration schema")

    runtime_value = document.get("Runtime", {})
    isolation_value = document.get("Isolation", {})
    fusion_value = document.get("Fusion", {})
    placement_value = document.get("D4xxPlacement", {})
    if not all(isinstance(value, dict) for value in
               (runtime_value, isolation_value, fusion_value,
                placement_value)):
        raise ValueError(
            "Runtime, Isolation, Fusion, and D4xxPlacement must be objects"
        )

    runtime = RuntimeOptions(
        inference_fps=_number(runtime_value, "InferenceFps", 30, 15, 30),
        point_count=_number(runtime_value, "PointCount", 4096, 4096, 4096),
        frames_per_clip=_number(runtime_value, "FramesPerClip", 3, 3, 3),
        directml_device_index=_number(
            runtime_value, "DirectMlDeviceIndex", 0, 0, 15
        ),
        warmup_runs=_number(runtime_value, "WarmupRuns", 3, 1, 10),
        secondary_every_n=_number(
            runtime_value, "SecondaryEveryN", 3, 1, 30
        ),
    )
    isolation = IsolationOptions(
        minimum_x_m=_number(isolation_value, "MinimumX", -1.4, -5.0, 0.0),
        maximum_x_m=_number(isolation_value, "MaximumX", 1.4, 0.0, 5.0),
        minimum_y_m=_number(isolation_value, "MinimumY", -1.8, -5.0, 0.0),
        maximum_y_m=_number(isolation_value, "MaximumY", 1.3, 0.0, 5.0),
        minimum_z_m=_number(isolation_value, "MinimumZ", 0.45, 0.1, 3.0),
        maximum_z_m=_number(isolation_value, "MaximumZ", 4.5, 1.0, 8.0),
        voxel_size_m=_number(
            isolation_value, "VoxelSize", 0.075, 0.025, 0.20
        ),
        pixel_stride=_number(isolation_value, "PixelStride", 3, 1, 4),
        minimum_component_points=_number(
            isolation_value, "MinimumComponentPoints", 10, 2, 100
        ),
        minimum_person_points=_number(
            isolation_value, "MinimumPersonPoints", 500, 100, 10000
        ),
        cluster_offset_m=_number(
            isolation_value, "ClusterOffset", 0.2, 0.05, 0.5
        ),
        minimum_person_height_m=_number(
            isolation_value, "MinimumPersonHeight", 0.65, 0.2, 1.5
        ),
        minimum_acquisition_height_m=_number(
            isolation_value, "MinimumAcquisitionHeight", 1.15, 0.5, 2.0
        ),
        maximum_person_height_m=_number(
            isolation_value, "MaximumPersonHeight", 2.50, 1.5, 3.0
        ),
        maximum_person_width_m=_number(
            isolation_value, "MaximumPersonWidth", 2.50, 0.5, 4.0
        ),
        maximum_person_depth_m=_number(
            isolation_value, "MaximumPersonDepth", 1.80, 0.3, 3.0
        ),
        background_calibration_frames=_number(
            isolation_value, "BackgroundCalibrationFrames", 45, 0, 120
        ),
        background_margin_m=_number(
            isolation_value, "BackgroundMargin", 0.10, 0.01, 0.30
        ),
        dropout_hold_ms=_number(
            isolation_value, "DropoutHoldMs", 100, 0, 250
        ),
    )
    if isolation.minimum_x_m >= isolation.maximum_x_m:
        raise ValueError("MinimumX must be below MaximumX")
    if isolation.minimum_y_m >= isolation.maximum_y_m:
        raise ValueError("MinimumY must be below MaximumY")
    if isolation.minimum_z_m >= isolation.maximum_z_m:
        raise ValueError("MinimumZ must be below MaximumZ")
    if isolation.minimum_person_height_m >= isolation.maximum_person_height_m:
        raise ValueError(
            "MinimumPersonHeight must be below MaximumPersonHeight"
        )
    if isolation.minimum_acquisition_height_m < isolation.minimum_person_height_m:
        raise ValueError(
            "MinimumAcquisitionHeight must not be below MinimumPersonHeight"
        )

    configured_primary = _number(
        fusion_value, "PrimaryCamera", 0, 0, 1
    )
    primary_device = _number(
        placement_value, "PrimaryDevice", configured_primary, 0, 1
    )
    primary_serial = placement_value.get("PrimarySerial", "")
    if not isinstance(primary_serial, str):
        raise ValueError("D4xxPlacement.PrimarySerial must be a string")
    fusion = FusionOptions(
        mode=str(fusion_value.get("Mode", "primary")).lower(),
        primary_camera=primary_device,
        agreement_distance_m=_number(
            fusion_value, "AgreementDistance", 0.22, 0.05, 1.0
        ),
        severe_disagreement_m=_number(
            fusion_value, "SevereDisagreement", 0.45, 0.1, 2.0
        ),
        tracked_confidence=_number(
            fusion_value, "TrackedConfidence", 0.55, 0.1, 1.0
        ),
        inferred_confidence=_number(
            fusion_value, "InferredConfidence", 0.15, 0.0, 0.9
        ),
    )
    if fusion.mode not in ("primary", "calibrated"):
        raise ValueError("Fusion.Mode must be primary or calibrated")
    if fusion.inferred_confidence >= fusion.tracked_confidence:
        raise ValueError("InferredConfidence must be below TrackedConfidence")
    if fusion.agreement_distance_m >= fusion.severe_disagreement_m:
        raise ValueError(
            "AgreementDistance must be below SevereDisagreement"
        )

    camera_values = fusion_value.get("Cameras", [{}, {}])
    if not isinstance(camera_values, list) or len(camera_values) != 2:
        raise ValueError("Fusion.Cameras must contain exactly two objects")
    transforms = []
    serials = []
    identity = np_identity_matrix()
    for index, camera in enumerate(camera_values):
        if not isinstance(camera, dict):
            raise ValueError(f"Fusion.Cameras[{index}] must be an object")
        serial = camera.get("Serial", "")
        if not isinstance(serial, str):
            raise ValueError(f"Fusion.Cameras[{index}].Serial must be a string")
        matrix = camera.get("CameraToStage", identity)
        transforms.append(RigidTransform.from_matrix(matrix))
        serials.append(serial)
    if fusion.mode == "calibrated":
        if any(not value for value in serials):
            raise ValueError(
                "calibrated fusion requires two serial-numbered camera "
                "transforms"
            )
        if serials[0] == serials[1]:
            raise ValueError(
                "calibrated fusion camera serials must be distinct"
            )
    return WorkerConfig(
        runtime=runtime,
        isolation=isolation,
        fusion=fusion,
        transforms=(transforms[0], transforms[1]),
        camera_serials=(serials[0], serials[1]),
        primary_serial=primary_serial,
    )


def np_identity_matrix() -> tuple[float, ...]:
    return (
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    )
