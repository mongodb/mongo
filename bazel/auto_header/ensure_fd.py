#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Ensure a fast `fd` binary exists locally:
- Detect OS/arch
- Download from your S3 mirror once (validated by SHA)
- Cache under ~/.cache/fd-binaries/v10.3.0/<platform>/fd[.exe] by default
- Or use FD_CACHE_DIR environment variable to override
- Return absolute path to the binary, or None if unsupported

Respects FD_PATH (force custom path) and FORCE_NO_FD (disable fd entirely).
"""

from __future__ import annotations
import os, stat, sys, platform
from pathlib import Path
import traceback

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.append(str(REPO_ROOT))

FD_VERSION = "v10.3.0"


def _triplet() -> tuple[str, str]:
    sysname = platform.system().lower()  # linux|darwin|windows
    mach = platform.machine().lower()
    if mach in ("x86_64", "amd64"):
        arch = "amd64"
    elif mach in ("aarch64", "arm64"):
        arch = "arm64"
    elif mach in ("ppc64le", "s390x"):
        arch = mach
    else:
        arch = mach
    return sysname, arch


def _s3_url_for(sysname: str, arch: str) -> str | None:
    base = "https://mdb-build-public.s3.amazonaws.com/fd-binaries"
    if sysname == "darwin" and arch in ("amd64", "arm64"):
        return f"{base}/{FD_VERSION}/fd-darwin-{arch}"
    if sysname == "linux" and arch in ("amd64", "arm64"):
        return f"{base}/{FD_VERSION}/fd-linux-{arch}"
    if sysname == "windows" and arch in ("amd64", "arm64"):
        return f"{base}/{FD_VERSION}/fd-windows-{arch}.exe"
    return None  # unsupported → trigger fallback


def _make_executable(p: Path) -> None:
    if os.name != "nt":
        p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def _cache_dir() -> Path:
    override = os.environ.get("FD_CACHE_DIR")
    if override:
        return Path(override).expanduser().resolve() / FD_VERSION
    return Path.home() / ".cache" / "fd-binaries" / FD_VERSION


def ensure_fd() -> str | None:
    """
    Returns absolute path to cached fd binary, or None if not available.
    """
    if os.environ.get("FORCE_NO_FD"):
        return None
    if os.environ.get("FD_PATH"):
        return os.environ["FD_PATH"]

    sysname, arch = _triplet()
    s3_url = _s3_url_for(sysname, arch)
    if not s3_url:
        return None

    exe_name = "fd.exe" if sysname == "windows" else "fd"
    outdir = _cache_dir() / f"{sysname}-{arch}"
    outdir.mkdir(parents=True, exist_ok=True)
    local = outdir / exe_name

    if local.exists():
        return local

    try:
        from buildscripts.s3_binary.download import download_s3_binary

        ok = download_s3_binary(
            s3_path=s3_url,
            local_path=str(local),
            remote_sha_allowed=False,
            ignore_file_not_exist=False,
        )
        if not ok:
            return None
        _make_executable(local)
        return str(local.resolve())
    except Exception:
        traceback.print_exc()
        return None
