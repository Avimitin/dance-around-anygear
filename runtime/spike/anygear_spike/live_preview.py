"""Loopback web viewer for live D4xx isolation and SPiKE inference."""

from __future__ import annotations

import argparse
import base64
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
import os
from pathlib import Path
import sys
import threading
import time
from typing import Any
from urllib.parse import urlparse

import numpy as np

from .config import load_config
from .depth import estimate_background, isolate_player, sample_points
from .fusion import kinematic_confidence, point_support_confidence
from .model_runtime import RealtimeSpikeModel
from .protocol import CAMERA_COUNT, JOINT_COUNT, DepthSnapshot, IpcChannel, WorkerState
from .temporal import MAXIMUM_CLIP_FRAME_GAP_NS
from .worker import configure_logging, resolve_primary_camera


MODES = ("camera-0", "camera-1", "dual")
INPUT_WAIT_TIMEOUT_MS = 20
MAXIMUM_PREVIEW_POINTS = 2400
DEPTH_PREVIEW_STRIDE = 8


class ViewerControl:
    def __init__(self, mode: str = "dual") -> None:
        self._lock = threading.Lock()
        self._mode = mode
        self._recalibration_revision = 0
        self.stop_event = threading.Event()

    def mode(self) -> str:
        with self._lock:
            return self._mode

    def set_mode(self, mode: str) -> None:
        if mode not in MODES:
            raise ValueError(f"unsupported inference mode: {mode}")
        with self._lock:
            self._mode = mode

    def request_recalibration(self) -> None:
        with self._lock:
            self._recalibration_revision += 1

    def recalibration_revision(self) -> int:
        with self._lock:
            return self._recalibration_revision


class PreviewStore:
    def __init__(self, control: ViewerControl) -> None:
        self._lock = threading.Lock()
        self._control = control
        self._document: dict[str, Any] = {
            "schema": "dance-around-anygear.d4xx-spike-live-preview.v1",
            "updated_ms": 0,
            "stage": "starting",
            "message": "Loading SPiKE model",
            "mode": control.mode(),
            "primary_camera": 0,
            "inference_ms": None,
            "inference_hz": 0.0,
            "cameras": [empty_camera(index) for index in range(CAMERA_COUNT)],
        }

    def update(self, **values: Any) -> None:
        with self._lock:
            self._document.update(values)
            self._document["updated_ms"] = int(time.time() * 1000)
            self._document["mode"] = self._control.mode()

    def camera(self, index: int, value: dict[str, Any]) -> None:
        with self._lock:
            cameras = self._document["cameras"].copy()
            cameras[index] = value
            self._document["cameras"] = cameras
            self._document["updated_ms"] = int(time.time() * 1000)
            self._document["mode"] = self._control.mode()

    def snapshot(self) -> bytes:
        with self._lock:
            value = dict(self._document)
            value["mode"] = self._control.mode()
        return json.dumps(
            value, ensure_ascii=False, separators=(",", ":"), allow_nan=False
        ).encode("utf-8")


def empty_camera(index: int) -> dict[str, Any]:
    return {
        "index": index,
        "serial": "",
        "sequence": 0,
        "capture_fps": 0.0,
        "age_ms": None,
        "skipped_frames": 0,
        "calibration_frames": 0,
        "calibration_target": 0,
        "depth": None,
        "player": {
            "valid": False,
            "point_count": 0,
            "extent_m": [0.0, 0.0, 0.0],
            "centroid_m": [0.0, 0.0, 0.0],
            "points_mm_b64": "",
        },
        "pose": {
            "valid": False,
            "sequence": 0,
            "joints_m": [],
            "confidence": [],
            "inference_ms": None,
        },
    }


class PreviewHttpServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        html: bytes,
        store: PreviewStore,
        control: ViewerControl,
    ) -> None:
        super().__init__(address, PreviewRequestHandler)
        self.html = html
        self.store = store
        self.control = control


class PreviewRequestHandler(BaseHTTPRequestHandler):
    server: PreviewHttpServer

    def log_message(self, format: str, *args: Any) -> None:
        logging.debug("viewer HTTP: " + format, *args)

    def _send(self, status: HTTPStatus, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        path = urlparse(self.path).path
        if path == "/":
            self._send(HTTPStatus.OK, "text/html; charset=utf-8", self.server.html)
        elif path == "/api/state":
            self._send(
                HTTPStatus.OK,
                "application/json; charset=utf-8",
                self.server.store.snapshot(),
            )
        else:
            self._send(HTTPStatus.NOT_FOUND, "text/plain; charset=utf-8", b"not found")

    def _read_json(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise ValueError("invalid Content-Length") from error
        if length < 0 or length > 4096:
            raise ValueError("request body is too large")
        if length == 0:
            return {}
        value = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("request body must be an object")
        return value

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        path = urlparse(self.path).path
        try:
            value = self._read_json()
            if path == "/api/mode":
                mode = value.get("mode")
                if not isinstance(mode, str):
                    raise ValueError("mode must be a string")
                self.server.control.set_mode(mode)
            elif path == "/api/recalibrate":
                self.server.control.request_recalibration()
            elif path == "/api/stop":
                self.server.control.stop_event.set()
            else:
                self._send(
                    HTTPStatus.NOT_FOUND,
                    "application/json; charset=utf-8",
                    b'{"ok":false,"error":"not found"}',
                )
                return
            self._send(
                HTTPStatus.OK,
                "application/json; charset=utf-8",
                b'{"ok":true}',
            )
        except (ValueError, json.JSONDecodeError) as error:
            body = json.dumps(
                {"ok": False, "error": str(error)}, separators=(",", ":")
            ).encode("utf-8")
            self._send(
                HTTPStatus.BAD_REQUEST,
                "application/json; charset=utf-8",
                body,
            )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ipc-mapping", required=True)
    parser.add_argument("--ipc-input-event", required=True)
    parser.add_argument("--ipc-output-event", required=True)
    parser.add_argument("--ipc-parent-pid", required=True, type=int)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--runtime-config", required=True, type=Path)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("--port must be between 1024 and 65535")
    return args


def encode_depth(frame: DepthSnapshot) -> dict[str, Any]:
    sampled = np.ascontiguousarray(
        frame.depth[::DEPTH_PREVIEW_STRIDE, ::DEPTH_PREVIEW_STRIDE],
        dtype="<u2",
    )
    return {
        "width": int(sampled.shape[1]),
        "height": int(sampled.shape[0]),
        "source_width": frame.width,
        "source_height": frame.height,
        "depth_scale_m": round(frame.depth_scale_m, 8),
        "principal_x": round(frame.principal_x, 4),
        "principal_y": round(frame.principal_y, 4),
        "focal_x": round(frame.focal_x, 4),
        "focal_y": round(frame.focal_y, 4),
        "z16_b64": base64.b64encode(sampled.tobytes()).decode("ascii"),
    }


def encode_points(points: np.ndarray) -> str:
    value = np.asarray(points, dtype=np.float32)
    if len(value) > MAXIMUM_PREVIEW_POINTS:
        indices = np.linspace(
            0, len(value) - 1, MAXIMUM_PREVIEW_POINTS, dtype=np.int64
        )
        value = value[indices]
    millimetres = np.clip(
        np.rint(value * 1000.0), -32768, 32767
    ).astype("<i2", copy=False)
    return base64.b64encode(millimetres.tobytes()).decode("ascii")


def camera_payload(
    index: int,
    frame: DepthSnapshot | None,
    points: np.ndarray,
    joints: np.ndarray,
    confidence: np.ndarray,
    pose_valid: bool,
    pose_sequence: int,
    inference_ms: float | None,
    capture_fps: float,
    arrival_ns: int,
    skipped_frames: int,
    calibration_frames: int,
    calibration_target: int,
) -> dict[str, Any]:
    result = empty_camera(index)
    result.update({
        "serial": frame.serial if frame is not None else "",
        "sequence": frame.frame_sequence if frame is not None else 0,
        "capture_fps": round(capture_fps, 1),
        "age_ms": (
            round(max(0, time.monotonic_ns() - arrival_ns) / 1e6, 1)
            if arrival_ns else None
        ),
        "skipped_frames": skipped_frames,
        "calibration_frames": calibration_frames,
        "calibration_target": calibration_target,
        "depth": encode_depth(frame) if frame is not None else None,
    })
    if len(points):
        extent = points.max(axis=0) - points.min(axis=0)
        centroid = points.mean(axis=0)
        result["player"] = {
            "valid": True,
            "point_count": int(len(points)),
            "extent_m": np.round(extent, 3).tolist(),
            "centroid_m": np.round(centroid, 3).tolist(),
            "points_mm_b64": encode_points(points),
        }
    if pose_valid:
        result["pose"] = {
            "valid": True,
            "sequence": pose_sequence,
            "joints_m": np.round(joints, 4).tolist(),
            "confidence": np.round(confidence, 3).tolist(),
            "inference_ms": (
                round(inference_ms, 2) if inference_ms is not None else None
            ),
        }
    return result


def capture_rate(arrivals: deque[int]) -> float:
    if len(arrivals) < 2:
        return 0.0
    elapsed = (arrivals[-1] - arrivals[0]) / 1e9
    return (len(arrivals) - 1) / elapsed if elapsed > 0 else 0.0


def select_camera(mode: str, ready: list[bool], next_dual: int) -> tuple[int | None, int]:
    if mode == "camera-0":
        return (0 if ready[0] else None), next_dual
    if mode == "camera-1":
        return (1 if ready[1] else None), next_dual
    for offset in range(CAMERA_COUNT):
        candidate = (next_dual + offset) % CAMERA_COUNT
        if ready[candidate]:
            return candidate, 1 - candidate
    return None, next_dual


def reset_pipeline(
    histories: list[deque[np.ndarray]],
    background_samples: list[list[np.ndarray]],
    backgrounds: list[np.ndarray | None],
    points: list[np.ndarray],
    pose_valid: list[bool],
) -> None:
    for history in histories:
        history.clear()
    for samples in background_samples:
        samples.clear()
    for index in range(CAMERA_COUNT):
        backgrounds[index] = None
        points[index] = np.empty((0, 3), dtype=np.float32)
        pose_valid[index] = False


def run(channel: IpcChannel, args: argparse.Namespace) -> None:
    config = load_config(args.runtime_config)
    channel.monitor_parent(args.ipc_parent_pid)
    channel.set_worker_process_id(os.getpid())
    channel.set_state(WorkerState.LOADING_MODEL)
    logging.info(
        "viewer stage 1/3: loading SPiKE model (DirectML device=%d)",
        config.runtime.directml_device_index,
    )
    model = RealtimeSpikeModel(
        args.model,
        device_index=config.runtime.directml_device_index,
        warmup_runs=config.runtime.warmup_runs,
    )
    if model.frames_per_clip != config.runtime.frames_per_clip or (
        model.point_count != config.runtime.point_count
    ):
        raise RuntimeError("runtime config does not match the SPiKE model")

    html_path = Path(__file__).with_name("live_preview.html")
    html = html_path.read_bytes()
    control = ViewerControl("dual")
    store = PreviewStore(control)
    server = PreviewHttpServer(("127.0.0.1", args.port), html, store, control)
    server_thread = threading.Thread(
        target=server.serve_forever,
        name="preview-http",
        daemon=True,
    )
    server_thread.start()

    histories = [deque(maxlen=model.frames_per_clip) for _ in range(CAMERA_COUNT)]
    background_samples: list[list[np.ndarray]] = [
        [] for _ in range(CAMERA_COUNT)
    ]
    backgrounds: list[np.ndarray | None] = [None] * CAMERA_COUNT
    latest_frames: list[DepthSnapshot | None] = [None] * CAMERA_COUNT
    latest_points = [
        np.empty((0, 3), dtype=np.float32) for _ in range(CAMERA_COUNT)
    ]
    joints = np.zeros((CAMERA_COUNT, JOINT_COUNT, 3), dtype=np.float32)
    confidence = np.zeros((CAMERA_COUNT, JOINT_COUNT), dtype=np.float32)
    pose_valid = [False] * CAMERA_COUNT
    pose_sequence = [0] * CAMERA_COUNT
    inference_ms: list[float | None] = [None] * CAMERA_COUNT
    source_sequence = [0] * CAMERA_COUNT
    source_time_ns = [0] * CAMERA_COUNT
    arrival_ns = [0] * CAMERA_COUNT
    arrivals = [deque(maxlen=90) for _ in range(CAMERA_COUNT)]
    skipped_frames = [0] * CAMERA_COUNT
    last_candidate_ns = [0] * CAMERA_COUNT
    inference_arrivals: deque[int] = deque(maxlen=90)
    recalibration_revision = control.recalibration_revision()
    next_dual = config.fusion.primary_camera
    primary_camera = config.fusion.primary_camera

    store.update(
        stage="calibrating",
        message="Keep both camera views empty while the background is measured",
        primary_camera=primary_camera,
    )
    channel.set_state(WorkerState.READY)
    logging.info(
        "viewer stage 2/3: http://127.0.0.1:%d/ ready; keep stage empty",
        args.port,
    )

    with ThreadPoolExecutor(
        max_workers=CAMERA_COUNT, thread_name_prefix="preview-depth"
    ) as executor:
        try:
            while (
                channel.parent_is_running()
                and not channel.shutdown_requested()
                and not control.stop_event.is_set()
            ):
                channel.wait_for_input(INPUT_WAIT_TIMEOUT_MS)
                channel.heartbeat()

                current_revision = control.recalibration_revision()
                if current_revision != recalibration_revision:
                    recalibration_revision = current_revision
                    reset_pipeline(
                        histories,
                        background_samples,
                        backgrounds,
                        latest_points,
                        pose_valid,
                    )
                    store.update(
                        stage="calibrating",
                        message=(
                            "Recalibrating: leave both camera views empty"
                        ),
                    )
                    logging.info("viewer background recalibration requested")

                frames = [
                    channel.read_depth(index, source_sequence[index])
                    for index in range(CAMERA_COUNT)
                ]
                if not any(frame is not None for frame in frames):
                    continue
                now_ns = time.monotonic_ns()
                for index, frame in enumerate(frames):
                    if frame is None:
                        continue
                    previous_sequence = source_sequence[index]
                    previous_time = source_time_ns[index]
                    if previous_sequence and frame.frame_sequence > previous_sequence + 1:
                        skipped_frames[index] += (
                            frame.frame_sequence - previous_sequence - 1
                        )
                    if previous_time:
                        gap = frame.host_time_ns - previous_time
                        # The viewer reports skipped source frames but keeps a
                        # clip when its real time spacing still fits the model's
                        # temporal window. This prevents the latest-frame IPC
                        # from hiding an otherwise useful live diagnostic pose.
                        if gap <= 0 or gap > MAXIMUM_CLIP_FRAME_GAP_NS:
                            histories[index].clear()
                            pose_valid[index] = False
                    source_sequence[index] = frame.frame_sequence
                    source_time_ns[index] = frame.host_time_ns
                    latest_frames[index] = frame
                    arrival_ns[index] = now_ns
                    arrivals[index].append(now_ns)

                calibration_target = config.isolation.background_calibration_frames
                if calibration_target > 0 and any(
                    background is None for background in backgrounds
                ):
                    ready = []
                    for index, frame in enumerate(frames):
                        if frame is None or backgrounds[index] is not None:
                            continue
                        background_samples[index].append(frame.depth)
                        if len(background_samples[index]) >= calibration_target:
                            ready.append(index)
                    futures = {
                        index: executor.submit(
                            estimate_background, background_samples[index]
                        )
                        for index in ready
                    }
                    for index, future in futures.items():
                        backgrounds[index] = future.result()
                        background_samples[index].clear()
                        logging.info(
                            "viewer camera %d background ready", index
                        )
                    for index in range(CAMERA_COUNT):
                        store.camera(index, camera_payload(
                            index,
                            latest_frames[index],
                            latest_points[index],
                            joints[index],
                            confidence[index],
                            False,
                            pose_sequence[index],
                            inference_ms[index],
                            capture_rate(arrivals[index]),
                            arrival_ns[index],
                            skipped_frames[index],
                            (
                                calibration_target
                                if backgrounds[index] is not None
                                else len(background_samples[index])
                            ),
                            calibration_target,
                        ))
                    if any(background is None for background in backgrounds):
                        counts = [
                            calibration_target
                            if backgrounds[index] is not None
                            else len(background_samples[index])
                            for index in range(CAMERA_COUNT)
                        ]
                        store.update(
                            stage="calibrating",
                            message=(
                                "Keep stage empty: background "
                                f"{counts[0]}/{calibration_target}, "
                                f"{counts[1]}/{calibration_target}"
                            ),
                        )
                        continue
                    for history in histories:
                        history.clear()
                    store.update(
                        stage="waiting-for-player",
                        message="Background ready; enter the stage",
                    )
                    channel.set_state(WorkerState.RUNNING)
                    logging.info("viewer stage 3/3: background ready")

                isolation_now_ns = time.monotonic_ns()
                futures = {
                    index: executor.submit(
                        isolate_player,
                        frame,
                        replace(
                            config.isolation,
                            minimum_person_height_m=(
                                config.isolation.minimum_person_height_m
                                if isolation_now_ns - last_candidate_ns[index]
                                <= 750_000_000
                                else config.isolation.minimum_acquisition_height_m
                            ),
                        ),
                        backgrounds[index],
                    )
                    for index, frame in enumerate(frames)
                    if frame is not None
                }
                for index, future in futures.items():
                    points = future.result()
                    latest_points[index] = points
                    if len(points) >= config.isolation.minimum_person_points:
                        last_candidate_ns[index] = time.monotonic_ns()
                        histories[index].append(
                            sample_points(points, model.point_count)
                        )
                    else:
                        histories[index].clear()
                        pose_valid[index] = False

                ready = [
                    len(latest_points[index])
                    >= config.isolation.minimum_person_points
                    and len(histories[index]) == model.frames_per_clip
                    for index in range(CAMERA_COUNT)
                ]
                mode = control.mode()
                selected, next_dual = select_camera(mode, ready, next_dual)
                if selected is not None:
                    clip = np.stack(histories[selected]).astype(
                        np.float32, copy=True
                    )
                    centroid = clip.reshape(-1, 3).mean(axis=0)
                    clip -= centroid.reshape(1, 1, 3)
                    started_ns = time.perf_counter_ns()
                    inferred = model.infer(clip[None, ...])[0]
                    elapsed_ms = (time.perf_counter_ns() - started_ns) / 1e6
                    predicted = inferred + centroid.reshape(1, 3)
                    support = (
                        point_support_confidence(
                            predicted, latest_points[selected]
                        )
                        * kinematic_confidence(predicted)
                    )
                    joints[selected] = predicted
                    confidence[selected] = support
                    pose_sequence[selected] = source_sequence[selected]
                    inference_ms[selected] = elapsed_ms
                    pose_valid[selected] = bool(
                        np.count_nonzero(
                            support >= config.fusion.inferred_confidence
                        ) >= 10
                    )
                    inference_arrivals.append(time.monotonic_ns())

                any_player = False
                for index in range(CAMERA_COUNT):
                    any_player |= bool(len(latest_points[index]))
                    store.camera(index, camera_payload(
                        index,
                        latest_frames[index],
                        latest_points[index],
                        joints[index],
                        confidence[index],
                        pose_valid[index],
                        pose_sequence[index],
                        inference_ms[index],
                        capture_rate(arrivals[index]),
                        arrival_ns[index],
                        skipped_frames[index],
                        calibration_target,
                        calibration_target,
                    ))
                if all(frame is not None for frame in latest_frames):
                    serials = tuple(
                        frame.serial for frame in latest_frames if frame is not None
                    )
                    primary_camera = resolve_primary_camera(config, serials)
                store.update(
                    stage="tracking" if any_player else "waiting-for-player",
                    message=(
                        "Player isolated; compare point cloud with skeleton"
                        if any_player
                        else "No player component; check framing and foreground"
                    ),
                    primary_camera=primary_camera,
                    inference_ms=(
                        round(inference_ms[selected], 2)
                        if selected is not None
                        and inference_ms[selected] is not None
                        else None
                    ),
                    inference_hz=round(capture_rate(inference_arrivals), 1),
                )
        finally:
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=2.0)


def main() -> int:
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
        logging.info("live preview stopped")
        return 0
    except Exception as error:
        logging.exception("live preview failed")
        if channel is not None:
            channel.report_fault(f"{type(error).__name__}: {error}")
        return 1
    finally:
        if channel is not None:
            channel.close()


if __name__ == "__main__":
    raise SystemExit(main())
