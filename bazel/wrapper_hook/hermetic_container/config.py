"""Hermetic container configuration dataclasses and config construction."""

from __future__ import annotations

import dataclasses
import enum
import hashlib
import json
import os
import pathlib
import platform
from collections.abc import Mapping

from .constants import REPO_ROOT
from .distro import (
    DockerImage,
    _linux_host_container_supported,
    _safe_name,
    load_remote_execution_containers,
    normalize_arch,
    parse_docker_image,
    select_distro,
)
from .download import _resolve_container_bazel
from .engflow import _hermetic_container_engflow_auth_helper
from .env import _env_is_true
from .fsutil import (
    _container_user,
    _hermetic_container_bazel_user_output_root,
    _hermetic_container_state_dir,
)
from .volumes import (
    _collect_env_vars,
    _collect_volumes,
    _hermetic_container_git_layer_enabled,
    _hermetic_container_image_with_git_layer,
)


@dataclasses.dataclass(frozen=True)
class HermeticContainerConfig:
    """Configuration needed to construct a hermetic_container DockerInstance."""

    distro: str
    docker_image: DockerImage
    instance_name: str
    bazel_real: str
    bazel_command: str
    bazel_user_output_root: str
    hermetic_container_run_file: str
    user: str
    volumes: list[str]
    env_vars: list[str]
    platform: str
    privileged: bool
    dockerfile: str = ""
    credential_helper: str = ""


class IntegrationMode(enum.Enum):
    """High-level route selected by tools/bazel after wrapper-hook processing."""

    DIRECT = "direct"
    FULL_CONTAINER = "full_container"
    LINUX_CROSS_HOST_RBE = "linux_cross_host_rbe"
    LINUX_HOST_CONTAINER = "linux_host_container"
    MACOS_CROSS_HOST = "macos_cross_host"
    WINDOWS_CROSS_HOST = "windows_cross_host"


def build_hermetic_container_config(
    bazel_real: str,
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
    containers: Mapping[str, Mapping[str, str]] | None = None,
    system: str | None = None,
    docker_command: str | None = None,
) -> HermeticContainerConfig:
    system = system or platform.system()
    if system == "Linux" and not _linux_host_container_supported():
        raise RuntimeError(
            "Full-container hermetic_container is only supported on Linux x86_64 and aarch64 hosts"
        )
    containers = containers or load_remote_execution_containers()
    arch = normalize_arch()
    distro = select_distro(containers, env=env, arch=arch)

    image_override = env.get("MONGO_HERMETIC_CONTAINER_IMAGE")
    container_url = image_override or containers[distro]["container-url"]
    docker_image = parse_docker_image(container_url)
    dockerfile = ""
    if _hermetic_container_git_layer_enabled(env, system):
        docker_image, dockerfile = _hermetic_container_image_with_git_layer(
            repo_root, docker_image, docker_command=docker_command
        )
    credential_helper = _hermetic_container_engflow_auth_helper(repo_root, env, system)

    bazel_real_path = pathlib.Path(bazel_real).resolve()
    bazel_command, bazel_mount = _resolve_container_bazel(
        bazel_real_path,
        env=env,
        repo_root=repo_root,
        system=system,
    )
    output_root = _hermetic_container_bazel_user_output_root(env, repo_root, system=system)
    image_hash = hashlib.sha256(docker_image.full_name.encode()).hexdigest()[:12]
    instance_name = _safe_name(f"mongo_hermetic_container_{distro}_{arch}_{image_hash}")
    hermetic_container_state = _hermetic_container_state_dir(repo_root)
    run_file = hermetic_container_state / f"{instance_name}.run"

    return HermeticContainerConfig(
        distro=distro,
        docker_image=docker_image,
        instance_name=instance_name,
        bazel_real=str(bazel_real_path),
        bazel_command=bazel_command,
        bazel_user_output_root=output_root,
        hermetic_container_run_file=str(run_file),
        user=_container_user(system),
        volumes=_collect_volumes(repo_root, bazel_mount, output_root, env, system=system),
        env_vars=_collect_env_vars(env, hermetic_container_state, system=system),
        platform=env.get("MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM", ""),
        privileged=_env_is_true(env.get("MONGO_HERMETIC_CONTAINER_PRIVILEGED")),
        dockerfile=dockerfile,
        credential_helper=credential_helper,
    )


def _config_fingerprint(config: HermeticContainerConfig) -> str:
    config_parts = {
        "bazel_command": config.bazel_command,
        "bazel_real": config.bazel_real,
        "bazel_user_output_root": config.bazel_user_output_root,
        "docker_image": config.docker_image.full_name,
        "dockerfile": config.dockerfile,
        "credential_helper": config.credential_helper,
        "env_vars": sorted(config.env_vars),
        "platform": config.platform,
        "privileged": config.privileged,
        "user": config.user,
        "volumes": sorted(config.volumes),
    }
    encoded = json.dumps(config_parts, sort_keys=True).encode()
    return hashlib.sha256(encoded).hexdigest()


def _run_file_matches_config(run_file: pathlib.Path, fingerprint: str) -> bool:
    try:
        return run_file.read_text(encoding="utf-8").strip() == fingerprint
    except OSError:
        return False


def _hermetic_container_output_base(
    config: HermeticContainerConfig, docker_instance: object
) -> pathlib.Path:
    workspace_digest = getattr(docker_instance, "bazel_output_base_digest", "") or getattr(
        docker_instance, "workspace_hex_digest", ""
    )
    output_root = pathlib.Path(config.bazel_user_output_root)
    return output_root / workspace_digest if workspace_digest else output_root
