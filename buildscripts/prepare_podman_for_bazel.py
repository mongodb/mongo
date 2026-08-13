#!/usr/bin/env python3
"""Prepare the rootless Podman runtime used by local Bazel container actions."""

from __future__ import annotations

import json
import os
import pathlib
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import uuid
from collections.abc import Callable, Mapping, Sequence

_PODMAN_RUNTIME_DIR_PREFIX = "mongo-linux-podman-runtime-"
_PODMAN_STORAGE_DIR_PREFIX = "mongo-linux-podman-storage-"
_PODMAN_TASK_ID_ENV = "MONGO_PODMAN_TASK_ID"
_COMMAND_TIMEOUT_SECONDS = 60
_CONTAINER_ACTIONS_DISABLED_VALUES = {"0", "false", "no", "off"}

Runner = Callable[..., subprocess.CompletedProcess[str]]
Which = Callable[[str], str | None]


def _run(argv: Sequence[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(argv), check=False, text=True, **kwargs)


def _command_name(command: str) -> str | None:
    try:
        argv = shlex.split(command)
    except ValueError:
        return None
    return pathlib.Path(argv[0]).name if argv else None


def _is_podman_docker_shim(command: str, runner: Runner = _run) -> bool:
    if _command_name(command) in {None, "podman"}:
        return False

    try:
        result = runner(
            [*shlex.split(command), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired, ValueError):
        return False

    version_output = f"{result.stdout}\n{result.stderr}".lower()
    return "emulate docker cli using podman" in version_output or "podman version" in version_output


def podman_is_in_use(
    env: Mapping[str, str],
    *,
    system: str | None = None,
    which: Which = shutil.which,
    runner: Runner = _run,
) -> bool:
    """Match the Linux runtime selection used by hermetic_container_integration.py."""

    if (system or platform.system()) != "Linux":
        return False
    if env.get("MONGO_LINUX_CONTAINER_ACTIONS", "").lower() in _CONTAINER_ACTIONS_DISABLED_VALUES:
        return False

    podman = which("podman")
    if podman is None:
        return False

    explicit_runtime = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND")
    if explicit_runtime:
        return _command_name(explicit_runtime) == "podman" or _is_podman_docker_shim(
            explicit_runtime, runner
        )

    docker = which("docker")
    if docker is None or _is_podman_docker_shim(docker, runner):
        return True

    # The Bazel wrapper prefers Docker when its daemon is healthy and falls back to
    # Podman when Docker is unavailable.
    try:
        docker_info = runner(
            [docker, "info"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        return True
    return docker_info.returncode != 0


def _podman_task_root(env: Mapping[str, str], runtime_root: pathlib.Path | None) -> pathlib.Path:
    root = pathlib.Path("/tmp") if runtime_root is None else runtime_root
    task_id = env.get(_PODMAN_TASK_ID_ENV, "")
    if task_id:
        safe_task_id = re.sub(r"[^A-Za-z0-9_.-]", "_", task_id)
        root /= f"mongo-linux-podman-task-{safe_task_id}"
    return root


def _ensure_owned_directory(path: pathlib.Path, uid: int) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = path.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or (
        platform.system() == "Linux" and metadata.st_uid != uid
    ):
        raise OSError(f"Podman directory is not a directory owned by uid {uid}: {path}")
    path.chmod(0o700)


def _podman_storage_config(runtime_dir: pathlib.Path, uid: int) -> pathlib.Path:
    storage_dir = runtime_dir.parent / f"{_PODMAN_STORAGE_DIR_PREFIX}{uid}"
    graph_root = storage_dir / "graphroot"
    run_root = storage_dir / "runroot"
    _ensure_owned_directory(storage_dir, uid)
    _ensure_owned_directory(graph_root, uid)
    _ensure_owned_directory(run_root, uid)

    config_path = storage_dir / "storage.conf"
    content = "\n".join(
        [
            "[storage]",
            'driver = "overlay"',
            f"graphroot = {json.dumps(str(graph_root))}",
            f"runroot = {json.dumps(str(run_root))}",
            f"rootless_storage_path = {json.dumps(str(graph_root))}",
            "",
        ]
    )
    mount_program = shutil.which("fuse-overlayfs")
    if mount_program:
        content += "\n".join(
            [
                "[storage.options.overlay]",
                f"mount_program = {json.dumps(mount_program)}",
                "",
            ]
        )
    try:
        metadata = config_path.lstat()
    except FileNotFoundError:
        metadata = None
    if metadata is not None:
        if not stat.S_ISREG(metadata.st_mode) or (
            platform.system() == "Linux" and metadata.st_uid != uid
        ):
            raise OSError(
                "Podman storage configuration is not a file owned by " f"uid {uid}: {config_path}"
            )
        if config_path.read_text(encoding="utf-8") == content:
            config_path.chmod(0o600)
            return config_path

    temporary_path = config_path.with_name(f".{config_path.name}.{os.getpid()}.{uuid.uuid4().hex}")
    try:
        temporary_path.write_text(content, encoding="utf-8")
        temporary_path.chmod(0o600)
        temporary_path.replace(config_path)
    finally:
        temporary_path.unlink(missing_ok=True)
    return config_path


def _podman_runtime_env(
    env: Mapping[str, str],
    *,
    uid: int,
    runtime_root: pathlib.Path | None,
) -> dict[str, str]:
    task_root = _podman_task_root(env, runtime_root)
    runtime_dir = task_root / f"{_PODMAN_RUNTIME_DIR_PREFIX}{uid}"
    _ensure_owned_directory(runtime_dir, uid)
    storage_config = _podman_storage_config(runtime_dir, uid)

    runtime_env = dict(env)
    runtime_env.update(
        {
            "CONTAINERS_STORAGE_CONF": str(storage_config),
            "XDG_RUNTIME_DIR": str(runtime_dir),
            "TMPDIR": str(runtime_dir),
            "TMP": str(runtime_dir),
            "TEMP": str(runtime_dir),
        }
    )
    return runtime_env


def _emit(result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)


def prepare_podman_for_bazel(
    env: Mapping[str, str] | None = None,
    *,
    runner: Runner = _run,
    system: str | None = None,
    which: Which = shutil.which,
    uid: int | None = None,
    runtime_root: pathlib.Path | None = None,
) -> int:
    current_env = dict(env or os.environ)
    if not podman_is_in_use(current_env, system=system, which=which, runner=runner):
        print("Podman is not the selected Bazel container runtime; no preparation needed.")
        return 0

    podman_uid = os.getuid() if uid is None else uid
    runtime_env = _podman_runtime_env(current_env, uid=podman_uid, runtime_root=runtime_root)
    print(f"Preparing Podman for Bazel container actions for uid {podman_uid}.")
    print(f"Using task-scoped Podman storage: {runtime_env['CONTAINERS_STORAGE_CONF']}.")

    linger = runner(
        ["sudo", "loginctl", "enable-linger", str(podman_uid)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    _emit(linger)
    if linger.returncode != 0:
        print(
            "WARNING: Could not enable systemd user lingering; continuing with Podman cleanup.",
            file=sys.stderr,
        )

    for command in (
        ["podman", "system", "migrate"],
        ["podman", "system", "reset", "--force"],
        ["podman", "info"],
    ):
        result = runner(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=runtime_env,
        )
        _emit(result)
        if result.returncode != 0:
            return result.returncode

    return 0


if __name__ == "__main__":
    sys.exit(prepare_podman_for_bazel())
