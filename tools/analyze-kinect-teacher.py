"""Report Kinect gameplay-filter displacement and lag from a paired capture."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from anygear_spike.teacher_data import (
    capture_integrity,
    load_teacher_capture,
    teacher_prefilter_diagnostics,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--maximum-lag-frames", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    capture = load_teacher_capture(args.capture)
    report = teacher_prefilter_diagnostics(
        capture, maximum_lag_frames=args.maximum_lag_frames
    )
    report["source_manifest_sha256"] = sha256(
        capture.root / "manifest.json"
    )
    report["source_capture"] = capture_integrity(capture)
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    print(encoded, end="")
    if args.output is not None:
        output = args.output.resolve()
        if output.exists():
            raise ValueError(f"refusing to overwrite diagnostics: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
