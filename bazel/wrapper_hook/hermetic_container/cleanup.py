"""Hermetic container output cleanup and expunge."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
from collections.abc import Sequence

from .config import HermeticContainerConfig
from .constants import HERMETIC_CONTAINER_OUTPUT_BASE_PRESERVED_DIRS, REPO_ROOT
from .env import _info
from .fsutil import _clean_directory_contents, _remove_path


def _expunge_hermetic_container_outputs(
    docker_instance: object, config: HermeticContainerConfig
) -> int:
    _info("stopping hermetic_container container before expunging output root")
    for command in ["stop", "rm"]:
        docker_instance._run_silent_command(  # pylint: disable=protected-access
            docker_instance._with_docker_machine(  # pylint: disable=protected-access
                f"{docker_instance.docker_command} {command} {docker_instance.instance_name}"
            ),
            ignore_output=True,
        )

    pathlib.Path(config.hermetic_container_run_file).unlink(missing_ok=True)

    output_root = pathlib.Path(config.bazel_user_output_root)
    if output_root.is_symlink():
        output_root.unlink()
    elif output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    _info(f"expunged hermetic_container output root: {output_root}")
    return 0


def _clean_hermetic_container_outputs(
    docker_instance: object, config: HermeticContainerConfig
) -> int:
    if docker_instance.is_running():
        _info("shutting down Bazel server in hermetic_container container")
        rc = docker_instance.send_command(["shutdown"])
        if rc:
            return rc

    output_root = pathlib.Path(config.bazel_user_output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    workspace_digest = getattr(docker_instance, "bazel_output_base_digest", "") or getattr(
        docker_instance, "workspace_hex_digest", ""
    )
    output_base = output_root / workspace_digest if workspace_digest else output_root
    output_base.mkdir(parents=True, exist_ok=True)

    preserved_dir_names = set(HERMETIC_CONTAINER_OUTPUT_BASE_PRESERVED_DIRS)
    if REPO_ROOT.name:
        preserved_dir_names.add(REPO_ROOT.name)

    for child in output_base.iterdir():
        if child.name in preserved_dir_names:
            _clean_directory_contents(child)
        else:
            _remove_path(child)

    for dirname in preserved_dir_names:
        (output_base / dirname).mkdir(parents=True, exist_ok=True)

    _info(f"cleaned hermetic_container output base: {output_base}")
    return 0


def _clean_requested_expunge(args: Sequence[str]) -> bool:
    return any(arg in {"--expunge", "--expunge_async"} for arg in args)


def _clean_host_outputs(bazel_real: str, args: Sequence[str]) -> int:
    _info("cleaning local Bazel output root")
    return subprocess.run([bazel_real, *args], check=False, cwd=REPO_ROOT).returncode
