"""PyInstaller entry point for the SPiKE worker."""

from anygear_spike.worker import main


if __name__ == "__main__":
    raise SystemExit(main())
