"""Pose-quality metrics shared by offline D430 model evaluation tools."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .teacher_data import JOINT_NAMES


ENDPOINTS = (6, 7, 13, 14)
LEFT_RIGHT_PAIRS = ((2, 3), (4, 5), (6, 7), (9, 10), (11, 12), (13, 14))


@dataclass
class PreviousPose:
    frame: int
    prediction: np.ndarray
    target: np.ndarray
    mask: np.ndarray


class PoseMetrics:
    def __init__(self) -> None:
        self.samples = 0
        self.error_sum = 0.0
        self.joint_count = 0
        self.correct_10cm = 0
        self.per_joint_error = np.zeros(len(JOINT_NAMES), dtype=np.float64)
        self.per_joint_count = np.zeros(len(JOINT_NAMES), dtype=np.int64)
        self.endpoint_error_sum = 0.0
        self.endpoint_count = 0
        self.swap_count = 0
        self.swap_total = 0
        self.velocity_error_sum = 0.0
        self.velocity_count = 0
        self.previous: dict[tuple[str, int], PreviousPose] = {}

    def update(
        self,
        prediction: np.ndarray,
        target: np.ndarray,
        visibility: np.ndarray,
        minimum_visibility: float,
        stream: tuple[str, int],
        frame: int,
    ) -> None:
        predicted = np.asarray(prediction, dtype=np.float32)
        expected = np.asarray(target, dtype=np.float32)
        confidence = np.asarray(visibility, dtype=np.float32)
        if predicted.shape != (15, 3) or expected.shape != (15, 3) or (
            confidence.shape != (15,)
        ):
            raise ValueError("pose metric arrays have incompatible shapes")
        if not np.all(np.isfinite(predicted)) or not np.all(
            np.isfinite(expected)
        ) or not np.all(np.isfinite(confidence)):
            raise ValueError("pose metric arrays contain non-finite values")
        if np.any((confidence < 0.0) | (confidence > 1.0)):
            raise ValueError("pose visibility is outside 0..1")
        mask = confidence >= minimum_visibility
        distance = np.linalg.norm(predicted - expected, axis=1)
        self.samples += 1
        self.error_sum += float(distance[mask].sum())
        self.joint_count += int(mask.sum())
        self.correct_10cm += int(np.count_nonzero((distance < 0.10) & mask))
        self.per_joint_error += np.where(mask, distance, 0.0)
        self.per_joint_count += mask.astype(np.int64)

        endpoint_mask = mask[list(ENDPOINTS)]
        self.endpoint_error_sum += float(
            distance[list(ENDPOINTS)][endpoint_mask].sum()
        )
        self.endpoint_count += int(endpoint_mask.sum())
        for left, right in LEFT_RIGHT_PAIRS:
            if not mask[left] or not mask[right]:
                continue
            correct = np.linalg.norm(predicted[left] - expected[left]) + (
                np.linalg.norm(predicted[right] - expected[right])
            )
            swapped = np.linalg.norm(predicted[left] - expected[right]) + (
                np.linalg.norm(predicted[right] - expected[left])
            )
            self.swap_count += int(swapped < correct)
            self.swap_total += 1

        previous = self.previous.get(stream)
        if previous is not None:
            if frame <= previous.frame:
                raise ValueError("teacher device frames are not increasing")
            if frame == previous.frame + 1:
                temporal_mask = mask & previous.mask
                error = np.linalg.norm(
                    (predicted - previous.prediction)
                    - (expected - previous.target),
                    axis=1,
                )
                self.velocity_error_sum += float(error[temporal_mask].sum())
                self.velocity_count += int(temporal_mask.sum())
        self.previous[stream] = PreviousPose(
            frame=frame,
            prediction=predicted.copy(),
            target=expected.copy(),
            mask=mask.copy(),
        )

    def result(self) -> dict:
        per_joint = {}
        for index, name in enumerate(JOINT_NAMES):
            count = int(self.per_joint_count[index])
            per_joint[name] = {
                "mean_error_m": (
                    float(self.per_joint_error[index] / count)
                    if count else None
                ),
                "tracked_count": count,
            }
        return {
            "samples": self.samples,
            "tracked_joint_count": self.joint_count,
            "mpjpe_m": (
                self.error_sum / self.joint_count
                if self.joint_count else None
            ),
            "pck_10cm": (
                self.correct_10cm / self.joint_count
                if self.joint_count else None
            ),
            "endpoint_error_m": (
                self.endpoint_error_sum / self.endpoint_count
                if self.endpoint_count else None
            ),
            "left_right_swap_fraction": (
                self.swap_count / self.swap_total if self.swap_total else None
            ),
            "left_right_swap_count": self.swap_count,
            "velocity_error_m_per_frame": (
                self.velocity_error_sum / self.velocity_count
                if self.velocity_count else None
            ),
            "velocity_joint_count": self.velocity_count,
            "per_joint": per_joint,
        }
