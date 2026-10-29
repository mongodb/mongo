"""Shared persistent-container configuration, publication, and runner preflight.

The filenames retain the original Linux protocol names for existing output bases.
Platform modules choose action strategies and container policies.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Iterator

from bazel.toolchains.cc.container_action import exclusive_file_lock

from .constants import REPO_ROOT
from .distro import _safe_name
from .env import _info
from .fsutil import _linux_action_sandbox_base, _run_container_network_command
from .podman import (
    _compact_runtime_detail,
    _container_runtime_env,
    _is_podman_command,
    _podman_anonymous_runtime_env,
    _podman_authentication_failure,
    _print_container_command_output,
)

CONTAINER_ACTIONS_CONFIG_FILENAME = "mongo_linux_container_actions.json"
CONTAINER_ACTIONS_LOCK_FILENAME = "mongo_linux_container_actions.lock"
CONTAINER_ACTIONS_GENERATION_FILENAME = "mongo_linux_output_base_generation"


def state_dir(repo_root: pathlib.Path = REPO_ROOT) -> pathlib.Path:
    return repo_root / ".tmp" / "linux_container_actions"


@contextlib.contextmanager
def lock(output_base: pathlib.Path) -> Iterator[None]:
    """Serializes config publication, requiring a real lock even for unsupported hosts."""
    with exclusive_file_lock(
        output_base.resolve() / CONTAINER_ACTIONS_LOCK_FILENAME, required=True
    ):
        yield


def ensure_container_image(docker_command: str, image: str) -> bool:
    """Pulls the build container image up front so racing actions never pull it.

    Dozens of dynamically scheduled local branches can start simultaneously; letting each
    `docker run` trigger an implicit pull of the same image is slow and has corrupted
    Docker layer stores in practice. Pull once, before Bazel starts.
    """
    docker = shlex.split(docker_command)
    try:
        runtime_env = _container_runtime_env(docker)
    except OSError as exc:
        _info(f"could not prepare Podman runtime directory: {exc}")
        return False
    inspect = subprocess.run(
        [*docker, "image", "inspect", image],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=runtime_env,
    )
    if inspect.returncode == 0:
        return True

    _info(f"pulling build container image {image}")
    # Keep stdout clean: commands like aquery/compiledb parse this process's stdout.
    pull = _run_container_network_command(
        [*docker, "pull", image],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=runtime_env,
        description=f"pulling build container image {image}",
    )
    _print_container_command_output(pull)
    if (
        pull.returncode
        and _is_podman_command(docker)
        and _podman_authentication_failure(pull)
        and runtime_env is not None
    ):
        # Quay's public build images should remain usable when an Evergreen host has
        # an expired robot login. Retry anonymously without mutating the host auth
        # file; private images still fail normally after this second attempt.
        anonymous_env = _podman_anonymous_runtime_env(runtime_env)
        _info(
            f"registry credentials were rejected while pulling {image}; "
            "retrying without host credentials"
        )
        pull = _run_container_network_command(
            [*docker, "pull", image],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=anonymous_env,
            description=f"anonymous pull of build container image {image}",
        )
        _print_container_command_output(pull)
        runtime_env = anonymous_env
    if pull.returncode:
        return False

    inspect = subprocess.run(
        [*docker, "image", "inspect", image],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=runtime_env,
    )
    return inspect.returncode == 0


def output_base_generation(output_base: pathlib.Path) -> tuple[str, bool]:
    """Returns the output base's action-container generation token, creating it on first use."""
    output_base.mkdir(parents=True, exist_ok=True)
    generation_path = output_base / CONTAINER_ACTIONS_GENERATION_FILENAME
    new_generation = False
    try:
        generation = generation_path.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        new_generation = True
        generation = os.urandom(16).hex()
        temporary = generation_path.with_name(f".{generation_path.name}.{os.getpid()}.{generation}")
        temporary.write_text(generation + "\n", encoding="utf-8")
        try:
            # A hard link publishes a completely written token without replacing a
            # generation concurrently created by another wrapper process.
            os.link(temporary, generation_path)
        except FileExistsError:
            pass
        finally:
            temporary.unlink(missing_ok=True)
        generation = generation_path.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9a-f]{32}", generation):
        raise RuntimeError(f"Invalid action container generation: {generation_path}")
    return generation, new_generation


def publish_container_actions_config(
    config: dict,
    output_base: pathlib.Path,
    generation: str,
    default_prefix: str,
) -> tuple[pathlib.Path, dict]:
    """Names the action container after the config and writes the config atomically."""
    config["output_base_generation"] = generation
    prefix = config.get("container_prefix", default_prefix)
    digest = hashlib.sha256(
        (
            f"{config['repo_root']}|{output_base}|{config['image']}|"
            f"{config['container_layout_version']}|{generation}"
        ).encode()
    ).hexdigest()[:12]
    config["container_name"] = _safe_name(f"{prefix}_{digest}")[:120]
    path = output_base / CONTAINER_ACTIONS_CONFIG_FILENAME
    content = json.dumps(config, indent=2, sort_keys=True) + "\n"
    try:
        if path.read_text(encoding="utf-8") == content:
            return path, config
    except OSError:
        pass

    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        text=True,
    )
    temporary_path = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as temporary:
            temporary.write(content)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)
    return path, config


def persistent_container_options(output_base: pathlib.Path, runner: pathlib.Path) -> list[str]:
    """Build the common protocol options before platform-specific action strategies."""
    sandbox_base = _linux_action_sandbox_base(output_base)
    return [
        "--experimental_enable_persistent_container_sandbox",
        f"--experimental_persistent_container_python={sys.executable}",
        f"--experimental_persistent_container_runner={runner}",
        f"--experimental_persistent_container_config={output_base / CONTAINER_ACTIONS_CONFIG_FILENAME}",
        f"--experimental_persistent_container_worker_dir={sandbox_base / 'persistent-workers'}",
        f"--sandbox_base={sandbox_base}",
    ]


def preflight_container_runner(
    runner: pathlib.Path,
    config_path: pathlib.Path,
    *,
    timeout: float,
    timeout_description: str = "container preflight",
    report_output: bool = False,
) -> tuple[bool, str]:
    """Start a runner's container, preserving each platform's diagnostic policy."""
    options = {"errors": "replace"} if report_output else {}
    try:
        result = subprocess.run(
            [sys.executable, str(runner), "--ensure-container", str(config_path)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            **options,
        )
    except subprocess.TimeoutExpired:
        return False, f"{timeout_description} timed out after {timeout}s"

    if report_output:
        detail = ((result.stderr or "") + (result.stdout or "")).strip()
        if detail:
            print(detail, file=sys.stderr, flush=True)
    else:
        detail = result.stderr or result.stdout
    return result.returncode == 0, _compact_runtime_detail(detail)
