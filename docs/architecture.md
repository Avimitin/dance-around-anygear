# Architecture

## Runtime flow

```text
Spice -k dance_around_anygear_<backend>.dll
  -> Spice LoadLibrary (before game entry)
  -> spice_sdk_entry_point (outside DllMain/loader lock)
  -> select backend and set bounded defaults
  -> validate the VP4U exports embedded in the same DLL
  -> redirect Unity's later visionposewrapper.dll request to that DLL
  -> Unity VP4U bridge calls the compatibility ABI
  -> backend produces preview/calibration/pose callbacks
```

Each entry DLL is monolithic. It owns selection, dependency discovery, stage
logging, loader redirection, the documented VP4U compatibility ABI, and its sensor
backend. Sensor-specific code remains separated internally so Kinect, webcam,
SteamVR, and future D4xx targets share the same ABI core. A backend may have adjacent
data/runtime dependencies, but Spice loads only the entry DLL.

Heavy initialization must not occur in `DllMain`. Spice's `-k` lifecycle calls
the exported `spice_sdk_entry_point` before `avs::game::entry_main`, which is
the safe initialization point used here.

## Backend layout

Kinect release archives use this layout:

```text
dance_around_anygear_kinect.dll
```

Webcam evaluation archives use this layout:

```text
dance_around_anygear_webcam.dll        <- only Spice -k target
dance_around_anygear_webcam/
  libmediapipe.dll                     <- loaded by absolute plugin-relative path
  pose_landmarker_lite.task
  LICENSE.mediapipe.txt
  NOTICE.mediapipe.txt
```

Those binary inputs are not committed. Bootstrap/package scripts accept only
the pinned MediaPipe v1.0.0 Windows runtime and versioned model hashes.

SteamVR release archives use this layout:

```text
dance_around_anygear_steamvr.dll        <- only Spice -k target
dance_around_anygear_steamvr/
  openvr_api.dll                        <- loaded by absolute plugin-relative path
  LICENSE.openvr.txt
```

The entry DLL initializes OpenVR as a background pose client. It neither
creates a rendering context nor submits frames to a headset. HMD, controller,
and user-assigned generic-tracker poses are reconstructed into the common
33-landmark stage representation before the shared VP4U body mapper runs.

## Ownership rules

Anygear owns transformations that make a different physical sensing device
look like the D435-facing VP4U ABI. It does not own:

- game process creation or Unity command-line arguments;
- virtual display, window sizing, swap-chain, or overlay behavior;
- BI2X/ICCA/card-reader emulation;
- keyboard/controller binding;
- e-amusement networking;
- modification of managed game assemblies.

Those belong in Spice.

## Failure policy

A backend mismatch must fail visibly. Kinect never falls back to a webcam, and
the webcam backend never reports an unavailable camera as a synthetic device.
The default log records four plugin stages, then five Unity/ABI/sensor stages
and tracking state transitions. It does not emit periodic joint or resource
telemetry. Deep resource and hardware diagnostics stay in maintainer scripts.
