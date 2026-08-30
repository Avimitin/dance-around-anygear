"""Fixed-shape SPiKE inference through ONNX Runtime DirectML."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnxruntime as ort


class RealtimeSpikeModel:
    frames_per_clip = 3
    point_count = 4096

    def __init__(
        self,
        model_path: Path,
        device_index: int = 0,
        warmup_runs: int = 3,
    ) -> None:
        if not model_path.is_file():
            raise FileNotFoundError(model_path)
        available = ort.get_available_providers()
        if "DmlExecutionProvider" not in available:
            raise RuntimeError(
                "the ONNX Runtime DirectML provider is unavailable"
            )
        options = ort.SessionOptions()
        # DirectML requires sequential execution and disabled memory patterns.
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        options.enable_mem_pattern = False
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        options.graph_optimization_level = (
            ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        )
        options.log_severity_level = 3
        providers = [
            ("DmlExecutionProvider", {"device_id": device_index}),
            "CPUExecutionProvider",
        ]
        self.session = ort.InferenceSession(
            str(model_path), sess_options=options, providers=providers
        )
        model_input = self.session.get_inputs()
        model_output = self.session.get_outputs()
        if len(model_input) != 1 or len(model_output) != 1:
            raise RuntimeError("SPiKE ONNX model must have one input and output")
        self.input_name = model_input[0].name
        self.output_name = model_output[0].name
        expected_input = [1, self.frames_per_clip, self.point_count, 3]
        if model_input[0].shape != expected_input:
            raise RuntimeError(
                f"SPiKE ONNX input {model_input[0].shape} != {expected_input}"
            )
        if model_input[0].type != "tensor(float16)":
            raise RuntimeError("SPiKE ONNX input must be FP16")
        if model_output[0].shape != [1, 45]:
            raise RuntimeError("SPiKE ONNX output must have shape [1, 45]")
        zeros = np.zeros(expected_input, dtype=np.float16)
        for _ in range(warmup_runs):
            self.session.run([self.output_name], {self.input_name: zeros})

    def infer(self, clips: np.ndarray) -> np.ndarray:
        value = np.asarray(clips, dtype=np.float16)
        expected_tail = (
            self.frames_per_clip,
            self.point_count,
            3,
        )
        if value.ndim != 4 or value.shape[1:] != expected_tail:
            raise ValueError(
                f"SPiKE input tail {value.shape[1:]} != {expected_tail}"
            )
        outputs = []
        for clip in value:
            output = self.session.run(
                [self.output_name],
                {self.input_name: clip[None, ...]},
            )[0]
            outputs.append(output.reshape(15, 3))
        return np.asarray(outputs, dtype=np.float32)
