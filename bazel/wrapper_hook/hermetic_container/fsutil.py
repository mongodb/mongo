"""Host filesystem, user, lock, and output-layout helpers."""

from __future__ import annotations

import contextlib
import getpass
import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import time
from collections.abc import Mapping, Sequence

from .constants import (
    HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT_ENV,
    HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE_ENV,
    HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION,
)
from .env import _env_is_false, _info

CONTAINER_NETWORK_RETRY_ATTEMPTS = 3


CONTAINER_NETWORK_RETRY_DELAY_SECONDS = 1


def _run_container_network_command(
    command: Sequence[str], *, description: str, **run_kwargs: object
) -> subprocess.CompletedProcess:
    """Run a network-dependent container command with bounded exponential backoff."""
    for attempt in range(CONTAINER_NETWORK_RETRY_ATTEMPTS):
        try:
            result = subprocess.run(command, **run_kwargs)
        except (OSError, subprocess.SubprocessError) as exc:
            if attempt == CONTAINER_NETWORK_RETRY_ATTEMPTS - 1:
                raise
            detail = str(exc) or exc.__class__.__name__
            _info(
                f"{description} failed ({detail}); retrying in "
                f"{CONTAINER_NETWORK_RETRY_DELAY_SECONDS * 2**attempt}s"
            )
        else:
            if result.returncode == 0 or attempt == CONTAINER_NETWORK_RETRY_ATTEMPTS - 1:
                return result
            _info(
                f"{description} failed with exit code {result.returncode}; retrying in "
                f"{CONTAINER_NETWORK_RETRY_DELAY_SECONDS * 2**attempt}s"
            )

        time.sleep(CONTAINER_NETWORK_RETRY_DELAY_SECONDS * 2**attempt)

    raise AssertionError("container network retry loop exited unexpectedly")


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _current_user() -> str:
    try:
        return getpass.getuser()
    except Exception:
        return "user"


def _host_home(env: Mapping[str, str]) -> pathlib.Path | None:
    if env.get("HOME"):
        return pathlib.Path(env["HOME"])
    if env.get("USERPROFILE"):
        return pathlib.Path(env["USERPROFILE"])

    try:
        return pathlib.Path.home()
    except RuntimeError:
        return None


def _hermetic_container_state_dir(repo_root: pathlib.Path) -> pathlib.Path:
    return repo_root / ".tmp" / "hermetic_container"


def _hermetic_container_home_dir(repo_root: pathlib.Path) -> pathlib.Path:
    return _hermetic_container_state_dir(repo_root) / "home"


def _path_is_case_sensitive(path: pathlib.Path) -> bool:
    path.mkdir(parents=True, exist_ok=True)
    probe = path / f".case-check-{os.getpid()}"
    alternate = path / probe.name.upper()
    try:
        probe.write_text("1", encoding="utf-8")
        return not alternate.exists()
    finally:
        try:
            probe.unlink()
        except FileNotFoundError:
            pass


def _run_host_command(command: Sequence[str], description: str) -> None:
    try:
        subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError as exc:
        raise RuntimeError(f"{description} failed: {command[0]} was not found") from exc
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode(errors="replace").strip() if exc.stderr else ""
        detail = f": {stderr}" if stderr else ""
        raise RuntimeError(f"{description} failed{detail}") from exc


def _macos_case_sensitive_output_dir(
    repo_root: pathlib.Path, env: Mapping[str, str]
) -> pathlib.Path:
    hermetic_container_state = _hermetic_container_state_dir(repo_root)
    mount_point = hermetic_container_state / "bazel-output-case-sensitive"
    image = hermetic_container_state / "bazel-output-case-sensitive.sparsebundle"

    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
        return mount_point

    if _path_is_case_sensitive(mount_point):
        return mount_point

    image_size = env.get(HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE_ENV, "300g")
    if not image.exists():
        hermetic_container_state.mkdir(parents=True, exist_ok=True)
        _info(f"creating case-sensitive hermetic_container output image at {image}")
        _run_host_command(
            [
                "hdiutil",
                "create",
                "-size",
                image_size,
                "-type",
                "SPARSEBUNDLE",
                "-fs",
                "Case-sensitive APFS",
                "-volname",
                "mongo-hermetic_container-output",
                str(image),
            ],
            "creating case-sensitive hermetic_container output image",
        )

    mount_point.mkdir(parents=True, exist_ok=True)
    _info(f"mounting case-sensitive hermetic_container output image at {mount_point}")
    _run_host_command(
        ["hdiutil", "attach", "-mountpoint", str(mount_point), "-nobrowse", str(image)],
        "mounting case-sensitive hermetic_container output image",
    )

    if not _path_is_case_sensitive(mount_point):
        raise RuntimeError(
            f"Mounted hermetic_container output path is not case-sensitive: {mount_point}"
        )
    return mount_point


def _default_bazel_user_output_root(env: Mapping[str, str], system: str | None = None) -> str:
    if env.get("HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT"):
        return env["HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT"]

    if system == "Linux" and env.get("TEST_TMPDIR"):
        return str(pathlib.Path(env["TEST_TMPDIR"]) / f"_bazel_{_current_user()}")

    if system == "Windows" and env.get("LOCALAPPDATA"):
        return str(pathlib.Path(env["LOCALAPPDATA"]) / "bazel" / f"_bazel_{_current_user()}")

    if env.get("XDG_CACHE_HOME"):
        cache_root = pathlib.Path(env["XDG_CACHE_HOME"])
    else:
        host_home = _host_home(env)
        cache_root = host_home / ".cache" if host_home else pathlib.Path("/tmp")

    return str(cache_root / "bazel" / f"_bazel_{_current_user()}")


def _hermetic_container_bazel_user_output_root(
    env: Mapping[str, str],
    repo_root: pathlib.Path,
    system: str | None = None,
) -> str:
    if env.get("HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT"):
        return env["HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT"]
    if system in {"Darwin", "Windows"}:
        output_dir = _hermetic_container_state_dir(repo_root) / "bazel-output"
        if system == "Darwin" and not _env_is_false(
            env.get(HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT_ENV)
        ):
            output_dir = _macos_case_sensitive_output_dir(repo_root, env)
        return str(
            output_dir / HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION / f"_bazel_{_current_user()}"
        )
    return _default_bazel_user_output_root(env, system=system)


def _is_relative_to(path: pathlib.Path, parent: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
    except (OSError, ValueError):
        return False
    return True


def _parse_repo_env_assignment(assignment: str) -> tuple[str, str] | None:
    if "=" not in assignment:
        return None
    key, value = assignment.split("=", 1)
    key = key.strip()
    if not key:
        return None
    return key, value.strip().strip("\"'")


def _hardlink_or_copy(src: str, dst: str) -> None:
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def _read_key_value_file(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        contents = path.read_text(encoding="utf-8")
    except OSError:
        return values

    for raw_line in contents.splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def _container_user(system: str) -> str:
    if system == "Windows":
        return ""
    if hasattr(os, "getuid") and hasattr(os, "getgid"):
        return f"{os.getuid()}:{os.getgid()}"
    return ""


@contextlib.contextmanager
def _exclusive_file_lock(lock_path: pathlib.Path):
    """Acquire an inter-process lock on Unix and Windows."""
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock_file:
        try:
            import fcntl
        except ImportError:
            fcntl = None

        if fcntl is not None:
            fcntl.flock(lock_file, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock_file, fcntl.LOCK_UN)
            return

        try:
            import msvcrt
        except ImportError:
            msvcrt = None

        if msvcrt is not None:
            lock_file.seek(0, os.SEEK_END)
            if lock_file.tell() == 0:
                lock_file.write(b"\0")
                lock_file.flush()
            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            return

        # All supported host platforms provide one of the locking APIs above.
        # Keep unsupported platforms usable for dry-run and unit-test paths.
        yield


@contextlib.contextmanager
def _hermetic_container_lock(run_file: pathlib.Path):
    """Serialize use of one deterministic full-container instance."""
    lock_path = run_file.with_name(f"{run_file.name}.lock")
    with _exclusive_file_lock(lock_path):
        yield


def _linux_action_sandbox_base(output_base: pathlib.Path) -> pathlib.Path:
    """Return a writable sandbox path that does not overlap the read-only output base."""
    if not output_base.name:
        raise ValueError("Bazel output base cannot be the filesystem root")
    return output_base.with_name(f"{output_base.name}-mongo-action-sandbox")


def _ensure_linux_action_sandbox_base(output_base: pathlib.Path) -> pathlib.Path:
    """Create and return the writable sandbox path used by local container actions."""
    sandbox_base = _linux_action_sandbox_base(output_base)
    sandbox_base.mkdir(parents=True, exist_ok=True)
    return sandbox_base


def _linux_shared_install_dir(output_base: pathlib.Path) -> pathlib.Path:
    """Return the root for per-configuration bazel-bin/install convenience trees."""
    if not output_base.name:
        raise ValueError("Bazel output base cannot be the filesystem root")
    return output_base.with_name(f"{output_base.name}-mongo-shared-install")


def _host_temp_shared_install_dir(output_base: pathlib.Path) -> pathlib.Path:
    """Return a host-temp shared install root outside Bazel's output-root hierarchy."""
    if not output_base.name:
        raise ValueError("Bazel output base cannot be the filesystem root")
    return pathlib.Path(tempfile.gettempdir()) / f"{output_base.name}-mongo-shared-install"


def _linux_native_shared_install_dir(output_base: pathlib.Path) -> pathlib.Path:
    """Return the host-temp shared install root for native Linux actions."""
    return _host_temp_shared_install_dir(output_base)


def _macos_shared_install_dir(output_base: pathlib.Path) -> pathlib.Path:
    """Return the host-local shared install root for Darwin actions."""
    return _host_temp_shared_install_dir(output_base)


def _remove_path(path: pathlib.Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def _clean_directory_contents(path: pathlib.Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for child in path.iterdir():
        _remove_path(child)


def _symlink_target(link: pathlib.Path) -> pathlib.Path | None:
    if not link.is_symlink():
        return None
    target = pathlib.Path(os.readlink(link))
    if not target.is_absolute():
        target = link.parent / target
    return target
