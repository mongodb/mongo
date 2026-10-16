"""Pure path conversion shared by container entry points.

Callers choose destination drive case and supply the WSL mount prefix. This module
does not read environment variables, select engines, or resolve host symlinks.
"""

from __future__ import annotations

import pathlib
import re


def container_path(
    path: pathlib.PurePath | str, *, windows: bool = False, lowercase_drive: bool = False
) -> str:
    """Convert a host path to its Linux container destination."""
    value = str(path)
    if not windows and not re.match(r"^[A-Za-z]:[\\/]", value):
        return pathlib.PurePosixPath(value).as_posix()

    windows_path = pathlib.PureWindowsPath(value)
    drive = windows_path.drive.rstrip(":")
    if windows_path.drive:
        if len(drive) != 1 or not drive.isascii() or not drive.isalpha() or not windows_path.root:
            raise ValueError(f"Expected a drive-qualified Windows path: {path}")
        if lowercase_drive:
            drive = drive.lower()
        return pathlib.PurePosixPath("/", drive, *windows_path.parts[1:]).as_posix()
    return windows_path.as_posix()


def drive_mount_prefix(prefix: str) -> str:
    """Validate a WSL mount prefix before using it in a privileged mount command."""
    normalized = prefix.rstrip("/")
    if not re.fullmatch(r"/(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+", normalized) or any(
        part in {".", ".."} for part in normalized.split("/")
    ):
        raise ValueError(f"Invalid WSL drive mount prefix: {prefix!r}")
    return normalized


def wsl_volume_source(path: pathlib.PurePath | str, mount_prefix: str) -> str:
    """Convert a drive-qualified Windows source to the Docker daemon's DrvFS path."""
    windows_path = pathlib.PureWindowsPath(str(path))
    if not windows_path.drive:
        return str(path)
    destination = container_path(path, windows=True, lowercase_drive=True)
    return f"{drive_mount_prefix(mount_prefix)}{destination}"
