"""Depth-only SPiKE worker launched automatically by the Anygear DLL."""

from __future__ import annotations

import argparse
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
import logging
import os
from pathlib import Path
import sys
import time

import numpy as np

from .config import WorkerConfig, load_config
from .depth import estimate_background, isolate_player, sample_points
from .fusion import FusionResult, RigidTransform, fuse_views
from .model_runtime import RealtimeSpikeModel
from .protocol import (
    CAMERA_COUNT,
    JOINT_COUNT,
    IpcChannel,
    PoseSnapshot,
    WorkerState,
)
from .temporal import realtime_clip_frame_is_contiguous


POSE_FUSED_VALID = 1 << 0
POSE_CAMERA_VALID = (1 << 1, 1 << 2)
MAXIMUM_INPUT_AGE_NS = 150_000_000
CALIBRATION_STARTUP_TIMEOUT_NS = 10_000_000_000
CALIBRATION_STALL_NS = 2_000_000_000
INPUT_WAIT_TIMEOUT_MS = 20
TELEMETRY_WINDOW_OUTPUTS = 300
SOURCE_CADENCE_TOLERANCE_RATIO = 0.25


class RuntimeTelemetry:
    def __init__(
        self,
        window_outputs: int = TELEMETRY_WINDOW_OUTPUTS,
        started_ns: int | None = None,
    ) -> None:
        if window_outputs <= 0:
            raise ValueError("telemetry window must be positive")
        self.window_outputs = window_outputs
        self.started_ns = (
            time.monotonic_ns() if started_ns is None else started_ns
        )
        self.pipeline_us: list[int] = []
        self.inference_us: list[int] = []
        self.source_age_us: list[int] = []
        self.valid_outputs = 0

    @staticmethod
    def _percentiles_ms(values: list[int]) -> tuple[float, float]:
        if not values:
            return float("nan"), float("nan")
        result = np.percentile(
            np.asarray(values, dtype=np.float64), (50.0, 95.0)
        ) / 1000.0
        return float(result[0]), float(result[1])

    def record(
        self,
        pipeline_us: int,
        source_time_ns: int,
        valid: bool,
        inference_us: int | None = None,
        now_ns: int | None = None,
    ) -> dict[str, float] | None:
        now = time.monotonic_ns() if now_ns is None else now_ns
        self.pipeline_us.append(max(0, int(pipeline_us)))
        if inference_us is not None:
            self.inference_us.append(max(0, int(inference_us)))
        if source_time_ns > 0 and now >= source_time_ns:
            self.source_age_us.append((now - source_time_ns) // 1000)
        self.valid_outputs += int(valid)
        if len(self.pipeline_us) < self.window_outputs:
            return None

        duration_seconds = max((now - self.started_ns) / 1e9, 1e-9)
        pipeline_p50, pipeline_p95 = self._percentiles_ms(self.pipeline_us)
        inference_p50, inference_p95 = self._percentiles_ms(self.inference_us)
        source_p50, source_p95 = self._percentiles_ms(self.source_age_us)
        summary = {
            "output_hz": len(self.pipeline_us) / duration_seconds,
            "pose_percent": 100.0 * self.valid_outputs /
                len(self.pipeline_us),
            "pipeline_p50_ms": pipeline_p50,
            "pipeline_p95_ms": pipeline_p95,
            "inference_p50_ms": inference_p50,
            "inference_p95_ms": inference_p95,
            "source_age_p50_ms": source_p50,
            "source_age_p95_ms": source_p95,
        }
        self.started_ns = now
        self.pipeline_us.clear()
        self.inference_us.clear()
        self.source_age_us.clear()
        self.valid_outputs = 0
        return summary


def log_telemetry(summary: dict[str, float] | None) -> None:
    if summary is None:
        return
    logging.info(
        "cadence: output=%.1fHz pose=%.1f%% pipeline=%.2f/%.2fms "
        "inference=%.2f/%.2fms source-age=%.2f/%.2fms (P50/P95)",
        summary["output_hz"],
        summary["pose_percent"],
        summary["pipeline_p50_ms"],
        summary["pipeline_p95_ms"],
        summary["inference_p50_ms"],
        summary["inference_p95_ms"],
        summary["source_age_p50_ms"],
        summary["source_age_p95_ms"],
    )


def frame_is_fresh(last_frame_ns: int, now_ns: int) -> bool:
    return last_frame_ns > 0 and 0 <= now_ns - last_frame_ns <= (
        MAXIMUM_INPUT_AGE_NS
    )


def candidate_dropout_is_bridgeable(
    last_candidate_ns: int,
    now_ns: int,
    hold_ms: int,
) -> bool:
    """Keep a complete temporal clip across one short isolation miss."""
    elapsed_ns = now_ns - last_candidate_ns
    return (
        last_candidate_ns > 0
        and hold_ms > 0
        and 0 <= elapsed_ns <= hold_ms * 1_000_000
    )


def source_sample_is_due(
    candidate_time_ns: int,
    prediction_time_ns: int,
    minimum_interval_ns: int,
) -> bool:
    """Limit inference by source time without phase-locking to completion."""
    if candidate_time_ns <= 0:
        return False
    if prediction_time_ns <= 0:
        return True
    elapsed_ns = candidate_time_ns - prediction_time_ns
    if elapsed_ns <= 0:
        return False
    tolerance_ns = int(
        max(1_000_000, minimum_interval_ns * SOURCE_CADENCE_TOLERANCE_RATIO)
    )
    return elapsed_ns >= max(1, minimum_interval_ns - tolerance_ns)


def select_inference_indices(
    camera_ready: list[bool],
    prediction_valid: list[bool],
    prediction_sequence: list[int],
    source_sequence: list[int],
    primary: int,
    fusion_mode: str,
    output_count: int,
    secondary_every_n: int,
) -> list[int]:
    """Choose at most one fresh clip for the fixed batch-1 model."""
    secondary = 1 - primary
    needs_inference = [
        camera_ready[index]
        and (
            not prediction_valid[index]
            or prediction_sequence[index] != source_sequence[index]
        )
        for index in range(CAMERA_COUNT)
    ]
    if not camera_ready[primary]:
        return [secondary] if needs_inference[secondary] else []
    if fusion_mode != "calibrated" or not camera_ready[secondary]:
        return [primary] if needs_inference[primary] else []

    # Establish the primary pose first. Once it can be reused, a due
    # secondary refresh replaces (rather than follows) the primary inference
    # for this output. This keeps every iteration inside the batch-1 latency
    # envelope while the next primary frame repairs the one-output age.
    if not prediction_valid[primary] and needs_inference[primary]:
        return [primary]
    secondary_due = (
        not prediction_valid[secondary]
        or output_count % secondary_every_n == 0
    )
    if secondary_due and needs_inference[secondary]:
        return [secondary]
    return [primary] if needs_inference[primary] else []


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ipc-mapping", required=True)
    parser.add_argument("--ipc-input-event", required=True)
    parser.add_argument("--ipc-output-event", required=True)
    parser.add_argument("--ipc-parent-pid", required=True, type=int)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--runtime-config", type=Path)
    parser.add_argument("--log", type=Path)
    return parser.parse_args()


def configure_logging(path: Path | None) -> None:
    handlers: list[logging.Handler] = []
    if path is not None:
        path.parent.mkdir(parents=True, exist_ok=True)
        handlers.append(logging.FileHandler(path, encoding="utf-8"))
    else:
        handlers.append(logging.StreamHandler(sys.stderr))
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s.%(msecs)03d %(levelname)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=handlers,
    )


def resolve_transforms(
    config: WorkerConfig, serials: tuple[str, str]
) -> tuple[RigidTransform, RigidTransform]:
    if not any(config.camera_serials):
        return config.transforms
    by_serial = {
        serial: transform
        for serial, transform in zip(config.camera_serials, config.transforms)
        if serial
    }
    result = []
    for serial in serials:
        if serial not in by_serial:
            if config.fusion.mode == "calibrated":
                raise RuntimeError(
                    f"camera serial {serial!r} has no calibrated transform"
                )
            result.append(RigidTransform.identity())
        else:
            result.append(by_serial[serial])
    return result[0], result[1]


def resolve_primary_camera(
    config: WorkerConfig, serials: tuple[str, str]
) -> int:
    if not config.primary_serial:
        return config.fusion.primary_camera
    try:
        return serials.index(config.primary_serial)
    except ValueError as error:
        raise RuntimeError(
            f"primary camera serial {config.primary_serial!r} was not found"
        ) from error


def select_depth_processing_indices(
    fusion_mode: str, primary_camera: int
) -> tuple[int, ...]:
    """Return the cameras whose point clouds are needed for this mode."""
    if fusion_mode == "primary":
        return (primary_camera,)
    return tuple(range(CAMERA_COUNT))


def empty_pose(
    generation: int,
    source_time_ns: int,
    source_sequence: list[int],
    elapsed_us: int,
) -> PoseSnapshot:
    return PoseSnapshot(
        generation=generation,
        source_time_ns=source_time_ns,
        worker_time_us=elapsed_us,
        flags=0,
        fused_joints=np.zeros((JOINT_COUNT, 3), dtype=np.float32),
        fused_confidence=np.zeros(JOINT_COUNT, dtype=np.float32),
        fused_tracking_state=np.zeros(JOINT_COUNT, dtype=np.uint8),
        camera_joints=np.zeros(
            (CAMERA_COUNT, JOINT_COUNT, 3), dtype=np.float32
        ),
        camera_confidence=np.zeros(
            (CAMERA_COUNT, JOINT_COUNT), dtype=np.float32
        ),
        camera_centroid=np.zeros((CAMERA_COUNT, 3), dtype=np.float32),
        source_sequence=source_sequence,
    )


def pose_snapshot(
    generation: int,
    source_time_ns: int,
    source_sequence: list[int],
    elapsed_us: int,
    fusion: FusionResult,
    centroids: np.ndarray,
    camera_valid: list[bool],
) -> PoseSnapshot:
    flags = POSE_FUSED_VALID if fusion.valid else 0
    for index, valid in enumerate(camera_valid):
        if valid:
            flags |= POSE_CAMERA_VALID[index]
    return PoseSnapshot(
        generation=generation,
        source_time_ns=source_time_ns,
        worker_time_us=elapsed_us,
        flags=flags,
        fused_joints=fusion.joints,
        fused_confidence=fusion.confidence,
        fused_tracking_state=fusion.tracking_state,
        camera_joints=fusion.camera_joints,
        camera_confidence=fusion.camera_confidence,
        camera_centroid=centroids,
        source_sequence=source_sequence,
    )


def run(channel: IpcChannel, args: argparse.Namespace) -> None:
    config = load_config(args.runtime_config)
    channel.monitor_parent(args.ipc_parent_pid)
    channel.set_worker_process_id(os.getpid())
    channel.set_state(WorkerState.LOADING_MODEL)
    logging.info(
        "stage 1/4: loading SPiKE model (FP16, DirectML device=%d)",
        config.runtime.directml_device_index,
    )
    model = RealtimeSpikeModel(
        args.model,
        device_index=config.runtime.directml_device_index,
        warmup_runs=config.runtime.warmup_runs,
    )
    if model.frames_per_clip != config.runtime.frames_per_clip:
        raise RuntimeError("runtime clip length does not match the model")
    if model.point_count != config.runtime.point_count:
        raise RuntimeError("runtime point count does not match the model")
    logging.info(
        "stage 2/4: model ready (%d frames x %d points, fixed batch=1)",
        model.frames_per_clip,
        model.point_count,
    )

    histories = [
        deque(maxlen=model.frames_per_clip) for _ in range(CAMERA_COUNT)
    ]
    latest_points = [
        np.empty((0, 3), dtype=np.float32) for _ in range(CAMERA_COUNT)
    ]
    background_samples: list[list[np.ndarray]] = [
        [] for _ in range(CAMERA_COUNT)
    ]
    backgrounds: list[np.ndarray | None] = [None] * CAMERA_COUNT
    prediction_points = [
        np.empty((0, 3), dtype=np.float32) for _ in range(CAMERA_COUNT)
    ]
    predictions = np.zeros(
        (CAMERA_COUNT, JOINT_COUNT, 3), dtype=np.float32
    )
    prediction_centroids = np.zeros(
        (CAMERA_COUNT, 3), dtype=np.float32
    )
    prediction_sequence = [0] * CAMERA_COUNT
    prediction_time_ns = [0] * CAMERA_COUNT
    prediction_valid = [False] * CAMERA_COUNT
    latest_valid = [False] * CAMERA_COUNT
    logged_valid = [False] * CAMERA_COUNT
    source_sequence = [0] * CAMERA_COUNT
    source_time_ns = [0] * CAMERA_COUNT
    candidate_sequence = [0] * CAMERA_COUNT
    candidate_time_ns = [0] * CAMERA_COUNT
    last_frame_monotonic_ns = [0] * CAMERA_COUNT
    serials = [""] * CAMERA_COUNT
    transforms: tuple[RigidTransform, RigidTransform] | None = None
    fusion_options = config.fusion
    previous_joints = None
    previous_time_ns = None
    last_candidate_monotonic_ns = [0] * CAMERA_COUNT
    generation = 0
    output_count = 0
    minimum_interval_ns = int(1_000_000_000 / config.runtime.inference_fps)
    next_inference_ns = 0
    calibration_started_ns = time.monotonic_ns()
    telemetry = RuntimeTelemetry()
    reset_telemetry_after_calibration = (
        config.isolation.background_calibration_frames > 0
    )
    channel.set_state(WorkerState.READY)
    logging.info(
        "stage 3/4: IPC ready (stride=%d, voxel=%.3fm, fusion=%s, "
        "background=%d frames)",
        config.isolation.pixel_stride,
        config.isolation.voxel_size_m,
        config.fusion.mode,
        config.isolation.background_calibration_frames,
    )

    with ThreadPoolExecutor(
        max_workers=CAMERA_COUNT, thread_name_prefix="depth"
    ) as executor:
        while channel.parent_is_running() and not channel.shutdown_requested():
            channel.wait_for_input(INPUT_WAIT_TIMEOUT_MS)
            if channel.shutdown_requested():
                break
            started_ns = time.perf_counter_ns()
            frames = [
                channel.read_depth(index, source_sequence[index])
                for index in range(CAMERA_COUNT)
            ]
            updated = [frame is not None for frame in frames]
            if not any(updated):
                channel.heartbeat()
            input_now_ns = time.monotonic_ns()
            for index, frame in enumerate(frames):
                if frame is not None:
                    last_frame_monotonic_ns[index] = input_now_ns
            if input_now_ns - calibration_started_ns > (
                CALIBRATION_STARTUP_TIMEOUT_NS
            ):
                never_started = [
                    index for index, last_frame_ns in enumerate(
                        last_frame_monotonic_ns
                    )
                    if last_frame_ns == 0
                ]
                if never_started:
                    raise RuntimeError(
                        "depth input never started: camera "
                        + ", ".join(str(index) for index in never_started)
                    )
            calibration_frames = (
                config.isolation.background_calibration_frames
            )
            if calibration_frames > 0:
                calibration_ready = []
                for index, frame in enumerate(frames):
                    if frame is None or backgrounds[index] is not None:
                        continue
                    source_sequence[index] = frame.frame_sequence
                    source_time_ns[index] = frame.host_time_ns
                    serials[index] = frame.serial
                    background_samples[index].append(frame.depth)
                    sample_count = len(background_samples[index])
                    if sample_count == 1:
                        logging.info(
                            "camera %d background calibration started; "
                            "keep the stage empty",
                            index,
                        )
                    if sample_count >= calibration_frames:
                        calibration_ready.append(index)
                calibration_futures = {
                    index: executor.submit(
                        estimate_background, background_samples[index]
                    )
                    for index in calibration_ready
                }
                for index, future in calibration_futures.items():
                    sample_count = len(background_samples[index])
                    backgrounds[index] = future.result()
                    background_samples[index].clear()
                    logging.info(
                        "camera %d background calibration complete "
                        "(%d frames)",
                        index, sample_count,
                    )
                calibrating = any(
                    background is None for background in backgrounds
                )
                if calibrating:
                    calibration_now_ns = time.monotonic_ns()
                    if calibration_now_ns - calibration_started_ns > (
                        CALIBRATION_STARTUP_TIMEOUT_NS
                    ):
                        stalled = [
                            index
                            for index, last_frame_ns in enumerate(
                                last_frame_monotonic_ns
                            )
                            if last_frame_ns == 0 or (
                                calibration_now_ns - last_frame_ns
                                > CALIBRATION_STALL_NS
                            )
                        ]
                        if stalled:
                            raise RuntimeError(
                                "depth input stalled during background "
                                "calibration: camera "
                                + ", ".join(str(index) for index in stalled)
                            )
                    now_ns = time.perf_counter_ns()
                    if now_ns >= next_inference_ns:
                        next_inference_ns = now_ns + minimum_interval_ns
                        generation += 1
                        elapsed_us = (
                            time.perf_counter_ns() - started_ns
                        ) // 1000
                        channel.publish_pose(empty_pose(
                            generation,
                            max(source_time_ns),
                            source_sequence,
                            elapsed_us,
                        ))
                        if output_count == 0:
                            channel.set_state(WorkerState.RUNNING)
                            logging.info(
                                "stage 4/4: first result published "
                                "(%dus, calibrating background)",
                                elapsed_us,
                            )
                        output_count += 1
                        log_telemetry(telemetry.record(
                            elapsed_us,
                            max(source_time_ns),
                            False,
                        ))
                    continue
                if reset_telemetry_after_calibration:
                    telemetry = RuntimeTelemetry()
                    reset_telemetry_after_calibration = False
                    logging.info(
                        "realtime telemetry window started after background "
                        "calibration"
                    )
            for index, frame in enumerate(frames):
                if frame is not None and not serials[index]:
                    serials[index] = frame.serial
            if transforms is None and all(serials):
                transforms = resolve_transforms(
                    config, (serials[0], serials[1])
                )
                fusion_options = replace(
                    config.fusion,
                    primary_camera=resolve_primary_camera(
                        config, (serials[0], serials[1])
                    ),
                )
                logging.info(
                    "camera order: 0=%s, 1=%s, primary=%d",
                    serials[0], serials[1], fusion_options.primary_camera,
                )
            processing_indices = select_depth_processing_indices(
                config.fusion.mode, fusion_options.primary_camera
            )
            for index, frame in enumerate(frames):
                if frame is None or index in processing_indices:
                    continue
                # Keep the latest-frame cursor current without spending CPU on
                # a point cloud that primary-only fusion will never consume.
                source_sequence[index] = frame.frame_sequence
                source_time_ns[index] = frame.host_time_ns
                serials[index] = frame.serial
            isolation_now_ns = time.monotonic_ns()
            futures = {
                index: executor.submit(
                    isolate_player,
                    frame,
                    replace(
                        config.isolation,
                        minimum_person_height_m=(
                            config.isolation.minimum_person_height_m
                            if isolation_now_ns
                            - last_candidate_monotonic_ns[index]
                            <= 750_000_000
                            else config.isolation.minimum_acquisition_height_m
                        ),
                    ),
                    backgrounds[index],
                )
                for index, frame in enumerate(frames)
                if frame is not None and index in processing_indices
            }
            for index, future in futures.items():
                frame = frames[index]
                assert frame is not None
                points = future.result()
                previous_sequence = source_sequence[index]
                previous_time_ns = source_time_ns[index]
                continuity_lost = (
                    previous_sequence != 0
                    and frame.frame_sequence != previous_sequence
                    and not realtime_clip_frame_is_contiguous(
                        previous_sequence,
                        previous_time_ns,
                        frame.frame_sequence,
                        frame.host_time_ns,
                    )
                )
                if continuity_lost:
                    histories[index].clear()
                    prediction_valid[index] = False
                    prediction_time_ns[index] = 0
                    latest_points[index] = np.empty(
                        (0, 3), dtype=np.float32
                    )
                    latest_valid[index] = False
                    candidate_sequence[index] = 0
                    candidate_time_ns[index] = 0
                    last_candidate_monotonic_ns[index] = 0
                    logging.warning(
                        "camera %d depth continuity lost: sequence %d -> %d, "
                        "gap=%.1fms; rebuilding temporal clip",
                        index,
                        previous_sequence,
                        frame.frame_sequence,
                        (frame.host_time_ns - previous_time_ns) / 1e6,
                    )
                source_sequence[index] = frame.frame_sequence
                source_time_ns[index] = frame.host_time_ns
                serials[index] = frame.serial
                measured_valid = len(points) >= (
                    config.isolation.minimum_person_points
                )
                if measured_valid:
                    latest_points[index] = points
                    latest_valid[index] = True
                    candidate_sequence[index] = frame.frame_sequence
                    candidate_time_ns[index] = frame.host_time_ns
                    last_candidate_monotonic_ns[index] = time.monotonic_ns()
                    histories[index].append(
                        sample_points(points, model.point_count)
                    )
                elif not (
                    latest_valid[index]
                    and len(histories[index]) > 0
                    and candidate_dropout_is_bridgeable(
                        last_candidate_monotonic_ns[index],
                        isolation_now_ns,
                        config.isolation.dropout_hold_ms,
                    )
                ):
                    latest_points[index] = points
                    latest_valid[index] = False
                    histories[index].clear()
                    prediction_valid[index] = False
                    prediction_time_ns[index] = 0

                if latest_valid[index] != logged_valid[index]:
                    if latest_valid[index]:
                        extent = points.max(axis=0) - points.min(axis=0)
                        centroid = points.mean(axis=0)
                        logging.info(
                            "camera %d player candidate: points=%d "
                            "extent=(%.2f,%.2f,%.2f)m "
                            "centroid=(%.2f,%.2f,%.2f)m",
                            index, len(points), *extent, *centroid,
                        )
                    else:
                        logging.info("camera %d player candidate lost", index)
                    logged_valid[index] = latest_valid[index]

            freshness_now_ns = time.monotonic_ns()
            input_fresh = [
                frame_is_fresh(last_frame_monotonic_ns[index], freshness_now_ns)
                for index in range(CAMERA_COUNT)
            ]
            for index, fresh in enumerate(input_fresh):
                if not fresh and latest_valid[index]:
                    latest_valid[index] = False
                    latest_points[index] = np.empty(
                        (0, 3), dtype=np.float32
                    )
                    histories[index].clear()
                    prediction_valid[index] = False
                    prediction_time_ns[index] = 0
                    candidate_sequence[index] = 0
                    candidate_time_ns[index] = 0
                    last_candidate_monotonic_ns[index] = 0
                    if logged_valid[index]:
                        logging.info(
                            "camera %d player candidate lost (depth input stale)",
                            index,
                        )
                        logged_valid[index] = False
            camera_ready = [
                input_fresh[index]
                and latest_valid[index]
                and len(histories[index]) == model.frames_per_clip
                for index in range(CAMERA_COUNT)
            ]
            generation += 1
            newest_source_time_ns = max(
                source_time_ns[index] for index in processing_indices
            )
            if not any(camera_ready):
                now_ns = time.perf_counter_ns()
                if now_ns < next_inference_ns:
                    continue
                next_inference_ns = now_ns + minimum_interval_ns
                elapsed_us = (time.perf_counter_ns() - started_ns) // 1000
                channel.publish_pose(empty_pose(
                    generation, newest_source_time_ns,
                    source_sequence, elapsed_us
                ))
                if output_count == 0:
                    channel.set_state(WorkerState.RUNNING)
                    logging.info(
                        "stage 4/4: first result published (%dus, valid=False)",
                        elapsed_us,
                    )
                output_count += 1
                log_telemetry(telemetry.record(
                    elapsed_us, newest_source_time_ns, False
                ))
                continue

            newest_time_ns = max(
                candidate_time_ns[index] for index in processing_indices
            )

            primary = fusion_options.primary_camera
            secondary = 1 - primary
            inference_indices = select_inference_indices(
                camera_ready,
                prediction_valid,
                prediction_sequence,
                candidate_sequence,
                primary,
                config.fusion.mode,
                output_count,
                config.runtime.secondary_every_n,
            )
            inference_indices = [
                index for index in inference_indices
                if source_sample_is_due(
                    candidate_time_ns[index],
                    prediction_time_ns[index],
                    minimum_interval_ns,
                )
            ]
            if not inference_indices:
                continue
            clips = []
            current_centroids = []
            for index in inference_indices:
                clip = np.stack(histories[index]).astype(
                    np.float32, copy=True
                )
                centroid = clip.reshape(-1, 3).mean(axis=0)
                clip -= centroid.reshape(1, 1, 3)
                clips.append(clip)
                current_centroids.append(centroid)
            inference_us = None
            if clips:
                inference_started_ns = time.perf_counter_ns()
                inferred = model.infer(np.stack(clips))
                inference_us = (
                    time.perf_counter_ns() - inference_started_ns
                ) // 1000
                for result_index, camera_index in enumerate(inference_indices):
                    centroid = current_centroids[result_index]
                    predictions[camera_index] = (
                        inferred[result_index] + centroid[None, :]
                    )
                    prediction_centroids[camera_index] = centroid
                    prediction_points[camera_index] = latest_points[camera_index]
                    prediction_sequence[camera_index] = source_sequence[
                        camera_index
                    ]
                    prediction_time_ns[camera_index] = candidate_time_ns[
                        camera_index
                    ]
                    prediction_valid[camera_index] = True
            maximum_prediction_age = max(
                2, config.runtime.secondary_every_n * 2
            )
            camera_valid = [
                prediction_valid[index]
                and input_fresh[index]
                and latest_valid[index]
                and candidate_sequence[index] - prediction_sequence[index]
                    <= maximum_prediction_age
                for index in range(CAMERA_COUNT)
            ]
            active_transforms = transforms or config.transforms
            elapsed_seconds = None
            if previous_time_ns is not None and newest_time_ns > previous_time_ns:
                elapsed_seconds = (newest_time_ns - previous_time_ns) / 1e9
            fusion = fuse_views(
                predictions,
                prediction_points,
                camera_valid,
                active_transforms,
                fusion_options,
                previous_joints=previous_joints,
                elapsed_seconds=elapsed_seconds,
            )
            elapsed_us = (time.perf_counter_ns() - started_ns) // 1000
            channel.publish_pose(pose_snapshot(
                generation,
                newest_time_ns,
                source_sequence,
                elapsed_us,
                fusion,
                prediction_centroids,
                camera_valid,
            ))
            output_count += 1
            log_telemetry(telemetry.record(
                elapsed_us,
                newest_time_ns,
                fusion.valid,
                inference_us=inference_us,
            ))
            if fusion.valid:
                previous_joints = fusion.joints.copy()
                previous_time_ns = newest_time_ns
            if output_count == 1:
                channel.set_state(WorkerState.RUNNING)
                logging.info(
                    "stage 4/4: first pose published (%dus, valid=%s)",
                    elapsed_us,
                    fusion.valid,
                )
    if not channel.parent_is_running():
        logging.info("host process exited; worker is stopping")


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return 0
    args = parse_arguments()
    configure_logging(args.log)
    channel = None
    try:
        channel = IpcChannel(
            args.ipc_mapping,
            args.ipc_input_event,
            args.ipc_output_event,
        )
        run(channel, args)
        channel.set_state(WorkerState.STOPPING)
        channel.set_state(WorkerState.STOPPED)
        logging.info("worker stopped")
        return 0
    except Exception as error:
        logging.exception("worker failed")
        if channel is not None:
            channel.report_fault(f"{type(error).__name__}: {error}")
        return 1
    finally:
        if channel is not None:
            channel.close()


if __name__ == "__main__":
    raise SystemExit(main())
