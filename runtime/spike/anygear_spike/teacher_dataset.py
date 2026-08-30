"""Validated deterministic shards derived from paired teacher recordings."""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
import hashlib
import json
from pathlib import Path

import numpy as np

from .teacher_data import DATASET_SCHEMA, JOINT_NAMES


REQUIRED_ARRAYS = {
    "clips.npy": ("float16", (3, 4096, 3)),
    "joints_m.npy": ("float32", (15, 3)),
    "visibility.npy": ("float32", (15,)),
    "centroids_m.npy": ("float32", (3,)),
    "point_counts.npy": ("int32", (3,)),
    "pair.npy": ("int64", ()),
    "teacher_frame.npy": ("int64", ()),
    "device_frame.npy": ("int64", ()),
    "teacher_time_ns.npy": ("int64", ()),
    "delta_ns.npy": ("int64", ()),
}


@dataclass(frozen=True)
class Shard:
    root: Path
    samples: int
    files: dict[str, Path]
    session_hash: str
    session: str
    device_index: int


@dataclass(frozen=True)
class DatasetIdentity:
    manifest: Path
    manifest_sha256: str
    source_manifest_sha256: str
    source_capture_sha256: str
    session: str


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def contained_file(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise ValueError(f"dataset file escapes its root: {relative}") from error
    if not path.is_file():
        raise ValueError(f"dataset file is absent: {path}")
    return path


def _valid_sha256(value: str) -> bool:
    return len(value) == 64 and all(
        character in "0123456789ABCDEFabcdef" for character in value
    )


def _source_capture_hash(value: dict, manifest_hash: str, path: Path) -> str:
    if value.get("schema") != "dance-around-anygear.capture-integrity.v1":
        raise ValueError(f"unsupported source-capture identity: {path}")
    files = value.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError(f"source-capture file inventory is empty: {path}")
    names = []
    for item in files:
        name = str(item.get("path", ""))
        file_hash = str(item.get("sha256", ""))
        try:
            byte_count = int(item.get("bytes", -1))
        except (TypeError, ValueError) as error:
            raise ValueError(
                f"source-capture file size is invalid: {path}"
            ) from error
        pure = Path(name)
        if not name or pure.is_absolute() or "\\" in name or (
            ".." in pure.parts
        ) or byte_count < 0 or not _valid_sha256(file_hash):
            raise ValueError(f"source-capture file entry is invalid: {path}")
        names.append(name)
    if names != sorted(names) or len(names) != len(set(names)):
        raise ValueError(f"source-capture file order is invalid: {path}")
    manifest_entries = [
        item for item in files if item["path"] == "manifest.json"
    ]
    if len(manifest_entries) != 1 or (
        str(manifest_entries[0]["sha256"]).upper() != manifest_hash
    ):
        raise ValueError(f"source manifest is absent from capture identity: {path}")
    encoded = json.dumps(
        files, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    computed = hashlib.sha256(encoded).hexdigest().upper()
    declared = str(value.get("sha256", ""))
    if not _valid_sha256(declared) or declared.upper() != computed:
        raise ValueError(f"source-capture aggregate hash mismatch: {path}")
    return computed


def load_dataset_manifests(
    paths: list[Path], verify_hashes: bool = True
) -> tuple[list[Shard], list[DatasetIdentity]]:
    shards = []
    identities = []
    seen_manifests = set()
    seen_sessions = set()
    for path_value in paths:
        path = path_value.resolve()
        if path in seen_manifests:
            raise ValueError(f"teacher dataset was supplied twice: {path}")
        seen_manifests.add(path)
        if not path.is_file():
            raise ValueError(f"teacher dataset manifest is absent: {path}")
        root = path.parent
        value = json.loads(path.read_text(encoding="utf-8"))
        if value.get("schema") != DATASET_SCHEMA:
            raise ValueError(f"unsupported teacher dataset schema: {path}")
        if tuple(value.get("joint_order", [])) != JOINT_NAMES:
            raise ValueError(f"teacher joint order differs from SPiKE: {path}")
        if int(value.get("frames_per_clip", 0)) != 3 or (
            int(value.get("points_per_frame", 0)) != 4096
        ):
            raise ValueError(
                f"teacher dataset has an incompatible input shape: {path}"
            )
        source_hash = str(value["source_manifest_sha256"])
        if not _valid_sha256(source_hash):
            raise ValueError(f"invalid source-manifest hash: {path}")
        source_hash = source_hash.upper()
        capture_hash = _source_capture_hash(
            value.get("source_capture", {}), source_hash, path
        )
        if capture_hash in seen_sessions:
            raise ValueError(
                f"capture session was supplied more than once: {path}"
            )
        seen_sessions.add(capture_hash)
        session = str(value["source_session"])
        identities.append(
            DatasetIdentity(
                manifest=path,
                manifest_sha256=sha256(path),
                source_manifest_sha256=source_hash,
                source_capture_sha256=capture_hash,
                session=session,
            )
        )
        devices = value.get("devices", [])
        if tuple(int(device["index"]) for device in devices) != (0, 1):
            raise ValueError(
                f"teacher dataset must contain devices 0 and 1: {path}"
            )
        for device in devices:
            device_index = int(device["index"])
            device_samples = 0
            for shard_value in device["shards"]:
                directory = (
                    root
                    / f"device-{device_index}"
                    / shard_value["directory"]
                ).resolve()
                try:
                    directory.relative_to(root)
                except ValueError as error:
                    raise ValueError(
                        "shard directory escapes dataset root"
                    ) from error
                files = {}
                metadata = shard_value["files"]
                shard_samples = int(shard_value["samples"])
                if shard_samples <= 0:
                    raise ValueError(f"teacher shard is empty: {directory}")
                if set(metadata) != set(REQUIRED_ARRAYS):
                    raise ValueError(
                        f"shard file set is incomplete: {directory}"
                    )
                for name, expected in REQUIRED_ARRAYS.items():
                    array_path = contained_file(directory, name)
                    file_metadata = metadata[name]
                    if int(file_metadata["bytes"]) != array_path.stat().st_size:
                        raise ValueError(
                            f"teacher shard size mismatch: {array_path}"
                        )
                    expected_hash = str(file_metadata["sha256"])
                    if not _valid_sha256(expected_hash):
                        raise ValueError(
                            f"teacher shard has an invalid hash: {array_path}"
                        )
                    if verify_hashes and sha256(array_path) != (
                        expected_hash.upper()
                    ):
                        raise ValueError(
                            f"teacher shard hash mismatch: {array_path}"
                        )
                    array = np.load(
                        array_path, mmap_mode="r", allow_pickle=False
                    )
                    expected_shape = (shard_samples, *expected[1])
                    if str(array.dtype) != expected[0] or (
                        array.shape != expected_shape
                    ):
                        raise ValueError(
                            f"teacher shard array contract mismatch: {array_path}"
                        )
                    if str(file_metadata["dtype"]) != str(array.dtype) or (
                        tuple(file_metadata["shape"]) != array.shape
                    ):
                        raise ValueError(
                            f"teacher shard metadata mismatch: {array_path}"
                        )
                    files[name] = array_path
                shards.append(
                    Shard(
                        root=directory,
                        samples=shard_samples,
                        files=files,
                        session_hash=capture_hash,
                        session=session,
                        device_index=device_index,
                    )
                )
                device_samples += shard_samples
            if device_samples != int(device["samples"]):
                raise ValueError(
                    f"teacher device sample count mismatch: device {device_index}"
                )
            if device_samples <= 0:
                raise ValueError(
                    f"teacher device contains no samples: device {device_index}"
                )
    if not shards:
        raise ValueError("no teacher shards were found")
    return shards, identities


@lru_cache(maxsize=12)
def load_shard_arrays(root: str) -> dict[str, np.ndarray]:
    directory = Path(root)
    return {
        name.removesuffix(".npy"): np.load(
            directory / name, mmap_mode="r", allow_pickle=False
        )
        for name in REQUIRED_ARRAYS
    }
