from __future__ import annotations

import json

import pytest

from anygear_spike.config import load_config


def test_default_config_uses_primary_camera(tmp_path) -> None:
    config = load_config(tmp_path / "absent.json")
    assert config.runtime.inference_fps == 30
    assert config.runtime.point_count == 4096
    assert config.runtime.frames_per_clip == 3
    assert config.runtime.directml_device_index == 0
    assert config.runtime.warmup_runs == 3
    assert config.runtime.secondary_every_n == 3
    assert config.isolation.pixel_stride == 3
    assert config.isolation.background_calibration_frames == 45
    assert config.isolation.background_margin_m == pytest.approx(0.10)
    assert config.fusion.mode == "primary"
    assert config.fusion.primary_camera == 0
    assert config.primary_serial == ""


def test_d4xx_placement_selects_primary_camera(tmp_path) -> None:
    path = tmp_path / "spike.json"
    path.write_text(json.dumps({
        "D4xxPlacement": {
            "PrimaryDevice": 1,
            "PrimarySerial": "camera-b",
        },
        "Fusion": {"PrimaryCamera": 0},
    }))
    config = load_config(path)
    assert config.fusion.primary_camera == 1
    assert config.primary_serial == "camera-b"


def test_calibrated_mode_requires_serials(tmp_path) -> None:
    path = tmp_path / "spike.json"
    path.write_text(json.dumps({"Fusion": {"Mode": "calibrated"}}))
    with pytest.raises(ValueError, match="serial-numbered"):
        load_config(path)


def test_calibrated_mode_requires_both_distinct_serials(tmp_path) -> None:
    path = tmp_path / "spike.json"
    path.write_text(json.dumps({
        "Fusion": {
            "Mode": "calibrated",
            "Cameras": [{"Serial": "camera-a"}, {}],
        }
    }))
    with pytest.raises(ValueError, match="two serial-numbered"):
        load_config(path)

    path.write_text(json.dumps({
        "Fusion": {
            "Mode": "calibrated",
            "Cameras": [
                {"Serial": "camera-a"},
                {"Serial": "camera-a"},
            ],
        }
    }))
    with pytest.raises(ValueError, match="distinct"):
        load_config(path)


def test_non_rigid_camera_matrix_is_rejected(tmp_path) -> None:
    path = tmp_path / "spike.json"
    path.write_text(json.dumps({
        "Fusion": {
            "Cameras": [
                {"CameraToStage": [2, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 1, 0, 0, 0, 0, 1]},
                {},
            ]
        }
    }))
    with pytest.raises(ValueError, match="orthonormal"):
        load_config(path)
