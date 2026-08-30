"""Export the published SPiKE checkpoint as a fixed DirectML ONNX model."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import warnings

import onnx
import torch
from torch import nn


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "runtime" / "spike"))

from anygear_spike.spike_model import build_itop_side_model  # noqa: E402


FRAMES = 3
POINTS = 4096
REFERENCES = 128
NEIGHBORS = 32


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


class FixedPrimaryExport(nn.Module):
    """Static one-camera graph used by the realtime DirectML worker."""

    def __init__(self, model: nn.Module) -> None:
        super().__init__()
        self.model = model
        self.register_buffer(
            "source_indices",
            torch.arange(POINTS).view(1, 1, POINTS),
            persistent=False,
        )
        self.register_buffer(
            "time",
            torch.arange(1, FRAMES + 1, dtype=torch.float16).view(
                1, FRAMES, 1, 1
            ),
            persistent=False,
        )

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        points = value.reshape(FRAMES, POINTS, 3).contiguous()
        selection_points = points.float()
        valid = selection_points.square().sum(dim=-1) > 1.0e-3
        minimum_distance = torch.full_like(
            selection_points[..., 0], 1.0e10
        )
        farthest = torch.zeros(
            (FRAMES,), dtype=torch.long, device=value.device
        )
        choices = [farthest]
        for _ in range(1, REFERENCES):
            centroid = torch.gather(
                selection_points,
                1,
                farthest.view(FRAMES, 1, 1).expand(-1, 1, 3),
            )
            distance = (selection_points - centroid).square().sum(dim=-1)
            minimum_distance = torch.minimum(minimum_distance, distance)
            farthest = minimum_distance.masked_fill(~valid, -1.0).argmax(
                dim=1
            )
            choices.append(farthest)
        reference_indices = torch.stack(choices, dim=1)

        transposed = points.transpose(1, 2).contiguous()
        reference_transposed = torch.gather(
            transposed,
            2,
            reference_indices.unsqueeze(1).expand(-1, 3, -1),
        )
        references = reference_transposed.transpose(1, 2).contiguous()
        distance_squared = (
            references.float().unsqueeze(2)
            - points.float().unsqueeze(1)
        ).square().sum(dim=-1)
        inside = distance_squared < 0.2**2
        candidates = self.source_indices.expand(
            FRAMES, REFERENCES, POINTS
        ).masked_fill(~inside, POINTS)
        selected = torch.topk(
            candidates,
            k=NEIGHBORS,
            dim=-1,
            largest=False,
            sorted=True,
        ).values
        first = torch.where(
            selected[..., :1] == POINTS,
            torch.zeros_like(selected[..., :1]),
            selected[..., :1],
        )
        selected = torch.where(selected == POINTS, first, selected)
        flat_indices = selected.reshape(
            FRAMES, 1, REFERENCES * NEIGHBORS
        ).expand(-1, 3, -1)
        neighbors = torch.gather(
            transposed, 2, flat_indices
        ).reshape(FRAMES, 3, REFERENCES, NEIGHBORS)

        displacement = torch.cat(
            (
                neighbors - reference_transposed.unsqueeze(3),
                torch.zeros_like(neighbors[:, :1]),
            ),
            dim=1,
        )
        feature = self.model.stem.conv_d(displacement).max(dim=-1).values
        coordinates = references.reshape(1, FRAMES, REFERENCES, 3)
        features = feature.reshape(
            1, FRAMES, 1024, REFERENCES
        ).permute(0, 1, 3, 2).reshape(1, FRAMES * REFERENCES, 1024)
        space_time = torch.cat(
            (
                coordinates,
                self.time.expand(1, -1, REFERENCES, -1),
            ),
            dim=-1,
        ).reshape(1, FRAMES * REFERENCES, 4)
        position = self.model.pos_embed(
            space_time.permute(0, 2, 1)
        ).permute(0, 2, 1)
        output = self.model.transformer(position + features).max(dim=1).values
        return self.model.mlp_head(output)


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
    model = build_itop_side_model().eval().half()
    model.load_state_dict(source_state, strict=True)
    export_model = FixedPrimaryExport(model).eval()
    dummy = torch.zeros((1, FRAMES, POINTS, 3), dtype=torch.float16)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    warnings.filterwarnings("ignore", category=DeprecationWarning)
    warnings.filterwarnings("ignore", category=torch.jit.TracerWarning)
    with torch.inference_mode():
        torch.onnx.export(
            export_model,
            dummy,
            args.output,
            input_names=["clips"],
            output_names=["joints"],
            opset_version=20,
            dynamo=False,
            do_constant_folding=True,
        )
    onnx.checker.check_model(str(args.output), full_check=True)
    exported = onnx.load(str(args.output), load_external_data=False)
    if len(exported.graph.input) != 1 or len(exported.graph.output) != 1:
        raise RuntimeError("exported model has an unexpected interface")
    metadata = {
        "schema": "dance-around-anygear.spike-onnx.v1",
        "source_sha256": sha256(args.source),
        "output_sha256": sha256(args.output),
        "output_bytes": args.output.stat().st_size,
        "opset": 20,
        "input": [1, FRAMES, POINTS, 3],
        "input_dtype": "float16",
        "output": [1, 45],
        "runtime": "ONNX Runtime DirectML",
    }
    rendered = json.dumps(metadata, indent=2) + "\n"
    print(rendered, end="")
    if args.metadata is not None:
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
