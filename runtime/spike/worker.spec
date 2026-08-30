# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path


runtime_root = Path(SPECPATH)
analysis = Analysis(
    [str(runtime_root / "worker_entry.py")],
    pathex=[str(runtime_root)],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["pytest", "torch"],
    noarchive=False,
    optimize=1,
)
python_archive = PYZ(analysis.pure)
executable = EXE(
    python_archive,
    analysis.scripts,
    [],
    exclude_binaries=True,
    name="dance_around_anygear_spike_worker",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
)
collection = COLLECT(
    executable,
    analysis.binaries,
    analysis.datas,
    strip=False,
    upx=False,
    name="dance_around_anygear_spike_worker",
)
