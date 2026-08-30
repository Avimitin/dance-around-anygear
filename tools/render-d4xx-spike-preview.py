"""Render the production D4xx/SPiKE input and pose for a recorded session."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import struct
import sys
import zlib

import numpy as np


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SPIKE_RUNTIME_ROOT = REPOSITORY_ROOT / "runtime" / "spike"
sys.path.insert(0, str(SPIKE_RUNTIME_ROOT))

from anygear_spike.config import load_config  # noqa: E402
from anygear_spike.depth import (  # noqa: E402
    estimate_background,
    isolate_player,
    sample_points,
)
from anygear_spike.fusion import (  # noqa: E402
    BONE_RANGES,
    kinematic_confidence,
    point_support_confidence,
)
from anygear_spike.model_runtime import RealtimeSpikeModel  # noqa: E402
from anygear_spike.protocol import DepthSnapshot  # noqa: E402


JOINT_NAMES = (
    "Head", "Neck", "R Shoulder", "L Shoulder", "R Elbow", "L Elbow",
    "R Hand", "L Hand", "Torso", "R Hip", "L Hip", "R Knee", "L Knee",
    "R Foot", "L Foot",
)


@dataclass(frozen=True)
class PreviewResult:
    camera_index: int
    serial: str
    selected_frame: int | None
    point_count: int
    extent_m: list[float]
    centroid_m: list[float]
    tracked_joints: int
    inferred_joints: int
    confidence: list[float]
    note: str


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--background", type=Path, required=True)
    parser.add_argument("--performer", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def load_manifest(root: Path) -> dict:
    path = root / "manifest.json"
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != "dance-around-anygear.d4xx-depth.v1":
        raise ValueError(f"unsupported capture manifest: {path}")
    return document


def device_by_serial(manifest: dict, serial: str) -> dict:
    for device in manifest["devices"]:
        if str(device["serial"]) == serial:
            return device
    raise ValueError(f"background capture has no device serial {serial}")


def read_frames(root: Path, manifest: dict, device: dict) -> np.ndarray:
    width = int(manifest["width"])
    height = int(manifest["height"])
    frames = int(device["frame_count"])
    path = root / str(device["file"])
    values = np.fromfile(path, dtype="<u2")
    expected = frames * width * height
    if values.size != expected:
        raise ValueError(f"{path} has {values.size} samples, expected {expected}")
    return values.reshape(frames, height, width)


def make_snapshot(
    manifest: dict, device: dict, frames: np.ndarray, index: int,
    camera_index: int,
) -> DepthSnapshot:
    intrinsics = device["intrinsics"]
    metadata = device["frames"][index]
    return DepthSnapshot(
        camera_index=camera_index,
        frame_sequence=int(metadata["sequence"]),
        host_time_ns=int(metadata["host_time_ns"]),
        depth_scale_m=float(device["depth_scale_m"]),
        width=int(manifest["width"]),
        height=int(manifest["height"]),
        principal_x=float(intrinsics["principal_x"]),
        principal_y=float(intrinsics["principal_y"]),
        focal_x=float(intrinsics["focal_x"]),
        focal_y=float(intrinsics["focal_y"]),
        distortion_model=int(intrinsics["distortion_model"]),
        distortion=tuple(float(value) for value in intrinsics["distortion"]),
        device_index=int(device["index"]),
        serial=str(device["serial"]),
        depth=frames[index],
    )


def valid_triplet(device: dict, last_index: int) -> bool:
    rows = device["frames"][last_index - 2:last_index + 1]
    for previous, current in zip(rows, rows[1:]):
        sequence_gap = int(current["sequence"]) - int(previous["sequence"])
        time_gap_ms = (
            int(current["host_time_ns"]) - int(previous["host_time_ns"])
        ) / 1e6
        if sequence_gap != 1 or not 0.0 < time_gap_ms <= 75.0:
            return False
    return True


def choose_triplet(points: list[np.ndarray], device: dict) -> int | None:
    candidates: list[tuple[float, int]] = []
    for index in range(2, len(points)):
        if any(len(points[item]) == 0 for item in range(index - 2, index + 1)):
            continue
        if not valid_triplet(device, index):
            continue
        center = points[index - 1]
        extent = center.max(axis=0) - center.min(axis=0)
        # Prefer a complete upright body and penalize room fragments extending
        # far behind it. Point count remains a useful tie breaker.
        score = float(min(len(points[item]) for item in range(index - 2, index + 1)))
        score -= 2500.0 * max(0.0, 1.20 - float(extent[1]))
        score -= 1800.0 * max(0.0, float(extent[2]) - 0.90)
        candidates.append((score, index))
    return max(candidates)[1] if candidates else None


def png_chunk(name: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload)) + name + payload
        + struct.pack(">I", zlib.crc32(name + payload) & 0xFFFFFFFF)
    )


def write_depth_png(path: Path, depth: np.ndarray) -> None:
    valid = depth > 0
    rgb = np.zeros((*depth.shape, 3), dtype=np.uint8)
    if np.any(valid):
        low, high = np.percentile(depth[valid], (2.0, 98.0))
        span = max(float(high - low), 1.0)
        value = np.clip((depth.astype(np.float32) - low) / span, 0.0, 1.0)
        value = 1.0 - value
        rgb[..., 0] = np.clip(4.0 * value - 1.5, 0.0, 1.0) * 255
        rgb[..., 1] = np.clip(1.0 - np.abs(4.0 * value - 2.0), 0.0, 1.0) * 255
        rgb[..., 2] = np.clip(1.5 - 4.0 * value, 0.0, 1.0) * 255
        rgb[~valid] = 0
    scanlines = b"".join(b"\x00" + row.tobytes() for row in rgb)
    header = struct.pack(">IIBBBBB", depth.shape[1], depth.shape[0], 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(scanlines, 9))
        + png_chunk(b"IEND", b"")
    )


def write_ply(path: Path, points: np.ndarray) -> None:
    with path.open("w", encoding="ascii", newline="\n") as output:
        output.write("ply\nformat ascii 1.0\n")
        output.write(f"element vertex {len(points)}\n")
        output.write("property float x\nproperty float y\nproperty float z\nend_header\n")
        for x, y, z_value in points:
            output.write(f"{x:.6f} {y:.6f} {z_value:.6f}\n")


def project(
    point: np.ndarray, axes: tuple[int, int], bounds: tuple[float, float, float, float],
    box: tuple[float, float, float, float],
) -> tuple[float, float]:
    minimum_x, maximum_x, minimum_y, maximum_y = bounds
    x, y = float(point[axes[0]]), float(point[axes[1]])
    left, top, width, height = box
    screen_x = left + (x - minimum_x) / (maximum_x - minimum_x) * width
    screen_y = top + height - (y - minimum_y) / (maximum_y - minimum_y) * height
    return screen_x, screen_y


def render_panel(
    points: np.ndarray, joints: np.ndarray | None, confidence: np.ndarray | None,
    axes: tuple[int, int], bounds: tuple[float, float, float, float],
    box: tuple[float, float, float, float], title: str,
) -> list[str]:
    left, top, width, height = box
    output = [
        f'<rect x="{left}" y="{top}" width="{width}" height="{height}" '
        'fill="#111827" stroke="#64748b"/>',
        f'<text x="{left + 10}" y="{top + 22}" fill="#e2e8f0" '
        f'font-size="16">{title}</text>',
    ]
    for point in points[::max(1, len(points) // 3000)]:
        x, y = project(point, axes, bounds, box)
        if left <= x <= left + width and top <= y <= top + height:
            output.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="1.15" fill="#38bdf8" '
                'fill-opacity="0.38"/>'
            )
    if joints is None or confidence is None:
        return output
    for first, second, *_ in BONE_RANGES:
        x1, y1 = project(joints[first], axes, bounds, box)
        x2, y2 = project(joints[second], axes, bounds, box)
        strength = min(float(confidence[first]), float(confidence[second]))
        color = "#22c55e" if strength >= 0.55 else "#f59e0b" if strength >= 0.15 else "#ef4444"
        output.append(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'stroke="{color}" stroke-width="4" stroke-linecap="round"/>'
        )
    for index, joint in enumerate(joints):
        x, y = project(joint, axes, bounds, box)
        strength = float(confidence[index])
        color = "#22c55e" if strength >= 0.55 else "#f59e0b" if strength >= 0.15 else "#ef4444"
        output.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="{color}"/>')
    return output


def render_svg(path: Path, panels: list[dict]) -> None:
    width = 1500
    row_height = 500
    height = 90 + row_height * len(panels)
    output = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#020617"/>',
        '<text x="30" y="38" fill="#f8fafc" font-size="24" font-family="Segoe UI, sans-serif">'
        'D4xx / SPiKE production preview</text>',
        '<text x="30" y="66" fill="#94a3b8" font-size="15" font-family="Segoe UI, sans-serif">'
        'Blue: isolated depth points. Green/orange/red: tracked/inferred/unsupported joints.</text>',
    ]
    for row, panel in enumerate(panels):
        top = 90 + row * row_height
        output.append(
            f'<text x="30" y="{top + 22}" fill="#f8fafc" font-size="20" '
            f'font-family="Segoe UI, sans-serif">Camera {panel["index"]} · {panel["serial"]}'
            f' · {panel["note"]}</text>'
        )
        front_box = (30.0, float(top + 40), 690.0, 420.0)
        side_box = (780.0, float(top + 40), 690.0, 420.0)
        output.extend(render_panel(
            panel["points"], panel["joints"], panel["confidence"],
            (0, 1), (-1.6, 1.6, -1.8, 1.5), front_box,
            "Camera view: X / Y",
        ))
        output.extend(render_panel(
            panel["points"], panel["joints"], panel["confidence"],
            (2, 1), (0.4, 4.5, -1.8, 1.5), side_box,
            "Side projection: Z / Y",
        ))
    output.append("</svg>")
    path.write_text("\n".join(output), encoding="utf-8")


def main() -> int:
    args = parse_arguments()
    args.output.mkdir(parents=True, exist_ok=False)
    background_manifest = load_manifest(args.background)
    performer_manifest = load_manifest(args.performer)
    config = load_config(args.config)
    model = RealtimeSpikeModel(
        args.model,
        device_index=config.runtime.directml_device_index,
        warmup_runs=1,
    )
    panels: list[dict] = []
    reports: list[PreviewResult] = []
    for camera_index, performer_device in enumerate(performer_manifest["devices"]):
        serial = str(performer_device["serial"])
        background_device = device_by_serial(background_manifest, serial)
        background_frames = read_frames(
            args.background, background_manifest, background_device
        )
        performer_frames = read_frames(
            args.performer, performer_manifest, performer_device
        )
        background = estimate_background(list(background_frames))
        isolated = [
            isolate_player(
                make_snapshot(
                    performer_manifest, performer_device, performer_frames,
                    index, camera_index,
                ),
                config.isolation,
                background,
            )
            for index in range(len(performer_frames))
        ]
        selected = choose_triplet(isolated, performer_device)
        if selected is None:
            depth_index = len(performer_frames) // 2
            points = isolated[depth_index]
            joints = None
            confidence = None
            note = "no complete three-frame player clip"
        else:
            depth_index = selected - 1
            clip = np.stack([
                sample_points(isolated[index], model.point_count)
                for index in range(selected - 2, selected + 1)
            ]).astype(np.float32)
            centroid = clip.reshape(-1, 3).mean(axis=0)
            inferred = model.infer((clip - centroid.reshape(1, 1, 3))[None, ...])[0]
            joints = inferred + centroid.reshape(1, 3)
            points = isolated[depth_index]
            confidence = point_support_confidence(joints, points) * kinematic_confidence(joints)
            note = f"frame {depth_index}; {len(points)} isolated points"
        write_depth_png(
            args.output / f"camera-{camera_index}-depth.png",
            performer_frames[depth_index],
        )
        write_ply(args.output / f"camera-{camera_index}-points.ply", points)
        if len(points):
            extent = (points.max(axis=0) - points.min(axis=0)).tolist()
            centroid_value = points.mean(axis=0).tolist()
        else:
            extent = [0.0, 0.0, 0.0]
            centroid_value = [0.0, 0.0, 0.0]
        confidence_values = confidence if confidence is not None else np.zeros(15)
        reports.append(PreviewResult(
            camera_index=camera_index,
            serial=serial,
            selected_frame=depth_index if selected is not None else None,
            point_count=len(points),
            extent_m=[round(float(value), 4) for value in extent],
            centroid_m=[round(float(value), 4) for value in centroid_value],
            tracked_joints=int(np.count_nonzero(confidence_values >= 0.55)),
            inferred_joints=int(np.count_nonzero(
                (confidence_values >= 0.15) & (confidence_values < 0.55)
            )),
            confidence=[round(float(value), 4) for value in confidence_values],
            note=note,
        ))
        panels.append({
            "index": camera_index,
            "serial": serial,
            "points": points,
            "joints": joints,
            "confidence": confidence,
            "note": note,
        })
    render_svg(args.output / "spike-preview.svg", panels)
    report = {
        "schema": "dance-around-anygear.d4xx-spike-preview.v1",
        "background_capture": str(args.background.resolve()),
        "performer_capture": str(args.performer.resolve()),
        "config": str(args.config.resolve()),
        "model": str(args.model.resolve()),
        "primary_camera": config.fusion.primary_camera,
        "fusion_mode": config.fusion.mode,
        "cameras": [asdict(value) for value in reports],
        "joint_names": JOINT_NAMES,
    }
    (args.output / "preview-report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(args.output / "spike-preview.svg")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
