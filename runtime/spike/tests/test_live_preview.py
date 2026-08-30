from __future__ import annotations

import base64
import json

import numpy as np

from anygear_spike.live_preview import (
    PreviewStore,
    ViewerControl,
    encode_points,
    select_camera,
)


def test_dual_mode_alternates_ready_cameras() -> None:
    selected, following = select_camera("dual", [True, True], 0)
    assert (selected, following) == (0, 1)
    selected, following = select_camera("dual", [True, True], following)
    assert (selected, following) == (1, 0)


def test_single_mode_never_substitutes_another_camera() -> None:
    assert select_camera("camera-0", [False, True], 1)[0] is None
    assert select_camera("camera-1", [True, False], 0)[0] is None


def test_preview_point_payload_is_little_endian_millimetres() -> None:
    points = np.asarray(((0.125, -0.25, 2.5),), dtype=np.float32)
    encoded = encode_points(points)
    decoded = np.frombuffer(base64.b64decode(encoded), dtype="<i2")
    np.testing.assert_array_equal(decoded, (125, -250, 2500))


def test_preview_store_emits_strict_json_and_current_mode() -> None:
    control = ViewerControl("dual")
    store = PreviewStore(control)
    control.set_mode("camera-1")
    document = json.loads(store.snapshot())
    assert document["mode"] == "camera-1"
    assert len(document["cameras"]) == 2
