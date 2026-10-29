"""Bazel binary discovery, download, and verification for containers."""

from __future__ import annotations

import hashlib
import os
import pathlib
import re
import tempfile
import urllib.error
import urllib.request
from collections.abc import Mapping

from .constants import DEFAULT_HERMETIC_CONTAINER_CONTAINER_ARCH
from .distro import normalize_arch
from .env import _info
from .fsutil import _hermetic_container_state_dir, _read_key_value_file, _sha256_file
from .volumes import _container_path

LINUX_BAZEL_ENV = "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL"


LINUX_BAZEL_SHA256_ENV = "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_SHA256"


CONTAINER_BAZEL_ENV = "MONGO_HERMETIC_CONTAINER_CONTAINER_BAZEL"


def _bazel_version(repo_root: pathlib.Path, env: Mapping[str, str]) -> str:
    if env.get("USE_BAZEL_VERSION"):
        return env["USE_BAZEL_VERSION"]
    version_file = repo_root / ".bazelversion"
    try:
        for line in version_file.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped:
                return stripped
    except OSError:
        pass
    return "latest"


def _bazelisk_base_url(repo_root: pathlib.Path, env: Mapping[str, str]) -> str | None:
    if env.get("BAZELISK_BASE_URL"):
        return env["BAZELISK_BASE_URL"]
    return _read_key_value_file(repo_root / ".bazeliskrc").get("BAZELISK_BASE_URL")


def _linux_bazel_filename(version: str, arch: str) -> str:
    return f"bazel-{version}-linux-{arch}"


def _default_container_bazel_arch(system: str) -> str:
    if system == "Windows":
        return DEFAULT_HERMETIC_CONTAINER_CONTAINER_ARCH

    arch = normalize_arch()
    if arch == "aarch64":
        return "arm64"
    return arch


def _linux_bazel_url(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
    version: str,
    arch: str,
) -> str:
    if env.get("MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_URL"):
        return env["MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_URL"]

    filename = _linux_bazel_filename(version, arch)
    base_url = _bazelisk_base_url(repo_root, env)
    if base_url:
        return f"{base_url.rstrip('/')}/{version}/{filename}"

    version_base, sep, rc = version.partition("rc")
    rc_or_release = f"rc{rc}" if sep else "release"
    return f"https://releases.bazel.build/{version_base}/{rc_or_release}/{filename}"


def _parse_sha256(value: str, *, source: str) -> str:
    """Return a normalized SHA-256 digest read from a checksum value."""

    token = value.strip().split(maxsplit=1)[0] if value.strip() else ""
    if not re.fullmatch(r"[0-9a-fA-F]{64}", token):
        raise RuntimeError(f"Invalid SHA-256 checksum from {source}")
    return token.lower()


def _linux_bazel_checksum_file(path: pathlib.Path) -> pathlib.Path:
    return path.with_name(f"{path.name}.sha256")


def _read_linux_bazel_checksum(path: pathlib.Path) -> str | None:
    try:
        return _parse_sha256(path.read_text(encoding="utf-8"), source=str(path))
    except (OSError, RuntimeError):
        return None


def _linux_bazel_cache_is_valid(binary: pathlib.Path, expected_sha256: str | None = None) -> bool:
    checksum = _read_linux_bazel_checksum(_linux_bazel_checksum_file(binary))
    if not (
        checksum and (expected_sha256 is None or checksum == expected_sha256) and binary.is_file()
    ):
        return False
    try:
        return _sha256_file(binary) == checksum
    except OSError:
        return False


def _download_linux_bazel_checksum(url: str) -> str:
    checksum_url = f"{url}.sha256"
    try:
        with urllib.request.urlopen(checksum_url) as response:
            return _parse_sha256(
                response.read().decode("utf-8"),
                source=checksum_url,
            )
    except (OSError, UnicodeDecodeError, urllib.error.URLError) as exc:
        raise RuntimeError(
            "Failed to download a checksum for Linux Bazel for hermetic_container from "
            f"{checksum_url}. Set {LINUX_BAZEL_SHA256_ENV} to a trusted SHA-256 digest "
            "when using a mirror without checksum sidecars."
        ) from exc


def _write_linux_bazel_checksum(destination: pathlib.Path, checksum: str) -> None:
    checksum_path = _linux_bazel_checksum_file(destination)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=checksum_path.parent,
        prefix=f".{checksum_path.name}.",
        suffix=".tmp",
        text=True,
    )
    temporary_path = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(f"{checksum}\n")
            output.flush()
            os.fsync(output.fileno())
        temporary_path.replace(checksum_path)
    finally:
        temporary_path.unlink(missing_ok=True)


def _download_file(url: str, destination: pathlib.Path, expected_sha256: str) -> None:
    """Atomically download a verified Linux Bazel binary into the local cache."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=destination.parent,
        prefix=f".{destination.name}.",
        suffix=".tmp",
    )
    temporary_path = pathlib.Path(temporary_name)
    digest = hashlib.sha256()
    try:
        try:
            with os.fdopen(descriptor, "wb") as output:
                with urllib.request.urlopen(url) as response:
                    while chunk := response.read(1024 * 1024):
                        digest.update(chunk)
                        output.write(chunk)
                output.flush()
                os.fsync(output.fileno())
        except (OSError, urllib.error.URLError) as exc:
            raise RuntimeError(
                f"Failed to download Linux Bazel for hermetic_container from {url}: {exc}"
            ) from exc

        actual_sha256 = digest.hexdigest()
        if actual_sha256 != expected_sha256:
            raise RuntimeError(
                "Downloaded Linux Bazel for hermetic_container does not match the expected "
                f"SHA-256: expected {expected_sha256}, got {actual_sha256}"
            )

        temporary_path.chmod(0o755)
        temporary_path.replace(destination)
        _write_linux_bazel_checksum(destination, expected_sha256)
    finally:
        temporary_path.unlink(missing_ok=True)


def _resolve_container_bazel(
    bazel_real: pathlib.Path,
    env: Mapping[str, str],
    repo_root: pathlib.Path,
    system: str,
) -> tuple[str, pathlib.Path | None]:
    """Return the Bazel command inside the container and the host path to mount."""

    if system == "Linux":
        return str(bazel_real), bazel_real

    if env.get(CONTAINER_BAZEL_ENV):
        return env[CONTAINER_BAZEL_ENV], None

    if env.get(LINUX_BAZEL_ENV):
        linux_bazel = pathlib.Path(env[LINUX_BAZEL_ENV])
        return _container_path(linux_bazel, system), linux_bazel

    arch = env.get("MONGO_HERMETIC_CONTAINER_CONTAINER_ARCH", _default_container_bazel_arch(system))
    version = _bazel_version(repo_root, env)
    filename = _linux_bazel_filename(version, arch)
    linux_bazel = _hermetic_container_state_dir(repo_root) / "bazel" / filename / "bin" / "bazel"

    configured_checksum = env.get(LINUX_BAZEL_SHA256_ENV)
    expected_sha256 = (
        _parse_sha256(configured_checksum, source=LINUX_BAZEL_SHA256_ENV)
        if configured_checksum
        else None
    )
    if (
        not _linux_bazel_cache_is_valid(linux_bazel, expected_sha256)
        and env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") != "1"
    ):
        url = _linux_bazel_url(repo_root, env, version, arch)
        if expected_sha256 is None:
            expected_sha256 = _download_linux_bazel_checksum(url)
        _info(f"downloading Linux Bazel for hermetic_container from {url}")
        _download_file(url, linux_bazel, expected_sha256)

    return _container_path(linux_bazel, system), linux_bazel
