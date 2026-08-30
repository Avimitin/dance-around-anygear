"""Versioned Windows shared-memory protocol used by the SPiKE worker."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import ctypes
from ctypes import wintypes
import mmap
import struct
import time
from typing import Sequence

import numpy as np


MAGIC = 0x4B505344
VERSION = 1
CAMERA_COUNT = 2
JOINT_COUNT = 15
MAXIMUM_DEPTH_WIDTH = 848
MAXIMUM_DEPTH_HEIGHT = 480
MAXIMUM_DEPTH_SAMPLES = MAXIMUM_DEPTH_WIDTH * MAXIMUM_DEPTH_HEIGHT

CONTROL_BYTES = 256
DEPTH_HEADER_BYTES = 128
DEPTH_FRAME_BYTES = 814_208
POSE_FRAME_BYTES = 896
CAMERAS_OFFSET = CONTROL_BYTES
POSE_OFFSET = CAMERAS_OFFSET + CAMERA_COUNT * DEPTH_FRAME_BYTES
MAPPING_BYTES = POSE_OFFSET + POSE_FRAME_BYTES

CONTROL_FIXED = struct.Struct("<IHHIIIIIIiIIiq")
DEPTH_METADATA = struct.Struct("<QqfIIffffi5fI32s")
POSE_METADATA = struct.Struct("<QqIIII")

WORKER_STATE_OFFSET = 32
WORKER_PROCESS_ID_OFFSET = 40
SHUTDOWN_REQUESTED_OFFSET = 44
WORKER_HEARTBEAT_OFFSET = 48
ERROR_MESSAGE_OFFSET = 56
ERROR_MESSAGE_BYTES = 160

POSE_FUSED_JOINTS_OFFSET = POSE_OFFSET + 40
POSE_FUSED_CONFIDENCE_OFFSET = POSE_OFFSET + 220
POSE_FUSED_STATE_OFFSET = POSE_OFFSET + 280
POSE_CAMERA_JOINTS_OFFSET = POSE_OFFSET + 320
POSE_CAMERA_CONFIDENCE_OFFSET = POSE_OFFSET + 680
POSE_CAMERA_CENTROID_OFFSET = POSE_OFFSET + 800
POSE_SOURCE_SEQUENCE_OFFSET = POSE_OFFSET + 824

WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
SYNCHRONIZE = 0x00100000
EVENT_MODIFY_STATE = 0x0002


class WorkerState(IntEnum):
    EMPTY = 0
    STARTING = 1
    LOADING_MODEL = 2
    READY = 3
    RUNNING = 4
    FAULTED = 5
    STOPPING = 6
    STOPPED = 7


@dataclass(frozen=True)
class DepthSnapshot:
    camera_index: int
    frame_sequence: int
    host_time_ns: int
    depth_scale_m: float
    width: int
    height: int
    principal_x: float
    principal_y: float
    focal_x: float
    focal_y: float
    distortion_model: int
    distortion: tuple[float, ...]
    device_index: int
    serial: str
    depth: np.ndarray


@dataclass(frozen=True)
class PoseSnapshot:
    generation: int
    source_time_ns: int
    worker_time_us: int
    flags: int
    fused_joints: np.ndarray
    fused_confidence: np.ndarray
    fused_tracking_state: np.ndarray
    camera_joints: np.ndarray
    camera_confidence: np.ndarray
    camera_centroid: np.ndarray
    source_sequence: Sequence[int]


def _configure_kernel32():
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenEventW.argtypes = (
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.LPCWSTR,
    )
    kernel32.OpenEventW.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    )
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
    kernel32.SetEvent.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    return kernel32


class IpcChannel:
    """Worker-side view of a host-created mapping and two auto-reset events."""

    def __init__(
        self,
        mapping_name: str,
        input_event_name: str,
        output_event_name: str,
    ) -> None:
        if not mapping_name or not input_event_name or not output_event_name:
            raise ValueError("all IPC object names are required")
        self._kernel32 = _configure_kernel32()
        self._mapping = mmap.mmap(
            -1, MAPPING_BYTES, tagname=mapping_name, access=mmap.ACCESS_WRITE
        )
        self._buffer = (ctypes.c_ubyte * MAPPING_BYTES).from_buffer(
            self._mapping
        )
        self._input_event = self._open_event(input_event_name)
        self._output_event = self._open_event(output_event_name)
        self._parent_process = None
        self.host_process_id = 0
        self._closed = False
        self._validate_header()

    def _open_event(self, name: str):
        handle = self._kernel32.OpenEventW(
            SYNCHRONIZE | EVENT_MODIFY_STATE, False, name
        )
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def _validate_header(self) -> None:
        values = CONTROL_FIXED.unpack_from(self._mapping, 0)
        (
            magic,
            version,
            header_bytes,
            mapping_bytes,
            camera_count,
            maximum_width,
            maximum_height,
            maximum_samples,
            joint_count,
            _worker_state,
            _host_pid,
            _worker_pid,
            _shutdown,
            _heartbeat,
        ) = values
        expected = (
            MAGIC,
            VERSION,
            CONTROL_BYTES,
            MAPPING_BYTES,
            CAMERA_COUNT,
            MAXIMUM_DEPTH_WIDTH,
            MAXIMUM_DEPTH_HEIGHT,
            MAXIMUM_DEPTH_SAMPLES,
            JOINT_COUNT,
        )
        actual = (
            magic,
            version,
            header_bytes,
            mapping_bytes,
            camera_count,
            maximum_width,
            maximum_height,
            maximum_samples,
            joint_count,
        )
        if actual != expected:
            raise RuntimeError(
                f"incompatible SPiKE IPC header: {actual!r} != {expected!r}"
            )
        self.host_process_id = int(_host_pid)

    def monitor_parent(self, process_id: int) -> None:
        if process_id <= 0 or process_id != self.host_process_id:
            raise RuntimeError("SPiKE IPC parent process ID does not match")
        handle = self._kernel32.OpenProcess(SYNCHRONIZE, False, process_id)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        self._parent_process = handle

    def parent_is_running(self) -> bool:
        if not self._parent_process:
            return False
        result = self._kernel32.WaitForSingleObject(self._parent_process, 0)
        if result == WAIT_TIMEOUT:
            return True
        if result == WAIT_OBJECT_0:
            return False
        raise ctypes.WinError(ctypes.get_last_error())

    def _long(self, offset: int) -> ctypes.c_long:
        return ctypes.c_long.from_buffer(self._mapping, offset)

    def _longlong(self, offset: int) -> ctypes.c_longlong:
        return ctypes.c_longlong.from_buffer(self._mapping, offset)

    def _read_sequence(self, offset: int) -> int:
        # The protocol is Windows x64-only and every sequence is 64-bit aligned.
        # Aligned loads/stores are atomic on this target; there is exactly one
        # writer for each sequence and event signaling supplies the cross-process
        # synchronization point.
        return int(self._longlong(offset).value)

    def _begin_write(self, offset: int) -> None:
        value = self._longlong(offset)
        result = int(value.value) + 1
        if result % 2 == 0:
            result += 1
        value.value = result

    def _end_write(self, offset: int) -> None:
        value = self._longlong(offset)
        result = int(value.value) + 1
        if result % 2 != 0:
            result += 1
        value.value = result

    def set_state(self, state: WorkerState) -> None:
        self._long(WORKER_STATE_OFFSET).value = int(state)
        self._kernel32.SetEvent(self._output_event)

    def set_worker_process_id(self, process_id: int) -> None:
        struct.pack_into("<I", self._mapping, WORKER_PROCESS_ID_OFFSET, process_id)

    def heartbeat(self) -> None:
        now_ms = time.monotonic_ns() // 1_000_000
        self._longlong(WORKER_HEARTBEAT_OFFSET).value = now_ms

    def shutdown_requested(self) -> bool:
        return bool(struct.unpack_from("<i", self._mapping,
                                       SHUTDOWN_REQUESTED_OFFSET)[0])

    def wait_for_input(self, timeout_ms: int) -> bool:
        result = self._kernel32.WaitForSingleObject(
            self._input_event, max(0, int(timeout_ms))
        )
        if result == WAIT_OBJECT_0:
            return True
        if result == WAIT_TIMEOUT:
            return False
        raise ctypes.WinError(ctypes.get_last_error())

    def read_depth(
        self, camera_index: int, after_sequence: int = 0
    ) -> DepthSnapshot | None:
        if not 0 <= camera_index < CAMERA_COUNT:
            raise IndexError(camera_index)
        frame_offset = CAMERAS_OFFSET + camera_index * DEPTH_FRAME_BYTES
        for _ in range(5):
            first = self._read_sequence(frame_offset)
            if first == 0 or first % 2 != 0:
                continue
            metadata = DEPTH_METADATA.unpack_from(
                self._mapping, frame_offset + 8
            )
            (
                frame_sequence,
                host_time_ns,
                depth_scale_m,
                width,
                height,
                principal_x,
                principal_y,
                focal_x,
                focal_y,
                distortion_model,
                *tail,
            ) = metadata
            distortion = tuple(float(value) for value in tail[:5])
            device_index = int(tail[5])
            serial_raw = tail[6]
            sample_count = int(width) * int(height)
            if (
                frame_sequence <= after_sequence
                or width <= 0
                or height <= 0
                or width > MAXIMUM_DEPTH_WIDTH
                or height > MAXIMUM_DEPTH_HEIGHT
                or sample_count > MAXIMUM_DEPTH_SAMPLES
            ):
                return None
            depth = np.ndarray(
                (height, width),
                dtype="<u2",
                buffer=self._mapping,
                offset=frame_offset + DEPTH_HEADER_BYTES,
            ).copy()
            second = self._read_sequence(frame_offset)
            if first == second and second % 2 == 0:
                serial = serial_raw.split(b"\0", 1)[0].decode(
                    "ascii", errors="replace"
                )
                return DepthSnapshot(
                    camera_index=camera_index,
                    frame_sequence=int(frame_sequence),
                    host_time_ns=int(host_time_ns),
                    depth_scale_m=float(depth_scale_m),
                    width=int(width),
                    height=int(height),
                    principal_x=float(principal_x),
                    principal_y=float(principal_y),
                    focal_x=float(focal_x),
                    focal_y=float(focal_y),
                    distortion_model=int(distortion_model),
                    distortion=distortion,
                    device_index=device_index,
                    serial=serial,
                    depth=depth,
                )
        return None

    def publish_pose(self, pose: PoseSnapshot) -> None:
        fused = np.asarray(pose.fused_joints, dtype=np.float32)
        fused_confidence = np.asarray(
            pose.fused_confidence, dtype=np.float32
        )
        fused_state = np.asarray(pose.fused_tracking_state, dtype=np.uint8)
        camera = np.asarray(pose.camera_joints, dtype=np.float32)
        camera_confidence = np.asarray(
            pose.camera_confidence, dtype=np.float32
        )
        centroids = np.asarray(pose.camera_centroid, dtype=np.float32)
        if fused.shape != (JOINT_COUNT, 3):
            raise ValueError("fused_joints must have shape (15, 3)")
        if fused_confidence.shape != (JOINT_COUNT,):
            raise ValueError("fused_confidence must have shape (15,)")
        if fused_state.shape != (JOINT_COUNT,):
            raise ValueError("fused_tracking_state must have shape (15,)")
        if camera.shape != (CAMERA_COUNT, JOINT_COUNT, 3):
            raise ValueError("camera_joints must have shape (2, 15, 3)")
        if camera_confidence.shape != (CAMERA_COUNT, JOINT_COUNT):
            raise ValueError("camera_confidence must have shape (2, 15)")
        if centroids.shape != (CAMERA_COUNT, 3):
            raise ValueError("camera_centroid must have shape (2, 3)")
        if len(pose.source_sequence) != CAMERA_COUNT:
            raise ValueError("source_sequence must contain two values")

        self._begin_write(POSE_OFFSET)
        POSE_METADATA.pack_into(
            self._mapping,
            POSE_OFFSET + 8,
            int(pose.generation),
            int(pose.source_time_ns),
            int(pose.worker_time_us),
            int(pose.flags),
            JOINT_COUNT,
            0,
        )
        self._mapping[POSE_FUSED_JOINTS_OFFSET:
                      POSE_FUSED_JOINTS_OFFSET + fused.nbytes] = fused.tobytes()
        self._mapping[POSE_FUSED_CONFIDENCE_OFFSET:
                      POSE_FUSED_CONFIDENCE_OFFSET + fused_confidence.nbytes] = (
            fused_confidence.tobytes()
        )
        self._mapping[POSE_FUSED_STATE_OFFSET:
                      POSE_FUSED_STATE_OFFSET + fused_state.nbytes] = (
            fused_state.tobytes()
        )
        self._mapping[POSE_CAMERA_JOINTS_OFFSET:
                      POSE_CAMERA_JOINTS_OFFSET + camera.nbytes] = camera.tobytes()
        self._mapping[POSE_CAMERA_CONFIDENCE_OFFSET:
                      POSE_CAMERA_CONFIDENCE_OFFSET + camera_confidence.nbytes] = (
            camera_confidence.tobytes()
        )
        self._mapping[POSE_CAMERA_CENTROID_OFFSET:
                      POSE_CAMERA_CENTROID_OFFSET + centroids.nbytes] = (
            centroids.tobytes()
        )
        struct.pack_into(
            "<2Q", self._mapping, POSE_SOURCE_SEQUENCE_OFFSET,
            *(int(value) for value in pose.source_sequence),
        )
        self._end_write(POSE_OFFSET)
        self.heartbeat()
        self._kernel32.SetEvent(self._output_event)

    def report_fault(self, message: str) -> None:
        encoded = message.encode("utf-8", errors="replace")[:
            ERROR_MESSAGE_BYTES - 1]
        self._mapping[ERROR_MESSAGE_OFFSET:
                      ERROR_MESSAGE_OFFSET + ERROR_MESSAGE_BYTES] = (
            encoded + b"\0" * (ERROR_MESSAGE_BYTES - len(encoded))
        )
        self.set_state(WorkerState.FAULTED)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        for handle_name in (
            "_parent_process", "_input_event", "_output_event"
        ):
            handle = getattr(self, handle_name, None)
            if handle:
                self._kernel32.CloseHandle(handle)
                setattr(self, handle_name, None)
        del self._buffer
        self._mapping.close()

    def __enter__(self) -> "IpcChannel":
        return self

    def __exit__(self, *_exc_info) -> None:
        self.close()
