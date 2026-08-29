# Third-party notices

## Google MediaPipe

`src/vp4u/mediapipe_c_api.h` is a minimal ABI declaration derived from the
MediaPipe Tasks C API at tag `v1.0.0`, licensed under Apache License 2.0. The
source tree does not contain the MediaPipe runtime. The dependency bootstrap
extracts `libmediapipe.dll`, its Apache-2.0 `LICENSE`, and its `NOTICE` from the
pinned official Windows wheel into the ignored `.deps/` directory.

The separately hosted `pose_landmarker_lite.task` model is not part of this
repository. Google documents its download for local projects, but explicit
redistribution terms for that model bundle have not been verified. Therefore
the packaging script permits it only in bundles labelled local evaluation and
blocks public webcam release packaging.

## Spice SDK ABI

`include/anygear/spice_sdk.h` describes the public C ABI used by DLLs loaded via
Spice `-k`. It is maintained as an ABI declaration, not linked Spice code.
Spice is distributed under GNU GPL version 3.

## Kinect for Windows ABI

The Kinect backend is compiled against `NuiApi.h` from a user-installed Kinect
for Windows SDK 1.8 and dynamically resolves `Kinect10.dll` at runtime. The SDK
headers, runtime, and import library are external build/runtime dependencies.

## Valve OpenVR

The SteamVR backend compiles against the generated `openvr_capi.h` and ships
the official Windows x86-64 `openvr_api.dll` from Valve OpenVR `v2.15.6`.
OpenVR is licensed under the BSD 3-Clause License. The SDK/runtime files are
fetched into the ignored dependency cache and verified against the hashes in
`dependency-lock.json`; they are not committed to this source tree. Packaged
runtime directories include the OpenVR license text.
