"""Host distro detection and container image parsing."""

from __future__ import annotations

import dataclasses
import os
import pathlib
import platform
import re
from collections.abc import Callable, Mapping

from .constants import (
    AL2023_DISTRO,
    LINUX_HOST_CONTAINER_ARCHES,
    MONGO_TOOLCHAIN_VERSION_V5_FILE,
    REMOTE_EXECUTION_CONTAINERS_FILE,
)

_DISTRO_PATTERN_MAP = {
    "Ubuntu 18*": "ubuntu18",
    "Ubuntu 20*": "ubuntu20",
    "Ubuntu 22*": "ubuntu22",
    "Pop!_OS 22*": "ubuntu22",
    "Ubuntu 24*": "ubuntu24",
    "Amazon Linux 2": "amazon_linux_2",
    "Amazon Linux 2023": AL2023_DISTRO,
    "Debian GNU/Linux 10": "debian10",
    "Debian GNU/Linux 12": "debian12",
    "Red Hat Enterprise Linux 8*": "rhel8",
    "Red Hat Enterprise Linux 9*": "rhel9",
    "Red Hat Enterprise Linux 10*": "rhel10",
    "Fedora*": "rhel10",
    "SLES 15*": "suse15",
}


@dataclasses.dataclass(frozen=True)
class DockerImage:
    """A Docker image split into hermetic_container's repository/image fields."""

    full_name: str
    repository: str
    image_name: str
    digest_or_tag: str


def _read_bzl_mapping(path: pathlib.Path, symbol: str) -> dict:
    values: dict[str, object] = {}
    with open(path, encoding="utf-8") as f:
        code = compile(f.read(), str(path), "exec")
        exec(code, {}, values)
    mapping = values.get(symbol)
    if not isinstance(mapping, dict):
        raise RuntimeError(f"{path} did not define {symbol}")
    return mapping


def load_remote_execution_containers() -> dict[str, dict[str, str]]:
    return _read_bzl_mapping(REMOTE_EXECUTION_CONTAINERS_FILE, "REMOTE_EXECUTION_CONTAINERS")


def _parse_os_release(path: pathlib.Path) -> dict[str, str]:
    values = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "=" not in line:
                    continue
                key, value = line.rstrip().split("=", 1)
                values[key] = value.strip('"')
    except OSError:
        return {}
    return values


def detect_host_distro(
    os_release_path: pathlib.Path = pathlib.Path("/etc/os-release"),
    system_release_path: pathlib.Path = pathlib.Path("/etc/system-release"),
) -> str | None:
    values = _parse_os_release(os_release_path)
    name = values.get("NAME")
    version_id = values.get("VERSION_ID")
    if not name or not version_id:
        return None

    distro = f"{name} {version_id}"
    for distro_pattern, simplified_name in _DISTRO_PATTERN_MAP.items():
        if "*" not in distro_pattern:
            if distro == distro_pattern:
                if simplified_name == AL2023_DISTRO:
                    try:
                        system_release = system_release_path.read_text(encoding="utf-8")
                    except OSError:
                        system_release = ""
                    minor_match = re.search(
                        r"Amazon Linux release 2023\.([0-9]+)\.",
                        system_release,
                    )
                    if minor_match and minor_match.group(1) == "3":
                        return "amazon_linux_2023_3"
                return simplified_name
            continue

        prefix, suffix = distro_pattern.split("*", 1)
        if distro.startswith(prefix) and distro.endswith(suffix):
            return simplified_name
    return None


def normalize_arch(machine: str | None = None) -> str:
    machine = (machine or platform.machine()).lower()
    if machine in {"arm64", "aarch64"}:
        return "aarch64"
    if machine in {"amd64", "x86_64"}:
        return "x86_64"
    return machine


def _linux_host_container_supported(machine: str | None = None) -> bool:
    return normalize_arch(machine) in LINUX_HOST_CONTAINER_ARCHES


def _linux_host_container_distro(
    env: Mapping[str, str],
    machine: str | None = None,
    containers: Mapping[str, Mapping[str, str]] | None = None,
    detected_distro: str | None = None,
) -> str | None:
    """Returns the host distro when it has both a pinned container and a toolchain."""
    arch = normalize_arch(machine)
    distro = env.get("MONGO_HERMETIC_CONTAINER_DISTRO") or detected_distro or detect_host_distro()
    if not distro:
        return None
    try:
        containers = containers or load_remote_execution_containers()
    except (OSError, RuntimeError):
        return None
    if distro in containers and has_mongo_toolchain(distro, arch):
        return distro
    return None


def _toolchain_key(distro: str, arch: str) -> str:
    return f"{distro}_{arch}"


def has_mongo_toolchain(
    distro: str,
    arch: str,
    toolchain_file: pathlib.Path = MONGO_TOOLCHAIN_VERSION_V5_FILE,
) -> bool:
    try:
        contents = toolchain_file.read_text(encoding="utf-8")
    except OSError:
        return False
    return f'"{_toolchain_key(distro, arch)}"' in contents


def select_distro(
    containers: Mapping[str, Mapping[str, str]],
    env: Mapping[str, str] = os.environ,
    detected_distro: str | None = None,
    arch: str | None = None,
    toolchain_supported: Callable[[str, str], bool] = has_mongo_toolchain,
) -> str:
    override = env.get("MONGO_HERMETIC_CONTAINER_DISTRO")
    if override:
        if override not in containers:
            raise RuntimeError(
                f"MONGO_HERMETIC_CONTAINER_DISTRO={override} is not a known RBE container"
            )
        return override

    arch = arch or normalize_arch()
    distro = detected_distro or detect_host_distro()
    if distro in containers and toolchain_supported(distro, arch):
        return distro

    if AL2023_DISTRO in containers:
        return AL2023_DISTRO

    raise RuntimeError(f"Could not find {AL2023_DISTRO} fallback RBE container")


def parse_docker_image(container_url: str) -> DockerImage:
    full_name = container_url.removeprefix("docker://")
    repository, sep, image_name = full_name.rpartition("/")
    if not sep:
        raise RuntimeError(f"Invalid Docker image URL: {container_url}")

    digest_or_tag = "latest"
    if "@sha256:" in image_name:
        digest_or_tag = image_name.split("@sha256:", 1)[1]
    elif ":" in image_name:
        digest_or_tag = image_name.rsplit(":", 1)[1]

    return DockerImage(
        full_name=full_name,
        repository=repository,
        image_name=image_name,
        digest_or_tag=digest_or_tag,
    )


def _is_digest_pinned_container_url(container_url: str) -> bool:
    """Returns whether a container URL names an immutable sha256 image digest."""
    image = container_url.removeprefix("docker://")
    return re.search(r"@sha256:[0-9a-f]{64}$", image) is not None


def _safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)
