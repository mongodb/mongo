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
    if not override and os.environ.get("CI"):
        return Path(os.getcwd()) / ".cache" / "fd-binaries" / FD_VERSION
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


RG_VERSION = "v15.1.0"


def _rg_cache_dir() -> Path:
    override = os.environ.get("RG_CACHE_DIR")
    if not override and os.environ.get("CI"):
        return Path(os.getcwd()) / ".cache" / "rg-binaries" / FD_VERSION
    if override:
        return Path(override).expanduser().resolve() / RG_VERSION
    return Path.home() / ".cache" / "rg-binaries" / RG_VERSION


def _rg_s3_url_for(sysname: str, arch: str) -> str | None:
    """
    Map (os, arch) -> S3 path for our prebuilt rg artifacts.
    Filenames match our build scripts:
      - macOS: universal2 (single file for both x86_64/arm64)
      - Linux: manylinux2014-{x86_64|aarch64|s390x|ppc64le}
      - Windows: rg-windows-x86_64.exe (and optional arm64)
    """
    base = f"https://mdb-build-public.s3.amazonaws.com/rg-binaries/{RG_VERSION}"

    if sysname == "darwin":
        # universal2 single binary for both arches
        return f"{base}/rg-macos-universal2"

    if sysname == "linux":
        if arch in ("amd64", "x86_64"):
            return f"{base}/rg-manylinux2014-x86_64"
        if arch in ("arm64", "aarch64"):
            return f"{base}/rg-manylinux2014-aarch64"
        if arch == "s390x":
            return f"{base}/rg-manylinux2014-s390x"
        if arch == "ppc64le":
            return f"{base}/rg-manylinux2014-ppc64le"
        return None

    if sysname == "windows":
        if arch in ("amd64", "x86_64"):
            return f"{base}/rg-windows-x86_64.exe"
        if arch in ("arm64", "aarch64"):
            # include if you also publish arm64
            return f"{base}/rg-windows-arm64.exe"
        return None

    return None


def ensure_rg() -> str | None:
    """
    Returns absolute path to cached ripgrep (rg) binary, or None if unsupported/failed.

    Env overrides:
      - FORCE_NO_RG: if set -> return None
      - RG_PATH: absolute path to use instead of downloading
      - RG_CACHE_DIR: base cache dir (default ~/.cache/rg-binaries/<ver>)
    """
    if os.environ.get("FORCE_NO_RG"):
        return None
    if os.environ.get("RG_PATH"):
        return os.environ["RG_PATH"]

    sysname, arch = _triplet()
    s3_url = _rg_s3_url_for(sysname, arch)
    if not s3_url:
        return None

    exe_name = "rg.exe" if sysname == "windows" else "rg"
    # For macOS universal, we don’t want the arch in the leaf cache dir name to duplicate entries
    leaf = f"{sysname}-{arch}" if sysname != "darwin" else f"{sysname}-universal2"
    outdir = _rg_cache_dir() / leaf
    outdir.mkdir(parents=True, exist_ok=True)
    local = outdir / exe_name

    if local.exists():
        return str(local.resolve())

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
