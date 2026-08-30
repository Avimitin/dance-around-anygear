"""Temporal contracts for realtime and recorded SPiKE clips."""

from __future__ import annotations


# Three-frame SPiKE clips are trained at 30 Hz. The shared-memory transport is
# deliberately latest-frame-only, so a slower consumer can observe a monotonic
# source-sequence jump even though the camera never stalled. The in-game host
# can occasionally deliver a 3-4 frame jump while DirectML is active; 125 ms
# keeps that bounded live cadence but remains below the runtime's 150 ms stale
# input deadline. Longer gaps still rebuild the clip.
MAXIMUM_CLIP_FRAME_GAP_NS = 125_000_000
MAXIMUM_RECORDED_CLIP_FRAME_GAP_NS = 75_000_000


def realtime_clip_frame_is_contiguous(
    previous_sequence: int,
    previous_time_ns: int,
    current_sequence: int,
    current_time_ns: int,
    maximum_gap_ns: int = MAXIMUM_CLIP_FRAME_GAP_NS,
) -> bool:
    if previous_sequence == 0:
        return True
    elapsed_ns = current_time_ns - previous_time_ns
    return (
        current_sequence > previous_sequence
        and 0 < elapsed_ns <= maximum_gap_ns
    )


def recorded_clip_frame_is_contiguous(
    previous_sequence: int,
    previous_time_ns: int,
    current_sequence: int,
    current_time_ns: int,
    maximum_gap_ns: int = MAXIMUM_RECORDED_CLIP_FRAME_GAP_NS,
) -> bool:
    """Require an adjacent source frame in an offline training recording."""
    if previous_sequence == 0:
        return True
    return (
        current_sequence == previous_sequence + 1
        and realtime_clip_frame_is_contiguous(
            previous_sequence,
            previous_time_ns,
            current_sequence,
            current_time_ns,
            maximum_gap_ns,
        )
    )
