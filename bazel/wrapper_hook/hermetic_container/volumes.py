"""Container volume and environment collection, including git-layer images."""

from __future__ import annotations

import hashlib
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
from collections.abc import Mapping

from .constants import (
    DEFAULT_CONTAINER_CA_BUNDLE,
    DEFAULT_HOST_CA_BUNDLE,
    HERMETIC_CONTAINER_GIT_LAYER_ENV,
    MACOS_CROSS_PASSTHROUGH_ENVS,
    MACOS_CROSS_PATH_ENVS,
    WINDOWS_CROSS_PASSTHROUGH_ENVS,
    WINDOWS_CROSS_PATH_ENVS,
)
from .distro import DockerImage
from .env import _env_is_false, _env_is_true
from .fsutil import (
    _current_user,
    _exclusive_file_lock,
    _hermetic_container_state_dir,
    _host_home,
    _is_relative_to,
    _run_container_network_command,
)
from .podman import _container_runtime_env


def _hermetic_container_git_layer_enabled(env: Mapping[str, str], system: str) -> bool:
    value = env.get(HERMETIC_CONTAINER_GIT_LAYER_ENV)
    if _env_is_false(value):
        return False
    if _env_is_true(value):
        return True
    return system in {"Darwin", "Windows"}


def _hermetic_container_git_layer_dockerfile(base_image: DockerImage) -> str:
    return f"""FROM {base_image.full_name}
USER root
RUN set -eux; \\
    if command -v dnf >/dev/null 2>&1; then \\
        dnf install -y git tar gzip unzip patch xz bzip2 ca-certificates python3; \\
        dnf install -y ncurses-compat-libs || true; \\
        dnf clean all && rm -rf /var/cache/dnf; \\
    elif command -v yum >/dev/null 2>&1; then \\
        yum install -y git tar gzip unzip patch xz bzip2 ca-certificates python3; \\
        yum install -y ncurses-compat-libs || true; \\
        yum clean all && rm -rf /var/cache/yum; \\
    elif command -v apt-get >/dev/null 2>&1; then \\
        export DEBIAN_FRONTEND=noninteractive; \\
        apt-get update; \\
        apt-get install -y --no-install-recommends git tar gzip unzip patch xz-utils bzip2 ca-certificates python3; \\
        apt-get install -y --no-install-recommends libtinfo5 || true; \\
        rm -rf /var/lib/apt/lists/*; \\
    elif command -v zypper >/dev/null 2>&1; then \\
        zypper --non-interactive install git tar gzip unzip patch xz bzip2 ca-certificates python3; \\
        zypper --non-interactive install libncurses5 || true; \\
        zypper clean --all; \\
    else \\
        command -v git >/dev/null 2>&1; \\
        command -v tar >/dev/null 2>&1; \\
        command -v python3 >/dev/null 2>&1; \\
    fi; \\
    git --version; \\
    tar --version; \\
    python3 --version
"""


def _hermetic_container_image_with_git_layer(
    repo_root: pathlib.Path,
    base_image: DockerImage,
    docker_command: str | None = None,
) -> tuple[DockerImage, str]:
    dockerfile_content = _hermetic_container_git_layer_dockerfile(base_image)
    layer_hash = hashlib.sha256(dockerfile_content.encode()).hexdigest()[:16]
    dockerfile = (
        _hermetic_container_state_dir(repo_root)
        / "dockerfiles"
        / f"git-layer-{layer_hash}.Dockerfile"
    )
    dockerfile.parent.mkdir(parents=True, exist_ok=True)
    if not dockerfile.exists() or dockerfile.read_text(encoding="utf-8") != dockerfile_content:
        dockerfile.write_text(dockerfile_content, encoding="utf-8")

    docker_image = DockerImage(
        full_name=f"mongo-hermetic_container-local/bazel-remote-execution-git:{layer_hash}",
        repository="mongo-hermetic_container-local",
        image_name=f"bazel-remote-execution-git:{layer_hash}",
        digest_or_tag=layer_hash,
    )

    if docker_command is None:
        return docker_image, str(dockerfile)

    docker = shlex.split(docker_command)
    if not docker:
        raise RuntimeError("The Docker command for the derived hermetic container is empty")
    runtime_env = _container_runtime_env(docker)
    lock_path = dockerfile.parent.parent / "git-layer.lock"

    with _exclusive_file_lock(lock_path):
        inspect = subprocess.run(
            [*docker, "image", "inspect", "--format={{.Id}}", docker_image.full_name],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            env=runtime_env,
        )
        if inspect.returncode:
            build = _run_container_network_command(
                [
                    *docker,
                    "build",
                    "-t",
                    docker_image.full_name,
                    "-f",
                    str(dockerfile),
                    str(repo_root),
                ],
                check=False,
                stdout=sys.stderr,
                stderr=sys.stderr,
                env=runtime_env,
                description=f"building derived hermetic container image {docker_image.full_name}",
            )
            if build.returncode:
                raise RuntimeError(
                    f"failed to build derived hermetic container image {docker_image.full_name}"
                )
            inspect = subprocess.run(
                [*docker, "image", "inspect", "--format={{.Id}}", docker_image.full_name],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                env=runtime_env,
            )

        image_id = inspect.stdout.strip()
        if inspect.returncode or re.fullmatch(r"sha256:[0-9a-f]{64}", image_id) is None:
            raise RuntimeError(
                f"could not determine the content identity of derived hermetic container "
                f"image {docker_image.full_name}"
            )

        # Image IDs include the parent image and rootfs layer diff IDs. They therefore change
        # when the package manager installs different versions. The Dockerfile hash alone is
        # not sufficient for the action cache key when OS repository versions float.
        content_tag = f"git-{image_id.removeprefix('sha256:')}"
        content_image = DockerImage(
            full_name=f"{docker_image.repository}/bazel-remote-execution-git:{content_tag}",
            repository=docker_image.repository,
            image_name=f"bazel-remote-execution-git:{content_tag}",
            digest_or_tag=content_tag,
        )
        tagged = subprocess.run(
            [*docker, "tag", docker_image.full_name, content_image.full_name],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=runtime_env,
        )
        if tagged.returncode:
            raise RuntimeError(
                f"failed to assign content-addressed tag {content_image.full_name} "
                "to derived hermetic container image"
            )

    return content_image, str(dockerfile)


def _container_path(path: pathlib.Path | str, system: str | None = None) -> str:
    value = str(path)
    if system == "Windows" or re.match(r"^[A-Za-z]:[\\/]", value):
        windows_path = pathlib.PureWindowsPath(value)
        drive = windows_path.drive.rstrip(":")
        parts = [part for part in windows_path.parts[1:] if part not in {"\\", "/"}]
        return pathlib.PurePosixPath("/", drive, *parts).as_posix()
    return pathlib.PurePosixPath(value).as_posix()


def _path_volume(
    path: pathlib.Path,
    read_only: bool = False,
    system: str | None = None,
    container_path: str | None = None,
) -> str | None:
    if not path.exists():
        return None
    suffix = ":ro" if read_only else ""
    return f"{path}:{container_path or _container_path(path, system)}{suffix}"


def _host_ca_bundle(env: Mapping[str, str]) -> pathlib.Path | None:
    candidates = [
        env.get("MONGO_HERMETIC_CONTAINER_CA_BUNDLE"),
        env.get("SSL_CERT_FILE"),
        str(DEFAULT_HOST_CA_BUNDLE),
    ]

    for candidate in candidates:
        if not candidate:
            continue
        path = pathlib.Path(candidate)
        if path.is_file():
            return path
    return None


def _prepare_grpc_roots_dir(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
) -> pathlib.Path | None:
    ca_bundle = _host_ca_bundle(env)
    if not ca_bundle:
        return None

    grpc_roots_dir = repo_root / ".tmp" / "hermetic_container" / "grpc_roots"
    grpc_roots_dir.mkdir(parents=True, exist_ok=True)
    roots_file = grpc_roots_dir / "roots.pem"
    shutil.copyfile(ca_bundle, roots_file)
    roots_file.chmod(0o644)
    return grpc_roots_dir


def _container_cert_env_values(env: Mapping[str, str]) -> dict[str, str]:
    ca_bundle = _host_ca_bundle(env)
    if ca_bundle:
        container_ca_bundle = env.get(
            "MONGO_HERMETIC_CONTAINER_CONTAINER_CA_BUNDLE",
            DEFAULT_CONTAINER_CA_BUNDLE,
        )
        return {
            "SSL_CERT_FILE": container_ca_bundle,
            "GRPC_DEFAULT_SSL_ROOTS_FILE_PATH": container_ca_bundle,
            "CURL_CA_BUNDLE": container_ca_bundle,
            "REQUESTS_CA_BUNDLE": container_ca_bundle,
        }

    return {
        var_name: env[var_name]
        for var_name in [
            "SSL_CERT_FILE",
            "GRPC_DEFAULT_SSL_ROOTS_FILE_PATH",
            "CURL_CA_BUNDLE",
            "REQUESTS_CA_BUNDLE",
        ]
        if env.get(var_name)
    }


def _collect_volumes(
    repo_root: pathlib.Path,
    bazel_mount: pathlib.Path | None,
    output_root: str,
    env: Mapping[str, str],
    system: str | None = None,
) -> list[str]:
    volumes: list[str] = []
    pathlib.Path(output_root).mkdir(parents=True, exist_ok=True)
    hermetic_container_state = _hermetic_container_state_dir(repo_root)

    volume_paths: list[tuple[pathlib.Path, bool, str | None]] = [
        (pathlib.Path(output_root), False, None),
    ]
    if bazel_mount is not None:
        volume_paths.insert(0, (bazel_mount.parent, True, None))

    host_home = _host_home(env)
    if host_home:
        volume_paths.extend(
            [
                (host_home / ".config" / "engflow_auth", False, None),
                (host_home / ".local" / "bin", True, None),
            ]
        )
    for var_name in [*WINDOWS_CROSS_PATH_ENVS, *MACOS_CROSS_PATH_ENVS]:
        if env.get(var_name):
            path = pathlib.Path(env[var_name])
            if not _is_relative_to(path, hermetic_container_state):
                volume_paths.append((path, True, None))

    for path, read_only, container_path in volume_paths:
        volume = _path_volume(
            path,
            read_only=read_only,
            system=system,
            container_path=container_path,
        )
        if volume:
            volumes.append(volume)

    ca_bundle = _host_ca_bundle(env)
    if ca_bundle:
        container_ca_bundle = env.get(
            "MONGO_HERMETIC_CONTAINER_CONTAINER_CA_BUNDLE",
            DEFAULT_CONTAINER_CA_BUNDLE,
        )
        volumes.append(f"{ca_bundle}:{container_ca_bundle}:ro")
        grpc_roots_dir = _prepare_grpc_roots_dir(repo_root, env)
        if grpc_roots_dir:
            volumes.append(f"{grpc_roots_dir}:/usr/share/grpc:ro")

    # Give non-root container users a writable HOME-adjacent path for hermetic_container state.
    hermetic_container_state.mkdir(parents=True, exist_ok=True)
    volumes.append(
        f"{hermetic_container_state}:{_container_path(hermetic_container_state, system)}"
    )
    return volumes


def _collect_env_vars(
    env: Mapping[str, str],
    fallback_home: pathlib.Path,
    system: str | None = None,
) -> list[str]:
    host_home = _host_home(env)
    if system in {"Darwin", "Windows"}:
        home_path = fallback_home / "home"
        home_path.mkdir(parents=True, exist_ok=True)
        home = _container_path(home_path, system)
    else:
        home = (
            _container_path(host_home, system)
            if host_home
            else _container_path(fallback_home, system)
        )
    env_vars = [
        "MONGO_BAZEL_IN_HERMETIC_CONTAINER=1",
        "BAZELISK_SKIP_WRAPPER=1",
        f"HOME={home}",
        f"USER={_current_user()}",
    ]

    for var_name in [
        "MONGO_HERMETIC_CONTAINER_DISTRO",
        "MONGO_HERMETIC_CONTAINER_IMAGE",
        "SSL_CERT_DIR",
        "REVERSE_REMOTE_API_ATTEMPT_ORDER",
        "GIT_DIR",
        "BAZELISK_BASE_URL",
        "USE_BAZEL_VERSION",
    ]:
        if env.get(var_name):
            env_vars.append(f"{var_name}={env[var_name]}")

    for var_name in [*WINDOWS_CROSS_PASSTHROUGH_ENVS, *MACOS_CROSS_PASSTHROUGH_ENVS]:
        if not env.get(var_name):
            continue
        value = env[var_name]
        if var_name in [*WINDOWS_CROSS_PATH_ENVS, *MACOS_CROSS_PATH_ENVS]:
            value = _container_path(value, system)
        env_vars.append(f"{var_name}={value}")

    for var_name, value in _container_cert_env_values(env).items():
        env_vars.append(f"{var_name}={value}")

    return env_vars
