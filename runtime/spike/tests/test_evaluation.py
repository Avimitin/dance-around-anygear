from __future__ import annotations

import numpy as np

from anygear_spike.evaluation import PoseMetrics


def test_pose_metrics_measure_identity_and_velocity_errors() -> None:
    target = np.zeros((15, 3), dtype=np.float32)
    target[:, 1] = np.linspace(0.0, 1.4, 15)
    target[2, 0], target[3, 0] = -0.3, 0.3
    target[6, 0], target[7, 0] = -0.6, 0.6
    target[13, 0], target[14, 0] = -0.2, 0.2
    visibility = np.ones(15, dtype=np.float32)
    metrics = PoseMetrics()
    metrics.update(target, target, visibility, 0.99, ("session", 0), 10)
    moved = target.copy()
    moved[:, 1] += 0.05
    prediction = moved.copy()
    prediction[6], prediction[7] = moved[7].copy(), moved[6].copy()
    metrics.update(prediction, moved, visibility, 0.99, ("session", 0), 11)
    result = metrics.result()
    assert result["samples"] == 2
    assert result["mpjpe_m"] > 0.0
    assert result["left_right_swap_count"] == 1
    assert result["velocity_error_m_per_frame"] > 0.0


def test_pose_metrics_do_not_bridge_missing_frames() -> None:
    pose = np.zeros((15, 3), dtype=np.float32)
    visibility = np.ones(15, dtype=np.float32)
    metrics = PoseMetrics()
    metrics.update(pose, pose, visibility, 0.99, ("session", 0), 1)
    metrics.update(pose, pose, visibility, 0.99, ("session", 0), 3)
    assert metrics.result()["velocity_joint_count"] == 0
