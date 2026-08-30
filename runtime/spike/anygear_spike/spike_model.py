"""Portable inference form of the SPiKE ITOP-SIDE network.

The network layout is adapted from SPiKE at the revision recorded in
``dependency-lock.json``.  It keeps the published parameter names so the
published checkpoint can be loaded directly, while replacing the optional
PointNet++ extension with fixed-shape PyTorch operators.
"""

from __future__ import annotations

import torch
import torch.nn.functional as functional
from torch import nn


def _furthest_point_sample(points: torch.Tensor, count: int) -> torch.Tensor:
    """Point-zero-seeded FPS used by the published PointNet++ operator."""
    batch, point_count, dimensions = points.shape
    if dimensions != 3 or not 0 < count <= point_count:
        raise ValueError("invalid point cloud shape or sample count")
    selection_points = points.float()
    indices = torch.zeros(
        (batch, count), dtype=torch.long, device=points.device
    )
    minimum_distance = torch.full(
        (batch, point_count),
        1.0e10,
        dtype=torch.float32,
        device=points.device,
    )
    valid = selection_points.square().sum(dim=-1) > 1.0e-3
    batch_indices = torch.arange(batch, device=points.device)
    farthest = torch.zeros(batch, dtype=torch.long, device=points.device)
    for sample in range(1, count):
        centroid = selection_points[batch_indices, farthest].unsqueeze(1)
        distance = (selection_points - centroid).square().sum(dim=-1)
        minimum_distance = torch.minimum(minimum_distance, distance)
        farthest = minimum_distance.masked_fill(~valid, -1.0).argmax(dim=1)
        indices[:, sample] = farthest
    return indices


def _gather_points(
    features: torch.Tensor, indices: torch.Tensor
) -> torch.Tensor:
    expanded = indices.unsqueeze(1).expand(-1, features.shape[1], -1)
    return torch.gather(features, 2, expanded)


def _ball_query(
    points: torch.Tensor,
    references: torch.Tensor,
    radius: float,
    count: int,
) -> torch.Tensor:
    distance_squared = (
        references.float().unsqueeze(2) - points.float().unsqueeze(1)
    ).square().sum(dim=-1)
    inside = distance_squared < radius * radius
    point_count = points.shape[1]
    source_indices = torch.arange(
        point_count, dtype=torch.long, device=points.device
    ).view(1, 1, point_count)
    candidates = source_indices.expand_as(inside).masked_fill(
        ~inside, point_count
    )
    selected = torch.topk(
        candidates, k=count, dim=-1, largest=False, sorted=True
    ).values
    first = torch.where(
        selected[..., :1] == point_count,
        torch.zeros_like(selected[..., :1]),
        selected[..., :1],
    )
    return torch.where(selected == point_count, first, selected)


def _group_points(
    features: torch.Tensor, indices: torch.Tensor
) -> torch.Tensor:
    batch, channels, _ = features.shape
    _, references, neighbors = indices.shape
    flat = indices.reshape(batch, 1, references * neighbors).expand(
        -1, channels, -1
    )
    return torch.gather(features, 2, flat).reshape(
        batch, channels, references, neighbors
    )


class PointSpatialConv(nn.Module):
    def __init__(
        self,
        radius: float,
        neighbors: int,
        stride: int,
        dimensions: int,
    ) -> None:
        super().__init__()
        self.spatial_kernel_size = radius
        self.nsamples = neighbors
        self.spatial_stride = stride
        self.conv_d = nn.Sequential(
            nn.Conv2d(4, dimensions, 1, bias=False)
        )
        # The published ITOP configuration has one MLP channel, so this layer
        # is intentionally an identity while retaining its state-dict path.
        self.mlp = nn.Sequential()

    def forward(
        self, xyzs: torch.Tensor
    ) -> tuple[torch.Tensor, torch.Tensor]:
        if xyzs.ndim != 4 or xyzs.shape[-1] != 3:
            raise ValueError("xyzs must have shape [batch, frames, points, 3]")
        batch, frames, point_count, _ = xyzs.shape
        reference_count = point_count // self.spatial_stride
        points = xyzs.reshape(batch * frames, point_count, 3).contiguous()
        reference_indices = _furthest_point_sample(points, reference_count)
        transposed = points.transpose(1, 2).contiguous()
        references_transposed = _gather_points(
            transposed, reference_indices
        )
        references = references_transposed.transpose(1, 2).contiguous()
        neighbor_indices = _ball_query(
            points, references, self.spatial_kernel_size, self.nsamples
        )
        neighbors = _group_points(transposed, neighbor_indices)
        time_offset = torch.zeros(
            (batch * frames, 1, reference_count, self.nsamples),
            dtype=xyzs.dtype,
            device=xyzs.device,
        )
        displacement = torch.cat(
            (neighbors - references_transposed.unsqueeze(3), time_offset),
            dim=1,
        )
        feature = self.conv_d(displacement)
        feature = torch.max(self.mlp(feature), dim=-1).values
        return (
            references.reshape(batch, frames, reference_count, 3),
            feature.reshape(
                batch, frames, feature.shape[1], reference_count
            ),
        )


class Residual(nn.Module):
    def __init__(self, function: nn.Module) -> None:
        super().__init__()
        self.fn = function

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.fn(value) + value


class PreNorm(nn.Module):
    def __init__(self, dimensions: int, function: nn.Module) -> None:
        super().__init__()
        self.norm = nn.LayerNorm(dimensions)
        self.fn = function

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.fn(self.norm(value))


class FeedForward(nn.Module):
    def __init__(
        self, dimensions: int, hidden_dimensions: int, dropout: float
    ) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(dimensions, hidden_dimensions),
            nn.GELU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_dimensions, dimensions),
            nn.Dropout(dropout),
        )

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.net(value)


class Attention(nn.Module):
    def __init__(
        self,
        dimensions: int,
        heads: int,
        head_dimensions: int,
        dropout: float,
    ) -> None:
        super().__init__()
        inner_dimensions = head_dimensions * heads
        self.heads = heads
        self.scale = head_dimensions**-0.5
        self.to_qkv = nn.Linear(dimensions, inner_dimensions * 3, bias=False)
        self.to_out = nn.Sequential(
            nn.Linear(inner_dimensions, dimensions),
            nn.GELU(),
            nn.Dropout(dropout),
        )

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        batch, tokens, _ = value.shape
        query, key, content = self.to_qkv(value).chunk(3, dim=-1)
        head_dimensions = query.shape[-1] // self.heads

        def split_heads(tensor: torch.Tensor) -> torch.Tensor:
            return tensor.reshape(
                batch, tokens, self.heads, head_dimensions
            ).permute(0, 2, 1, 3)

        attended = functional.scaled_dot_product_attention(
            split_heads(query),
            split_heads(key),
            split_heads(content),
            dropout_p=0.0,
            scale=self.scale,
        )
        attended = attended.permute(0, 2, 1, 3).reshape(
            batch, tokens, self.heads * head_dimensions
        )
        return self.to_out(attended)


class Transformer(nn.Module):
    def __init__(
        self,
        dimensions: int,
        depth: int,
        heads: int,
        head_dimensions: int,
        mlp_dimensions: int,
        dropout: float,
    ) -> None:
        super().__init__()
        self.layers = nn.ModuleList(
            [
                nn.ModuleList(
                    [
                        Residual(
                            PreNorm(
                                dimensions,
                                Attention(
                                    dimensions,
                                    heads,
                                    head_dimensions,
                                    dropout,
                                ),
                            )
                        ),
                        Residual(
                            PreNorm(
                                dimensions,
                                FeedForward(
                                    dimensions, mlp_dimensions, dropout
                                ),
                            )
                        ),
                    ]
                )
                for _ in range(depth)
            ]
        )

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        for attention, feed_forward in self.layers:
            value = attention(value)
            value = feed_forward(value)
        return value


class SPiKE(nn.Module):
    def __init__(self, joint_coordinates: int = 45) -> None:
        super().__init__()
        dimensions = 1024
        self.stem = PointSpatialConv(0.2, 32, 32, dimensions)
        self.pos_embed = nn.Conv1d(4, dimensions, 1, bias=True)
        self.transformer = Transformer(
            dimensions, 5, 8, 256, 2048, 0.0
        )
        self.mlp_head = nn.Sequential(
            nn.LayerNorm(dimensions),
            nn.Linear(dimensions, 2048),
            nn.GELU(),
            nn.Dropout(0.0),
            nn.Linear(2048, joint_coordinates),
        )

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        coordinates, features = self.stem(value)
        batch, frames, points, _ = coordinates.shape
        time = torch.arange(
            1,
            frames + 1,
            device=value.device,
            dtype=coordinates.dtype,
        ).view(1, frames, 1, 1).expand(batch, -1, points, -1)
        space_time = torch.cat((coordinates, time), dim=-1).reshape(
            batch, frames * points, 4
        )
        features = features.permute(0, 1, 3, 2).reshape(
            batch, frames * points, features.shape[2]
        )
        position = self.pos_embed(space_time.permute(0, 2, 1)).permute(
            0, 2, 1
        )
        output = self.transformer(position + features)
        return self.mlp_head(torch.max(output, dim=1).values)


def build_itop_side_model() -> SPiKE:
    return SPiKE(joint_coordinates=45)
