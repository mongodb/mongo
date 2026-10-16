"""Bind-mount paths and verify input visibility for cross-build action containers.

Callers supply the host and container paths. Action routing, tool preparation, and
retry policy remain in the platform runner.
"""

from __future__ import annotations

import os
import pathlib
import platform
import re
import sys
import time
from collections.abc import Sequence

from bazel.hermetic_container import container_paths
from bazel.toolchains.cc import container_action as shared
from bazel.toolchains.cc.container_engine import (
    DOCKER_HOST_MODE_ENV,
    WSL_DOCKER_HOST_MODES,
    _wsl_local_docker_repair_instructions,
)

WSL_DRIVE_MOUNT_PREFIX_ENV = "MONGO_MACOS_CROSS_ACTION_WSL_DRIVE_MOUNT_PREFIX"

DEFAULT_WSL_DRIVE_MOUNT_PREFIX = "/mnt"

WSL_CONTAINER_INPUT_VISIBILITY_TIMEOUT_SECONDS = 120


def container_path(path: pathlib.Path | str) -> str:
    """Convert a host path to the path used inside the Linux action container."""
    return container_paths.container_path(
        path, windows=platform.system() == "Windows", lowercase_drive=True
    )


def wsl_volume_source(path: pathlib.PureWindowsPath | str) -> str:
    """Apply the action runner's configured WSL Docker source path policy."""
    return container_paths.wsl_volume_source(path, _drive_mount_prefix())


def wsl_absolute_path_mounts(
    paths: Sequence[pathlib.Path | str | None],
) -> list[str]:
    """Mount WSL paths at the same path inside a WSL-backed action container.

    Bazel can create absolute symlinks in the Windows output tree. When viewed
    from WSL, those symlinks target paths such as /mnt/z/.... The regular
    Windows cross mounts intentionally translate the same files to container
    paths such as /z/... and /mongo-execroot/..., which leaves the absolute
    symlinks dangling. Mirroring the relevant WSL paths preserves those
    symlink targets without exposing unrelated portions of a drive.
    """
    if (
        platform.system() != "Windows"
        or os.environ.get(DOCKER_HOST_MODE_ENV) not in WSL_DOCKER_HOST_MODES
    ):
        return []

    wsl_paths: set[str] = set()
    for path in paths:
        if path is None:
            continue
        windows_path = pathlib.PureWindowsPath(str(path))
        if not windows_path.drive:
            continue
        wsl_paths.add(wsl_volume_source(str(windows_path)).rstrip("/"))

    # Do not add nested mounts when an ancestor already mirrors the whole
    # tree. This also keeps the Docker command small for paths under the
    # checkout or Bazel output base.
    selected_paths: list[str] = []
    for path in sorted(wsl_paths, key=lambda value: (value.count("/"), value)):
        if any(path == parent or path.startswith(f"{parent}/") for parent in selected_paths):
            continue
        selected_paths.append(path)

    mounts: list[str] = []
    for path in selected_paths:
        mounts.extend(["-v", f"{path}:{path}"])
    return mounts


def _docker_volume_source(path: pathlib.Path, *, resolve: bool = True) -> str:
    resolved = path.resolve() if resolve else path
    if (
        platform.system() == "Windows"
        and os.environ.get(DOCKER_HOST_MODE_ENV) in WSL_DOCKER_HOST_MODES
    ):
        return wsl_volume_source(str(resolved))
    return str(resolved)


def volume_at(
    source: pathlib.Path,
    destination: pathlib.Path,
    read_only: bool = False,
    resolve_source: bool = True,
) -> list[str]:
    suffix = ":ro" if read_only else ""
    return [
        "-v",
        f"{_docker_volume_source(source, resolve=resolve_source)}:{container_path(destination)}{suffix}",
    ]


def execroot_alias_mounts(
    execroot: pathlib.Path | None, container_execroot: pathlib.Path
) -> list[str]:
    if platform.system() != "Windows" or execroot is None:
        return []

    mounts: list[str] = []
    for alias_name in ("bazel-out", "external"):
        alias = execroot / alias_name
        try:
            # Bazel may create these entries as Windows junctions. Python can
            # report the junction itself as the resolved path even though WSL
            # can mount and follow the raw path, so retain the raw alias as a
            # fallback instead of silently omitting the mount.
            if not alias.exists() and not os.path.lexists(alias):
                continue
            resolved = alias.resolve(strict=True)
        except OSError:
            resolved = alias
        # Docker Engine running inside WSL can resolve a Windows junction when
        # it opens the raw DrvFS path. Using Path.resolve() first can instead
        # select the junction target as a different bind source, which may not
        # contain the current Bazel output tree.
        if os.environ.get(DOCKER_HOST_MODE_ENV) in WSL_DOCKER_HOST_MODES:
            source = alias
        else:
            source = resolved if resolved != alias and resolved.exists() else alias
        mounts.extend(
            volume_at(
                source,
                container_execroot / alias_name,
                resolve_source=source != alias,
            )
        )
    return mounts


def _windows_drive_letters(paths: Sequence[pathlib.Path | str | None]) -> list[str]:
    """Returns the lower-case drive letters used by the given Windows paths."""
    drives: set[str] = set()
    for path in paths:
        if path is None:
            continue
        drive = pathlib.PureWindowsPath(str(path)).drive.rstrip(":")
        if len(drive) == 1 and drive.isalpha():
            drives.add(drive.lower())
    return sorted(drives)


def _should_mount_drives_in_docker_vm() -> bool:
    """Only the TCP-reached WSL engine on Windows hosts needs host-side drive mounts."""
    return (
        platform.system() == "Windows"
        and os.environ.get(DOCKER_HOST_MODE_ENV) in WSL_DOCKER_HOST_MODES
        and os.environ.get("DOCKER_HOST", "").startswith("tcp://")
    )


def ensure_docker_vm_drive_mounts(
    docker: Sequence[str],
    image: str,
    paths: Sequence[pathlib.Path | str | None],
) -> None:
    if not _should_mount_drives_in_docker_vm():
        return
    drives = _windows_drive_letters(paths)
    if not drives:
        return
    prefix = _drive_mount_prefix()
    print(
        "INFO: ensuring drives "
        + ", ".join(f"{drive.upper()}:" for drive in drives)
        + f" are mounted under {prefix} inside the WSL Docker VM",
        file=sys.stderr,
    )
    mounted, detail = shared.mount_windows_drives_in_docker_vm(docker, image, drives, prefix)
    if mounted:
        print(f"INFO: WSL Docker VM drive mounts:\n{detail}", file=sys.stderr)
    else:
        print(
            f"WARNING: could not mount host drives inside the WSL Docker VM: {detail}",
            file=sys.stderr,
        )


def _container_preflight_paths(
    cwd: pathlib.Path, container_cwd: pathlib.Path, tool_args: Sequence[str]
) -> list[tuple[pathlib.Path, pathlib.Path]]:
    """Returns host/container paths that prove action inputs are visible in the container."""
    paths: list[tuple[pathlib.Path, pathlib.Path]] = [(cwd, container_cwd)]
    response_files: list[pathlib.Path] = []
    for argument in tool_args:
        if not argument.startswith("@"):
            continue
        response_file = pathlib.Path(argument[1:])
        if not response_file.is_absolute():
            response_file = cwd / response_file
        if response_file.is_file():
            response_files.append(response_file)
            try:
                relative_response_file = response_file.relative_to(cwd)
            except ValueError:
                continue
            paths.append((response_file, container_cwd / relative_response_file))

    action_path_pattern = re.compile(r"(?<![A-Za-z0-9_./-])(?:bazel-out|external)/[^\s\"']+")
    for response_file in response_files:
        try:
            contents = response_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in action_path_pattern.finditer(contents):
            relative_path = match.group(0).rstrip(",")
            host_path = cwd / relative_path
            if host_path.is_file():
                paths.append((host_path, container_cwd / relative_path))

    unique_paths: list[tuple[pathlib.Path, pathlib.Path]] = []
    seen: set[pathlib.Path] = set()
    for host_path, container_path in paths:
        if container_path in seen:
            continue
        seen.add(container_path)
        unique_paths.append((host_path, container_path))
    return unique_paths


def verify_container_action_inputs(
    docker: Sequence[str],
    name: str,
    cwd: pathlib.Path,
    container_cwd: pathlib.Path,
    tool_args: Sequence[str],
    *,
    report_error: bool = True,
    deadline: float | None = None,
) -> bool:
    if deadline is None:
        deadline = time.monotonic() + shared.DOCKER_REQUEST_TIMEOUT_SECONDS
    # pathlib.Path uses WindowsPath when this wrapper runs on Windows. That
    # turns a POSIX container path such as /mongo-execroot into
    # ``\\mongo-execroot``. Docker is invoked through WSL, so always pass a
    # normalized POSIX path to the container-side command.
    preflight = [
        (host_path, container_path(destination))
        for host_path, destination in _container_preflight_paths(cwd, container_cwd, tool_args)
    ]
    for host_path, normalized_container_path in _missing_container_paths(
        docker, name, preflight, deadline=deadline
    ):
        if report_error:
            if platform.system() == "Windows":
                repair = (
                    "The Docker daemon must run in the same WSL distribution as the drvfs mount.\n"
                    f"{_wsl_local_docker_repair_instructions()}"
                )
            else:
                repair = "Verify that the Docker daemon has access to the host path."
            print(
                "ERROR: the Docker daemon cannot see a cross action input in the "
                f"container: {normalized_container_path}\n"
                f"Host path exists at: {host_path}\n{repair}",
                file=sys.stderr,
            )
        return False
    return True


def _missing_container_paths(
    docker: Sequence[str],
    name: str,
    paths: Sequence[tuple[pathlib.Path, str]],
    *,
    deadline: float,
) -> list[tuple[pathlib.Path, str]]:
    """Returns unverified input pairs using one manifest on every host platform."""
    missing = shared.missing_container_path_indices(
        docker, name, [destination for _, destination in paths], deadline=deadline
    )
    return list(paths) if missing is None else [paths[index] for index in sorted(missing)]


def _drive_mount_prefix() -> str:
    return container_paths.drive_mount_prefix(
        os.environ.get(WSL_DRIVE_MOUNT_PREFIX_ENV, DEFAULT_WSL_DRIVE_MOUNT_PREFIX)
    )


def wait_for_container_action_inputs(
    docker: Sequence[str],
    name: str,
    cwd: pathlib.Path,
    container_cwd: pathlib.Path,
    tool_args: Sequence[str],
    timeout: float = WSL_CONTAINER_INPUT_VISIBILITY_TIMEOUT_SECONDS,
) -> bool:
    """Wait for filesystem propagation within a wall-clock deadline."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if verify_container_action_inputs(
            docker,
            name,
            cwd,
            container_cwd,
            tool_args,
            report_error=False,
            deadline=deadline,
        ):
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(1.0, remaining))
    return False
