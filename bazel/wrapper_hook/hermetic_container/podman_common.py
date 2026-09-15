"""The Podman contract shared between the Evergreen setup script and the wrapper.

``buildscripts/prepare_podman_for_bazel.py`` (host preparation before Bazel runs)
and ``bazel/wrapper_hook/hermetic_container/podman.py`` (the Bazel wrapper's
runtime management) must agree on the environment variables, directory naming,
and Docker-shim detection that select and scope the rootless Podman runtime.
This module is the single source of that contract: it deliberately imports
nothing from the rest of the wrapper so it can be used by either side.
"""

from __future__ import annotations

import pathlib
import re
import shlex
import subprocess
from collections.abc import Callable, Mapping, Sequence

PODMAN_RUNTIME_DIR_PREFIX = "mongo-linux-podman-runtime-"
PODMAN_STORAGE_DIR_PREFIX = "mongo-linux-podman-storage-"
PODMAN_TASK_ID_ENV = "MONGO_PODMAN_TASK_ID"
PODMAN_REQUIRED_ENV = "MONGO_PODMAN_REQUIRED"
PODMAN_AUTH_FILE_ENV = "REGISTRY_AUTH_FILE"
PODMAN_CONFIG_ENV = "CONTAINERS_CONF"

COMMAND_TIMEOUT_SECONDS = 60
CONTAINER_ACTIONS_DISABLED_VALUES = {"0", "false", "no", "off"}
TRUE_VALUES = {"1", "true", "yes", "on"}

Runner = Callable[..., subprocess.CompletedProcess[str]]
Which = Callable[[str], str | None]


def env_is_true(value: str | None) -> bool:
    return value is not None and value.strip().lower() in TRUE_VALUES


def run(argv: Sequence[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(argv), check=False, text=True, **kwargs)


def command_name(command: str) -> str | None:
    try:
        argv = shlex.split(command)
    except ValueError:
        return None
    return pathlib.Path(argv[0]).name if argv else None


def is_podman_docker_shim(command: str, runner: Runner = run) -> bool:
    """Return whether *command* is Podman's Docker-compatibility entry point.

    The podman-docker package commonly installs a command named ``docker``. Its
    name alone is therefore insufficient to decide whether the Podman-specific
    rootless runtime environment and ``--userns=keep-id`` options are needed.
    """
    if command_name(command) in {None, "podman"}:
        return False

    try:
        result = runner(
            [*shlex.split(command), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired, ValueError):
        return False

    version_output = f"{result.stdout}\n{result.stderr}".lower()
    return (
        "emulate docker cli using podman" in version_output
        or re.search(r"(^|\n)\s*podman version\b", version_output) is not None
    )


def podman_task_id(env: Mapping[str, str]) -> str:
    return env.get(PODMAN_TASK_ID_ENV, "")
