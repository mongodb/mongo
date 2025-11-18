"""Wrapper utilities for the Bazel build system."""

import multiprocessing
import os
import pathlib
import platform

from bazel.wrapper_hook.wrapper_debug import wrapper_debug

_UNKNOWN = "Unknown"
_REPO_ROOT = str(pathlib.Path(__file__).parent.parent.parent)


def get_terminal_stream(fd_env_var: str):
    """Return a Python file object for the original terminal FD."""
    fd_str = os.environ.get(fd_env_var)
    if not fd_str:
        return None

    # Handle Windows CON device
    if fd_str == "CON":
        # On Windows, open CON device for console output
        # Use the appropriate stream based on the variable name
        if "STDOUT" in fd_env_var:
            try:
                return open("CON", "w", buffering=1)
            except (OSError, IOError):
                return None
        elif "STDERR" in fd_env_var:
            try:
                return open("CON", "w", buffering=1)
            except (OSError, IOError):
                return None
        return None

    # Handle Unix file descriptors
    if fd_str.isdigit():
        fd = int(fd_str)
        try:
            return os.fdopen(fd, "w", buffering=1)
        except (OSError, ValueError):
            return None

    return None


def cpu_info() -> str:
    """CPU count - works on all platforms"""
    try:
        return str(os.cpu_count() or multiprocessing.cpu_count())
    except Exception as _e:
        wrapper_debug(f"Failed to get CPU count {_e}")
        return _UNKNOWN


def memory_info(mem_type: str) -> str:
    """Memory - Linux only"""
    memory_gb = _UNKNOWN

    if not platform.system() == "Linux":
        return _UNKNOWN

    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if line.startswith(f"{mem_type}:"):
                    kb = int(line.split()[1])
                    memory_gb = str(round(kb / (1024 * 1024), 2))
                    break
    except Exception as _e:
        wrapper_debug(f"Failed to get memory info {_e}")
        return _UNKNOWN

    return memory_gb


def filesystem_info() -> tuple[str, int]:
    """Filesystem type - Linux only"""
    fs_type = (_UNKNOWN, 0)
    if not platform.system() == "Linux":
        return (_UNKNOWN, 0)

    try:
        repo_path = _REPO_ROOT
        with open("/proc/mounts", "r") as f:
            best_mountpoint_len = 0
            for line in f:
                parts = line.split()
                if len(parts) >= 3:
                    mountpoint, fstype = parts[1], parts[2]
                    if repo_path.startswith(mountpoint) and len(mountpoint) > best_mountpoint_len:
                        filesystem_type = fstype
                        best_mountpoint_len = len(mountpoint)
                        fs_type = (filesystem_type, best_mountpoint_len)
    except Exception as _e:
        wrapper_debug(f"Failed to get filesystem type {_e}")
        return (_UNKNOWN, 0)

    return fs_type
