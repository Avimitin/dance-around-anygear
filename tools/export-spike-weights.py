"""Export the published SPiKE checkpoint as an inference-only FP16 state."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import torch


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()

    checkpoint = torch.load(
        args.source, map_location="cpu", weights_only=True
    )
    source_state = checkpoint.get("model", checkpoint)
    runtime_state = {
        name: (
            value.detach().half().contiguous()
            if torch.is_floating_point(value)
            else value.detach().contiguous()
        )
        for name, value in source_state.items()
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    torch.save(runtime_state, args.output)
    verified = torch.load(
        args.output, map_location="cpu", weights_only=True
    )
    if list(verified) != list(runtime_state):
        raise RuntimeError("runtime state verification failed")

    metadata = {
        "schema": "dance-around-anygear.spike-weights.v1",
        "source_sha256": sha256(args.source),
        "output_sha256": sha256(args.output),
        "output_bytes": args.output.stat().st_size,
        "tensor_count": len(runtime_state),
        "floating_dtype": "float16",
        "optimizer_included": False,
    }
    rendered = json.dumps(metadata, indent=2) + "\n"
    print(rendered, end="")
    if args.metadata is not None:
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
