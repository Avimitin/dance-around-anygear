"""Small protocol peer used by the cross-language IPC test."""

from __future__ import annotations

import argparse
import os
import time

import numpy as np

from .protocol import IpcChannel, PoseSnapshot, WorkerState


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ipc-mapping", required=True)
    parser.add_argument("--ipc-input-event", required=True)
    parser.add_argument("--ipc-output-event", required=True)
    parser.add_argument("--ipc-parent-pid", required=True, type=int)
    args = parser.parse_args()
    with IpcChannel(
        args.ipc_mapping, args.ipc_input_event, args.ipc_output_event
    ) as channel:
        channel.monitor_parent(args.ipc_parent_pid)
        channel.set_worker_process_id(os.getpid())
        channel.set_state(WorkerState.READY)
        sequences = [0, 0]
        deadline = time.monotonic() + 10.0
        while (channel.parent_is_running() and
               not channel.shutdown_requested() and
               time.monotonic() < deadline):
            channel.wait_for_input(100)
            if channel.shutdown_requested():
                break
            frames = [
                channel.read_depth(index, sequences[index])
                for index in range(2)
            ]
            for index, frame in enumerate(frames):
                if frame is not None:
                    sequences[index] = frame.frame_sequence
            if not any(frame is not None for frame in frames):
                continue
            if not all(sequences):
                continue
            joints = np.zeros((15, 3), dtype=np.float32)
            joints[:, 0] = np.arange(15, dtype=np.float32)
            joints[:, 1] = float(frames[0].depth[0, 0]) if frames[0] else 1000.0
            camera = np.stack((joints, joints + 1.0))
            channel.publish_pose(PoseSnapshot(
                generation=1,
                source_time_ns=max(
                    frame.host_time_ns for frame in frames if frame is not None
                ),
                worker_time_us=1234,
                flags=0x7,
                fused_joints=joints,
                fused_confidence=np.ones(15, dtype=np.float32),
                fused_tracking_state=np.full(15, 2, dtype=np.uint8),
                camera_joints=camera,
                camera_confidence=np.ones((2, 15), dtype=np.float32),
                camera_centroid=np.zeros((2, 3), dtype=np.float32),
                source_sequence=sequences,
            ))
            channel.set_state(WorkerState.RUNNING)
        channel.set_state(WorkerState.STOPPED)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
