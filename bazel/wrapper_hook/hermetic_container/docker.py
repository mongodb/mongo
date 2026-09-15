"""Docker daemon checks and Linux container runtime selection."""

from __future__ import annotations

import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
from collections.abc import Mapping

from .constants import DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS, MACOS_CROSS_DEFAULT_CONFIG_ENV
from .env import _env_is_true
from .podman import (
    PODMAN_STALE_RUNTIME_MARKER,
    _compact_runtime_detail,
    _container_runtime_env,
    _is_podman_command,
    _recover_stale_podman_runtime,
)
from .podman_common import (
    PODMAN_REQUIRED_ENV,
)
from .podman_common import (
    is_podman_docker_shim as _is_podman_docker_shim,
)

DOCKER_API_TOO_NEW_RE = re.compile(r"Maximum supported API version is ([0-9.]+)")


WSL_DOCKER_HOST_MODE = "wsl"


WSL_DOCKER_HOST_ENV = "MONGO_HERMETIC_CONTAINER_WSL_DOCKER_HOST"


WSL_DOCKER_API_VERSION_ENV = "MONGO_HERMETIC_CONTAINER_WSL_DOCKER_API_VERSION"


WSL_DOCKER_API_VERSION_DEFAULT = "1.52"


WSL_DRIVE_MOUNT_PREFIX_ENV = "MONGO_HERMETIC_CONTAINER_WSL_DRIVE_MOUNT_PREFIX"


def _use_wsl_docker(env: Mapping[str, str]) -> bool:
    return env.get("MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE", "").lower() == WSL_DOCKER_HOST_MODE


def _docker_daemon_status(docker_command: str) -> tuple[bool, str]:
    # Plain `info` is supported by both Docker and Podman. Docker's
    # `{{.ServerVersion}}` template is not portable to older Podman releases.
    argv = [*shlex.split(docker_command or "docker"), "info"]
    try:
        runtime_env = _container_runtime_env(argv)
    except OSError as exc:
        return False, f"could not prepare Podman runtime directory: {exc}"
    try:
        result = subprocess.run(
            argv,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
            env=runtime_env,
        )
    except FileNotFoundError:
        return False, f"Docker command not found: {argv[0]}"
    except subprocess.TimeoutExpired:
        return (
            False,
            f"{' '.join(shlex.quote(part) for part in argv)} timed out after "
            f"{DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS}s",
        )

    if result.returncode == 0:
        return True, ""

    detail = (result.stderr or result.stdout).strip()
    if _is_podman_command(argv) and PODMAN_STALE_RUNTIME_MARKER in detail:
        assert runtime_env is not None
        recovered, recovery_detail = _recover_stale_podman_runtime(argv, runtime_env)
        if recovered:
            return True, ""
        detail += f"\nAutomatic Podman runtime recovery failed: {recovery_detail}"

    api_match = DOCKER_API_TOO_NEW_RE.search(detail)
    if api_match and "DOCKER_API_VERSION" not in (runtime_env or os.environ):
        api_version = api_match.group(1)
        retry_env = dict(runtime_env or os.environ)
        retry_env["DOCKER_API_VERSION"] = api_version
        try:
            retry_result = subprocess.run(
                argv,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=retry_env,
            )
        except subprocess.TimeoutExpired:
            retry_result = None

        if retry_result is not None and retry_result.returncode == 0:
            os.environ["DOCKER_API_VERSION"] = api_version
            return True, ""

    if not detail:
        detail = f"{' '.join(shlex.quote(part) for part in argv)} exited with {result.returncode}"
    return False, _compact_runtime_detail(detail)


def _select_linux_container_runtime(env: Mapping[str, str]) -> tuple[str | None, str]:
    """Select a Docker-compatible Linux container runtime.

    An explicit HERMETIC_CONTAINER_DOCKER_COMMAND remains authoritative unless the
    task explicitly requires Podman. Otherwise prefer Docker when it is usable and
    fall back to Podman, which supports the image, mount, exec, network, user, and
    lifecycle operations used by Linux action containers. If the Docker command is
    Podman's compatibility shim, use the real Podman executable so that the action
    wrapper applies Podman's rootless bind-mount handling.
    """
    podman_required = _env_is_true(env.get(PODMAN_REQUIRED_ENV))
    explicit = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND")
    if explicit:
        if podman_required:
            explicit_parts = shlex.split(explicit)
            if not explicit_parts or pathlib.Path(explicit_parts[0]).name != "podman":
                return (
                    None,
                    "Podman is required for this task but an explicit non-Podman runtime was set",
                )
        ready, detail = _docker_daemon_status(explicit)
        return (explicit, "") if ready else (None, detail)

    failures: list[str] = []
    candidates: list[tuple[str, str]] = []
    docker_command = shutil.which("docker")
    podman_command = shutil.which("podman")
    if podman_required:
        if podman_command is None:
            return None, "Podman is required for this task but the podman command is not found"
        ready, detail = _docker_daemon_status(podman_command)
        return (podman_command, "") if ready else (None, detail)

    if docker_command is None:
        failures.append("docker: command not found")
    elif _is_podman_docker_shim(docker_command):
        if podman_command is None:
            failures.append(
                "docker: Podman compatibility shim detected, but the podman command is not found"
            )
        else:
            candidates.append(("podman", podman_command))
    else:
        candidates.append(("docker", docker_command))

    if podman_command is None:
        failures.append("podman: command not found")
    elif not any(command == podman_command for _, command in candidates):
        candidates.append(("podman", podman_command))

    for candidate, command in candidates:
        ready, detail = _docker_daemon_status(command)
        if ready:
            return command, ""
        failures.append(f"{candidate}: {detail}")
    return None, "; ".join(failures)


def _print_docker_daemon_error(
    docker_command: str,
    detail: str,
    system: str,
) -> None:
    print(
        "ERROR: hermetic_container requires a running Docker daemon, but Docker is not reachable.",
        file=sys.stderr,
    )
    if system == "Darwin":
        print(
            "Start Docker Desktop and wait for `docker info` to succeed, then retry the Bazel command.",
            file=sys.stderr,
        )
    elif system == "Windows":
        print(
            "Start the WSL2 Docker Engine or fix DOCKER_HOST/HERMETIC_CONTAINER_DOCKER_COMMAND, "
            "then retry the Bazel command.",
            file=sys.stderr,
        )
        print(
            "Windows cross builds need Linux containers; native Windows-container Docker runtimes "
            "cannot run the Linux RBE image.",
            file=sys.stderr,
        )
    else:
        print(
            "Start Docker or fix DOCKER_HOST/HERMETIC_CONTAINER_DOCKER_COMMAND, then retry the Bazel command.",
            file=sys.stderr,
        )
    print(
        f"Checked with: {' '.join(shlex.quote(part) for part in [*shlex.split(docker_command or 'docker'), 'info'])}",
        file=sys.stderr,
    )
    if detail:
        print("Docker said:", file=sys.stderr)
        print(detail, file=sys.stderr)
    print(
        "To bypass hermetic_container for this command, set MONGO_BAZEL_USE_HERMETIC_CONTAINER=0.",
        file=sys.stderr,
    )
    if system == "Darwin":
        print(
            f"To disable opted-in automatic macOS cross config selection, unset "
            f"{MACOS_CROSS_DEFAULT_CONFIG_ENV} or set it to 0.",
            file=sys.stderr,
        )
