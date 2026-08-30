# Third-party notices

## nlohmann/json

`third_party/nlohmann/json.hpp` is the standalone header from nlohmann/json
`v3.12.0`, vendored byte-for-byte from the official release asset and pinned by
SHA-256 in `dependency-lock.json`. It is licensed under the MIT License; the
upstream license text is preserved at `third_party/nlohmann/LICENSE.MIT`.

## Google MediaPipe

`src/vp4u/mediapipe_c_api.h` is a minimal ABI declaration derived from the
MediaPipe Tasks C API at tag `v1.0.0`, licensed under Apache License 2.0. The
source tree does not contain the MediaPipe runtime. The dependency bootstrap
extracts `libmediapipe.dll`, its Apache-2.0 `LICENSE`, and its `NOTICE` from the
pinned official Windows wheel into the ignored `.deps/` directory.

The separately hosted `pose_landmarker_lite.task` model is not part of this
repository. Google identifies the Pose Landmarker model as Apache-2.0; the
bootstrap pins the exact model by SHA-256 and release archives carry the
MediaPipe license and notice.

## Spice SDK ABI

`include/anygear/spice_sdk.h` describes the public C ABI used by DLLs loaded via
Spice `-k`. It is maintained as an ABI declaration, not linked Spice code.
Spice is distributed under GNU GPL version 3.

## Kinect for Windows ABI

The Kinect backend is compiled against headers statically extracted from a
hash-pinned official Kinect for Windows SDK 1.8 installer and dynamically
resolves `Kinect10.dll` at runtime. SDK files are held only in the ignored
dependency cache; the runtime remains a user-installed dependency.

## Valve OpenVR

The SteamVR backend compiles against the generated `openvr_capi.h` and ships
the official Windows x86-64 `openvr_api.dll` from Valve OpenVR `v2.15.6`.
OpenVR is licensed under the BSD 3-Clause License. The SDK/runtime files are
fetched into the ignored dependency cache and verified against the hashes in
`dependency-lock.json`; they are not committed to this source tree. Packaged
runtime directories include the OpenVR license text.

## SPiKE

`runtime/spike/anygear_spike/spike_model.py` contains a fixed-shape inference
adaptation of the SPiKE ITOP-SIDE network. The source revision and published
checkpoint are pinned in `dependency-lock.json`. SPiKE is licensed under the
MIT License; its license text is preserved at
`third_party/spike/LICENSE.MIT`.

## Intel librealsense

The D4xx/SPiKE package carries the official Windows x86-64 librealsense 2.50.0
runtime extracted from Intel's pinned Unity package. The source archive,
runtime, and output license are verified by size and SHA-256. librealsense is
licensed under Apache License 2.0; its license accompanies the runtime.

## ONNX Runtime DirectML worker

The frozen D4xx/SPiKE worker redistributes pinned runtime components from ONNX
Runtime DirectML 1.23.0, CPython 3.10.5, NumPy 2.2.6, PyInstaller 6.22.2, and
setuptools. Voxel labeling uses connected-components-3d 4.0.0 under
LGPL-3.0-or-later. The worker build copies the corresponding
license files and ONNX Runtime third-party notice into the worker directory;
the package script refuses an incomplete notice set.

PyTorch 2.11.0 CPU and ONNX 1.22.0 are checkpoint-conversion dependencies only
and are not included in release archives.
