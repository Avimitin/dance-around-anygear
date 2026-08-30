"""Fine-tune the portable SPiKE model on checked D430 teacher shards."""

from __future__ import annotations

import argparse
from bisect import bisect_right
import json
import math
from pathlib import Path
import random
import time

import numpy as np
import torch
import torch.nn.functional as functional
from torch import nn
from torch.utils.data import DataLoader, Dataset

from anygear_spike.spike_model import build_itop_side_model
from anygear_spike.teacher_data import JOINT_NAMES
from anygear_spike.teacher_dataset import (
    Shard,
    load_dataset_manifests,
    load_shard_arrays,
    sha256,
)


BONES = (
    (0, 1), (1, 2), (1, 3),
    (2, 4), (3, 5), (4, 6), (5, 7),
    (1, 8), (8, 9), (8, 10),
    (9, 11), (10, 12), (11, 13), (12, 14),
)
LEFT_RIGHT_PAIRS = ((2, 3), (4, 5), (6, 7), (9, 10), (11, 12), (13, 14))
ENDPOINTS = (6, 7, 13, 14)


class TeacherDataset(Dataset):
    def __init__(self, shards: list[Shard], minimum_visibility: float):
        self.shards = shards
        self.minimum_visibility = minimum_visibility
        self.cumulative = []
        total = 0
        for shard in shards:
            total += shard.samples
            self.cumulative.append(total)

    def __len__(self) -> int:
        return self.cumulative[-1]

    def __getitem__(self, index: int) -> dict[str, torch.Tensor]:
        if index < 0:
            index += len(self)
        shard_index = bisect_right(self.cumulative, index)
        if shard_index >= len(self.shards):
            raise IndexError(index)
        first = self.cumulative[shard_index - 1] if shard_index else 0
        local = index - first
        shard = self.shards[shard_index]
        arrays = load_shard_arrays(str(shard.root))
        previous = local
        temporal_valid = False
        if local > 0 and (
            int(arrays["device_frame"][local])
            == int(arrays["device_frame"][local - 1]) + 1
        ):
            previous = local - 1
            temporal_valid = True

        def tensor(name: str, sample: int, dtype: torch.dtype) -> torch.Tensor:
            # Copy a single read-only memmap slice into ordinary writable RAM.
            return torch.as_tensor(
                np.array(arrays[name][sample], copy=True), dtype=dtype
            )

        visibility = tensor("visibility", local, torch.float32)
        previous_visibility = tensor("visibility", previous, torch.float32)
        return {
            "clip": tensor("clips", local, torch.float32),
            "joints": tensor("joints_m", local, torch.float32),
            "mask": visibility >= self.minimum_visibility,
            "previous_clip": tensor("clips", previous, torch.float32),
            "previous_joints": tensor("joints_m", previous, torch.float32),
            "previous_mask": previous_visibility >= self.minimum_visibility,
            "temporal_valid": torch.tensor(temporal_valid, dtype=torch.bool),
        }


def masked_mean(value: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    weights = mask.to(dtype=value.dtype)
    return (value * weights).sum() / weights.sum().clamp_min(1.0)


def training_loss(
    prediction: torch.Tensor,
    target: torch.Tensor,
    mask: torch.Tensor,
    previous_prediction: torch.Tensor,
    previous_target: torch.Tensor,
    previous_mask: torch.Tensor,
    temporal_valid: torch.Tensor,
) -> tuple[torch.Tensor, dict[str, torch.Tensor]]:
    joint_error = functional.smooth_l1_loss(
        prediction, target, beta=0.05, reduction="none"
    ).mean(dim=2)
    joint_weights = torch.ones(
        prediction.shape[1], dtype=prediction.dtype, device=prediction.device
    )
    joint_weights[list(ENDPOINTS)] = 1.5
    position = masked_mean(joint_error * joint_weights, mask)

    bone_losses = []
    bone_masks = []
    for first, second in BONES:
        predicted_length = torch.linalg.vector_norm(
            prediction[:, first] - prediction[:, second], dim=1
        )
        target_length = torch.linalg.vector_norm(
            target[:, first] - target[:, second], dim=1
        )
        bone_losses.append(torch.abs(predicted_length - target_length))
        bone_masks.append(mask[:, first] & mask[:, second])
    bone = masked_mean(
        torch.stack(bone_losses, dim=1), torch.stack(bone_masks, dim=1)
    )

    velocity_error = functional.smooth_l1_loss(
        prediction - previous_prediction,
        target - previous_target,
        beta=0.05,
        reduction="none",
    ).mean(dim=2)
    velocity_mask = mask & previous_mask & temporal_valid[:, None]
    velocity = masked_mean(velocity_error, velocity_mask)

    identity_losses = []
    identity_masks = []
    for left, right in LEFT_RIGHT_PAIRS:
        correct = torch.linalg.vector_norm(
            prediction[:, left] - target[:, left], dim=1
        ) + torch.linalg.vector_norm(
            prediction[:, right] - target[:, right], dim=1
        )
        swapped = torch.linalg.vector_norm(
            prediction[:, left] - target[:, right], dim=1
        ) + torch.linalg.vector_norm(
            prediction[:, right] - target[:, left], dim=1
        )
        identity_losses.append(functional.relu(0.05 + correct - swapped))
        identity_masks.append(mask[:, left] & mask[:, right])
    identity = masked_mean(
        torch.stack(identity_losses, dim=1),
        torch.stack(identity_masks, dim=1),
    )
    total = position + 0.25 * bone + 0.20 * velocity + 0.10 * identity
    return total, {
        "position": position,
        "bone": bone,
        "velocity": velocity,
        "identity": identity,
    }


class Metrics:
    def __init__(self) -> None:
        self.error_sum = 0.0
        self.joint_count = 0
        self.correct_10cm = 0
        self.endpoint_error_sum = 0.0
        self.endpoint_count = 0
        self.swap_count = 0
        self.swap_total = 0
        self.velocity_error_sum = 0.0
        self.velocity_count = 0

    def update(
        self,
        prediction: torch.Tensor,
        target: torch.Tensor,
        mask: torch.Tensor,
        previous_prediction: torch.Tensor,
        previous_target: torch.Tensor,
        previous_mask: torch.Tensor,
        temporal_valid: torch.Tensor,
    ) -> None:
        distance = torch.linalg.vector_norm(prediction - target, dim=2)
        self.error_sum += float(distance[mask].sum())
        self.joint_count += int(mask.sum())
        self.correct_10cm += int(((distance < 0.10) & mask).sum())
        endpoint_mask = mask[:, list(ENDPOINTS)]
        self.endpoint_error_sum += float(
            distance[:, list(ENDPOINTS)][endpoint_mask].sum()
        )
        self.endpoint_count += int(endpoint_mask.sum())
        for left, right in LEFT_RIGHT_PAIRS:
            valid = mask[:, left] & mask[:, right]
            correct = torch.linalg.vector_norm(
                prediction[:, left] - target[:, left], dim=1
            ) + torch.linalg.vector_norm(
                prediction[:, right] - target[:, right], dim=1
            )
            swapped = torch.linalg.vector_norm(
                prediction[:, left] - target[:, right], dim=1
            ) + torch.linalg.vector_norm(
                prediction[:, right] - target[:, left], dim=1
            )
            self.swap_count += int(((swapped < correct) & valid).sum())
            self.swap_total += int(valid.sum())
        temporal_mask = mask & previous_mask & temporal_valid[:, None]
        velocity_error = torch.linalg.vector_norm(
            (prediction - previous_prediction) -
            (target - previous_target),
            dim=2,
        )
        self.velocity_error_sum += float(velocity_error[temporal_mask].sum())
        self.velocity_count += int(temporal_mask.sum())

    def result(self) -> dict[str, float | int | None]:
        return {
            "mpjpe_m": self.error_sum / self.joint_count
            if self.joint_count else None,
            "pck_10cm": self.correct_10cm / self.joint_count
            if self.joint_count else None,
            "endpoint_error_m": self.endpoint_error_sum / self.endpoint_count
            if self.endpoint_count else None,
            "left_right_swap_fraction": self.swap_count / self.swap_total
            if self.swap_total else None,
            "velocity_error_m_per_frame": (
                self.velocity_error_sum / self.velocity_count
                if self.velocity_count else None
            ),
            "tracked_joint_count": self.joint_count,
        }


def move_batch(batch: dict, device: torch.device) -> dict:
    return {
        name: value.to(device, non_blocking=True)
        for name, value in batch.items()
    }


def run_epoch(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    optimizer: torch.optim.Optimizer | None,
    scaler,
) -> dict:
    training = optimizer is not None
    model.train(training)
    metrics = Metrics()
    loss_sums = {name: 0.0 for name in ("total", "position", "bone", "velocity", "identity")}
    batches = 0
    started = time.perf_counter()
    context = torch.enable_grad if training else torch.inference_mode
    with context():
        for batch in loader:
            batch = move_batch(batch, device)
            if training:
                optimizer.zero_grad(set_to_none=True)
            with torch.autocast(
                device_type=device.type,
                dtype=torch.float16,
                enabled=device.type == "cuda",
            ):
                combined = torch.cat(
                    (batch["clip"], batch["previous_clip"]), dim=0
                )
                combined_prediction = model(combined).reshape(-1, 15, 3)
                prediction, previous_prediction = combined_prediction.chunk(2)
                total, parts = training_loss(
                    prediction,
                    batch["joints"],
                    batch["mask"],
                    previous_prediction,
                    batch["previous_joints"],
                    batch["previous_mask"],
                    batch["temporal_valid"],
                )
            if training:
                scaler.scale(total).backward()
                scaler.unscale_(optimizer)
                torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                scaler.step(optimizer)
                scaler.update()
            loss_sums["total"] += float(total.detach())
            for name, value in parts.items():
                loss_sums[name] += float(value.detach())
            metrics.update(
                prediction.detach(),
                batch["joints"],
                batch["mask"],
                previous_prediction.detach(),
                batch["previous_joints"],
                batch["previous_mask"],
                batch["temporal_valid"],
            )
            batches += 1
    result = metrics.result()
    result.update(
        {name + "_loss": value / max(1, batches)
         for name, value in loss_sums.items()}
    )
    result["seconds"] = time.perf_counter() - started
    result["batches"] = batches
    return result


def set_reproducible_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    torch.use_deterministic_algorithms(True)
    if hasattr(torch.backends, "cuda"):
        torch.backends.cuda.matmul.allow_tf32 = False
        # The memory-efficient SDPA backward kernel is nondeterministic on the
        # target Windows/CUDA stack. Training uses the deterministic math
        # implementation; this does not change the released ONNX graph.
        torch.backends.cuda.enable_flash_sdp(False)
        torch.backends.cuda.enable_mem_efficient_sdp(False)
        torch.backends.cuda.enable_math_sdp(True)
    if hasattr(torch.backends, "cudnn"):
        torch.backends.cudnn.allow_tf32 = False
        torch.backends.cudnn.benchmark = False
        torch.backends.cudnn.deterministic = True


def save_checkpoint(
    path: Path,
    model: nn.Module,
    optimizer,
    scheduler,
    scaler,
    data_generator: torch.Generator,
    epoch: int,
    best_validation_mpjpe: float,
    arguments: dict,
    datasets: dict,
) -> None:
    temporary = path.with_suffix(path.suffix + ".partial")
    torch.save(
        {
            "schema": "dance-around-anygear.spike-finetune.v1",
            "model": model.state_dict(),
            "optimizer": optimizer.state_dict(),
            "scheduler": scheduler.state_dict(),
            "scaler": scaler.state_dict(),
            "data_generator_state": data_generator.get_state(),
            "epoch": epoch,
            "best_validation_mpjpe": best_validation_mpjpe,
            "arguments": arguments,
            "datasets": datasets,
        },
        temporary,
    )
    temporary.replace(path)


def self_test() -> int:
    target = torch.zeros((2, 15, 3), dtype=torch.float32)
    for pair_index, (left, right) in enumerate(LEFT_RIGHT_PAIRS, 1):
        target[:, left, 0] = -0.1 * pair_index
        target[:, right, 0] = 0.1 * pair_index
    mask = torch.ones((2, 15), dtype=torch.bool)
    temporal = torch.tensor((False, True), dtype=torch.bool)
    perfect, _ = training_loss(
        target, target, mask, target, target, mask, temporal
    )
    damaged = target.clone()
    damaged[:, 6] += 0.4
    wrong, _ = training_loss(
        damaged, target, mask, target, target, mask, temporal
    )
    hidden = mask.clone()
    hidden[:, 6] = False
    masked, _ = training_loss(
        damaged, target, hidden, target, target, hidden, temporal
    )
    if not math.isclose(float(perfect), 0.0, abs_tol=1e-7):
        raise RuntimeError("perfect pose has nonzero training loss")
    if float(wrong) <= float(perfect) or float(masked) >= float(wrong):
        raise RuntimeError("masked teacher loss self-test failed")
    print("PASS: SPiKE teacher training losses and masks")
    return 0


def model_self_test(checkpoint_path: Path, device_name: str) -> int:
    device = torch.device(device_name)
    if device.type == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA model self-test selected without CUDA support")
    set_reproducible_seed(0)
    checkpoint = torch.load(
        checkpoint_path.resolve(), map_location="cpu", weights_only=True
    )
    model = build_itop_side_model()
    model.load_state_dict(checkpoint.get("model", checkpoint), strict=True)
    model.to(device).train()
    optimizer = torch.optim.AdamW(model.parameters(), lr=1.0e-5)
    generator = torch.Generator(device=device).manual_seed(0)
    clips = torch.rand(
        (2, 3, 4096, 3), generator=generator, device=device
    )
    clips[..., 0] = (clips[..., 0] - 0.5) * 1.2
    clips[..., 1] = (clips[..., 1] - 0.5) * 1.8
    clips[..., 2] = clips[..., 2] * 0.8 + 1.5
    target = torch.zeros((2, 15, 3), device=device)
    if device.type == "cuda":
        torch.cuda.reset_peak_memory_stats(device)
        torch.cuda.synchronize(device)
    started = time.perf_counter()
    optimizer.zero_grad(set_to_none=True)
    with torch.autocast(
        device_type=device.type,
        dtype=torch.float16,
        enabled=device.type == "cuda",
    ):
        prediction = model(clips).reshape(2, 15, 3)
        loss = functional.smooth_l1_loss(prediction, target)
    loss.backward()
    torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
    optimizer.step()
    if device.type == "cuda":
        torch.cuda.synchronize(device)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    output = {
        "device": str(device),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "effective_model_batch": 2,
        "elapsed_ms": elapsed_ms,
        "loss": float(loss.detach()),
        "peak_allocated_mib": (
            torch.cuda.max_memory_allocated(device) / (1024 * 1024)
            if device.type == "cuda" else None
        ),
        "peak_reserved_mib": (
            torch.cuda.max_memory_reserved(device) / (1024 * 1024)
            if device.type == "cuda" else None
        ),
    }
    print(json.dumps(output, indent=2))
    if not math.isfinite(output["loss"]):
        raise RuntimeError("full SPiKE training step produced a non-finite loss")
    print("PASS: full SPiKE forward/backward/optimizer step")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train", type=Path, nargs="+")
    parser.add_argument("--validation", type=Path, nargs="+")
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument("--learning-rate", type=float, default=1.0e-4)
    parser.add_argument("--weight-decay", type=float, default=1.0e-4)
    parser.add_argument("--minimum-visibility", type=float, default=0.99)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--skip-hash-check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--model-self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.model_self_test:
        if args.checkpoint is None:
            parser.error("--model-self-test requires --checkpoint")
        return model_self_test(args.checkpoint, args.device)
    if not args.train or not args.validation or args.checkpoint is None or (
        args.output is None
    ):
        parser.error("training requires --train, --validation, --checkpoint, and --output")
    if args.epochs < 1 or args.batch_size < 1 or args.workers < 0:
        parser.error("epochs/batch size must be positive and workers nonnegative")
    if not math.isfinite(args.learning_rate) or args.learning_rate <= 0.0:
        parser.error("learning rate must be positive and finite")
    if not math.isfinite(args.weight_decay) or args.weight_decay < 0.0:
        parser.error("weight decay must be nonnegative and finite")
    if not 0.0 <= args.minimum_visibility <= 1.0:
        parser.error("minimum visibility must be inside 0..1")

    output = args.output.resolve()
    resume_path = args.resume.resolve() if args.resume is not None else None
    if output.exists():
        if not output.is_dir() or (any(output.iterdir()) and resume_path is None):
            raise ValueError("training output exists and is not empty")
    else:
        if resume_path is not None:
            raise ValueError("resume requires its existing training output directory")
        output.mkdir(parents=True)
    if resume_path is not None and resume_path.parent != output:
        raise ValueError("resume checkpoint must be inside the selected output directory")
    verify_hashes = not args.skip_hash_check
    train_shards, train_identity = load_dataset_manifests(
        args.train, verify_hashes
    )
    validation_shards, validation_identity = load_dataset_manifests(
        args.validation, verify_hashes
    )
    train_sessions = {item.source_capture_sha256 for item in train_identity}
    validation_sessions = {
        item.source_capture_sha256 for item in validation_identity
    }
    overlap = train_sessions & validation_sessions
    if overlap:
        raise ValueError(
            "training and validation contain the same capture session: "
            + ", ".join(sorted(overlap))
        )

    device = torch.device(args.device)
    if device.type == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA training was selected but PyTorch cannot use CUDA")
    set_reproducible_seed(args.seed)
    train_dataset = TeacherDataset(train_shards, args.minimum_visibility)
    validation_dataset = TeacherDataset(
        validation_shards, args.minimum_visibility
    )
    generator = torch.Generator().manual_seed(args.seed)
    loader_options = {
        "batch_size": args.batch_size,
        "num_workers": args.workers,
        "pin_memory": device.type == "cuda",
        "persistent_workers": args.workers > 0,
    }
    train_loader = DataLoader(
        train_dataset, shuffle=True, generator=generator, **loader_options
    )
    validation_loader = DataLoader(
        validation_dataset, shuffle=False, **loader_options
    )

    checkpoint_path = args.checkpoint.resolve()
    published = torch.load(
        checkpoint_path, map_location="cpu", weights_only=True
    )
    model = build_itop_side_model()
    model.load_state_dict(published.get("model", published), strict=True)
    model.to(device)
    optimizer = torch.optim.AdamW(
        model.parameters(), lr=args.learning_rate,
        weight_decay=args.weight_decay
    )
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=args.epochs
    )
    scaler = torch.amp.GradScaler("cuda", enabled=device.type == "cuda")
    start_epoch = 0
    best_validation_mpjpe = math.inf
    if resume_path is not None:
        resume = torch.load(
            resume_path, map_location="cpu", weights_only=True
        )
        if resume.get("schema") != "dance-around-anygear.spike-finetune.v1":
            raise ValueError("unsupported fine-tuning checkpoint schema")
        model.load_state_dict(resume["model"], strict=True)
        optimizer.load_state_dict(resume["optimizer"])
        scheduler.load_state_dict(resume["scheduler"])
        scaler.load_state_dict(resume["scaler"])
        generator.set_state(resume["data_generator_state"])
        start_epoch = int(resume["epoch"]) + 1
        best_validation_mpjpe = float(resume["best_validation_mpjpe"])

    dataset_metadata = {
        "train": [item.__dict__ | {"manifest": str(item.manifest.name)}
                  for item in train_identity],
        "validation": [item.__dict__ | {"manifest": str(item.manifest.name)}
                       for item in validation_identity],
    }
    # Path objects are replaced by names above; retain no site-specific path.
    for split in dataset_metadata.values():
        for item in split:
            item["manifest"] = Path(item["manifest"]).name
    arguments = {
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "learning_rate": args.learning_rate,
        "weight_decay": args.weight_decay,
        "minimum_visibility": args.minimum_visibility,
        "seed": args.seed,
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "source_checkpoint_sha256": sha256(checkpoint_path),
    }
    history_path = output / "history.jsonl"
    if resume_path is not None:
        if resume.get("arguments") != arguments or (
            resume.get("datasets") != dataset_metadata
        ):
            raise ValueError("resume checkpoint arguments or datasets differ")
        if not history_path.is_file():
            raise ValueError("resume history is absent")
        history_lines = [
            line for line in history_path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        if not history_lines or int(json.loads(history_lines[-1])["epoch"]) != (
            start_epoch - 1
        ):
            raise ValueError("resume history does not end at the checkpoint epoch")
        if start_epoch >= args.epochs:
            raise ValueError("training run has already reached the requested epoch count")
        if not (output / "best-model-state.pth").is_file():
            raise ValueError("resume output has no retained best model state")
    for epoch in range(start_epoch, args.epochs):
        train_result = run_epoch(
            model, train_loader, device, optimizer, scaler
        )
        validation_result = run_epoch(
            model, validation_loader, device, None, scaler
        )
        scheduler.step()
        entry = {
            "epoch": epoch,
            "learning_rate": optimizer.param_groups[0]["lr"],
            "train": train_result,
            "validation": validation_result,
        }
        with history_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(entry, sort_keys=True) + "\n")
        validation_mpjpe = validation_result["mpjpe_m"]
        if validation_mpjpe is None:
            raise RuntimeError("validation produced no tracked joint metrics")
        save_checkpoint(
            output / "latest.pth",
            model,
            optimizer,
            scheduler,
            scaler,
            generator,
            epoch,
            min(best_validation_mpjpe, float(validation_mpjpe)),
            arguments,
            dataset_metadata,
        )
        if float(validation_mpjpe) < best_validation_mpjpe:
            best_validation_mpjpe = float(validation_mpjpe)
            save_checkpoint(
                output / "best.pth",
                model,
                optimizer,
                scheduler,
                scaler,
                generator,
                epoch,
                best_validation_mpjpe,
                arguments,
                dataset_metadata,
            )
            torch.save(model.state_dict(), output / "best-model-state.pth")
        print(json.dumps(entry, indent=2))

    summary = {
        "schema": "dance-around-anygear.spike-finetune-summary.v1",
        "best_validation_mpjpe_m": best_validation_mpjpe,
        "best_checkpoint": "best.pth",
        "best_model_state": "best-model-state.pth",
        "best_model_state_sha256": sha256(output / "best-model-state.pth"),
        "history": "history.jsonl",
        "arguments": arguments,
        "datasets": dataset_metadata,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"[OK] best validation MPJPE: {best_validation_mpjpe * 1000:.2f} mm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
