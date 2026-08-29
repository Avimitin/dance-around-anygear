# Release workflow

`.github/workflows/release.yml` runs only when a `v*` tag is pushed. Its first
step accepts only exact `vMAJOR.MINOR.PATCH` tags and requires that version to
match the CMake project version. A typical release is therefore:

```powershell
git tag -a v0.3.0 -m 'dance-around-anygear v0.3.0'
git push origin v0.3.0
```

The hosted `windows-2025` job restores only an ignored dependency cache, then
revalidates every downloaded input. It statically reads the pinned Kinect SDK
headers, checks out the pinned OpenVR commit, extracts the pinned MediaPipe
runtime/model, and uses the pinned WinLibs compiler. Hardware and the game are
not started in CI.

The release is created only after all of these gates pass:

1. source-only tree policy;
2. Release build of Kinect, webcam, and SteamVR entry DLLs;
3. Spice loader, SDK hook, VP4U ABI, deterministic SteamVR pose, and MediaPipe
   image tests;
4. two isolated builds with identical DLL hashes;
5. clean Git/build-manifest commit match;
6. exact ZIP dependency layout and release checksums.

The release job has only `contents: write` permission. It uses the repository's
short-lived `GITHUB_TOKEN` through GitHub CLI, verifies that the pushed tag
already exists, and refuses to replace an existing published release.

## Asset layout

```text
dance_around_anygear_kinect.dll
dance-around-anygear-v<version>-webcam-win64.zip
dance-around-anygear-v<version>-steamvr-win64.zip
dance-around-anygear-v<version>-build-manifest.json
SHA256SUMS
```

Kinect is the only direct DLL asset. Webcam and SteamVR remain ZIP files because
their DLLs resolve a backend-owned adjacent dependency directory. GitHub also
adds its normal source archives automatically.
