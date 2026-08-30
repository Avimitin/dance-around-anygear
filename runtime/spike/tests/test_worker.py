from __future__ import annotations

from anygear_spike.worker import (
    MAXIMUM_INPUT_AGE_NS,
    RuntimeTelemetry,
    candidate_dropout_is_bridgeable,
    frame_is_fresh,
    select_depth_processing_indices,
    select_inference_indices,
    source_sample_is_due,
)
from anygear_spike.temporal import (
    MAXIMUM_CLIP_FRAME_GAP_NS,
    realtime_clip_frame_is_contiguous,
)


def test_depth_input_freshness_has_a_hard_monotonic_deadline() -> None:
    now = 1_000_000_000
    assert frame_is_fresh(now - MAXIMUM_INPUT_AGE_NS, now)
    assert not frame_is_fresh(now - MAXIMUM_INPUT_AGE_NS - 1, now)
    assert not frame_is_fresh(0, now)
    assert not frame_is_fresh(now + 1, now)


def test_short_candidate_dropout_is_bridged_with_a_hard_deadline() -> None:
    last_candidate = 1_000_000_000
    assert candidate_dropout_is_bridgeable(
        last_candidate, last_candidate + 100_000_000, 100
    )
    assert not candidate_dropout_is_bridgeable(
        last_candidate, last_candidate + 100_000_001, 100
    )
    assert not candidate_dropout_is_bridgeable(0, last_candidate, 100)
    assert not candidate_dropout_is_bridgeable(
        last_candidate, last_candidate - 1, 100
    )
    assert not candidate_dropout_is_bridgeable(
        last_candidate, last_candidate, 0
    )


def test_inference_limit_uses_source_time_without_completion_phase_alias() -> None:
    interval = 33_333_333
    previous = 1_000_000_000
    assert source_sample_is_due(previous, 0, interval)
    # A nominal 30 Hz camera frame must remain due even when host-arrival
    # jitter puts it a few milliseconds before the exact 33.3 ms boundary.
    assert source_sample_is_due(previous + 30_000_000, previous, interval)
    # A staggered frame from another camera must not cause a duplicate run.
    assert not source_sample_is_due(
        previous + 16_666_667, previous, interval
    )
    assert not source_sample_is_due(previous, previous, interval)


def test_runtime_telemetry_reports_bounded_window_percentiles() -> None:
    telemetry = RuntimeTelemetry(window_outputs=3, started_ns=1_000_000_000)
    assert telemetry.record(
        1_000, 1_990_000_000, True, 10_000, 2_000_000_000
    ) is None
    assert telemetry.record(
        2_000, 2_980_000_000, False, None, 3_000_000_000
    ) is None
    summary = telemetry.record(
        3_000, 3_970_000_000, True, 20_000, 4_000_000_000
    )
    assert summary is not None
    assert summary["output_hz"] == 1.0
    assert summary["pose_percent"] == 200.0 / 3.0
    assert summary["pipeline_p50_ms"] == 2.0
    assert summary["pipeline_p95_ms"] == 2.9
    assert summary["inference_p50_ms"] == 15.0
    assert summary["source_age_p50_ms"] == 20.0


def test_inference_plan_reuses_fresh_predictions_and_falls_back() -> None:
    assert select_inference_indices(
        [True, True], [True, True], [10, 20], [10, 21],
        primary=0, fusion_mode="primary", output_count=1,
        secondary_every_n=3,
    ) == []
    assert select_inference_indices(
        [False, True], [False, True], [0, 20], [10, 21],
        primary=0, fusion_mode="primary", output_count=1,
        secondary_every_n=3,
    ) == [1]
    # A calibrated secondary refresh takes this output's single inference
    # slot instead of extending it to two DirectML runs.
    assert select_inference_indices(
        [True, True], [True, True], [9, 20], [10, 21],
        primary=0, fusion_mode="calibrated", output_count=3,
        secondary_every_n=3,
    ) == [1]
    assert select_inference_indices(
        [True, True], [False, False], [0, 0], [10, 20],
        primary=0, fusion_mode="calibrated", output_count=0,
        secondary_every_n=3,
    ) == [0]
    assert select_inference_indices(
        [True, True], [True, False], [10, 0], [11, 20],
        primary=0, fusion_mode="calibrated", output_count=1,
        secondary_every_n=3,
    ) == [1]


def test_temporal_clip_requires_monotonic_sequence_and_bounded_cadence() -> None:
    previous_time = 1_000_000_000
    assert realtime_clip_frame_is_contiguous(
        10, previous_time, 11,
        previous_time + MAXIMUM_CLIP_FRAME_GAP_NS,
    )
    # The IPC holds only the latest frame. A consumer running below the 30 Hz
    # camera cadence may legitimately skip a source sequence while the actual
    # temporal spacing remains suitable for inference.
    assert realtime_clip_frame_is_contiguous(
        10, previous_time, 12, previous_time + 33_333_333
    )
    assert not realtime_clip_frame_is_contiguous(
        10, previous_time, 11,
        previous_time + MAXIMUM_CLIP_FRAME_GAP_NS + 1,
    )
    assert not realtime_clip_frame_is_contiguous(
        10, previous_time, 11, previous_time
    )
    assert not realtime_clip_frame_is_contiguous(
        10, previous_time, 10, previous_time + 33_333_333
    )
    assert not realtime_clip_frame_is_contiguous(
        10, previous_time, 9, previous_time + 33_333_333
    )


def test_primary_mode_only_processes_the_selected_depth_camera() -> None:
    assert select_depth_processing_indices("primary", 0) == (0,)
    assert select_depth_processing_indices("primary", 1) == (1,)
    assert select_depth_processing_indices("calibrated", 0) == (0, 1)


def test_calibrated_scheduler_refreshes_both_with_one_inference_slot() -> None:
    source_sequence = [0, 0]
    prediction_sequence = [0, 0]
    prediction_valid = [False, False]
    selected = []
    for output_count in range(12):
        source_sequence = [value + 1 for value in source_sequence]
        plan = select_inference_indices(
            [True, True],
            prediction_valid,
            prediction_sequence,
            source_sequence,
            primary=0,
            fusion_mode="calibrated",
            output_count=output_count,
            secondary_every_n=3,
        )
        assert len(plan) <= 1
        assert plan
        camera = plan[0]
        selected.append(camera)
        prediction_sequence[camera] = source_sequence[camera]
        prediction_valid[camera] = True
    assert selected[:4] == [0, 1, 0, 1]
    assert selected.count(0) == 8
    assert selected.count(1) == 4
