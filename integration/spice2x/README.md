# Spice integration patch series

This directory contains the Spice-owned half of DANCE aROUND support. It is a
reviewable patch series against the official `spice2x/spice2x.github.io`
repository; no Spice binary is stored here.

## Pinned base

```text
repository: https://github.com/spice2x/spice2x.github.io.git
base:       4009bb8a840fb5ceabeef4ba9610a1eeb2804378
base date:  2026-08-28
```

Apply the patches in numeric order:

```powershell
git clone https://github.com/spice2x/spice2x.github.io.git spice2x
Set-Location spice2x
git checkout 4009bb8a840fb5ceabeef4ba9610a1eeb2804378
$patches = Get-ChildItem D:\Share\dance-around-anygear\integration\spice2x -Filter *.patch |
    Sort-Object Name
git am $patches.FullName
```

After application, `git rev-parse 'HEAD^{tree}'` must print
`2974a165d181cbb4ab222b6b8f80d8703657e07f`.

The five commits are deliberately separated for upstream review:

1. Append a size-gated `hook_library` callback to Spice SDK v0.3. This lets a
   `-k` plugin register an in-memory implementation for a later DLL request.
2. Resolve short `execexe` ordinal export thunks before installing MinHook
   trampolines and make the error path ordinal-safe.
3. Add DANCE aROUND detection, launch/heap defaults, controls/lights, BI2X and
   ICCA emulation, and a game-scoped MIDI enumeration switch.
4. Preserve a resizable Unity window, block DXGI exclusive-mode restoration,
   attach the window procedure on the HWND owner thread, and pace the main
   swap chain to at least two vertical intervals.
5. Honor Spice's existing `-windowsize width,height` and `-windowpos x,y`
   arguments for DANCE aROUND, including the first visible frame, and preserve
   the position after Unity window calls or an interactive drag.

The series was clean-built as the `spicetools_spice64` target with GCC 16.2.0
and Ninja on the pinned base. The static-DLL import check passed. The MinGW
linker emitted the pre-existing resource-manifest merge warning; it did not
fail the build.

## Responsibility boundary

These patches intentionally contain no Kinect, RealSense, webcam, VP4U, ONNX,
or MediaPipe implementation. Spice owns game/platform compatibility:

- local game launch and Unity process arguments;
- windowing, overlay, keyboard binding, card/ICCA and BI2X behavior;
- platform module aliases and virtual I/O;
- the generic `-k` lifecycle used by external plugins.

`dance_around_anygear_*.dll` owns only the external sensor-to-VP4U hardware
compatibility layer. A D435 user therefore needs no Anygear DLL.

## Review notes

- The SDK addition is append-only and guarded by the caller-provided struct
  size, so older plugins keep the existing ABI.
- UDN's AIO node hooks report ready only for the emulated BI2X/ICCA managers
  and nodes. Unrelated node pointers delegate to the original implementation.
- The newest upstream NDD graphics behavior remains intact; the graphics patch
  was ported hunk-by-hunk instead of replacing the old experimental files.
- The main HWND can become available through the D3D11 swap chain after the
  early `CreateWindow` hooks. Resizing it directly from `Present` risks a
  cross-thread deadlock, so the operation is posted to its owning message queue.
