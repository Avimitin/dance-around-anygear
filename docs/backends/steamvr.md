# SteamVR tracked-pose backend

`dance_around_anygear_steamvr.dll` turns SteamVR standing-space device poses
into the same VP4U body callbacks used by the other Anygear backends. Spice
loads only the Anygear entry DLL; that DLL locates the pinned OpenVR client in
its adjacent dependency directory.

## Hardware profile

The reliable default is six-point tracking:

- one HMD;
- left and right controllers;
- a tracker assigned as `Waist`;
- trackers assigned as `Left Foot` and `Right Foot`.

Assign generic trackers in SteamVR's **Manage Trackers** screen before starting
the game. The backend reads SteamVR's stored role assignments. It does not bind
roles by enumeration order or by a particular vendor's serial-number format.

The following roles improve reconstructed joints when present: chest, left and
right shoulder, elbow, and knee. Without those optional devices, shoulders,
elbows, hips, knees, ankles, face points, fingers, heels, and toes are inferred
from the nearest tracked points and marked accordingly in the VP4U joint state.

OpenVR skeletal actions describe hand bones only; they are not a full-body
source. This backend therefore uses tracked-device 6DoF poses for the body and
does not claim finger actions as leg or torso tracking.

## Runtime behavior

The client initializes as an OpenVR background application and polls current
poses at 60 Hz in standing coordinates. It does not open a camera, initialize
MediaPipe, create a VR render context, or submit compositor frames. Preview and
pose callbacks carry a shared 1x1 image token because the game callback expects
a non-null image descriptor alongside 3D joints.

On the first complete frame, the waist and facing basis establish a fixed room
calibration. The initial hip maps to `(0.55, 0.00, 2.30)` meters. Later lateral
and depth movement remains room-relative instead of being centered again each
frame. Start in a neutral stance facing the display when the game opens.

The archive layout is:

```text
dance_around_anygear_steamvr.dll
dance_around_anygear_steamvr/
  openvr_api.dll
  LICENSE.openvr.txt
```

Load it with one Spice argument:

```text
-k dance_around_anygear_steamvr.dll
```

## Configuration

Defaults can be changed in the existing `Vp4uWrap` JSON object:

```json
{
  "Vp4uWrap": {
    "SteamVrAutoCenter": 1,
    "SteamVrFaceToFace": 1,
    "SteamVrRequireWaist": 1,
    "SteamVrRequireFeet": 1,
    "SteamVrBodyHoldMs": 250,
    "SteamVrStageHipX": 0.55,
    "SteamVrStageHipY": 0.0,
    "SteamVrStageHipZ": 2.3
  }
}
```

`SteamVrRequireWaist=0` and `SteamVrRequireFeet=0` explicitly enable a
three-point evaluation mode. It estimates the torso and legs from the HMD,
controllers, and standing floor. That mode is useful for checking integration,
but it cannot measure dance footwork and is not the release default.

`SteamVrFaceToFace=1` applies the initial 180-degree stage yaw expected when the
performer and avatar face each other. Keep it enabled for the first hardware
test; if the installation's room origin was authored in the opposite direction,
set it to `0` and repeat calibration.

## Build and test

```powershell
./tools/bootstrap-openvr.ps1 -Download
./tools/build.ps1 -ToolchainRoot C:\path\to\mingw64
./tools/test.ps1 -SkipBuild
./tools/package.ps1 -Backend steamvr
```

The hardware probe is opt-in because initializing OpenVR may start or connect
to the local SteamVR runtime:

```powershell
./tools/test.ps1 -SkipBuild -HardwareSteamVr -Seconds 30
```

Add `-SteamVrAllowInferredBody` only for an intentional three-point evaluation.
The normal probe fails when no complete six-point body is observed.

Runtime logs report the pinned OpenVR version, the resolved role-to-device
table, and transitions between `TRACKED` and `NOT TRACKED`. A missing waist or
foot role remains `NOT TRACKED` instead of silently emitting a full body.
