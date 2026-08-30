"""Runtime support for the depth-only SPiKE backend."""

import os


# Small point-cloud matrices do not benefit from process-wide BLAS pools. Keep
# the worker bounded by default while preserving an explicit maintainer
# override for profiling.
for _thread_variable in (
    "OMP_NUM_THREADS",
    "OPENBLAS_NUM_THREADS",
    "MKL_NUM_THREADS",
    "NUMEXPR_NUM_THREADS",
):
    os.environ.setdefault(_thread_variable, "1")

from .protocol import (
    CAMERA_COUNT,
    JOINT_COUNT,
    DepthSnapshot,
    IpcChannel,
    PoseSnapshot,
    WorkerState,
)

__all__ = [
    "CAMERA_COUNT",
    "JOINT_COUNT",
    "DepthSnapshot",
    "IpcChannel",
    "PoseSnapshot",
    "WorkerState",
]
