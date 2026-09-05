"""MongoDB-specific integration between tools/bazel and the vendored hermetic_container."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import enum
import getpass
import hashlib
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
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from collections.abc import Callable, Iterator, Mapping, Sequence

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
HERMETIC_CONTAINER_SOURCE_ROOT = REPO_ROOT / "bazel" / "hermetic_container"
REMOTE_EXECUTION_CONTAINERS_FILE = (
    REPO_ROOT / "bazel" / "platforms" / "remote_execution_containers.bzl"
)
MONGO_TOOLCHAIN_VERSION_V5_FILE = (
    REPO_ROOT / "bazel" / "toolchains" / "cc" / "mongo_linux" / "mongo_toolchain_version_v5.bzl"
)

AL2023_DISTRO = "amazon_linux_2023"
HERMETIC_CONTAINER_DISABLED_VALUES = {"0", "false", "no", "off"}
HERMETIC_CONTAINER_ENABLED_VALUES = {"1", "true", "yes", "on"}
CONTAINER_MARKER_PATHS = (pathlib.Path("/.dockerenv"), pathlib.Path("/run/.containerenv"))
CONTAINER_CGROUP_PATH = pathlib.Path("/proc/1/cgroup")
CONTAINER_CGROUP_RE = re.compile(r"docker|kubepods|containerd|libpod|lxc", re.IGNORECASE)
KUBERNETES_SERVICE_HOST_ENV = "KUBERNETES_SERVICE_HOST"
DEFAULT_HOST_CA_BUNDLE = pathlib.Path("/etc/ssl/certs/ca-certificates.crt")
DEFAULT_CONTAINER_CA_BUNDLE = "/tmp/mongo-hermetic_container-ca-certificates.crt"
DEFAULT_HERMETIC_CONTAINER_CONTAINER_ARCH = "x86_64"
LINUX_HOST_CONTAINER_ARCHES = frozenset(["aarch64", "ppc64le", "s390x", "x86_64"])
LINUX_CONTAINER_ACTIONS_ENV = "MONGO_LINUX_CONTAINER_ACTIONS"
LINUX_DYNAMIC_SCHEDULING_ENV = "MONGO_LINUX_DYNAMIC_SCHEDULING"
NATIVE_TOOLCHAIN_CONFIG = "native_toolchain"
LINUX_DYNAMIC_LOCAL_LOAD_FACTOR = "0.125"
CONTAINERIZED_BES_KEYWORD = "MONGO_BUILD_CONTAINERIZED"
LINUX_CONTAINER_ACTIONS_CONFIG_FILENAME = "mongo_linux_container_actions.json"
LINUX_CONTAINER_ACTIONS_LOCK_FILENAME = "mongo_linux_container_actions.lock"
LINUX_CONTAINER_ACTIONS_GENERATION_FILENAME = "mongo_linux_output_base_generation"
LINUX_CONTAINER_ACTIONS_LAYOUT_VERSION = "v5"
LINUX_CONTAINER_ACTION_WRAPPER_SCRIPT = (
    REPO_ROOT / "bazel" / "toolchains" / "cc" / "mongo_linux" / "linux_container_action_wrapper.py"
)
LINUX_DYNAMIC_CONTAINER_MNEMONICS = ("CppCompile", "Rustc", "RustcMetadata")
LINUX_LOCAL_CONTAINER_MNEMONICS = ("HistoricRuntime",)
LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS = (
    "CppLink",
    "CppArchive",
    "SolibSymlink",
    "ExtractDebugInfo",
    "StripDebugInfo",
    "CcGenerateIntermediateDwp",
    "CcGenerateDwp",
)
LINUX_CONTAINER_TOOL_MNEMONICS = (
    "Action",
    "CcLtoBackendCompile",
    "CargoBuildScriptRun",
    "CargoLints",
    "CertificateGenerator",
    "Clippy",
    "ConfigHeaderGen",
    "CopyFile",
    "CopyWheel",
    "CppLTOIndexing",
    "DownloadWheel",
    "EmbedPublicKeyHeader",
    "ExtractCargoTomlEnvVars",
    "ExtractCertificateGenerationYear",
    "GenProto",
    "GenProtoDescriptorSet",
    "Genrule",
    "GpgExportArmored",
    "GpgSign",
    "IdlcGenerator",
    "MdBookBuild",
    "MongoPrettyPrinterTestCreation",
    "ProstGenProto",
    "PyCompile",
    "PyO3StubGen",
    "PyWriteBuildData",
    "PythonZipper",
    "RustBindgen",
    "RustProtocGen",
    "RustUnpretty",
    "RustWasmBindgen",
    "Rustdoc",
    "RustdocTestWriter",
    "RustdocZip",
    "Rustfmt",
    "TemplateRenderer",
    "WasmAotCompile",
    "WitBindgenC",
    "WheelInstall",
)
LINUX_LOCAL_TEST_MNEMONICS = ("CoverageReport", "TestRunner")
LINUX_HOST_CONTAINER_COMMANDS = frozenset(
    [
        "aquery",
        "build",
        "coverage",
        "cquery",
        "fetch",
        "query",
        "run",
        "test",
    ]
)
LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS = frozenset(["aquery", "cquery", "fetch", "query"])
LINUX_BAZEL_ENV = "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL"
LINUX_BAZEL_SHA256_ENV = "MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_SHA256"
CONTAINER_BAZEL_ENV = "MONGO_HERMETIC_CONTAINER_CONTAINER_BAZEL"
HERMETIC_CONTAINER_GIT_LAYER_ENV = "MONGO_HERMETIC_CONTAINER_GIT_LAYER"
HERMETIC_CONTAINER_REPOSITORY_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND = "/usr/bin/python3 bazel/workspace_status.py"
HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION = "v2"
HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT_ENV = (
    "MONGO_HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT"
)
HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE_ENV = "MONGO_HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE"
HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER_ENV = "MONGO_HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER"
ENGFLOW_AUTH_CLUSTER = "sodalite.cluster.engflow.com"
ENGFLOW_AUTH_URL_PREFIX = "https://github.com/EngFlow/auth/releases/download/v0.0.13/"
ENGFLOW_AUTH_LINUX_RELEASES = {
    "aarch64": (
        "engflow_auth_linux_arm64",
        "ad5ffee1e6db926f5066aa40ee35517b1993851d0063ac121dbf5b407c81e2bf",
    ),
    "x86_64": (
        "engflow_auth_linux_x64",
        "b731bae21628b2be321c24b342854c6ed1ed0326010e62a2ecf0b5650a56cf1a",
    ),
}
WINDOWS_CROSS_CONFIG = "windows-cross-x86_64"
WINDOWS_CROSS_DEFAULT_CONFIG_ENV = "MONGO_WINDOWS_CROSS_DEFAULT_CONFIG"
WINDOWS_CROSS_ACTION_WRAPPER_ENV = "MONGO_WINDOWS_CROSS_ACTION_WRAPPER"
WINDOWS_CROSS_RBE_CONTAINER_IMAGE_ENV = "MONGO_WINDOWS_CROSS_RBE_CONTAINER_IMAGE"
WINDOWS_CROSS_RBE_POOL_ENV = "MONGO_WINDOWS_CROSS_RBE_POOL"
WINDOWS_CROSS_REMOTE_EXECUTOR = "grpcs://sodalite.cluster.engflow.com"
WINDOWS_CROSS_RBE_POOL = "x86_64"
WINDOWS_CROSS_DEFAULT_COMMANDS = frozenset(
    [
        "aquery",
        "build",
        "cquery",
        "fetch",
    ]
)
LINUX_CROSS_RBE_CONFIG_RE = re.compile(
    r"^linux-(?P<target_arch>ppc64le|s390x)"
    r"(?:-(?P<target_distro>rhel8|rhel9))?"
    r"-cross-rbe"
    r"(?:-(?P<exec_arch>x86_64|arm64|aarch64))?$"
)
LINUX_CROSS_RBE_DEFAULT_TARGET_DISTRO = "rhel9"
LINUX_CROSS_RBE_DEFAULT_EXEC_DISTRO = "rhel9"
LINUX_CROSS_RBE_DEFAULT_EXEC_ARCH = "x86_64"
LINUX_CROSS_RBE_REMOTE_EXECUTOR = "grpcs://sodalite.cluster.engflow.com"
LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV = "MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE"
LINUX_CROSS_RBE_POOL_ENV = "MONGO_LINUX_CROSS_RBE_POOL"
LINUX_CROSS_RBE_X86_POOL = "x86_64"
LINUX_CROSS_RBE_ARM_POOL = "default"
MACOS_CROSS_CONFIGS = frozenset(
    [
        "macos-cross-arm64",
        "macos-cross-x86_64",
    ]
)
MACOS_CROSS_DEFAULT_CONFIG_ENV = "MONGO_MACOS_CROSS_DEFAULT_CONFIG"
MACOS_CROSS_TEST_RUNNER_ENV = "MONGO_MACOS_CROSS_TEST_RUNNER"
MACOS_CROSS_SPLIT_TEST_RUNNER_ENV = "MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER"
MACOS_CROSS_ACTION_WRAPPER_ENV = "MONGO_MACOS_CROSS_ACTION_WRAPPER"
MACOS_CROSS_RBE_CONTAINER_IMAGE_ENV = "MONGO_MACOS_CROSS_RBE_CONTAINER_IMAGE"
MACOS_CROSS_RBE_POOL_ENV = "MONGO_MACOS_CROSS_RBE_POOL"
MACOS_CROSS_LOCAL_CPU_RESOURCES_ENV = "MONGO_MACOS_CROSS_LOCAL_CPU_RESOURCES"
MACOS_CROSS_LOCAL_TEST_JOBS_ENV = "MONGO_MACOS_CROSS_LOCAL_TEST_JOBS"
MACOS_CROSS_REMOTE_EXECUTOR = "grpcs://sodalite.cluster.engflow.com"
MACOS_CROSS_RBE_POOL = "default"
MACOS_CROSS_DEFAULT_LOCAL_RESOURCE_VALUE = "HOST_CPUS"
DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS = 60
DOCKER_API_TOO_NEW_RE = re.compile(r"Maximum supported API version is ([0-9.]+)")
CONTAINER_NETWORK_RETRY_ATTEMPTS = 3
CONTAINER_NETWORK_RETRY_DELAY_SECONDS = 1
PODMAN_RUNTIME_DIR_PREFIX = "mongo-linux-podman-runtime-"
PODMAN_STORAGE_DIR_PREFIX = "mongo-linux-podman-storage-"
PODMAN_TASK_ID_ENV = "MONGO_PODMAN_TASK_ID"
PODMAN_STALE_RUNTIME_MARKER = "invalid internal status"
CROSS_HOST_ACTION_CONFIG_FILENAME = "mongo_cross_host_action.json"
RESMOKE_DEPS_PATH_SUFFIX = "_resmoke_deps_path.txt"
RESMOKE_DEPS_PATH_MAP_ENV = "DEPS_PATH_MAP_FILE"
HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV = "MONGO_HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS"
HERMETIC_CONTAINER_SYMLINK_PREFIX = "bazel-"
HERMETIC_CONTAINER_ROOT_CONVENIENCE_SYMLINKS = (
    "bazel-bin",
    "bazel-out",
    "bazel-testlogs",
)
HERMETIC_CONTAINER_OUTPUT_BASE_PRESERVED_DIRS = frozenset(["action_cache", "execroot", "external"])
WSL_DOCKER_HOST_MODE = "wsl"
WSL_DOCKER_HOST_ENV = "MONGO_HERMETIC_CONTAINER_WSL_DOCKER_HOST"
WSL_DOCKER_API_VERSION_ENV = "MONGO_HERMETIC_CONTAINER_WSL_DOCKER_API_VERSION"
WSL_DOCKER_API_VERSION_DEFAULT = "1.52"
WSL_DRIVE_MOUNT_PREFIX_ENV = "MONGO_HERMETIC_CONTAINER_WSL_DRIVE_MOUNT_PREFIX"
WINDOWS_CROSS_PATH_ENVS = [
    "MONGO_WINDOWS_CROSS_LLVM_PATH",
    "MONGO_WINDOWS_CROSS_SYSROOT_PATH",
]
WINDOWS_TOOLCHAIN_PIN_ENVS = [
    "BAZEL_VS",
    "BAZEL_VC",
    "BAZEL_VC_FULL_VERSION",
    "BAZEL_WINSDK_FULL_VERSION",
    "MONGO_VC_REDIST_FULL_VERSION",
]
WINDOWS_CROSS_SDK_ROOT_ENVS = [
    "MONGO_WINDOWS_CROSS_WINSDK_ROOT",
    "BAZEL_WINSDK_ROOT",
    "WINDOWSSDKDIR",
]
WINDOWS_CROSS_PASSTHROUGH_ENVS = [
    *WINDOWS_CROSS_PATH_ENVS,
    *WINDOWS_TOOLCHAIN_PIN_ENVS,
    *WINDOWS_CROSS_SDK_ROOT_ENVS,
    "MONGO_WINDOWS_CROSS_LLVM_URL",
    "MONGO_WINDOWS_CROSS_LLVM_SHA256",
    "MONGO_WINDOWS_CROSS_LLVM_STRIP_PREFIX",
    "MONGO_WINDOWS_CROSS_LLVM_VERSION",
    "MONGO_WINDOWS_CROSS_SYSROOT_URL",
    "MONGO_WINDOWS_CROSS_SYSROOT_SHA256",
    "MONGO_WINDOWS_CROSS_SYSROOT_STRIP_PREFIX",
]
MACOS_CROSS_PATH_ENVS = [
    "LLVM_PATH",
    "MACOS_SDK_PATH",
]
MACOS_CROSS_PASSTHROUGH_ENVS = [
    *MACOS_CROSS_PATH_ENVS,
    "MACOS_MIN_VERSION",
]
BAZEL_COMMANDS = frozenset(
    [
        "aquery",
        "build",
        "canonicalize-flags",
        "clean",
        "coverage",
        "cquery",
        "dump",
        "fetch",
        "help",
        "info",
        "license",
        "mobile-install",
        "mod",
        "print_action",
        "query",
        "run",
        "shutdown",
        "sync",
        "test",
        "vendor",
        "version",
    ]
)
# Bazel accepts both `--flag=value` and `--flag value`, but argv alone does not expose
# whether an option consumes the following token. The cross-host run/test planners need
# this list only to distinguish known separate option values from target patterns. Other
# options pass through unchanged. Their `--flag=value` form is unambiguous; supporting
# an option's separate-value form in a cross-host invocation requires adding it here.
CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE = frozenset(
    [
        "--action_env",
        "--build_metadata",
        "--config",
        "--define",
        "--disk_cache",
        "--extra_execution_platforms",
        "--jobs",
        "--platforms",
        "--repo_env",
        "--target_pattern_file",
        "--test_tag_filters",
        "--workspace_status_command",
    ]
)
TEST_FLAGS_WITH_SEPARATE_VALUE = frozenset(
    [
        "--cache_test_results",
        "--flaky_test_attempts",
        "--runs_per_test",
        "--test_filter",
        "--test_output",
        "--test_timeout",
    ]
)
TEST_FLAG_PREFIXES = tuple(flag + "=" for flag in TEST_FLAGS_WITH_SEPARATE_VALUE)
RUN_FLAGS_WITH_SEPARATE_VALUE = frozenset(
    [
        "--run_under",
        "--script_path",
    ]
)
RUN_FLAG_PREFIXES = tuple(flag + "=" for flag in RUN_FLAGS_WITH_SEPARATE_VALUE)
MACOS_CROSS_DEFAULT_COMMANDS = frozenset(["build", "test"])

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


@dataclasses.dataclass(frozen=True)
class WindowsCrossSysrootSpec:
    """Pinned host inputs used to build a Windows cross-compilation sysroot."""

    vc_path: pathlib.Path
    vc_full_version: str
    winsdk_root: pathlib.Path
    winsdk_full_version: str
    sources: dict[str, pathlib.Path]


@dataclasses.dataclass(frozen=True)
class MacOSCrossHostTestPlan:
    """Container build args plus host execution details for macOS cross tests."""

    build_args: list[str]
    startup_args: list[str]
    host_test_options: list[str]
    target_patterns: list[str]
    test_args: list[str]
    test_env: dict[str, str]
    test_tag_filters: list[str]
    build_event_json_file: str | None
    runs_per_test: int
    run_host_tests: bool


@dataclasses.dataclass(frozen=True)
class MacOSCrossHostRunPlan:
    """Container build args plus host execution details for macOS cross run."""

    build_args: list[str]
    target: str
    run_args: list[str]


@dataclasses.dataclass(frozen=True)
class LinuxCrossRBEConfig:
    """Linux target and execution platform selected by a cross-RBE config."""

    target_arch: str
    target_distro: str
    exec_arch: str
    exec_distro: str


class IntegrationMode(enum.Enum):
    """High-level route selected by tools/bazel after wrapper-hook processing."""

    DIRECT = "direct"
    FULL_CONTAINER = "full_container"
    LINUX_CROSS_HOST_RBE = "linux_cross_host_rbe"
    LINUX_HOST_CONTAINER = "linux_host_container"
    MACOS_CROSS_HOST = "macos_cross_host"
    WINDOWS_CROSS_HOST = "windows_cross_host"


def _env_is_false(value: str | None) -> bool:
    return value is not None and value.lower() in HERMETIC_CONTAINER_DISABLED_VALUES


def _env_is_true(value: str | None) -> bool:
    return value is not None and value.lower() in HERMETIC_CONTAINER_ENABLED_VALUES


def _is_running_in_container(
    env: Mapping[str, str] = os.environ,
    container_marker_paths: Sequence[pathlib.Path] = CONTAINER_MARKER_PATHS,
    cgroup_path: pathlib.Path = CONTAINER_CGROUP_PATH,
) -> bool:
    """Return whether the current process appears to be running in a container."""

    if (
        env.get("container")
        or env.get(KUBERNETES_SERVICE_HOST_ENV)
        or any(path.exists() for path in container_marker_paths)
    ):
        return True

    try:
        cgroup = cgroup_path.read_text(encoding="utf-8")
    except OSError:
        return False
    return CONTAINER_CGROUP_RE.search(cgroup) is not None


def _use_wsl_docker(env: Mapping[str, str]) -> bool:
    return env.get("MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE", "").lower() == WSL_DOCKER_HOST_MODE


def _prepare_hermetic_container_process_env(env: Mapping[str, str]) -> None:
    """Prepare environment variables consumed by the in-process hermetic_container module."""

    if not _use_wsl_docker(env):
        return

    os.environ.setdefault("HERMETIC_CONTAINER_VOLUME_SOURCE_MODE", "wsl")
    os.environ.setdefault(
        "DOCKER_HOST",
        env.get(WSL_DOCKER_HOST_ENV) or env.get("DOCKER_HOST") or "tcp://127.0.0.1:2375",
    )
    os.environ.setdefault(
        "DOCKER_API_VERSION",
        env.get(WSL_DOCKER_API_VERSION_ENV)
        or env.get("DOCKER_API_VERSION")
        or WSL_DOCKER_API_VERSION_DEFAULT,
    )
    if env.get(WSL_DRIVE_MOUNT_PREFIX_ENV):
        os.environ.setdefault(
            "HERMETIC_CONTAINER_WSL_DRIVE_MOUNT_PREFIX", env[WSL_DRIVE_MOUNT_PREFIX_ENV]
        )


def _supports_color(stream) -> bool:
    if os.name == "nt":
        return False
    if os.environ.get("NO_COLOR"):
        return False
    try:
        return stream.isatty()
    except Exception:
        return False


def _info_prefix(stream) -> str:
    if _supports_color(stream):
        return "\033[0;32mINFO:\033[0m"
    return "INFO:"


def _info(message: str) -> None:
    print(f"{_info_prefix(sys.stderr)} {message}", file=sys.stderr)


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


def _warning(message: str) -> None:
    print(f"WARNING: {message}", file=sys.stderr)


def _warn_native_fallback(reason: str) -> None:
    _warning(
        f"hermetic_container is unavailable because {reason}; running Bazel natively. "
        "Native build outputs may not be cache-compatible with hermetic builds."
    )


def select_integration_mode(
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
    docker_exists: Callable[[], bool] | None = None,
    args: Sequence[str] = (),
    machine: str | None = None,
    repo_root: pathlib.Path = REPO_ROOT,
) -> IntegrationMode:
    """Return the integration route for the final Bazel invocation."""

    system = system or platform.system()
    explicit = env.get("MONGO_BAZEL_USE_HERMETIC_CONTAINER")
    if _env_is_false(explicit):
        return IntegrationMode.DIRECT

    if system not in {"Darwin", "Linux", "Windows"}:
        return IntegrationMode.DIRECT

    if env.get("MONGO_BAZEL_IN_HERMETIC_CONTAINER") == "1":
        return IntegrationMode.DIRECT

    # Avoid nesting the local build container when Bazel is already running in a
    # container. An explicit opt-in still permits nested containers when needed.
    if system == "Linux" and not _env_is_true(explicit) and _is_running_in_container(env):
        return IntegrationMode.DIRECT

    if system == "Darwin":
        if (
            _env_is_true(env.get(MACOS_CROSS_SPLIT_TEST_RUNNER_ENV))
            and _bazel_command(args) == "test"
            and (
                _macos_cross_config_requested(args, env=env, repo_root=repo_root)
                or _should_default_macos_cross_config(args, env=env, system=system)
            )
        ):
            return IntegrationMode.FULL_CONTAINER
        if (
            _macos_cross_host_bazel_test_candidate(args, env, system)
            or _macos_cross_local_container_action_candidate(args, env, system)
            or _macos_cross_host_run_requested(args, env, system)
            or _should_default_macos_cross_config(args, env=env, system=system)
        ):
            return IntegrationMode.MACOS_CROSS_HOST
        return IntegrationMode.DIRECT

    if system == "Windows":
        if (
            _windows_cross_host_wrapper_action_candidate(args, env, system)
            or _windows_cross_host_run_requested(args, env, system)
            or _should_default_windows_hermetic_container(args, env=env, system=system)
        ):
            return IntegrationMode.WINDOWS_CROSS_HOST
        return IntegrationMode.DIRECT

    # macOS cross configurations retain their original Linux-hosted setup for now. In
    # particular, do not enable the generic Linux persistent-container layer or replace their
    # existing Python/toolchain selection.
    if _macos_cross_config_requested(args, env=env, repo_root=repo_root):
        _warn_native_fallback("macOS cross configurations retain their original setup")
        return IntegrationMode.DIRECT

    if _linux_cross_rbe_config(args, env=env, repo_root=repo_root) is not None:
        return IntegrationMode.LINUX_CROSS_HOST_RBE

    # The native compiler is installed on the host and is not available at the same
    # path inside the pinned Linux action container. Keep the native-toolchain config
    # natively executed even when Linux container actions are enabled by default.
    if _config_requested(args, NATIVE_TOOLCHAIN_CONFIG, env=env, repo_root=repo_root):
        return IntegrationMode.DIRECT

    # Linux builds keep Bazel on the host. Repository-provided build tools run inside
    # the pinned build container when they execute locally; tests, resmoke, repository
    # fetches, and Bazel bookkeeping run natively. Set
    # MONGO_LINUX_CONTAINER_ACTIONS=0 to build like normal without the container.
    if _env_is_false(env.get(LINUX_CONTAINER_ACTIONS_ENV)):
        return IntegrationMode.DIRECT

    arch = normalize_arch(machine)
    if not _linux_host_container_supported(arch):
        _warn_native_fallback(f"host architecture {arch!r} is unsupported")
        return IntegrationMode.DIRECT

    command = _bazel_command(args)
    # Cleaning is intentionally handled by the native Bazel output tree and the
    # hermetic-container output tree separately. It is not an action-running
    # command, so do not report the native fallback as a warning.
    if command == "clean":
        return IntegrationMode.DIRECT
    if command is not None and command not in LINUX_HOST_CONTAINER_COMMANDS:
        _warn_native_fallback(f"Bazel command {command!r} is not supported by host container mode")
        return IntegrationMode.DIRECT

    # Hosts whose distro has no pinned container or hermetic toolchain build like normal.
    host_distro = env.get("MONGO_HERMETIC_CONTAINER_DISTRO") or detect_host_distro()
    if _linux_host_container_distro(env, machine=arch, detected_distro=host_distro) is None:
        if host_distro is None:
            reason = "the host distro could not be detected"
        else:
            reason = (
                f"host distro {host_distro!r} has no pinned RBE container or MongoDB toolchain "
                f"for architecture {arch!r}"
            )
        _warn_native_fallback(reason)
        return IntegrationMode.DIRECT

    # Missing Docker is handled after mode selection. Supported Linux hosts enter
    # container mode and fall back to native, non-containerized build-tool execution
    # when the container services turn out to be unusable at runtime. The explicit
    # MONGO_LINUX_CONTAINER_ACTIONS=0 opt-out above skips container mode entirely.
    return IntegrationMode.LINUX_HOST_CONTAINER


def should_use_hermetic_container(
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
    docker_exists: Callable[[], bool] | None = None,
    args: Sequence[str] = (),
    machine: str | None = None,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    """Return whether tools/bazel should route through the integration layer."""

    return (
        select_integration_mode(
            env=env,
            system=system,
            docker_exists=docker_exists,
            args=args,
            machine=machine,
            repo_root=repo_root,
        )
        != IntegrationMode.DIRECT
    )


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


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _host_engflow_auth_helper(repo_root: pathlib.Path) -> pathlib.Path | None:
    bazelrc = repo_root / ".bazelrc.engflow_creds"
    try:
        lines = bazelrc.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None

    prefix = f"--credential_helper={ENGFLOW_AUTH_CLUSTER}="
    for line in lines:
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        for token in tokens:
            if token.startswith(prefix):
                return pathlib.Path(token[len(prefix) :])
    return None


def _host_engflow_env(env: Mapping[str, str]) -> dict[str, str]:
    result = dict(env)
    if platform.system() != "Windows":
        return result

    profile = result.get("USERPROFILE")
    if not profile and result.get("HOMEDRIVE") and result.get("HOMEPATH"):
        profile = result["HOMEDRIVE"] + result["HOMEPATH"]
    profile = profile or r"C:\Users\Administrator"
    result.setdefault("USERPROFILE", profile)
    result.setdefault("HOME", profile)
    result.setdefault("APPDATA", os.path.join(profile, "AppData", "Roaming"))
    result.setdefault("LOCALAPPDATA", os.path.join(profile, "AppData", "Local"))
    return result


def _sync_engflow_file_token(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
    token_home: pathlib.Path,
) -> None:
    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
        return

    host_helper = _host_engflow_auth_helper(repo_root)
    if host_helper is None or not os.access(host_helper, os.X_OK):
        return

    export = subprocess.run(
        [str(host_helper), "export", ENGFLOW_AUTH_CLUSTER],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=_host_engflow_env(os.environ),
    )
    if export.returncode or not export.stdout.strip():
        return

    token_home.mkdir(parents=True, exist_ok=True)
    import_env = _host_engflow_env(os.environ)
    import_env["HOME"] = str(token_home)
    import_env["USERPROFILE"] = str(token_home)
    import_env["APPDATA"] = str(token_home / "AppData" / "Roaming")
    import_env["LOCALAPPDATA"] = str(token_home / "AppData" / "Local")
    pathlib.Path(import_env["APPDATA"]).mkdir(parents=True, exist_ok=True)
    pathlib.Path(import_env["LOCALAPPDATA"]).mkdir(parents=True, exist_ok=True)
    import_result = subprocess.run(
        [str(host_helper), "import", "--store=file"],
        check=False,
        input=export.stdout,
        env=import_env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if import_result.returncode:
        return

    macos_token = (
        token_home
        / "Library"
        / "Application Support"
        / "engflow_auth"
        / "tokens"
        / ENGFLOW_AUTH_CLUSTER
    )
    linux_token = token_home / ".config" / "engflow_auth" / "tokens" / ENGFLOW_AUTH_CLUSTER
    windows_token = (
        token_home / "AppData" / "Roaming" / "engflow_auth" / "tokens" / ENGFLOW_AUTH_CLUSTER
    )
    source_token = next((path for path in [macos_token, windows_token] if path.exists()), None)
    if source_token is None:
        return

    linux_token.parent.mkdir(parents=True, exist_ok=True)
    linux_token.write_bytes(source_token.read_bytes())
    linux_token.chmod(0o600)


def _hermetic_container_engflow_auth_helper(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
    system: str,
) -> str:
    override = env.get(HERMETIC_CONTAINER_ENGFLOW_AUTH_HELPER_ENV)
    if _env_is_false(override):
        return ""
    if override:
        return _container_path(override, system)

    if system not in {"Darwin", "Windows"}:
        return ""
    if not (repo_root / ".bazelrc.engflow_creds").exists():
        return ""

    _sync_engflow_file_token(repo_root, env, _hermetic_container_home_dir(repo_root))

    arch = normalize_arch(
        env.get("MONGO_HERMETIC_CONTAINER_CONTAINER_ARCH", _default_container_bazel_arch(system))
    )
    if arch not in ENGFLOW_AUTH_LINUX_RELEASES:
        return ""

    release_name, expected_sha256 = ENGFLOW_AUTH_LINUX_RELEASES[arch]
    helper = (
        _hermetic_container_state_dir(repo_root) / "engflow_auth" / release_name / "engflow_auth"
    )
    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
        return _container_path(helper, system)
    if helper.exists() and _sha256_file(helper) == expected_sha256:
        helper.chmod(0o500)
        return _container_path(helper, system)

    helper.parent.mkdir(parents=True, exist_ok=True)
    tmp_helper = helper.with_suffix(".tmp")
    urllib.request.urlretrieve(ENGFLOW_AUTH_URL_PREFIX + release_name, tmp_helper)
    if _sha256_file(tmp_helper) != expected_sha256:
        tmp_helper.unlink(missing_ok=True)
        raise RuntimeError(f"Downloaded {release_name} does not match expected checksum")
    tmp_helper.chmod(0o500)
    tmp_helper.replace(helper)
    return _container_path(helper, system)


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


def _bazelrc_windows_repo_envs(repo_root: pathlib.Path) -> dict[str, str]:
    scopes = {
        "common:windows",
        "build:windows",
        f"common:{WINDOWS_CROSS_CONFIG}",
        f"build:{WINDOWS_CROSS_CONFIG}",
    }
    bazelrc = repo_root / ".bazelrc"
    values: dict[str, str] = {}

    try:
        lines = bazelrc.read_text(encoding="utf-8").splitlines()
    except OSError:
        return values

    for raw_line in lines:
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            tokens = shlex.split(line, posix=True)
        except ValueError:
            continue
        if not tokens or tokens[0] not in scopes:
            continue

        index = 1
        while index < len(tokens):
            token = tokens[index]
            assignment = None
            if token.startswith("--repo_env="):
                assignment = token.split("=", 1)[1]
            elif token == "--repo_env" and index + 1 < len(tokens):
                index += 1
                assignment = tokens[index]

            if assignment:
                parsed = _parse_repo_env_assignment(assignment)
                if parsed:
                    key, value = parsed
                    values[key] = value
            index += 1

    return values


def _repo_env_overrides_from_args(args: Sequence[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    index = 0
    while index < len(args):
        token = args[index]
        assignment = None
        if token.startswith("--repo_env="):
            assignment = token.split("=", 1)[1]
        elif token == "--repo_env" and index + 1 < len(args):
            index += 1
            assignment = args[index]

        if assignment:
            parsed = _parse_repo_env_assignment(assignment)
            if parsed:
                key, value = parsed
                values[key] = value
        index += 1
    return values


def _credential_helper_requested(args: Sequence[str], cluster: str) -> bool:
    index = 0
    prefix = f"--credential_helper={cluster}="
    while index < len(args):
        token = args[index]
        if token.startswith(prefix):
            return True
        if token == "--credential_helper" and index + 1 < len(args):
            index += 1
            if args[index].startswith(f"{cluster}="):
                return True
        index += 1
    return False


def _workspace_status_command_requested(args: Sequence[str]) -> bool:
    index = 0
    while index < len(args):
        token = args[index]
        if token.startswith("--workspace_status_command="):
            return True
        if token == "--workspace_status_command":
            return True
        index += 1
    return False


def _windows_cross_config_requested(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return _config_requested(args, WINDOWS_CROSS_CONFIG, env=env, repo_root=repo_root)


def _config_values(args: Sequence[str]) -> list[str]:
    values = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg.startswith("--config="):
            values.append(arg.split("=", 1)[1])
        elif arg == "--config" and index + 1 < len(args):
            index += 1
            values.append(args[index])
        index += 1
    return values


def _linux_cross_rbe_config(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> LinuxCrossRBEConfig | None:
    selected = None
    for config in _effective_config_values(args, env=env, repo_root=repo_root):
        match = LINUX_CROSS_RBE_CONFIG_RE.match(config)
        if not match:
            continue
        exec_arch = match.group("exec_arch") or LINUX_CROSS_RBE_DEFAULT_EXEC_ARCH
        if exec_arch == "arm64":
            exec_arch = "aarch64"
        selected = LinuxCrossRBEConfig(
            target_arch=match.group("target_arch"),
            target_distro=match.group("target_distro") or LINUX_CROSS_RBE_DEFAULT_TARGET_DISTRO,
            exec_arch=exec_arch,
            exec_distro=LINUX_CROSS_RBE_DEFAULT_EXEC_DISTRO,
        )
    return selected


def _platforms_requested(args: Sequence[str]) -> bool:
    index = 0
    while index < len(args):
        arg = args[index]
        if arg.startswith("--platforms="):
            return True
        if arg == "--platforms" and index + 1 < len(args):
            return True
        index += 1
    return False


def _macos_cross_config_requested(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return any(
        _config_requested(args, config, env=env, repo_root=repo_root)
        for config in MACOS_CROSS_CONFIGS
    )


def _bool_build_setting_enabled(
    args: Sequence[str],
    label: str,
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    index = 0
    options = _bazel_effective_options(args, env=env, repo_root=repo_root)
    while index < len(options):
        arg = options[index]
        if arg == f"--{label}":
            return True
        if arg == f"--{label}=True" or arg == f"--{label}=true" or arg == f"--{label}=1":
            return True
        if arg == f"--{label}=False" or arg == f"--{label}=false" or arg == f"--{label}=0":
            return False
        index += 1
    return False


def _remote_link_requested(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return (
        _config_requested(args, "remote_link", env=env, repo_root=repo_root)
        or _config_requested(args, "remote_test", env=env, repo_root=repo_root)
        or _bool_build_setting_enabled(
            args, "//bazel/config:remote_link", env=env, repo_root=repo_root
        )
    )


def _remote_execution_disabled_by_args(
    args: Sequence[str],
    initially_disabled: bool = False,
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return _remote_execution_disabled_for_options(
        _bazel_effective_options(args, env=env, repo_root=repo_root),
        initially_disabled=initially_disabled,
    )


def _remote_execution_disabled_for_options(
    options: Sequence[str],
    initially_disabled: bool = False,
) -> bool:
    disabled = initially_disabled
    index = 0
    while index < len(options):
        arg = options[index]
        if arg.startswith("--config="):
            if arg.split("=", 1)[1] in {"local", "no-remote-exec", "public-release-local"}:
                disabled = True
        elif arg == "--config" and index + 1 < len(options):
            index += 1
            if options[index] in {"local", "no-remote-exec", "public-release-local"}:
                disabled = True
        elif arg.startswith("--remote_executor="):
            disabled = arg.split("=", 1)[1] == ""
        elif arg == "--remote_executor" and index + 1 < len(options):
            index += 1
            disabled = options[index] == ""
        index += 1
    return disabled


def _remote_execution_disabled_by_workspace_rc(
    args: Sequence[str],
    repo_root: pathlib.Path,
    env: Mapping[str, str] = os.environ,
) -> bool:
    """Returns whether the effective Bazel RC files disable remote execution.

    The workspace rc imports late-generated files such as .bazelrc.evergreen and
    .bazelrc.local. The same effective-option resolver also includes the standard
    system/home RCs and explicit --bazelrc files, preserving their order and config
    alias expansion.
    """
    return _remote_execution_disabled_for_options(
        _bazel_effective_options(args, env=env, repo_root=repo_root, include_command_line=False)
    )


def _macos_cross_remote_execution_disabled(
    args: Sequence[str],
    env: Mapping[str, str],
) -> bool:
    if _env_is_true(env.get("MONGO_MACOS_CROSS_LOCAL_CONTAINER_ONLY")):
        return True

    return _remote_execution_disabled_by_args(args, env=env)


def _windows_cross_remote_execution_disabled(
    args: Sequence[str],
    env: Mapping[str, str],
) -> bool:
    if _env_is_true(env.get("MONGO_WINDOWS_CROSS_LOCAL_CONTAINER_ONLY")):
        return True

    return _macos_cross_remote_execution_disabled(args, env)


def _default_macos_cross_config(machine: str | None = None) -> str:
    arch = normalize_arch(machine)
    if arch == "aarch64":
        return "macos-cross-arm64"
    if arch == "x86_64":
        return "macos-cross-x86_64"
    raise RuntimeError(f"Unsupported macOS cross-compilation architecture: {arch}")


def _is_cross_default_run_target(target: str | None) -> bool:
    if not target:
        return False
    if target in {"format", "lint", "compiledb", "compiledb_only"}:
        return False

    if ":" in target:
        name = target.rsplit(":", 1)[1]
    else:
        name = pathlib.PurePosixPath(target).name
    return name.endswith("_test")


def _is_macos_cross_default_run_target(target: str | None) -> bool:
    return _is_cross_default_run_target(target)


def _bazel_run_target(args: Sequence[str]) -> str | None:
    command_index = _bazel_command_index(args)
    if command_index is None or args[command_index] != "run":
        return None

    command_args = list(args[command_index + 1 :])
    index = 0
    while index < len(command_args):
        arg = command_args[index]
        if arg == "--":
            return None
        if arg in CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE or arg in RUN_FLAGS_WITH_SEPARATE_VALUE:
            _, index = _consume_option_value(command_args, index)
            continue
        if arg.startswith(tuple(flag + "=" for flag in CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE)):
            index += 1
            continue
        if arg.startswith(RUN_FLAG_PREFIXES):
            index += 1
            continue
        if arg.startswith("-"):
            index += 1
            continue
        return arg
    return None


def _should_default_macos_cross_config(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
) -> bool:
    if (system or platform.system()) != "Darwin":
        return False
    if not (
        _env_is_true(env.get(MACOS_CROSS_DEFAULT_CONFIG_ENV))
        or _env_is_true(env.get("MONGO_BAZEL_USE_HERMETIC_CONTAINER"))
    ):
        return False
    if _macos_cross_config_requested(args, env=env):
        return False

    command = _bazel_command(args)
    if command in MACOS_CROSS_DEFAULT_COMMANDS:
        return True
    if command == "run":
        return _is_macos_cross_default_run_target(_bazel_run_target(args))
    return False


def _should_default_windows_cross_config(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
) -> bool:
    if (system or platform.system()) != "Windows":
        return False
    if not _env_is_true(env.get(WINDOWS_CROSS_DEFAULT_CONFIG_ENV)):
        return False
    if _windows_cross_config_requested(args, env=env):
        return False
    if _platforms_requested(args):
        return False
    command = _bazel_command(args)
    if command in WINDOWS_CROSS_DEFAULT_COMMANDS:
        return True
    if command == "run":
        return _is_cross_default_run_target(_bazel_run_target(args))
    return False


def _should_default_windows_hermetic_container(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
) -> bool:
    if _windows_cross_config_requested(args, env=env):
        return True
    return _should_default_windows_cross_config(args, env=env, system=system)


def _bazel_args_with_default_windows_cross_config(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
) -> list[str]:
    if not _should_default_windows_cross_config(args, env=env, system=system):
        return list(args)

    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)

    return [
        *args[: command_index + 1],
        f"--config={WINDOWS_CROSS_CONFIG}",
        *args[command_index + 1 :],
    ]


def _bazel_args_with_default_macos_cross_config(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    system: str | None = None,
    machine: str | None = None,
) -> list[str]:
    if not _should_default_macos_cross_config(args, env=env, system=system):
        return list(args)

    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)

    config = _default_macos_cross_config(machine)
    return [*args[: command_index + 1], f"--config={config}", *args[command_index + 1 :]]


def _bazel_command_index(args: Sequence[str]) -> int | None:
    for index, arg in enumerate(args):
        if arg in BAZEL_COMMANDS:
            return index
    return None


def _bazel_command(args: Sequence[str]) -> str | None:
    index = _bazel_command_index(args)
    if index is None:
        return None
    return args[index]


def _bazel_args_with_hermetic_container_symlink_prefix(args: Sequence[str]) -> list[str]:
    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)

    # Graph and repository-fetch commands do not execute actions. In particular,
    # query-family commands reject build-only options such as --symlink_prefix, so
    # leave their command line untouched on the Linux host integration path.
    if args[command_index] in LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS:
        return list(args)

    for arg in args[command_index + 1 :]:
        if arg == "--":
            break
        if arg == "--symlink_prefix" or arg.startswith("--symlink_prefix="):
            return list(args)

    return [
        *args[: command_index + 1],
        f"--symlink_prefix={HERMETIC_CONTAINER_SYMLINK_PREFIX}",
        *args[command_index + 1 :],
    ]


def _config_requested(
    args: Sequence[str],
    config_name: str,
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return config_name in _effective_config_values(args, env=env, repo_root=repo_root)


def _is_macos_cross_config_value(value: str | None) -> bool:
    return value in MACOS_CROSS_CONFIGS


def _is_macos_cross_config_arg(args: Sequence[str], index: int) -> bool:
    arg = args[index]
    if arg.startswith("--config="):
        return _is_macos_cross_config_value(arg.split("=", 1)[1])
    return (
        arg == "--config"
        and index + 1 < len(args)
        and _is_macos_cross_config_value(args[index + 1])
    )


def _windows_cross_repo_env_values(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path,
) -> dict[str, str]:
    values = _bazelrc_windows_repo_envs(repo_root)
    for key in [*WINDOWS_TOOLCHAIN_PIN_ENVS, *WINDOWS_CROSS_SDK_ROOT_ENVS]:
        if env.get(key):
            values[key] = env[key]
    values.update(_repo_env_overrides_from_args(args))
    return values


def _windows_cross_sysroot_archive_configured(
    args: Sequence[str],
    env: Mapping[str, str],
    values: Mapping[str, str] | None = None,
) -> bool:
    repo_env = _repo_env_overrides_from_args(args)

    def _value(key: str) -> str | None:
        return env.get(key) or repo_env.get(key) or (values or {}).get(key)

    url = _value("MONGO_WINDOWS_CROSS_SYSROOT_URL")
    sha = _value("MONGO_WINDOWS_CROSS_SYSROOT_SHA256")
    return bool(url and sha)


def _required_windows_pin(values: Mapping[str, str], key: str) -> str:
    value = (values.get(key) or "").strip()
    if value:
        return value
    raise RuntimeError(
        f"Windows cross sysroot generation requires {key}. "
        "Set it in .bazelrc or pass it with --repo_env."
    )


def _default_windows_sdk_root(values: Mapping[str, str]) -> pathlib.Path:
    for key in WINDOWS_CROSS_SDK_ROOT_ENVS:
        value = (values.get(key) or "").strip().rstrip("\\/")
        if value:
            return pathlib.Path(value)
    return (
        pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / ("Windows Kits")
        / "10"
    )


def _windows_cross_sysroot_spec(values: Mapping[str, str]) -> WindowsCrossSysrootSpec:
    vc_full_version = _required_windows_pin(values, "BAZEL_VC_FULL_VERSION")
    winsdk_full_version = _required_windows_pin(values, "BAZEL_WINSDK_FULL_VERSION")

    vc_value = (values.get("BAZEL_VC") or "").strip()
    if not vc_value and values.get("BAZEL_VS"):
        vc_value = str(pathlib.Path(values["BAZEL_VS"]) / "VC")
    if not vc_value:
        raise RuntimeError(
            "Windows cross sysroot generation requires BAZEL_VC or BAZEL_VS. "
            "Set it in .bazelrc or pass it with --repo_env."
        )

    vc_path = pathlib.Path(vc_value)
    winsdk_root = _default_windows_sdk_root(values)
    msvc_root = vc_path / "Tools" / "MSVC" / vc_full_version
    winsdk_include = winsdk_root / "Include" / winsdk_full_version
    winsdk_lib = winsdk_root / "Lib" / winsdk_full_version

    return WindowsCrossSysrootSpec(
        vc_path=vc_path,
        vc_full_version=vc_full_version,
        winsdk_root=winsdk_root,
        winsdk_full_version=winsdk_full_version,
        sources={
            "msvc/include": msvc_root / "include",
            "msvc/lib/x64": msvc_root / "lib" / "x64",
            "msvc/atlmfc/include": msvc_root / "ATLMFC" / "include",
            "msvc/atlmfc/lib/x64": msvc_root / "ATLMFC" / "lib" / "x64",
            "winsdk/include/ucrt": winsdk_include / "ucrt",
            "winsdk/include/shared": winsdk_include / "shared",
            "winsdk/include/um": winsdk_include / "um",
            "winsdk/include/winrt": winsdk_include / "winrt",
            "winsdk/include/cppwinrt": winsdk_include / "cppwinrt",
            "winsdk/lib/ucrt/x64": winsdk_lib / "ucrt" / "x64",
            "winsdk/lib/um/x64": winsdk_lib / "um" / "x64",
        },
    )


def _windows_cross_sysroot_manifest(spec: WindowsCrossSysrootSpec) -> dict[str, object]:
    return {
        "schema": 1,
        "arch": "x64",
        "vc_path": str(spec.vc_path),
        "vc_full_version": spec.vc_full_version,
        "winsdk_root": str(spec.winsdk_root),
        "winsdk_full_version": spec.winsdk_full_version,
        "sources": {relative: str(source) for relative, source in sorted(spec.sources.items())},
    }


def _windows_cross_generated_sysroot_path(
    repo_root: pathlib.Path,
    spec: WindowsCrossSysrootSpec,
) -> pathlib.Path:
    manifest = _windows_cross_sysroot_manifest(spec)
    fingerprint = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:12]
    name = (
        f"msvc-{_safe_name(spec.vc_full_version)}_"
        f"winsdk-{_safe_name(spec.winsdk_full_version)}_x64_{fingerprint}"
    )
    return _hermetic_container_state_dir(repo_root) / "windows-sysroots" / name


def _windows_cross_sysroot_is_current(
    sysroot_path: pathlib.Path,
    manifest: Mapping[str, object],
    sources: Mapping[str, pathlib.Path],
) -> bool:
    marker = sysroot_path / ".mongo_windows_cross_sysroot.json"
    try:
        existing = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    if existing != manifest:
        return False
    return all((sysroot_path / relative).is_dir() for relative in sources)


def _hardlink_or_copy(src: str, dst: str) -> None:
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def _remove_generated_sysroot(path: pathlib.Path, cache_root: pathlib.Path) -> None:
    if not path.exists():
        return
    if not _is_relative_to(path, cache_root) or path.resolve() == cache_root.resolve():
        raise RuntimeError(f"Refusing to remove unexpected sysroot path: {path}")
    shutil.rmtree(path)


def _create_windows_cross_sysroot(
    repo_root: pathlib.Path,
    spec: WindowsCrossSysrootSpec,
) -> pathlib.Path:
    missing = [
        f"{relative}: {source}"
        for relative, source in sorted(spec.sources.items())
        if not source.is_dir()
    ]
    if missing:
        raise RuntimeError(
            "Pinned Windows cross sysroot inputs are missing:\n  "
            + "\n  ".join(missing)
            + "\nInstall the pinned MSVC toolset, ATL/MFC component, and Windows SDK, "
            "or pass matching --repo_env overrides."
        )

    sysroot_path = _windows_cross_generated_sysroot_path(repo_root, spec)
    cache_root = _hermetic_container_state_dir(repo_root) / "windows-sysroots"
    manifest = _windows_cross_sysroot_manifest(spec)
    if _windows_cross_sysroot_is_current(sysroot_path, manifest, spec.sources):
        return sysroot_path

    _info(
        "creating Windows cross sysroot from "
        f"MSVC {spec.vc_full_version} and Windows SDK {spec.winsdk_full_version}"
    )
    _remove_generated_sysroot(sysroot_path, cache_root)
    sysroot_path.mkdir(parents=True, exist_ok=True)
    try:
        for relative, source in sorted(spec.sources.items()):
            destination = sysroot_path / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, destination, copy_function=_hardlink_or_copy)
        (sysroot_path / ".mongo_windows_cross_sysroot.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except Exception:
        _remove_generated_sysroot(sysroot_path, cache_root)
        raise
    return sysroot_path


def _windows_cross_default_llvm_path(repo_root: pathlib.Path) -> pathlib.Path | None:
    state_dir = _hermetic_container_state_dir(repo_root)
    for llvm_dir_name in ("windows-cross-llvm-19", "windows-cross-llvm"):
        llvm_path = state_dir / llvm_dir_name
        if os.path.lexists(llvm_path / "bin" / "clang") and os.path.lexists(
            llvm_path / "bin" / "clang-cl"
        ):
            return llvm_path
    return None


def _prepare_windows_cross_env(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path,
    system: str,
) -> Mapping[str, str]:
    if system != "Windows" or not _windows_cross_config_requested(
        args, env=env, repo_root=repo_root
    ):
        return env

    values = _windows_cross_repo_env_values(args, env, repo_root)
    prepared_env = dict(env)
    prepared_env.setdefault("MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE", WSL_DOCKER_HOST_MODE)
    for key in [*WINDOWS_TOOLCHAIN_PIN_ENVS, *WINDOWS_CROSS_SDK_ROOT_ENVS]:
        if values.get(key):
            prepared_env[key] = values[key]

    repo_env = _repo_env_overrides_from_args(args)
    llvm_archive_configured = bool(
        (
            prepared_env.get("MONGO_WINDOWS_CROSS_LLVM_URL")
            or repo_env.get("MONGO_WINDOWS_CROSS_LLVM_URL")
        )
        and (
            prepared_env.get("MONGO_WINDOWS_CROSS_LLVM_SHA256")
            or repo_env.get("MONGO_WINDOWS_CROSS_LLVM_SHA256")
        )
    )
    if (
        not prepared_env.get("MONGO_WINDOWS_CROSS_LLVM_PATH")
        and not repo_env.get("MONGO_WINDOWS_CROSS_LLVM_PATH")
        and not llvm_archive_configured
    ):
        default_llvm_path = _windows_cross_default_llvm_path(repo_root)
        if default_llvm_path is not None:
            prepared_env["MONGO_WINDOWS_CROSS_LLVM_PATH"] = str(default_llvm_path)

    sysroot_env = prepared_env.get("MONGO_WINDOWS_CROSS_SYSROOT_PATH")
    if sysroot_env and prepared_env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") != "1":
        sysroot_path = pathlib.Path(sysroot_env)
        if not sysroot_path.is_dir():
            raise RuntimeError(
                "MONGO_WINDOWS_CROSS_SYSROOT_PATH does not exist or is not a directory: "
                f"{sysroot_path}"
            )

    if not sysroot_env and not _windows_cross_sysroot_archive_configured(
        args, prepared_env, values
    ):
        spec = _windows_cross_sysroot_spec(values)
        sysroot_path = (
            _windows_cross_generated_sysroot_path(repo_root, spec)
            if prepared_env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1"
            else _create_windows_cross_sysroot(repo_root, spec)
        )
        prepared_env["MONGO_WINDOWS_CROSS_SYSROOT_PATH"] = str(sysroot_path)

    return prepared_env


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


def _bazel_version(repo_root: pathlib.Path, env: Mapping[str, str]) -> str:
    if env.get("USE_BAZEL_VERSION"):
        return env["USE_BAZEL_VERSION"]
    version_file = repo_root / ".bazelversion"
    try:
        for line in version_file.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped:
                return stripped
    except OSError:
        pass
    return "latest"


def _bazelisk_base_url(repo_root: pathlib.Path, env: Mapping[str, str]) -> str | None:
    if env.get("BAZELISK_BASE_URL"):
        return env["BAZELISK_BASE_URL"]
    return _read_key_value_file(repo_root / ".bazeliskrc").get("BAZELISK_BASE_URL")


def _linux_bazel_filename(version: str, arch: str) -> str:
    return f"bazel-{version}-linux-{arch}"


def _default_container_bazel_arch(system: str) -> str:
    if system == "Windows":
        return DEFAULT_HERMETIC_CONTAINER_CONTAINER_ARCH

    arch = normalize_arch()
    if arch == "aarch64":
        return "arm64"
    return arch


def _linux_bazel_url(
    repo_root: pathlib.Path,
    env: Mapping[str, str],
    version: str,
    arch: str,
) -> str:
    if env.get("MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_URL"):
        return env["MONGO_HERMETIC_CONTAINER_LINUX_BAZEL_URL"]

    filename = _linux_bazel_filename(version, arch)
    base_url = _bazelisk_base_url(repo_root, env)
    if base_url:
        return f"{base_url.rstrip('/')}/{version}/{filename}"

    version_base, sep, rc = version.partition("rc")
    rc_or_release = f"rc{rc}" if sep else "release"
    return f"https://releases.bazel.build/{version_base}/{rc_or_release}/{filename}"


def _parse_sha256(value: str, *, source: str) -> str:
    """Return a normalized SHA-256 digest read from a checksum value."""

    token = value.strip().split(maxsplit=1)[0] if value.strip() else ""
    if not re.fullmatch(r"[0-9a-fA-F]{64}", token):
        raise RuntimeError(f"Invalid SHA-256 checksum from {source}")
    return token.lower()


def _linux_bazel_checksum_file(path: pathlib.Path) -> pathlib.Path:
    return path.with_name(f"{path.name}.sha256")


def _read_linux_bazel_checksum(path: pathlib.Path) -> str | None:
    try:
        return _parse_sha256(path.read_text(encoding="utf-8"), source=str(path))
    except (OSError, RuntimeError):
        return None


def _linux_bazel_cache_is_valid(binary: pathlib.Path, expected_sha256: str | None = None) -> bool:
    checksum = _read_linux_bazel_checksum(_linux_bazel_checksum_file(binary))
    if not (
        checksum and (expected_sha256 is None or checksum == expected_sha256) and binary.is_file()
    ):
        return False
    try:
        return _sha256_file(binary) == checksum
    except OSError:
        return False


def _download_linux_bazel_checksum(url: str) -> str:
    checksum_url = f"{url}.sha256"
    try:
        with urllib.request.urlopen(checksum_url) as response:
            return _parse_sha256(
                response.read().decode("utf-8"),
                source=checksum_url,
            )
    except (OSError, UnicodeDecodeError, urllib.error.URLError) as exc:
        raise RuntimeError(
            "Failed to download a checksum for Linux Bazel for hermetic_container from "
            f"{checksum_url}. Set {LINUX_BAZEL_SHA256_ENV} to a trusted SHA-256 digest "
            "when using a mirror without checksum sidecars."
        ) from exc


def _write_linux_bazel_checksum(destination: pathlib.Path, checksum: str) -> None:
    checksum_path = _linux_bazel_checksum_file(destination)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=checksum_path.parent,
        prefix=f".{checksum_path.name}.",
        suffix=".tmp",
        text=True,
    )
    temporary_path = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(f"{checksum}\n")
            output.flush()
            os.fsync(output.fileno())
        temporary_path.replace(checksum_path)
    finally:
        temporary_path.unlink(missing_ok=True)


def _download_file(url: str, destination: pathlib.Path, expected_sha256: str) -> None:
    """Atomically download a verified Linux Bazel binary into the local cache."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=destination.parent,
        prefix=f".{destination.name}.",
        suffix=".tmp",
    )
    temporary_path = pathlib.Path(temporary_name)
    digest = hashlib.sha256()
    try:
        try:
            with os.fdopen(descriptor, "wb") as output:
                with urllib.request.urlopen(url) as response:
                    while chunk := response.read(1024 * 1024):
                        digest.update(chunk)
                        output.write(chunk)
                output.flush()
                os.fsync(output.fileno())
        except (OSError, urllib.error.URLError) as exc:
            raise RuntimeError(
                f"Failed to download Linux Bazel for hermetic_container from {url}: {exc}"
            ) from exc

        actual_sha256 = digest.hexdigest()
        if actual_sha256 != expected_sha256:
            raise RuntimeError(
                "Downloaded Linux Bazel for hermetic_container does not match the expected "
                f"SHA-256: expected {expected_sha256}, got {actual_sha256}"
            )

        temporary_path.chmod(0o755)
        temporary_path.replace(destination)
        _write_linux_bazel_checksum(destination, expected_sha256)
    finally:
        temporary_path.unlink(missing_ok=True)


def _resolve_container_bazel(
    bazel_real: pathlib.Path,
    env: Mapping[str, str],
    repo_root: pathlib.Path,
    system: str,
) -> tuple[str, pathlib.Path | None]:
    """Return the Bazel command inside the container and the host path to mount."""

    if system == "Linux":
        return str(bazel_real), bazel_real

    if env.get(CONTAINER_BAZEL_ENV):
        return env[CONTAINER_BAZEL_ENV], None

    if env.get(LINUX_BAZEL_ENV):
        linux_bazel = pathlib.Path(env[LINUX_BAZEL_ENV])
        return _container_path(linux_bazel, system), linux_bazel

    arch = env.get("MONGO_HERMETIC_CONTAINER_CONTAINER_ARCH", _default_container_bazel_arch(system))
    version = _bazel_version(repo_root, env)
    filename = _linux_bazel_filename(version, arch)
    linux_bazel = _hermetic_container_state_dir(repo_root) / "bazel" / filename / "bin" / "bazel"

    configured_checksum = env.get(LINUX_BAZEL_SHA256_ENV)
    expected_sha256 = (
        _parse_sha256(configured_checksum, source=LINUX_BAZEL_SHA256_ENV)
        if configured_checksum
        else None
    )
    if (
        not _linux_bazel_cache_is_valid(linux_bazel, expected_sha256)
        and env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") != "1"
    ):
        url = _linux_bazel_url(repo_root, env, version, arch)
        if expected_sha256 is None:
            expected_sha256 = _download_linux_bazel_checksum(url)
        _info(f"downloading Linux Bazel for hermetic_container from {url}")
        _download_file(url, linux_bazel, expected_sha256)

    return _container_path(linux_bazel, system), linux_bazel


def _container_user(system: str) -> str:
    if system == "Windows":
        return ""
    if hasattr(os, "getuid") and hasattr(os, "getgid"):
        return f"{os.getuid()}:{os.getgid()}"
    return ""


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


def _load_hermetic_container_module():
    if not (HERMETIC_CONTAINER_SOURCE_ROOT / "hermetic_container.py").exists():
        raise RuntimeError(
            f"Vendored hermetic_container checkout not found at {HERMETIC_CONTAINER_SOURCE_ROOT}"
        )
    sys.path.insert(0, str(HERMETIC_CONTAINER_SOURCE_ROOT))
    import hermetic_container  # pylint: disable=import-error

    return hermetic_container


def _run_direct(bazel_real: str, args: Sequence[str]) -> int:
    return subprocess.run([bazel_real, *args], check=False).returncode


def _bazel_args_with_native_install_strategy(args: Sequence[str]) -> list[str]:
    """Keep the shared install convenience tree outside native action sandboxes."""
    if _bazel_command(args) not in {"build", "coverage", "run", "test"}:
        return list(args)
    return _append_bazel_command_options(
        args,
        ["--strategy=MongoInstallRule=local"],
    )


def _is_podman_command(command: Sequence[str]) -> bool:
    return any(pathlib.Path(part).name == "podman" for part in command)


def _is_podman_docker_shim(command: str) -> bool:
    """Return whether *command* is Podman's Docker-compatibility entry point.

    The podman-docker package commonly installs a command named ``docker``.  Its
    name alone is therefore insufficient to decide whether the Podman-specific
    rootless runtime environment and ``--userns=keep-id`` options are needed.
    """
    argv = shlex.split(command)
    if not argv or _is_podman_command(argv):
        return False

    try:
        result = subprocess.run(
            [*argv, "--version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return False

    version_output = f"{result.stdout}\n{result.stderr}".lower()
    return (
        "emulate docker cli using podman" in version_output
        or re.search(r"(^|\n)\s*podman version\b", version_output) is not None
    )


def _podman_task_root(env: Mapping[str, str]) -> pathlib.Path:
    root = pathlib.Path("/tmp")
    task_id = env.get(PODMAN_TASK_ID_ENV, "")
    if task_id:
        root /= f"mongo-linux-podman-task-{_safe_name(task_id)}"
    return root


def _ensure_owned_podman_directory(path: pathlib.Path, uid: int) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = path.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != uid:
        raise OSError(f"Podman directory is not a directory owned by uid {uid}: {path}")
    path.chmod(0o700)


def _podman_storage_config(runtime_dir: pathlib.Path) -> pathlib.Path:
    uid = os.getuid()
    storage_dir = runtime_dir.parent / f"{PODMAN_STORAGE_DIR_PREFIX}{uid}"
    graph_root = storage_dir / "graphroot"
    run_root = storage_dir / "runroot"
    _ensure_owned_podman_directory(storage_dir, uid)
    _ensure_owned_podman_directory(graph_root, uid)
    _ensure_owned_podman_directory(run_root, uid)

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
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != uid:
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


def _podman_runtime_dir() -> pathlib.Path:
    """Return a private, task-scoped runtime directory outside the action workspace."""
    uid = os.getuid()
    path = _podman_task_root(os.environ) / f"{PODMAN_RUNTIME_DIR_PREFIX}{uid}"
    _ensure_owned_podman_directory(path, uid)
    return path


def _container_runtime_env(command: Sequence[str]) -> dict[str, str] | None:
    """Keep rootless Podman's runtime data out of an inherited task TMPDIR."""
    if not _is_podman_command(command):
        return None

    runtime_dir = _podman_runtime_dir()
    storage_config = _podman_storage_config(runtime_dir)
    runtime_env = dict(os.environ)
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


def _compact_runtime_detail(detail: str, limit: int = 2000) -> str:
    """Bound noisy runtime failures while retaining their first and last causes."""
    detail = detail.strip()
    if len(detail) <= limit:
        return detail
    half = (limit - len("\n... output truncated ...\n")) // 2
    return detail[:half] + "\n... output truncated ...\n" + detail[-half:]


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


@contextlib.contextmanager
def _podman_recovery_lock(runtime_env: Mapping[str, str]):
    """Serialize recovery of the rootless Podman pause process."""
    import fcntl

    runtime_dir = pathlib.Path(runtime_env["XDG_RUNTIME_DIR"])
    lock_path = runtime_dir / "mongo-podman-recovery.lock"
    with lock_path.open("a", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _podman_command_detail(result: subprocess.CompletedProcess[str]) -> str:
    return (result.stderr or result.stdout or "").strip()


def _podman_migration_is_allowed(env: Mapping[str, str]) -> bool:
    """Allow opting out of automatic `podman system migrate`."""
    return env.get("MONGO_BAZEL_PODMAN_AUTO_MIGRATE", "1").strip() not in ("0", "false", "False")


def _podman_runtime_is_task_scoped(env: Mapping[str, str]) -> bool:
    """Report whether Podman's runtime directory is private to one task.

    ``_podman_task_root`` gives each Evergreen task its own runtime and storage
    root, so no other user or task can own containers inside it. That makes
    recovery of a stale runtime safe without an explicit force opt-in.
    """
    return bool(env.get(PODMAN_TASK_ID_ENV, "").strip())


def _podman_migration_force_is_allowed(env: Mapping[str, str]) -> bool:
    """Allow forcing migration when Podman cannot enumerate containers."""
    return env.get("MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE", "0").strip().casefold() in (
        "1",
        "true",
        "yes",
        "on",
    )


def _podman_has_running_containers(
    podman_argv: Sequence[str],
    runtime_env: Mapping[str, str],
) -> tuple[bool | None, str]:
    """Report whether this user still has usable running containers.

    ``None`` means that Podman could not determine the answer. In particular,
    a stale pause process can make existing containers unreachable without
    stopping their workload processes, so a failed listing must not authorize
    a destructive migration by itself.
    """
    result = subprocess.run(
        [*podman_argv, "ps", "--quiet"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
        env=runtime_env,
    )
    if result.returncode != 0:
        return None, _podman_command_detail(result) or f"exit code {result.returncode}"
    return bool(result.stdout.strip()), ""


def _recover_stale_podman_runtime(
    info_argv: Sequence[str],
    runtime_env: Mapping[str, str],
) -> tuple[bool, str]:
    """Check whether another process repaired stale Podman state, under a lock."""
    try:
        with _podman_recovery_lock(runtime_env):
            # Another Bazel invocation may have recovered Podman while this one
            # waited for the lock, so check again before mutating runtime state.
            recheck = subprocess.run(
                info_argv,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=runtime_env,
            )
            if recheck.returncode == 0:
                return True, ""

            recheck_detail = _podman_command_detail(recheck)
            if PODMAN_STALE_RUNTIME_MARKER not in recheck_detail:
                return False, (
                    "Podman remained unavailable after acquiring the recovery lock: "
                    f"{recheck_detail or f'exit code {recheck.returncode}'}"
                )

            podman_argv = list(info_argv[:-1])
            if not _podman_migration_is_allowed(runtime_env):
                return False, (
                    "Automatic Podman runtime migration is disabled by "
                    "MONGO_BAZEL_PODMAN_AUTO_MIGRATE. Run `podman system migrate` manually "
                    "after verifying that stopping running containers is safe, then retry."
                )

            has_running_containers, container_check_detail = _podman_has_running_containers(
                podman_argv, runtime_env
            )
            # A listing that fails with the same stale-runtime signature is
            # evidence that the runtime is broken, not that containers are
            # reachable: every Podman command shares that state. Recover when the
            # runtime is private to this task, which is the case the guard below
            # is protecting against multi-tenant runtimes.
            listing_blocked_by_stale_runtime = (
                has_running_containers is None
                and PODMAN_STALE_RUNTIME_MARKER in container_check_detail
                and _podman_runtime_is_task_scoped(runtime_env)
            )
            if (
                has_running_containers is None
                and not listing_blocked_by_stale_runtime
                and not _podman_migration_force_is_allowed(runtime_env)
            ):
                return False, (
                    "Refusing to run `podman system migrate` because Podman could not "
                    "determine whether this user has running containers: "
                    f"{container_check_detail}. Set "
                    "MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE=1 only when this Podman runtime "
                    "is isolated and stopping its containers is safe, then retry."
                )

            if has_running_containers:
                return False, (
                    "Refusing to run `podman system migrate` because it stops the containers "
                    "currently running for this user. Run `podman system migrate` manually "
                    "after verifying that stopping those containers is safe, then retry."
                )

            migrate = subprocess.run(
                [*podman_argv, "system", "migrate"],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=runtime_env,
            )
            if migrate.returncode != 0:
                migrate_detail = _podman_command_detail(migrate)
                return False, (
                    "`podman system migrate` failed: "
                    f"{migrate_detail or f'exit code {migrate.returncode}'}"
                )

            retry = subprocess.run(
                info_argv,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=runtime_env,
            )
            if retry.returncode == 0:
                return True, ""

            retry_detail = _podman_command_detail(retry)
            return False, (
                "Podman remained unavailable after `podman system migrate`: "
                f"{retry_detail or f'exit code {retry.returncode}'}"
            )
    except FileNotFoundError as exc:
        return False, f"Podman recovery command not found: {exc.filename}"
    except subprocess.TimeoutExpired as exc:
        command = " ".join(shlex.quote(str(part)) for part in exc.cmd)
        return False, (
            f"{command} timed out after {DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS}s "
            "during stale Podman runtime recovery"
        )
    except OSError as exc:
        return False, f"could not recover stale Podman runtime state: {exc}"


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

    An explicit HERMETIC_CONTAINER_DOCKER_COMMAND remains authoritative. Otherwise prefer Docker
    when it is usable and fall back to Podman, which supports the image, mount, exec,
    network, user, and lifecycle operations used by Linux action containers. If the
    Docker command is Podman's compatibility shim, use the real Podman executable so
    that the action wrapper applies Podman's rootless bind-mount handling.
    """
    explicit = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND")
    if explicit:
        ready, detail = _docker_daemon_status(explicit)
        return (explicit, "") if ready else (None, detail)

    failures: list[str] = []
    candidates: list[tuple[str, str]] = []
    docker_command = shutil.which("docker")
    podman_command = shutil.which("podman")
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


def _macos_cross_host_test_requested(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return (
        _env_is_true(env.get(MACOS_CROSS_SPLIT_TEST_RUNNER_ENV))
        and not _env_is_false(env.get(MACOS_CROSS_TEST_RUNNER_ENV))
        and system == "Darwin"
        and _bazel_command(args) == "test"
        and _macos_cross_config_requested(args, env=env)
    )


def _macos_cross_local_container_action_candidate(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return (
        system == "Darwin"
        and not _env_is_true(env.get(MACOS_CROSS_SPLIT_TEST_RUNNER_ENV))
        and not _env_is_false(env.get(MACOS_CROSS_ACTION_WRAPPER_ENV))
        and _bazel_command(args) in {"build", "test"}
        and not _remote_link_requested(args, env=env)
        and (
            _macos_cross_config_requested(args, env=env)
            or _should_default_macos_cross_config(args, env=env, system=system)
        )
    )


def _macos_cross_local_container_action_requested(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return _macos_cross_local_container_action_candidate(
        args, env, system
    ) and _macos_cross_config_requested(args, env=env)


def _windows_cross_host_wrapper_action_candidate(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    command = _bazel_command(args)
    if command == "run" and not _is_cross_default_run_target(_bazel_run_target(args)):
        return False

    return (
        system == "Windows"
        and not _env_is_false(env.get(WINDOWS_CROSS_ACTION_WRAPPER_ENV))
        and command in {"build", "test", "run"}
        and not _windows_cross_remote_execution_disabled(args, env)
        and (
            _windows_cross_config_requested(args, env=env)
            or _should_default_windows_cross_config(args, env=env, system=system)
        )
    )


def _windows_cross_host_wrapper_action_requested(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return _windows_cross_host_wrapper_action_candidate(
        args, env, system
    ) and _windows_cross_config_requested(args, env=env)


def _macos_cross_host_bazel_test_candidate(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return (
        system == "Darwin"
        and not _env_is_true(env.get(MACOS_CROSS_SPLIT_TEST_RUNNER_ENV))
        and not _env_is_false(env.get(MACOS_CROSS_TEST_RUNNER_ENV))
        and _bazel_command(args) == "test"
        and _remote_link_requested(args, env=env)
        and (
            _macos_cross_config_requested(args, env=env)
            or _should_default_macos_cross_config(args, env=env, system=system)
        )
    )


def _macos_cross_host_bazel_test_requested(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return _macos_cross_host_bazel_test_candidate(
        args, env, system
    ) and _macos_cross_config_requested(args, env=env)


def _macos_cross_host_run_requested(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return (
        system == "Darwin"
        and not _env_is_false(env.get(MACOS_CROSS_TEST_RUNNER_ENV))
        and _bazel_command(args) == "run"
        and _macos_cross_config_requested(args, env=env)
    )


def _windows_cross_host_run_requested(
    args: Sequence[str],
    env: Mapping[str, str],
    system: str,
) -> bool:
    return (
        system == "Windows"
        and _bazel_command(args) == "run"
        and _windows_cross_config_requested(args, env=env)
    )


def _consume_option_value(args: Sequence[str], index: int) -> tuple[str | None, int]:
    arg = args[index]
    if "=" in arg:
        return arg.split("=", 1)[1], index + 1
    if index + 1 < len(args):
        return args[index + 1], index + 2
    return None, index + 1


def _add_test_env(test_env: dict[str, str], assignment: str, env: Mapping[str, str]) -> None:
    if "=" in assignment:
        key, value = assignment.split("=", 1)
        if key:
            test_env[key] = value
        return

    if assignment in env:
        test_env[assignment] = env[assignment]


def _read_target_pattern_file(path: str) -> list[str]:
    try:
        contents = pathlib.Path(path).read_text(encoding="utf-8")
    except OSError:
        return []

    return [
        line.strip()
        for line in contents.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def _parse_test_tag_filters(value: str) -> list[str]:
    return [tag.strip() for tag in value.split(",") if tag.strip()]


def _bazel_bool_flag_value(arg: str, name: str) -> bool | None:
    if arg == f"--{name}":
        return True
    if arg == f"--no{name}":
        return False
    prefix = f"--{name}="
    if arg.startswith(prefix):
        value = arg[len(prefix) :].lower()
        if value in HERMETIC_CONTAINER_ENABLED_VALUES:
            return True
        if value in HERMETIC_CONTAINER_DISABLED_VALUES:
            return False
    return None


def _append_bazel_command_options(args: Sequence[str], options: Sequence[str]) -> list[str]:
    if not options:
        return list(args)

    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)

    # Put generated options before the user's command arguments so an explicit command-line
    # value remains the last one seen by Bazel and therefore takes precedence.
    return [*args[: command_index + 1], *options, *args[command_index + 1 :]]


def _linux_cross_rbe_pool(config: LinuxCrossRBEConfig, env: Mapping[str, str]) -> str:
    if env.get(LINUX_CROSS_RBE_POOL_ENV):
        return env[LINUX_CROSS_RBE_POOL_ENV]
    if config.exec_arch == "x86_64":
        return LINUX_CROSS_RBE_X86_POOL
    return LINUX_CROSS_RBE_ARM_POOL


def _linux_cross_rbe_exec_properties(
    config: LinuxCrossRBEConfig,
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    image = env.get(LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV)
    if not image:
        containers = containers or load_remote_execution_containers()
        image = containers[config.exec_distro]["container-url"]

    return [
        f"container-image={image}",
        "dockerNetwork=standard",
        f"Pool={_linux_cross_rbe_pool(config, env)}",
    ]


def _linux_cross_rbe_execution_platform(config: LinuxCrossRBEConfig) -> str:
    exec_arch = "amd64" if config.exec_arch == "x86_64" else "arm64"
    return f"//bazel/platforms:{config.exec_distro}_{exec_arch}"


def _linux_cross_rbe_host_args(
    args: Sequence[str],
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
    repo_root: pathlib.Path = REPO_ROOT,
) -> list[str]:
    config = _linux_cross_rbe_config(args, env=env, repo_root=repo_root)
    if config is None:
        raise RuntimeError("Linux cross-RBE args require a linux-*-cross-rbe config")

    exec_properties = _linux_cross_rbe_exec_properties(config, env, containers=containers)
    return _append_bazel_command_options(
        args,
        [
            f"--remote_executor={LINUX_CROSS_RBE_REMOTE_EXECUTOR}",
            *[
                f"--remote_default_exec_properties={exec_property}"
                for exec_property in exec_properties
            ],
            f"--extra_execution_platforms={_linux_cross_rbe_execution_platform(config)}",
            *_macos_cross_linux_python_options(config.exec_arch),
            "--//bazel/config:idl_use_linux_python=True",
            "--//bazel/config:remote_link=True",
            # The execution platform is foreign to the host on s390x/ppc64le. Keep all
            # otherwise-unspecified actions on RBE so their execution tools do not run locally,
            # but allow actions explicitly marked no-remote (such as install/package actions)
            # to fall back to a usable local strategy.
            "--spawn_strategy=remote,local",
            "--strategy=CppCompile=remote",
            "--strategy=CppLink=remote",
            "--strategy=CppArchive=remote",
            "--strategy=SolibSymlink=remote",
            "--strategy=ExtractDebugInfo=remote",
            "--strategy=StripDebugInfo=remote",
            "--strategy=CcGenerateIntermediateDwp=remote",
            "--strategy=CcGenerateDwp=remote",
            "--strategy=IdlcGenerator=remote",
            "--features=-thin_archive",
            "--test_strategy=standalone",
            "--strategy=TestRunner=standalone",
        ],
    )


def _linux_host_container_state_dir(repo_root: pathlib.Path = REPO_ROOT) -> pathlib.Path:
    return repo_root / ".tmp" / "linux_container_actions"


def _linux_host_container_config(
    env: Mapping[str, str],
    machine: str | None = None,
    containers: Mapping[str, Mapping[str, str]] | None = None,
    repo_root: pathlib.Path = REPO_ROOT,
    container_command: str | None = None,
) -> dict[str, str]:
    """Returns the persistent-container config consumed by the Bazel action runner."""
    containers = containers or load_remote_execution_containers()
    arch = normalize_arch(machine)
    distro = _linux_host_container_distro(env, machine=machine, containers=containers)
    if distro is None:
        raise RuntimeError(
            "Linux host container mode requires a host distro with a pinned RBE "
            "container and mongo toolchain"
        )

    image_override = env.get("MONGO_HERMETIC_CONTAINER_IMAGE")
    if image_override and not _is_digest_pinned_container_url(image_override):
        raise RuntimeError(
            "MONGO_HERMETIC_CONTAINER_IMAGE must use an immutable "
            "@sha256:<64-hex-digit-digest> reference in Linux host container mode; "
            "mutable image tags cannot safely race local and remote execution"
        )

    container_url = image_override or containers[distro]["container-url"]
    image = container_url.removeprefix("docker://")
    image_hash = hashlib.sha256(image.encode()).hexdigest()[:12]
    state_dir = _linux_host_container_state_dir(repo_root)
    docker_command = container_command or env.get("HERMETIC_CONTAINER_DOCKER_COMMAND")
    if not docker_command:
        docker_command = shutil.which("docker") or shutil.which("podman") or "docker"
    docker_argv = shlex.split(docker_command)
    if len(docker_argv) == 1 and "/" not in docker_argv[0]:
        docker_executable = shutil.which(docker_argv[0])
        if docker_executable:
            docker_command = docker_executable
    config = {
        "image": image,
        "docker_command": docker_command,
        "network": env.get("HERMETIC_CONTAINER_NETWORK", "host"),
        "user": _container_user("Linux"),
        "home": str(state_dir / "home"),
        "repo_root": str(repo_root),
        "state_dir": str(state_dir),
        "container_layout_version": LINUX_CONTAINER_ACTIONS_LAYOUT_VERSION,
        "container_prefix": _safe_name(f"mongo_linux_action_{distro}_{arch}_{image_hash}"),
    }
    task_id = env.get(PODMAN_TASK_ID_ENV)
    if task_id:
        config["podman_task_id"] = task_id
    return config


def _startup_option_values(args: Sequence[str], name: str) -> list[str]:
    """Returns a Bazel startup option's command-line values in precedence order."""
    command_index = _bazel_command_index(args)
    startup_args = args if command_index is None else args[:command_index]
    values = []
    index = 0
    while index < len(startup_args):
        arg = startup_args[index]
        if arg.startswith(f"{name}="):
            values.append(arg.split("=", 1)[1])
        elif arg == name and index + 1 < len(startup_args):
            index += 1
            values.append(startup_args[index])
        index += 1
    return values


def _startup_option_value(args: Sequence[str], name: str) -> str | None:
    """Returns a Bazel startup option's highest-precedence command-line value."""
    values = _startup_option_values(args, name)
    return values[-1] if values else None


def _startup_boolean_option(args: Sequence[str], name: str, default: bool) -> bool:
    """Returns the effective value of a --[no]NAME startup option."""
    command_index = _bazel_command_index(args)
    startup_args = args if command_index is None else args[:command_index]
    enabled = f"--{name}"
    disabled = f"--no{name}"
    value = default
    for arg in startup_args:
        if arg == enabled:
            value = True
        elif arg == disabled:
            value = False
    return value


def _resolve_bazelrc_path(
    value: str,
    repo_root: pathlib.Path,
    relative_to: pathlib.Path,
) -> pathlib.Path:
    value = value.replace("%workspace%", str(repo_root))
    path = pathlib.Path(os.path.expanduser(value))
    return path if path.is_absolute() else relative_to / path


def _bazelrc_paths(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path,
) -> list[pathlib.Path]:
    if _startup_boolean_option(args, "ignore_all_rc_files", False):
        return []

    bazelrc_values = _startup_option_values(args, "--bazelrc")
    if "/dev/null" in bazelrc_values:
        return []

    paths = []
    if _startup_boolean_option(args, "system_rc", True):
        paths.append(pathlib.Path("/etc/bazel.bazelrc"))
    if _startup_boolean_option(args, "workspace_rc", True):
        workspace_rc = repo_root / ".bazelrc"
        paths.append(workspace_rc)
        # Evergreen's generated RC files historically reached this hook after Bazel
        # startup, so retain the wrapper's fallback for test/worktree roots that do not
        # have the repository's normal try-import lines yet. Avoid duplicating files that
        # the workspace RC already imports, which would change home/custom RC precedence.
        try:
            workspace_contents = workspace_rc.read_text(encoding="utf-8")
        except OSError:
            workspace_contents = ""
        for late_rc_name in (".bazelrc.evergreen", ".bazelrc.local"):
            if late_rc_name not in workspace_contents:
                paths.append(repo_root / late_rc_name)
    if _startup_boolean_option(args, "home_rc", True):
        home = _host_home(env)
        if home is not None:
            paths.append(home / ".bazelrc")

    for value in bazelrc_values:
        paths.append(_resolve_bazelrc_path(value, repo_root, repo_root))
    return paths


def _bazelrc_tokens(
    path: pathlib.Path,
    repo_root: pathlib.Path,
    active_paths: set[pathlib.Path] | None = None,
) -> list[list[str]]:
    """Reads an rc file and its in-place import/try-import directives."""
    try:
        resolved = path.resolve()
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []

    active_paths = set() if active_paths is None else active_paths
    if resolved in active_paths:
        return []
    active_paths.add(resolved)
    result = []
    try:
        for raw_line in lines:
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            try:
                tokens = shlex.split(line)
            except ValueError:
                continue
            if tokens and tokens[0] in {"import", "try-import"} and len(tokens) == 2:
                imported = _resolve_bazelrc_path(tokens[1], repo_root, path.parent)
                result.extend(_bazelrc_tokens(imported, repo_root, active_paths))
            elif tokens:
                result.append(tokens)
    finally:
        active_paths.remove(resolved)
    return result


def _bazel_command_line_options(args: Sequence[str]) -> list[str]:
    """Return command options, excluding target arguments and test arguments."""
    command_index = _bazel_command_index(args)
    start = command_index + 1 if command_index is not None else 0
    options = []
    for arg in args[start:]:
        if arg == "--":
            break
        options.append(arg)
    return options


def _bazel_effective_options(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path,
    include_command_line: bool = True,
) -> list[str]:
    """Return Bazel options after applying RC files and config aliases in order.

    Bazel reads system, workspace, home, and explicit --bazelrc files in that order.
    Config sections are collected from those files, while ordinary options and config
    selections are expanded at their original locations. This mirrors the subset of
    Bazel RC behavior needed before starting Bazel itself.
    """
    command = _bazel_command(args)
    config_options: dict[str, list[str]] = {}
    rc_options: list[str] = []

    for path in _bazelrc_paths(args, env, repo_root):
        for tokens in _bazelrc_tokens(path, repo_root):
            if not tokens:
                continue

            scope = tokens[0]
            if scope in {"common", command}:
                rc_options.extend(tokens[1:])
                continue

            # A bare --config=... line in the repository RC declares a config name;
            # it does not select that config. Only command/common lines contribute
            # active options here.
            config_scope, separator, config_name = scope.partition(":")
            if separator and config_scope in {"common", command}:
                config_options.setdefault(config_name, []).extend(tokens[1:])

    def expand(options: Sequence[str], active_configs: frozenset[str] = frozenset()) -> list[str]:
        expanded: list[str] = []
        index = 0
        while index < len(options):
            option = options[index]
            expanded.append(option)
            config_name = None
            if option.startswith("--config="):
                config_name = option.split("=", 1)[1]
            elif option == "--config" and index + 1 < len(options):
                index += 1
                expanded.append(options[index])
                config_name = options[index]

            if config_name and config_name not in active_configs:
                expanded.extend(
                    expand(config_options.get(config_name, ()), active_configs | {config_name})
                )
            index += 1
        return expanded

    effective = expand(rc_options)
    command_line_options = _bazel_command_line_options(args)
    if include_command_line:
        effective.extend(expand(command_line_options))
    else:
        # RC-only callers still need command-line config selections to expand the
        # corresponding RC aliases, while ordinary command-line options retain their
        # separate highest-precedence handling.
        command_line_configs: list[str] = []
        index = 0
        while index < len(command_line_options):
            option = command_line_options[index]
            if option.startswith("--config="):
                command_line_configs.append(option)
            elif option == "--config" and index + 1 < len(command_line_options):
                command_line_configs.extend(command_line_options[index : index + 2])
                index += 1
            index += 1
        effective.extend(expand(command_line_configs))
    return effective


def _effective_config_values(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path,
) -> list[str]:
    values = []
    options = _bazel_effective_options(args, env=env, repo_root=repo_root)
    index = 0
    while index < len(options):
        option = options[index]
        if option.startswith("--config="):
            values.append(option.split("=", 1)[1])
        elif option == "--config" and index + 1 < len(options):
            index += 1
            values.append(options[index])
        index += 1
    return values


def _bazelrc_startup_option_value(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path,
    name: str,
) -> str | None:
    value = None
    for path in _bazelrc_paths(args, env, repo_root):
        for tokens in _bazelrc_tokens(path, repo_root):
            if not tokens or tokens[0] != "startup":
                continue
            index = 1
            while index < len(tokens):
                token = tokens[index]
                if token.startswith(f"{name}="):
                    value = token.split("=", 1)[1]
                elif token == name and index + 1 < len(tokens):
                    index += 1
                    value = tokens[index]
                index += 1
    return value


def _bazel_output_base(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> pathlib.Path:
    """Returns the output base Bazel will use for this invocation.

    Mirrors Bazel's default computation (MD5 of the workspace path under the output user
    root), honoring command-line options and startup options from Bazel's rc files.
    """
    explicit = _startup_option_value(args, "--output_base") or _bazelrc_startup_option_value(
        args, env, repo_root, "--output_base"
    )
    if explicit:
        return pathlib.Path(os.path.expanduser(explicit))

    user_root = (
        _startup_option_value(args, "--output_user_root")
        or _bazelrc_startup_option_value(args, env, repo_root, "--output_user_root")
        or _default_bazel_user_output_root(env, system="Linux")
    )
    user_root_path = pathlib.Path(os.path.expanduser(user_root))
    candidates = []
    for workspace in [str(repo_root), str(repo_root.resolve())]:
        # Bazel uses MD5 for this non-security output-base identifier. Keep this in sync with
        # Bazel so the wrapper and Bazel resolve the same output tree.
        workspace_digest = hashlib.md5(  # nosemgrep: insecure-hash-algorithm-md5
            workspace.encode(), usedforsecurity=False
        ).hexdigest()
        candidate = user_root_path / workspace_digest
        if candidate not in candidates:
            candidates.append(candidate)

    for candidate in candidates:
        if candidate.exists():
            return candidate

    return candidates[0]


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


def _replace_bazel_startup_option(
    args: Sequence[str],
    name: str,
    value: str,
) -> list[str]:
    """Replaces a startup option while preserving its required pre-command position."""
    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)

    startup_args = []
    index = 0
    while index < command_index:
        arg = args[index]
        if arg.startswith(f"{name}="):
            index += 1
            continue
        if arg == name:
            index += 2
            continue
        startup_args.append(arg)
        index += 1
    return [*startup_args, f"{name}={value}", *args[command_index:]]


@contextlib.contextmanager
def _linux_container_actions_lock(output_base: pathlib.Path):
    """Serializes action-config publication and container preflight for one output base."""
    import fcntl

    output_base.mkdir(parents=True, exist_ok=True)
    lock_path = output_base.resolve() / LINUX_CONTAINER_ACTIONS_LOCK_FILENAME
    with lock_path.open("a", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _write_linux_container_actions_config_unlocked(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
    machine: str | None = None,
) -> tuple[pathlib.Path, dict[str, str]]:
    """Writes the container action config; the caller must hold the output-base lock."""
    config = _linux_host_container_config(env, machine=machine, repo_root=repo_root)
    output_base = _bazel_output_base(args, env, repo_root=repo_root)
    config["sandbox_base"] = str(_linux_action_sandbox_base(output_base))
    # Native MongoInstallRule actions publish outside their declared outputs. Use the same
    # host-temp tree for native actions and for containerized local actions so that the convenience
    # symlink always points at the artifacts that the build actually installed. The tree is mounted
    # explicitly into the container below; the wrapper chooses the action strategy at invocation
    # time.
    shared_install_dir = _linux_native_shared_install_dir(output_base)
    config["shared_install_dir"] = str(shared_install_dir)
    output_base.mkdir(parents=True, exist_ok=True)
    generation_path = output_base / LINUX_CONTAINER_ACTIONS_GENERATION_FILENAME
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
        raise RuntimeError(f"Invalid Linux action container generation: {generation_path}")
    if new_generation:
        for stale_shared_install_dir in (
            shared_install_dir,
            _linux_shared_install_dir(output_base),
        ):
            if stale_shared_install_dir.is_dir() and not stale_shared_install_dir.is_symlink():
                shutil.rmtree(stale_shared_install_dir)
            elif stale_shared_install_dir.exists() or stale_shared_install_dir.is_symlink():
                stale_shared_install_dir.unlink()
    shared_install_dir.mkdir(parents=True, exist_ok=True)
    config["output_base_generation"] = generation
    prefix = config.get("container_prefix", "mongo_linux_action")
    digest = hashlib.sha256(
        (
            f"{config['repo_root']}|{output_base}|{config['image']}|"
            f"{config['container_layout_version']}|{generation}"
        ).encode()
    ).hexdigest()[:12]
    config["container_name"] = _safe_name(f"{prefix}_{digest}")[:120]
    path = output_base / LINUX_CONTAINER_ACTIONS_CONFIG_FILENAME
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


def _write_linux_container_actions_config(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
    machine: str | None = None,
) -> tuple[pathlib.Path, dict[str, str]]:
    """Publishes the container action config atomically under the output-base lock."""
    output_base = _bazel_output_base(args, env, repo_root=repo_root)
    with _linux_container_actions_lock(output_base):
        return _write_linux_container_actions_config_unlocked(
            args,
            env,
            repo_root=repo_root,
            machine=machine,
        )


def _ensure_linux_container_image(docker_command: str, image: str) -> bool:
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
        stdout=sys.stderr,
        env=runtime_env,
        description=f"pulling build container image {image}",
    )
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


def _ensure_linux_action_container(config_path: pathlib.Path) -> tuple[bool, str]:
    """Start and execute a smoke check in the persistent action container."""
    command = [
        sys.executable,
        str(LINUX_CONTAINER_ACTION_WRAPPER_SCRIPT),
        "--ensure-container",
        str(config_path),
    ]
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return (
            False,
            f"container start/exec preflight timed out after "
            f"{DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS}s",
        )

    detail = _compact_runtime_detail(result.stderr or result.stdout)
    return result.returncode == 0, detail


def _linux_host_container_action_args(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> list[str]:
    # These commands only analyze the graph or fetch repositories. They do not run
    # local actions, and Bazel's query-family commands reject action-only options
    # (including --sandbox_base and persistent-container strategy flags).
    if _bazel_command(args) in LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS:
        return list(args)

    remote_link_requested = _remote_link_requested(args, env=env, repo_root=repo_root)
    output_base = _bazel_output_base(args, env, repo_root=repo_root)
    sandbox_base = _linux_action_sandbox_base(output_base)
    worker_dir = sandbox_base / "persistent-workers"
    config_path = output_base / LINUX_CONTAINER_ACTIONS_CONFIG_FILENAME
    options = [
        "--experimental_enable_persistent_container_sandbox",
        f"--experimental_persistent_container_python={sys.executable}",
        f"--experimental_persistent_container_runner={LINUX_CONTAINER_ACTION_WRAPPER_SCRIPT}",
        f"--experimental_persistent_container_config={config_path}",
        f"--experimental_persistent_container_worker_dir={worker_dir}",
        f"--sandbox_base={sandbox_base}",
        "--strategy=MongoInstallRule=persistent-container,local",
        *[
            f"--strategy={mnemonic}=persistent-container,local"
            for mnemonic in LINUX_LOCAL_CONTAINER_MNEMONICS
        ],
        *[
            f"--strategy={mnemonic}=persistent-container,local"
            for mnemonic in LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS
            if not remote_link_requested
        ],
    ]
    remote_execution_disabled = _remote_execution_disabled_by_args(
        args, env=env, repo_root=repo_root
    )
    if remote_execution_disabled:
        options.extend(
            [
                *[f"--strategy={mnemonic}=local" for mnemonic in LINUX_LOCAL_TEST_MNEMONICS],
                *[
                    f"--strategy={mnemonic}=persistent-container,local"
                    for mnemonic in LINUX_DYNAMIC_CONTAINER_MNEMONICS
                ],
                *[
                    f"--strategy={mnemonic}=persistent-container,local"
                    for mnemonic in LINUX_CONTAINER_TOOL_MNEMONICS
                    if mnemonic != "PyCompile"
                ],
                "--strategy=PyCompile=worker",
            ]
        )
    elif not _env_is_false(env.get(LINUX_DYNAMIC_SCHEDULING_ENV)):
        options.extend(
            [
                "--internal_spawn_scheduler",
                f"--experimental_dynamic_local_load_factor={LINUX_DYNAMIC_LOCAL_LOAD_FACTOR}",
                "--experimental_cpp_compile_resource_estimation",
            ]
        )
        for mnemonic in LINUX_DYNAMIC_CONTAINER_MNEMONICS:
            options.extend(
                [
                    f"--strategy={mnemonic}=dynamic",
                    f"--dynamic_local_strategy={mnemonic}=persistent-container",
                    f"--dynamic_remote_strategy={mnemonic}=remote",
                ]
            )
        options.extend(
            [
                f"--strategy={mnemonic}=remote,persistent-container,local"
                for mnemonic in LINUX_CONTAINER_TOOL_MNEMONICS
            ]
        )
    else:
        options.extend(
            [
                *[
                    f"--strategy={mnemonic}=remote"
                    for mnemonic in LINUX_DYNAMIC_CONTAINER_MNEMONICS
                ],
                *[
                    f"--strategy={mnemonic}=remote,persistent-container,local"
                    for mnemonic in LINUX_CONTAINER_TOOL_MNEMONICS
                ],
            ]
        )
    return _append_bazel_command_options(args, options)


def _macos_cross_rbe_exec_properties(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    image = env.get(MACOS_CROSS_RBE_CONTAINER_IMAGE_ENV)
    if not image:
        containers = containers or load_remote_execution_containers()
        distro = select_distro(containers, env=env, arch="aarch64")
        image = containers[distro]["container-url"]

    return [
        f"container-image={image}",
        "dockerNetwork=standard",
        f"Pool={env.get(MACOS_CROSS_RBE_POOL_ENV, MACOS_CROSS_RBE_POOL)}",
    ]


def _macos_cross_linux_python_options(arch: str) -> list[str]:
    return [
        "--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=1",
        f"--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH={arch}",
        f"--//bazel/config:macos_cross_linux_python_arch={arch}",
    ]


def _cross_host_action_runtime_environment(env: Mapping[str, str], system: str) -> dict[str, str]:
    """Returns host-local settings for the cross-action wrapper.

    These values are deliberately kept out of Bazel's action environment. Most of them are
    absolute paths, and putting them in ``--action_env`` makes otherwise identical remote
    actions use different cache keys on every checkout. The wrapper reads this manifest from
    the current output base immediately before it starts the local container.
    """
    runtime_env = {
        "MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND": env.get(
            "HERMETIC_CONTAINER_DOCKER_COMMAND", "docker"
        ),
        "MONGO_MACOS_CROSS_ACTION_REPO_ROOT": str(REPO_ROOT),
        "MONGO_MACOS_CROSS_ACTION_HOME": str(_hermetic_container_home_dir(REPO_ROOT)),
        "MONGO_MACOS_CROSS_ACTION_NETWORK": env.get("HERMETIC_CONTAINER_NETWORK", "host"),
        "MONGO_MACOS_CROSS_ACTION_USER": _container_user(system),
    }

    if env.get("MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"):
        runtime_env["MONGO_MACOS_CROSS_ACTION_PLATFORM"] = env[
            "MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"
        ]

    path_env_map = (
        {
            "MONGO_WINDOWS_CROSS_LLVM_PATH": "LLVM_PATH",
            "MONGO_WINDOWS_CROSS_SYSROOT_PATH": "MACOS_SDK_PATH",
        }
        if system == "Windows"
        else {
            "LLVM_PATH": "LLVM_PATH",
            "MACOS_SDK_PATH": "MACOS_SDK_PATH",
        }
    )
    for source, destination in path_env_map.items():
        if env.get(source):
            runtime_env[destination] = env[source]

    for var_name in ("DOCKER_HOST", "DOCKER_CONTEXT", "DOCKER_CONFIG"):
        if env.get(var_name):
            runtime_env[var_name] = env[var_name]

    return runtime_env


def _write_cross_host_action_config(
    args: Sequence[str], env: Mapping[str, str], system: str
) -> pathlib.Path:
    """Publishes host-only cross-action settings outside the Bazel action key."""
    output_base = _bazel_output_base(args, env, repo_root=REPO_ROOT)
    output_base.mkdir(parents=True, exist_ok=True)
    path = output_base / CROSS_HOST_ACTION_CONFIG_FILENAME
    content = (
        json.dumps(
            {
                "version": 1,
                "environment": _cross_host_action_runtime_environment(env, system),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    try:
        if path.read_text(encoding="utf-8") == content:
            return path
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
    return path


def _macos_cross_local_resource_options(env: Mapping[str, str]) -> list[str]:
    cpu_resources = env.get(
        MACOS_CROSS_LOCAL_CPU_RESOURCES_ENV,
        MACOS_CROSS_DEFAULT_LOCAL_RESOURCE_VALUE,
    )
    test_jobs = env.get(
        MACOS_CROSS_LOCAL_TEST_JOBS_ENV,
        MACOS_CROSS_DEFAULT_LOCAL_RESOURCE_VALUE,
    )
    options = []
    if not _env_is_false(cpu_resources):
        options.append(f"--local_resources=cpu={cpu_resources}")
    if not _env_is_false(test_jobs):
        options.append(f"--local_test_jobs={test_jobs}")
    return options


def _macos_cross_host_bazel_test_args(
    args: Sequence[str],
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    exec_properties = _macos_cross_rbe_exec_properties(env, containers=containers)
    return _append_bazel_command_options(
        args,
        [
            f"--remote_executor={MACOS_CROSS_REMOTE_EXECUTOR}",
            *[
                f"--remote_default_exec_properties={exec_property}"
                for exec_property in exec_properties
            ],
            *_macos_cross_linux_python_options("aarch64"),
            "--//bazel/config:idl_use_linux_python=True",
            "--//bazel/config:remote_link=True",
            "--spawn_strategy=local",
            # MongoInstallRule publishes the shared bazel-bin/install convenience tree, which
            # is outside the action's declared outputs and cannot be written from a sandbox.
            "--strategy=MongoInstallRule=local",
            "--strategy=CppCompile=remote",
            "--strategy=CppLink=remote",
            "--strategy=CppArchive=remote",
            "--strategy=SolibSymlink=remote",
            "--strategy=ExtractDebugInfo=remote",
            "--strategy=StripDebugInfo=remote",
            "--strategy=CcGenerateIntermediateDwp=remote",
            "--strategy=CcGenerateDwp=remote",
            "--strategy=IdlcGenerator=remote",
            "--features=-thin_archive",
            *_macos_cross_local_resource_options(env),
            "--test_strategy=standalone",
            "--strategy=TestRunner=standalone",
        ],
    )


def _windows_cross_rbe_exec_properties(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    image = env.get(WINDOWS_CROSS_RBE_CONTAINER_IMAGE_ENV)
    if not image:
        containers = containers or load_remote_execution_containers()
        distro = (
            "ubuntu18"
            if not env.get("MONGO_HERMETIC_CONTAINER_DISTRO") and "ubuntu18" in containers
            else select_distro(containers, env=env, arch="x86_64")
        )
        image = containers[distro]["container-url"]

    return [
        f"container-image={image}",
        "dockerNetwork=standard",
        f"Pool={env.get(WINDOWS_CROSS_RBE_POOL_ENV, WINDOWS_CROSS_RBE_POOL)}",
    ]


def _windows_cross_action_container_env_options(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    containers = containers or load_remote_execution_containers()
    arch = "x86_64"
    distro = (
        "ubuntu18"
        if not env.get("MONGO_HERMETIC_CONTAINER_DISTRO") and "ubuntu18" in containers
        else select_distro(containers, env=env, arch=arch)
    )
    image_override = env.get(WINDOWS_CROSS_RBE_CONTAINER_IMAGE_ENV) or env.get(
        "MONGO_HERMETIC_CONTAINER_IMAGE"
    )
    container_url = image_override or containers[distro]["container-url"]
    docker_image = parse_docker_image(container_url)
    if _env_is_true(env.get(HERMETIC_CONTAINER_GIT_LAYER_ENV)):
        docker_image, _ = _hermetic_container_image_with_git_layer(
            REPO_ROOT,
            docker_image,
            docker_command=(
                None
                if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1"
                else env.get("HERMETIC_CONTAINER_DOCKER_COMMAND", "docker")
            ),
        )

    image_hash = hashlib.sha256(docker_image.full_name.encode()).hexdigest()[:12]
    env_values = {
        "MONGO_WINDOWS_CROSS_ACTION_WRAPPER": "1",
        "MONGO_WINDOWS_CROSS_ACTION_CONTAINER_PREFIX": _safe_name(
            f"mongo_windows_cross_action_{distro}_{arch}_{image_hash}"
        ),
        "MONGO_WINDOWS_CROSS_ACTION_IMAGE": docker_image.full_name,
    }
    if env.get("MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"):
        env_values["MONGO_WINDOWS_CROSS_ACTION_PLATFORM"] = env[
            "MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"
        ]

    return [f"--action_env={key}={value}" for key, value in sorted(env_values.items())]


def _windows_cross_host_wrapper_action_args(
    args: Sequence[str],
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    exec_properties = _windows_cross_rbe_exec_properties(env, containers=containers)
    return _append_bazel_command_options(
        args,
        [
            "--//bazel/config:windows_cross_local_container_actions=True",
            "--//bazel/config:idl_use_linux_python=True",
            "--//bazel/config:disable_warnings_as_errors=True",
            *_macos_cross_linux_python_options("x86_64"),
            *[
                f"--repo_env={var_name}={env[var_name]}"
                for var_name in sorted(WINDOWS_CROSS_PASSTHROUGH_ENVS)
                if env.get(var_name)
            ],
            *(
                [f"--//bazel/config:windows_cross_host_path={env['PATH']}"]
                if env.get("PATH")
                else []
            ),
            f"--remote_executor={WINDOWS_CROSS_REMOTE_EXECUTOR}",
            *[
                f"--remote_default_exec_properties={exec_property}"
                for exec_property in exec_properties
            ],
            "--//bazel/config:remote_link=True",
            "--spawn_strategy=local",
            "--strategy=CppCompile=remote",
            "--strategy=CppLink=remote",
            "--strategy=CppArchive=remote",
            "--strategy=SolibSymlink=remote",
            "--strategy=ExtractDebugInfo=remote",
            "--strategy=StripDebugInfo=remote",
            "--strategy=CcGenerateIntermediateDwp=remote",
            "--strategy=CcGenerateDwp=remote",
            "--strategy=ConfigHeaderGen=remote",
            "--strategy=IdlcGenerator=remote",
            "--strategy=WindowsRC=remote",
            "--features=-thin_archive",
            "--test_strategy=standalone",
            "--strategy=TestRunner=standalone",
            *_windows_cross_action_container_env_options(env, containers=containers),
        ],
    )


def _macos_cross_action_container_env_options(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    containers = containers or load_remote_execution_containers()
    arch = "aarch64"
    distro = select_distro(containers, env=env, arch=arch)
    image_override = env.get("MONGO_HERMETIC_CONTAINER_IMAGE")
    container_url = image_override or containers[distro]["container-url"]
    docker_image = parse_docker_image(container_url)
    if _hermetic_container_git_layer_enabled(env, "Darwin"):
        docker_image, _ = _hermetic_container_image_with_git_layer(
            REPO_ROOT,
            docker_image,
            docker_command=(
                None
                if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1"
                else env.get("HERMETIC_CONTAINER_DOCKER_COMMAND", "docker")
            ),
        )

    image_hash = hashlib.sha256(docker_image.full_name.encode()).hexdigest()[:12]
    env_values = {
        "MONGO_MACOS_CROSS_ACTION_WRAPPER": "1",
        "MONGO_MACOS_CROSS_ACTION_CONTAINER_PREFIX": _safe_name(
            f"mongo_macos_cross_action_{distro}_{arch}_{image_hash}"
        ),
        "MONGO_MACOS_CROSS_ACTION_IMAGE": docker_image.full_name,
    }
    if env.get("MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"):
        env_values["MONGO_MACOS_CROSS_ACTION_PLATFORM"] = env[
            "MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"
        ]

    return [f"--action_env={key}={value}" for key, value in sorted(env_values.items())]


def _macos_cross_local_container_action_args(
    args: Sequence[str],
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    common_options = [
        "--//bazel/config:macos_cross_local_container_actions=True",
        "--//bazel/config:idl_use_linux_python=True",
        # The install rule publishes a shared convenience tree outside its declared outputs.
        "--strategy=MongoInstallRule=local",
        *_macos_cross_local_resource_options(env),
        "--test_strategy=standalone",
        "--strategy=TestRunner=standalone",
        *_macos_cross_action_container_env_options(env, containers=containers),
    ]

    if _macos_cross_remote_execution_disabled(args, env):
        action_options = [
            *_macos_cross_linux_python_options("aarch64"),
            "--strategy=CppCompile=local",
            "--strategy=CppLink=local",
            "--strategy=CppArchive=local",
            "--strategy=SolibSymlink=local",
            "--strategy=ExtractDebugInfo=local",
            "--strategy=StripDebugInfo=local",
            "--strategy=CcGenerateIntermediateDwp=local",
            "--strategy=CcGenerateDwp=local",
            "--strategy=IdlcGenerator=local",
        ]
    else:
        exec_properties = _macos_cross_rbe_exec_properties(env, containers=containers)
        action_options = [
            f"--remote_executor={MACOS_CROSS_REMOTE_EXECUTOR}",
            *[
                f"--remote_default_exec_properties={exec_property}"
                for exec_property in exec_properties
            ],
            *_macos_cross_linux_python_options("aarch64"),
            "--spawn_strategy=local",
            "--strategy=CppCompile=remote",
            "--strategy=IdlcGenerator=remote",
        ]
        if _remote_link_requested(args, env=env):
            action_options.extend(
                [
                    "--//bazel/config:remote_link=True",
                    "--strategy=CppLink=remote",
                    "--strategy=CppArchive=remote",
                    "--strategy=SolibSymlink=remote",
                    "--strategy=ExtractDebugInfo=remote",
                    "--strategy=StripDebugInfo=remote",
                    "--strategy=CcGenerateIntermediateDwp=remote",
                    "--strategy=CcGenerateDwp=remote",
                    "--features=-thin_archive",
                ]
            )
        else:
            action_options.extend(
                [
                    "--strategy=CppLink=local",
                    "--strategy=CppArchive=local",
                    "--strategy=SolibSymlink=local",
                    "--strategy=ExtractDebugInfo=local",
                    "--strategy=StripDebugInfo=local",
                    "--strategy=CcGenerateIntermediateDwp=local",
                    "--strategy=CcGenerateDwp=local",
                ]
            )

    return _append_bazel_command_options(
        args,
        [
            *common_options,
            *action_options,
        ],
    )


def _macos_cross_host_test_plan(
    args: Sequence[str],
    env: Mapping[str, str],
) -> MacOSCrossHostTestPlan:
    command_index = _bazel_command_index(args)
    if command_index is None or args[command_index] != "test":
        raise RuntimeError("macOS cross host test planning requires a bazel test command")

    startup_args = list(args[:command_index])
    build_args = [*startup_args, "build", "--build_tests_only"]
    host_test_options: list[str] = []
    target_patterns: list[str] = []
    test_args: list[str] = []
    test_env: dict[str, str] = {}
    test_tag_filters: list[str] = []
    build_event_json_file = None
    runs_per_test = 1
    run_host_tests = True

    command_args = list(args[command_index + 1 :])
    index = 0
    while index < len(command_args):
        arg = command_args[index]
        if arg == "--":
            test_args.extend(command_args[index + 1 :])
            host_test_options.extend("--test_arg=" + value for value in command_args[index + 1 :])
            break

        if arg == "--test_arg" or arg.startswith("--test_arg="):
            value, index = _consume_option_value(command_args, index)
            if value is not None:
                test_args.append(value)
                host_test_options.append("--test_arg=" + value)
            continue

        if arg == "--test_env" or arg.startswith("--test_env="):
            value, index = _consume_option_value(command_args, index)
            if value is not None:
                _add_test_env(test_env, value, env)
                host_test_options.append("--test_env=" + value)
            continue

        if arg == "--test_tag_filters" or arg.startswith("--test_tag_filters="):
            value, next_index = _consume_option_value(command_args, index)
            if value is not None:
                test_tag_filters = _parse_test_tag_filters(value)
            build_args.extend(command_args[index:next_index])
            index = next_index
            continue

        if arg == "--test_filter" or arg.startswith("--test_filter="):
            value, index = _consume_option_value(command_args, index)
            if value:
                test_env["TESTBRIDGE_TEST_ONLY"] = value
                host_test_options.append("--test_filter=" + value)
            continue

        if arg == "--runs_per_test" or arg.startswith("--runs_per_test="):
            value, index = _consume_option_value(command_args, index)
            if value:
                try:
                    runs_per_test = max(1, int(value))
                except ValueError:
                    runs_per_test = 1
                host_test_options.append("--runs_per_test=" + value)
            continue

        if arg in TEST_FLAGS_WITH_SEPARATE_VALUE:
            _, next_index = _consume_option_value(command_args, index)
            host_test_options.extend(command_args[index:next_index])
            index = next_index
            continue

        if arg.startswith(TEST_FLAG_PREFIXES):
            host_test_options.append(arg)
            index += 1
            continue

        if arg == "--target_pattern_file" or arg.startswith("--target_pattern_file="):
            value, next_index = _consume_option_value(command_args, index)
            if value is not None:
                target_patterns.extend(_read_target_pattern_file(value))
            build_args.extend(command_args[index:next_index])
            index = next_index
            continue

        if arg == "--build_event_json_file" or arg.startswith("--build_event_json_file="):
            value, next_index = _consume_option_value(command_args, index)
            if value is not None:
                build_event_json_file = value
            build_args.extend(command_args[index:next_index])
            index = next_index
            continue

        if arg in CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE:
            _, next_index = _consume_option_value(command_args, index)
            build_args.extend(command_args[index:next_index])
            if not _is_macos_cross_config_arg(command_args, index):
                host_test_options.extend(command_args[index:next_index])
            index = next_index
            continue

        if arg.startswith(tuple(flag + "=" for flag in CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE)):
            build_args.append(arg)
            if not _is_macos_cross_config_arg(command_args, index):
                host_test_options.append(arg)
            index += 1
            continue

        build_flag_value = _bazel_bool_flag_value(arg, "build")
        if build_flag_value is not None:
            run_host_tests = build_flag_value
            build_args.append(arg)
            index += 1
            continue

        build_args.append(arg)
        if arg.startswith("-"):
            host_test_options.append(arg)
        else:
            target_patterns.append(arg)
        index += 1

    return MacOSCrossHostTestPlan(
        build_args=build_args,
        startup_args=startup_args,
        host_test_options=host_test_options,
        target_patterns=target_patterns,
        test_args=test_args,
        test_env=test_env,
        test_tag_filters=test_tag_filters,
        build_event_json_file=build_event_json_file,
        runs_per_test=runs_per_test,
        run_host_tests=run_host_tests,
    )


def _cross_host_run_plan(args: Sequence[str], platform_name: str) -> MacOSCrossHostRunPlan:
    command_index = _bazel_command_index(args)
    if command_index is None or args[command_index] != "run":
        raise RuntimeError(f"{platform_name} cross host run planning requires a bazel run command")

    build_args = [*args[:command_index], "build"]
    target = ""
    run_args: list[str] = []

    command_args = list(args[command_index + 1 :])
    index = 0
    while index < len(command_args):
        arg = command_args[index]
        if arg == "--":
            run_args.extend(command_args[index + 1 :])
            break

        if arg in RUN_FLAGS_WITH_SEPARATE_VALUE:
            raise RuntimeError(
                f"{platform_name} cross host run does not support {arg}; "
                "disable cross host execution to use this Bazel option"
            )
        if arg.startswith(RUN_FLAG_PREFIXES):
            flag = arg.split("=", 1)[0]
            raise RuntimeError(
                f"{platform_name} cross host run does not support {flag}; "
                "disable cross host execution to use this Bazel option"
            )

        if arg in CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE:
            _, next_index = _consume_option_value(command_args, index)
            build_args.extend(command_args[index:next_index])
            index = next_index
            continue

        if arg.startswith(tuple(flag + "=" for flag in CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE)):
            build_args.append(arg)
            index += 1
            continue

        build_args.append(arg)
        if not arg.startswith("-") and not target:
            target = arg
        index += 1

    if not target:
        raise RuntimeError(f"{platform_name} cross host run planning requires a run target")

    return MacOSCrossHostRunPlan(
        build_args=build_args,
        target=target,
        run_args=run_args,
    )


def _macos_cross_host_run_plan(args: Sequence[str]) -> MacOSCrossHostRunPlan:
    return _cross_host_run_plan(args, "macOS")


def _windows_cross_host_run_plan(args: Sequence[str]) -> MacOSCrossHostRunPlan:
    return _cross_host_run_plan(args, "Windows")


def _fallback_test_labels(target_patterns: Sequence[str]) -> list[str]:
    labels = []
    for pattern in target_patterns:
        if "..." in pattern or pattern.startswith("-"):
            continue
        labels.append(pattern)
    return labels


def _query_string(value: str) -> str:
    return json.dumps(value)


def _exact_tag_query_regex(tag: str) -> str:
    return rf"(^|\[|, ){re.escape(tag)}($|,|\])"


def _host_test_query_expression(
    target_patterns: Sequence[str],
    test_tag_filters: Sequence[str],
) -> str:
    expression = "tests(set({}))".format(" ".join(target_patterns))
    for tag_filter in test_tag_filters:
        exclude = tag_filter.startswith("-")
        tag = tag_filter[1:] if exclude else tag_filter
        if not tag:
            continue

        tag_expression = 'attr("tags", {}, {})'.format(
            _query_string(_exact_tag_query_regex(tag)),
            expression,
        )
        if exclude:
            expression = f"({expression} except {tag_expression})"
        else:
            expression = tag_expression

    return expression


def _build_event_json_path(path: str, repo_root: pathlib.Path) -> pathlib.Path:
    build_event_path = pathlib.Path(path)
    if build_event_path.is_absolute():
        return build_event_path
    return repo_root / build_event_path


def _append_unique(labels: list[str], seen: set[str], label: str | None) -> None:
    if label and label not in seen:
        labels.append(label)
        seen.add(label)


def _host_test_labels_from_build_event_json(path: pathlib.Path) -> list[str]:
    pattern_labels: list[str] = []
    pattern_seen: set[str] = set()
    configured_test_labels: list[str] = []
    configured_test_seen: set[str] = set()
    skipped_labels: set[str] = set()

    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []

    for line in lines:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue

        if "pattern" in event.get("id", {}):
            for child in event.get("children", []):
                label = child.get("targetConfigured", {}).get("label")
                _append_unique(pattern_labels, pattern_seen, label)

        target_configured = event.get("id", {}).get("targetConfigured", {})
        label = target_configured.get("label")
        target_kind = event.get("configured", {}).get("targetKind", "")
        if isinstance(target_kind, str) and target_kind.endswith("_test rule"):
            _append_unique(configured_test_labels, configured_test_seen, label)

        target_completed = event.get("id", {}).get("targetCompleted", {})
        completed_label = target_completed.get("label")
        aborted = event.get("aborted", {})
        if completed_label and aborted.get("reason") == "SKIPPED":
            skipped_labels.add(completed_label)

    if pattern_labels and configured_test_labels:
        configured_test_set = set(configured_test_labels)
        return [
            label
            for label in pattern_labels
            if label in configured_test_set and label not in skipped_labels
        ]
    return [
        label for label in pattern_labels or configured_test_labels if label not in skipped_labels
    ]


def _expand_host_test_labels(
    bazel_real: str,
    plan: MacOSCrossHostTestPlan,
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> list[str]:
    if plan.build_event_json_file is not None:
        labels = _host_test_labels_from_build_event_json(
            _build_event_json_path(plan.build_event_json_file, repo_root)
        )
        if labels:
            return labels

    if not plan.target_patterns:
        return []

    query_env = _host_bazel_env(env)

    expression = _host_test_query_expression(plan.target_patterns, plan.test_tag_filters)
    result = subprocess.run(
        [bazel_real, "query", "--noshow_progress", "--output=label", expression],
        check=False,
        cwd=repo_root,
        env=query_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        _info("could not expand test patterns with bazel query; using explicit labels")
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        return _fallback_test_labels(plan.target_patterns)

    labels = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return labels or _fallback_test_labels(plan.target_patterns)


def _label_to_host_executable(
    label: str,
    repo_root: pathlib.Path = REPO_ROOT,
    bazel_bin_root: pathlib.Path | None = None,
    executable_suffix: str = "",
) -> pathlib.Path:
    if label.startswith("@"):
        raise RuntimeError(f"External test targets are not supported by cross host runner: {label}")

    if label.startswith("//"):
        label = label[2:]
    elif label.startswith(":"):
        label = label[1:]

    if ":" in label:
        package, name = label.split(":", 1)
    else:
        package = label
        name = pathlib.PurePosixPath(label).name

    filename = name
    if executable_suffix and not filename.endswith(executable_suffix):
        filename += executable_suffix

    return (bazel_bin_root or repo_root / "bazel-bin") / package / filename


def _resmoke_deps_path_file(
    label: str,
    repo_root: pathlib.Path = REPO_ROOT,
    bazel_bin_root: pathlib.Path | None = None,
) -> pathlib.Path:
    if label.startswith("@"):
        raise RuntimeError(
            f"External resmoke test targets are not supported by macOS cross host runner: {label}"
        )

    if label.startswith("//"):
        label = label[2:]
    elif label.startswith(":"):
        label = label[1:]

    if ":" in label:
        package, name = label.split(":", 1)
    else:
        package = label
        name = pathlib.PurePosixPath(label).name

    return (
        (bazel_bin_root or repo_root / "bazel-bin") / package / f"{name}{RESMOKE_DEPS_PATH_SUFFIX}"
    )


def _read_resmoke_deps_path(
    label: str,
    repo_root: pathlib.Path = REPO_ROOT,
    bazel_bin_root: pathlib.Path | None = None,
) -> str:
    deps_file = _resmoke_deps_path_file(label, repo_root, bazel_bin_root)
    deps = []
    for line in deps_file.read_text(encoding="utf-8").splitlines():
        value = line.strip()
        if not value:
            continue
        path = pathlib.Path(value)
        if path.is_absolute():
            deps.append(str(path))
            continue

        parts = path.parts
        if (
            bazel_bin_root is not None
            and len(parts) >= 3
            and parts[0] == "bazel-out"
            and parts[2] == "bin"
        ):
            deps.append(str((bazel_bin_root / pathlib.Path(*parts[3:])).resolve()))
        else:
            deps.append(str((repo_root / path).resolve()))

    return os.pathsep.join(deps)


def _write_resmoke_deps_path_map(
    labels: Sequence[str],
    repo_root: pathlib.Path = REPO_ROOT,
    bazel_bin_root: pathlib.Path | None = None,
) -> pathlib.Path:
    deps_path_map = {
        label: _read_resmoke_deps_path(label, repo_root, bazel_bin_root) for label in labels
    }
    content = json.dumps(deps_path_map, indent=2, sort_keys=True) + "\n"
    digest = hashlib.sha256(content.encode()).hexdigest()[:16]
    path = (
        _hermetic_container_state_dir(repo_root)
        / "macos-cross-resmoke-deps"
        / f"deps-path-map-{digest}.json"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


def _host_test_base_env(
    label: str,
    executable: pathlib.Path,
    plan: MacOSCrossHostTestPlan,
    repo_root: pathlib.Path,
) -> dict[str, str]:
    env = dict(os.environ)
    env.update(plan.test_env)
    env.setdefault("EXPERIMENTAL_SPLIT_XML_GENERATION", "1")
    env.setdefault("GTEST_OUTPUT", "")

    safe_label = _safe_name(label)
    test_root = repo_root / ".tmp" / "hermetic_container" / "macos-cross-testlogs" / safe_label
    undeclared_outputs = test_root / "test.outputs"
    test_tmpdir = test_root / "test.tmp"
    undeclared_outputs.mkdir(parents=True, exist_ok=True)
    test_tmpdir.mkdir(parents=True, exist_ok=True)

    env["TEST_TMPDIR"] = str(test_tmpdir)
    env["TEST_UNDECLARED_OUTPUTS_DIR"] = str(undeclared_outputs)
    env["XML_OUTPUT_FILE"] = str(test_root / "test.xml")
    env["TEST_BINARY"] = str(executable)
    env["TEST_TARGET"] = label
    env["TEST_WORKSPACE"] = "_main"
    env["PWD"] = str(repo_root)

    runfiles_dir = pathlib.Path(str(executable) + ".runfiles")
    if runfiles_dir.is_dir():
        env["RUNFILES_DIR"] = str(runfiles_dir)
        env["TEST_SRCDIR"] = str(runfiles_dir)

    return env


def _host_bazel_env(env: Mapping[str, str]) -> dict[str, str]:
    host_env = dict(env)
    host_env["BAZELISK_SKIP_WRAPPER"] = "1"
    host_env["MONGO_BAZEL_USE_HERMETIC_CONTAINER"] = "0"
    host_env["MONGO_MACOS_CROSS_DEFAULT_CONFIG"] = "0"
    host_env.pop("MONGO_BAZEL_IN_HERMETIC_CONTAINER", None)
    return host_env


def _run_macos_cross_host_resmoke_tests(
    bazel_real: str,
    labels: Sequence[str],
    plan: MacOSCrossHostTestPlan,
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
    bazel_bin_root: pathlib.Path | None = None,
) -> list[str]:
    if not labels:
        return []

    host_env = _host_bazel_env(env)
    deps_path_map_file = _write_resmoke_deps_path_map(labels, repo_root, bazel_bin_root)

    _info(f"running macOS cross resmoke tests on host: {len(labels)} target(s)")
    args = [
        bazel_real,
        *plan.startup_args,
        "test",
        "--//bazel/resmoke:skip_deps_for_cquery=True",
        *_macos_cross_local_resource_options(env),
        *plan.host_test_options,
        f"--test_env={RESMOKE_DEPS_PATH_MAP_ENV}={deps_path_map_file}",
        *labels,
    ]
    result = subprocess.run(
        args,
        check=False,
        cwd=repo_root,
        env=host_env,
    )

    return list(labels) if result.returncode != 0 else []


def _run_macos_cross_host_tests(
    bazel_real: str,
    plan: MacOSCrossHostTestPlan,
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> int:
    bazel_bin_root = _symlink_target(repo_root / "bazel-bin") or repo_root / "bazel-bin"
    labels = _expand_host_test_labels(bazel_real, plan, env, repo_root=repo_root)
    if not labels:
        print("ERROR: no test targets found for macOS cross host execution", file=sys.stderr)
        return 1

    resmoke_labels = [
        label
        for label in labels
        if _resmoke_deps_path_file(label, repo_root, bazel_bin_root).is_file()
    ]
    resmoke_label_set = set(resmoke_labels)
    executable_labels = [label for label in labels if label not in resmoke_label_set]

    failed: list[str] = []
    for label in executable_labels:
        executable = _label_to_host_executable(label, repo_root, bazel_bin_root)
        if not executable.is_file():
            print(
                f"ERROR: built test executable not found for {label}: {executable}", file=sys.stderr
            )
            failed.append(label)
            continue
        if not os.access(executable, os.X_OK):
            print(
                f"ERROR: built test executable is not executable for {label}: {executable}",
                file=sys.stderr,
            )
            failed.append(label)
            continue

        for run_index in range(plan.runs_per_test):
            suffix = f" ({run_index + 1}/{plan.runs_per_test})" if plan.runs_per_test > 1 else ""
            _info(f"running macOS cross test on host: {label}{suffix}")
            test_env = _host_test_base_env(label, executable, plan, repo_root)
            result = subprocess.run(
                [str(executable), *plan.test_args],
                check=False,
                cwd=repo_root,
                env=test_env,
            )
            if result.returncode != 0:
                failed.append(label)
                break

    if resmoke_labels:
        failed.extend(
            _run_macos_cross_host_resmoke_tests(
                bazel_real,
                resmoke_labels,
                plan,
                env,
                repo_root=repo_root,
                bazel_bin_root=bazel_bin_root,
            )
        )

    if failed:
        print("FAILED macOS cross host tests:", file=sys.stderr)
        for label in failed:
            print(f"  {label}", file=sys.stderr)
        return 1

    _info(f"all macOS cross host tests passed ({len(labels)} target(s))")
    return 0


def _host_binary_env(executable: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    runfiles_dir = pathlib.Path(str(executable) + ".runfiles")
    if runfiles_dir.is_dir():
        env["RUNFILES_DIR"] = str(runfiles_dir)
        env["TEST_SRCDIR"] = str(runfiles_dir)
    return env


def _run_macos_cross_host_binary(
    plan: MacOSCrossHostRunPlan,
    repo_root: pathlib.Path = REPO_ROOT,
) -> int:
    executable = _label_to_host_executable(plan.target, repo_root)
    if not executable.is_file():
        print(
            f"ERROR: built executable not found for {plan.target}: {executable}",
            file=sys.stderr,
        )
        return 1
    if not os.access(executable, os.X_OK):
        print(
            f"ERROR: built executable is not executable for {plan.target}: {executable}",
            file=sys.stderr,
        )
        return 1

    _info(f"running macOS cross executable on host: {plan.target}")
    return subprocess.run(
        [str(executable), *plan.run_args],
        check=False,
        cwd=repo_root,
        env=_host_binary_env(executable),
    ).returncode


def _run_windows_cross_host_binary(
    plan: MacOSCrossHostRunPlan,
    repo_root: pathlib.Path = REPO_ROOT,
) -> int:
    bazel_bin_root = _symlink_target(repo_root / "bazel-bin") or repo_root / "bazel-bin"
    executable = _label_to_host_executable(
        plan.target, repo_root, bazel_bin_root, executable_suffix=".exe"
    )
    if not executable.is_file():
        print(
            f"ERROR: built executable not found for {plan.target}: {executable}",
            file=sys.stderr,
        )
        return 1

    _info(f"running Windows cross executable on host: {plan.target}")
    return subprocess.run(
        [str(executable), *plan.run_args],
        check=False,
        cwd=repo_root,
        env=_host_binary_env(executable),
    ).returncode


def _remove_path(path: pathlib.Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def _clean_directory_contents(path: pathlib.Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for child in path.iterdir():
        _remove_path(child)


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


def _hermetic_container_output_base(
    config: HermeticContainerConfig, docker_instance: object
) -> pathlib.Path:
    workspace_digest = getattr(docker_instance, "bazel_output_base_digest", "") or getattr(
        docker_instance, "workspace_hex_digest", ""
    )
    output_root = pathlib.Path(config.bazel_user_output_root)
    return output_root / workspace_digest if workspace_digest else output_root


def _symlink_target(link: pathlib.Path) -> pathlib.Path | None:
    if not link.is_symlink():
        return None
    target = pathlib.Path(os.readlink(link))
    if not target.is_absolute():
        target = link.parent / target
    return target


def _workspace_convenience_symlink_name(repo_root: pathlib.Path) -> str:
    return f"{HERMETIC_CONTAINER_SYMLINK_PREFIX}{repo_root.name}"


def _managed_convenience_symlink_targets(
    repo_root: pathlib.Path = REPO_ROOT,
) -> dict[str, str]:
    convenience_dir = repo_root / pathlib.Path(HERMETIC_CONTAINER_SYMLINK_PREFIX).parent
    targets: dict[str, str] = {}
    names = (
        *HERMETIC_CONTAINER_ROOT_CONVENIENCE_SYMLINKS,
        _workspace_convenience_symlink_name(repo_root),
    )
    for name in names:
        target = _symlink_target(convenience_dir / name)
        if target is not None:
            targets[name] = str(target)
    return targets


def _hermetic_container_convenience_symlink_targets(
    config: HermeticContainerConfig,
    docker_instance: object,
    repo_root: pathlib.Path = REPO_ROOT,
) -> dict[str, str]:
    output_base = _hermetic_container_output_base(config, docker_instance).resolve()
    execroot = output_base / "execroot" / "_main"
    targets = {
        name: target
        for name, target in _managed_convenience_symlink_targets(repo_root).items()
        if _is_relative_to(pathlib.Path(target).resolve(), output_base)
    }
    targets.setdefault("bazel-out", str(execroot / "bazel-out"))

    bazel_bin = pathlib.Path(targets.get("bazel-bin", ""))
    if "bazel-testlogs" not in targets and bazel_bin.name == "bin":
        targets["bazel-testlogs"] = str(bazel_bin.parent / "testlogs")

    targets.setdefault(_workspace_convenience_symlink_name(repo_root), str(execroot))
    return targets


def _write_hermetic_container_convenience_symlink_marker(
    links: Mapping[str, str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    marker = env.get(HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV)
    if not marker:
        return

    marker_path = pathlib.Path(marker)
    data = {
        "repo_root": str(repo_root),
        "links": dict(links),
    }
    marker_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_marker = marker_path.with_suffix(marker_path.suffix + ".tmp")
    tmp_marker.write_text(json.dumps(data, sort_keys=True), encoding="utf-8")
    tmp_marker.replace(marker_path)


def _replace_symlink(link: pathlib.Path, target: pathlib.Path) -> None:
    if link.is_symlink() or link.is_file():
        link.unlink()
    elif link.exists():
        _info(f"not replacing non-symlink convenience path: {link}")
        return
    link.symlink_to(target, target_is_directory=True)


def _publish_convenience_symlinks(
    links: Mapping[str, str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    for name, target in links.items():
        _replace_symlink(repo_root / name, pathlib.Path(target))
    _write_hermetic_container_convenience_symlink_marker(links, env, repo_root)


def _publish_hermetic_container_convenience_symlinks(
    config: HermeticContainerConfig,
    docker_instance: object,
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    _publish_convenience_symlinks(
        _hermetic_container_convenience_symlink_targets(config, docker_instance, repo_root),
        env,
        repo_root,
    )


def _publish_linux_host_convenience_symlinks(
    env: Mapping[str, str], repo_root: pathlib.Path = REPO_ROOT
) -> None:
    _publish_convenience_symlinks(
        _managed_convenience_symlink_targets(repo_root),
        env,
        repo_root,
    )


def _publish_linux_shared_install_symlink(
    config: Mapping[str, str], repo_root: pathlib.Path = REPO_ROOT
) -> None:
    target_value = config.get("shared_install_dir")
    bazel_bin = repo_root / "bazel-bin"
    bazel_bin_target = _symlink_target(bazel_bin)
    if (
        not target_value
        or bazel_bin_target is None
        or not bazel_bin_target.is_dir()
        or bazel_bin_target.name != "bin"
        or bazel_bin_target.parent.parent.name != "bazel-out"
    ):
        return

    # Bazel-bin points at .../bazel-out/<configuration>/bin. The shared install root is
    # writable because the output base is mounted read-only in the action container; keep the
    # same configuration boundary that Bazel normally provides under bazel-out.
    shared_install_dir = pathlib.Path(target_value) / bazel_bin_target.parent.name
    shared_install_dir.mkdir(parents=True, exist_ok=True)

    link = bazel_bin / "install"
    if link.is_symlink() or link.is_file():
        link.unlink()
    elif link.is_dir():
        # This is generated output from the legacy unsandboxed install action.
        shutil.rmtree(link)
    link.symlink_to(shared_install_dir, target_is_directory=True)


def _publish_macos_shared_install_symlink(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> None:
    """Publish bazel-bin/install after Darwin actions use an external shared tree."""
    _publish_linux_shared_install_symlink(
        {"shared_install_dir": str(_macos_shared_install_dir(_bazel_output_base(args, env)))},
        repo_root=repo_root,
    )


def restore_hermetic_container_convenience_symlinks_from_env(
    env: Mapping[str, str] = os.environ,
) -> None:
    marker = env.get(HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV)
    if not marker:
        return

    marker_path = pathlib.Path(marker)
    try:
        data = json.loads(marker_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return

    repo_root_value = data.get("repo_root", "")
    if not isinstance(repo_root_value, str) or not repo_root_value:
        return
    repo_root = pathlib.Path(repo_root_value)

    links = data.get("links", {})
    if not isinstance(links, dict):
        return

    for name, target in links.items():
        if not isinstance(name, str) or not isinstance(target, str):
            continue
        _replace_symlink(repo_root / name, pathlib.Path(target))


@contextlib.contextmanager
def _temporary_hermetic_container_engflow_bazelrc(
    credential_helper: str,
    repo_root: pathlib.Path = REPO_ROOT,
    system: str | None = None,
) -> Iterator[str | None]:
    if not credential_helper:
        yield None
        return

    state_dir = _hermetic_container_state_dir(repo_root)
    state_dir.mkdir(parents=True, exist_ok=True)
    descriptor, bazelrc_name = tempfile.mkstemp(
        dir=state_dir,
        prefix=".engflow_credentials.",
        suffix=".bazelrc",
        text=True,
    )
    bazelrc = pathlib.Path(bazelrc_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as file:
            file.write(f"common --credential_helper={ENGFLOW_AUTH_CLUSTER}={credential_helper}\n")
        yield _container_path(bazelrc, system)
    finally:
        bazelrc.unlink(missing_ok=True)


def _bazel_args_with_hermetic_container_env(
    args: Sequence[str],
    env: Mapping[str, str],
    credential_helper: str = "",
    workspace_status_command: str = "",
) -> list[str]:
    if not args:
        return []

    command_index = _bazel_command_index(args)
    if command_index is None:
        return list(args)
    command = _bazel_command(args)
    if command not in {"aquery", "build", "coverage", "cquery", "fetch", "run", "test"}:
        return list(args)

    injected_options = []
    if "PATH" not in _repo_env_overrides_from_args(args):
        injected_options.append(f"--repo_env=PATH={HERMETIC_CONTAINER_REPOSITORY_PATH}")
    if credential_helper and not _credential_helper_requested(args, ENGFLOW_AUTH_CLUSTER):
        injected_options.append(f"--credential_helper={ENGFLOW_AUTH_CLUSTER}={credential_helper}")
    if workspace_status_command and not _workspace_status_command_requested(args):
        injected_options.append(f"--workspace_status_command={workspace_status_command}")
    cert_env = _container_cert_env_values(env)
    injected_options.extend(
        f"--action_env={var_name}={value}" for var_name, value in cert_env.items()
    )
    if command in {"coverage", "test"}:
        injected_options.extend(
            f"--test_env={var_name}={value}" for var_name, value in cert_env.items()
        )

    if _windows_cross_config_requested(args, env=env) or _macos_cross_config_requested(
        args, env=env
    ):
        if not any(arg.startswith("--remote_download_outputs=") for arg in args):
            injected_options.append("--remote_download_outputs=toplevel")
    if not injected_options:
        return list(args)
    return [*args[: command_index + 1], *injected_options, *args[command_index + 1 :]]


def _bazel_args_with_container_bes_keyword(args: Sequence[str], containerized: bool) -> list[str]:
    """Publish a build event service keyword recording how local build tools execute.

    Telemetry aggregates this keyword to measure how often builds fall back to native,
    non-containerized execution.
    """
    value = "true" if containerized else "false"
    return _append_bazel_command_options(
        args,
        [f"--bes_keywords={CONTAINERIZED_BES_KEYWORD}={value}"],
    )


def _run_linux_native_fallback(bazel_real: str, args: Sequence[str], env: Mapping[str, str]) -> int:
    """Run Bazel natively after the Linux build container services were unusable."""
    native_args = _bazel_args_with_container_bes_keyword(
        _bazel_args_with_native_install_strategy(args),
        containerized=False,
    )
    rc = _run_direct(bazel_real, native_args)
    if _bazel_command(args) in {"build", "coverage", "run", "test"}:
        _publish_linux_shared_install_symlink(
            {
                "shared_install_dir": str(
                    _linux_native_shared_install_dir(_bazel_output_base(args, env))
                )
            }
        )
    return rc


def run_hermetic_container(
    bazel_real: str, args: Sequence[str], env: Mapping[str, str] = os.environ
) -> int:
    system = platform.system()
    mode = select_integration_mode(env=env, system=system, args=args, repo_root=REPO_ROOT)
    if mode == IntegrationMode.DIRECT:
        direct_args = _bazel_args_with_native_install_strategy(args)
        rc = _run_direct(bazel_real, direct_args)
        if system == "Darwin" and _bazel_command(args) in {"build", "coverage", "run", "test"}:
            _publish_macos_shared_install_symlink(args, env)
        if system == "Linux" and _bazel_command(args) in {"build", "coverage", "run", "test"}:
            _publish_linux_shared_install_symlink(
                {
                    "shared_install_dir": str(
                        _linux_native_shared_install_dir(_bazel_output_base(args, env))
                    )
                }
            )
        if rc == 0 and system == "Linux" and _bazel_command(args) == "clean":
            _remove_path(_linux_native_shared_install_dir(_bazel_output_base(args, env)))
            _remove_path(_linux_shared_install_dir(_bazel_output_base(args, env)))
        if rc == 0 and system == "Darwin" and _bazel_command(args) == "clean":
            _remove_path(_macos_shared_install_dir(_bazel_output_base(args, env)))
        return rc

    args = _bazel_args_with_default_macos_cross_config(args, env=env, system=system)
    args = _bazel_args_with_default_windows_cross_config(args, env=env, system=system)
    if mode in {IntegrationMode.FULL_CONTAINER, IntegrationMode.LINUX_HOST_CONTAINER}:
        args = _bazel_args_with_hermetic_container_symlink_prefix(args)

    if mode == IntegrationMode.LINUX_CROSS_HOST_RBE:
        host_args = _linux_cross_rbe_host_args(args, env, repo_root=REPO_ROOT)
        if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
            print(
                json.dumps(
                    {
                        "linux_cross_host_rbe": {
                            "args": host_args,
                            "config": dataclasses.asdict(
                                _linux_cross_rbe_config(args, env=env, repo_root=REPO_ROOT)
                            ),
                        },
                    },
                    sort_keys=True,
                )
            )
            return 0

        _info("running Linux s390x/ppc64le cross compile/link actions on RBE through host Bazel")
        return _run_direct(bazel_real, host_args)

    if mode == IntegrationMode.LINUX_HOST_CONTAINER:
        if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
            host_args = _linux_host_container_action_args(args, env)
            host_args = _replace_bazel_startup_option(
                host_args,
                "--output_base",
                str(_bazel_output_base(args, env)),
            )
            print(
                json.dumps(
                    {
                        "linux_host_container": {
                            "args": host_args,
                            "config": _linux_host_container_config(env),
                        },
                    },
                    sort_keys=True,
                )
            )
            return 0

        if _bazel_command(args) in LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS:
            # These commands only analyze the graph or fetch repositories. Keep their
            # original Bazel options: action-only options are rejected by query-family
            # commands, and no container runtime or action sandbox is needed here.
            host_args = _linux_host_container_action_args(args, env)
            rc = _run_direct(bazel_real, host_args)
            if f"--symlink_prefix={HERMETIC_CONTAINER_SYMLINK_PREFIX}" in host_args:
                _publish_linux_host_convenience_symlinks(env)
            return rc

        container_command, runtime_detail = _select_linux_container_runtime(env)
        if container_command is None:
            detail = f" ({runtime_detail})" if runtime_detail else ""
            _warn_native_fallback(f"no usable Linux container runtime was found{detail}")
            return _run_linux_native_fallback(bazel_real, args, env)

        container_config = _linux_host_container_config(
            env,
            container_command=container_command,
        )
        if not _ensure_linux_container_image(container_command, container_config["image"]):
            _warn_native_fallback(
                f"the build container image {container_config['image']} could not be pulled"
            )
            return _run_linux_native_fallback(bazel_real, args, env)

        host_args = _linux_host_container_action_args(args, env)
        output_base = _bazel_output_base(args, env)
        # Publish the atomic action config and establish the container mount under the
        # output-base lock. The config and container are then stable for this invocation;
        # do not hold the lock while Bazel runs or nested Bazel invocations can deadlock.
        with _linux_container_actions_lock(output_base):
            config_path, container_config = _write_linux_container_actions_config_unlocked(
                args,
                {**env, "HERMETIC_CONTAINER_DOCKER_COMMAND": container_command},
            )
            host_args = _replace_bazel_startup_option(
                host_args,
                "--output_base",
                str(config_path.parent),
            )
            container_ready, container_detail = _ensure_linux_action_container(config_path)
            if not container_ready:
                detail_suffix = f" ({container_detail})" if container_detail else ""
                _warn_native_fallback(
                    f"the build container {container_config['container_name']} could not "
                    f"be started or reused{detail_suffix}"
                )
        if not container_ready:
            # Run the fallback after the output-base lock is released so nested Bazel
            # invocations during the native build cannot deadlock on the lock.
            return _run_linux_native_fallback(bazel_real, args, env)
        image_identifier = container_config["image"].rsplit("@", 1)[-1]
        _info(
            f"Container image {image_identifier} is running in the background "
            "to execute hermetic build actions."
        )
        remote_execution_disabled = _remote_execution_disabled_by_args(
            args, env=env, repo_root=REPO_ROOT
        )
        if remote_execution_disabled:
            _info("Remote execution is disabled; build tool actions use this local container.")
        elif not _env_is_false(env.get(LINUX_DYNAMIC_SCHEDULING_ENV)):
            _info("C++ and Rust compiler actions using dynamic scheduling.")
        else:
            _info(
                "Local link/archive actions use this container; " "compiler actions execute on RBE."
            )
        rc = _run_direct(
            bazel_real, _bazel_args_with_container_bes_keyword(host_args, containerized=True)
        )
        _publish_linux_shared_install_symlink(container_config)
        if f"--symlink_prefix={HERMETIC_CONTAINER_SYMLINK_PREFIX}" in host_args:
            _publish_linux_host_convenience_symlinks(env)
        return rc

    if mode == IntegrationMode.MACOS_CROSS_HOST:
        if _macos_cross_host_bazel_test_requested(args, env, system):
            host_args = _macos_cross_host_bazel_test_args(args, env)
            if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
                print(
                    json.dumps(
                        {
                            "macos_cross_host_bazel_test": {
                                "args": host_args,
                            },
                        },
                        sort_keys=True,
                    )
                )
                return 0

            _info("running macOS cross test through host Bazel with local test execution")
            rc = _run_direct(bazel_real, host_args)
            _publish_macos_shared_install_symlink(host_args, env)
            return rc

        if _macos_cross_local_container_action_requested(args, env, system):
            if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
                host_args = _macos_cross_local_container_action_args(args, env)
                print(
                    json.dumps(
                        {
                            "macos_cross_local_container_actions": {
                                "args": host_args,
                            },
                        },
                        sort_keys=True,
                    )
                )
                return 0

            remote_execution_disabled = _macos_cross_remote_execution_disabled(args, env)
            needs_local_container = remote_execution_disabled or not _remote_link_requested(
                args, env=env, repo_root=REPO_ROOT
            )
            if needs_local_container:
                _prepare_hermetic_container_process_env(env)
                docker_command = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND", "docker")
                _info("checking Docker daemon for macOS cross local container actions")
                docker_ready, docker_detail = _docker_daemon_status(docker_command)
                if not docker_ready:
                    _print_docker_daemon_error(docker_command, docker_detail, system)
                    return 1

            _info("preparing macOS cross build action routing")
            host_args = _macos_cross_local_container_action_args(args, env)
            _write_cross_host_action_config(host_args, env, system)
            if remote_execution_disabled:
                _info("running macOS cross build actions through a local container wrapper")
            elif _remote_link_requested(args, env=env, repo_root=REPO_ROOT):
                _info("running macOS cross build actions on RBE")
            else:
                _info("running macOS cross compile/IDL on RBE and link/archive actions locally")
            rc = _run_direct(bazel_real, host_args)
            _publish_macos_shared_install_symlink(host_args, env)
            return rc

        if _macos_cross_host_run_requested(args, env, system):
            host_run_plan = _macos_cross_host_run_plan(args)
            host_args = _macos_cross_local_container_action_args(host_run_plan.build_args, env)
            if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
                print(
                    json.dumps(
                        {
                            "macos_cross_host_run": dataclasses.asdict(
                                dataclasses.replace(host_run_plan, build_args=host_args)
                            )
                        },
                        sort_keys=True,
                    )
                )
                return 0
            _info("building macOS cross target through host Bazel")
            _write_cross_host_action_config(host_args, env, system)
            build_result = _run_direct(bazel_real, host_args)
            if build_result:
                return build_result
            _publish_macos_shared_install_symlink(host_args, env)
            return _run_macos_cross_host_binary(host_run_plan)

        return _run_direct(bazel_real, args)

    if mode == IntegrationMode.WINDOWS_CROSS_HOST:
        env = _prepare_windows_cross_env(args, env, REPO_ROOT, system)
        if _windows_cross_host_wrapper_action_requested(args, env, system):
            if _bazel_command(args) == "run":
                windows_host_run_plan = _windows_cross_host_run_plan(args)
                host_build_args = _windows_cross_host_wrapper_action_args(
                    windows_host_run_plan.build_args,
                    env,
                )
                windows_host_run_plan = dataclasses.replace(
                    windows_host_run_plan,
                    build_args=host_build_args,
                )
                if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
                    print(
                        json.dumps(
                            {
                                "windows_cross_host_wrapper_run": dataclasses.asdict(
                                    windows_host_run_plan
                                )
                            },
                            sort_keys=True,
                        )
                    )
                    return 0

                _info("building Windows cross target through host Bazel with RBE actions")
                _write_cross_host_action_config(host_build_args, env, system)
                build_result = _run_direct(bazel_real, host_build_args)
                if build_result:
                    return build_result
                return _run_windows_cross_host_binary(windows_host_run_plan)

            host_args = _windows_cross_host_wrapper_action_args(args, env)
            if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
                print(
                    json.dumps(
                        {
                            "windows_cross_host_wrapper_actions": {
                                "args": host_args,
                            },
                        },
                        sort_keys=True,
                    )
                )
                return 0

            _info("running Windows cross compile/IDL/link actions on RBE through host Bazel")
            _write_cross_host_action_config(host_args, env, system)
            return _run_direct(bazel_real, host_args)

        return _run_direct(bazel_real, args)

    env = _prepare_windows_cross_env(args, env, REPO_ROOT, system)

    docker_command = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND", "docker")
    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") != "1":
        _prepare_hermetic_container_process_env(env)
        docker_ready, docker_detail = _docker_daemon_status(docker_command)
        if not docker_ready:
            _print_docker_daemon_error(docker_command, docker_detail, system)
            return 1

    config = build_hermetic_container_config(
        bazel_real,
        env=env,
        system=system,
        docker_command=(
            None if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1" else docker_command
        ),
    )
    _info(
        f"running hermetic_container with {config.distro} container {config.docker_image.full_name}"
    )
    macos_host_test_plan = (
        _macos_cross_host_test_plan(args, env)
        if _macos_cross_host_test_requested(args, env, system)
        else None
    )
    macos_host_run_plan = (
        _macos_cross_host_run_plan(args)
        if _macos_cross_host_run_requested(args, env, system)
        else None
    )
    windows_host_run_plan = (
        _windows_cross_host_run_plan(args)
        if _windows_cross_host_run_requested(args, env, system)
        else None
    )

    if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
        if (
            macos_host_test_plan is not None
            or macos_host_run_plan is not None
            or windows_host_run_plan is not None
        ):
            print(
                json.dumps(
                    {
                        "hermetic_container_config": dataclasses.asdict(config),
                        **(
                            {"macos_cross_host_test": dataclasses.asdict(macos_host_test_plan)}
                            if macos_host_test_plan is not None
                            else {}
                        ),
                        **(
                            {"macos_cross_host_run": dataclasses.asdict(macos_host_run_plan)}
                            if macos_host_run_plan is not None
                            else {}
                        ),
                        **(
                            {"windows_cross_host_run": dataclasses.asdict(windows_host_run_plan)}
                            if windows_host_run_plan is not None
                            else {}
                        ),
                    },
                    sort_keys=True,
                )
            )
        else:
            print(json.dumps(dataclasses.asdict(config), sort_keys=True))
        return 0

    hermetic_container = _load_hermetic_container_module()
    docker_instance = hermetic_container.DockerInstance(
        instance_name=config.instance_name,
        image_name=config.docker_image.image_name,
        run_command="/bin/bash",
        docker_command=docker_command,
        dockerfile=config.dockerfile,
        repository=config.docker_image.repository,
        directory=str(REPO_ROOT),
        command=config.bazel_command,
        volumes=config.volumes,
        ports=env.get("HERMETIC_CONTAINER_PORTS", ""),
        env_vars=config.env_vars,
        gpus=env.get("HERMETIC_CONTAINER_GPUS", ""),
        platform=config.platform,
        shm_size=env.get("HERMETIC_CONTAINER_SHM_SIZE", ""),
        network=env.get("HERMETIC_CONTAINER_NETWORK", "host"),
        run_deps=[],
        docker_compose_file="",
        docker_compose_command=env.get(
            "HERMETIC_CONTAINER_DOCKER_COMPOSE_COMMAND", "docker-compose"
        ),
        docker_compose_project_name="mongo_hermetic_container",
        docker_compose_services="",
        bazel_user_output_root=config.bazel_user_output_root,
        bazel_rc_file=env.get("HERMETIC_CONTAINER_BAZEL_RC_FILE", ""),
        docker_run_privileged=config.privileged,
        docker_machine=env.get("HERMETIC_CONTAINER_DOCKER_MACHINE"),
        hermetic_container_run_file=config.hermetic_container_run_file,
        workspace_hex=True,
        delegated_volume=(
            not _use_wsl_docker(env)
            and not _env_is_false(env.get("HERMETIC_CONTAINER_DELEGATED_VOLUME"))
        ),
        user=config.user,
        docker_build_args=env.get("HERMETIC_CONTAINER_DOCKER_BUILD_ARGS", ""),
    )

    run_file = pathlib.Path(docker_instance.hermetic_container_run_file)
    if _bazel_command(args) == "clean":
        with _hermetic_container_lock(run_file):
            rc = _clean_host_outputs(bazel_real, args)
            if rc:
                return rc
            if _clean_requested_expunge(args):
                return _expunge_hermetic_container_outputs(docker_instance, config)
            return _clean_hermetic_container_outputs(docker_instance, config)

    fingerprint = _config_fingerprint(config)
    with _hermetic_container_lock(run_file):
        if not _run_file_matches_config(run_file, fingerprint) or not docker_instance.is_running():
            rc = docker_instance.start()
            if rc:
                return rc
            run_file.write_text(f"{fingerprint}\n", encoding="utf-8")

        if macos_host_test_plan is not None:
            hermetic_container_args = macos_host_test_plan.build_args
        elif macos_host_run_plan is not None:
            hermetic_container_args = macos_host_run_plan.build_args
        elif windows_host_run_plan is not None:
            hermetic_container_args = windows_host_run_plan.build_args
        else:
            hermetic_container_args = list(args)
        with _temporary_hermetic_container_engflow_bazelrc(
            config.credential_helper, system=system
        ) as bazelrc_file:
            bazel_args = _bazel_args_with_hermetic_container_env(
                hermetic_container_args,
                env,
                credential_helper=config.credential_helper,
                workspace_status_command=(
                    HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND
                    if system in {"Darwin", "Windows"}
                    else ""
                ),
            )
            if bazelrc_file:
                rc = docker_instance.send_command(bazel_args, bazel_rc_file=bazelrc_file)
            else:
                rc = docker_instance.send_command(bazel_args)
        if rc:
            return rc

        if f"--symlink_prefix={HERMETIC_CONTAINER_SYMLINK_PREFIX}" in hermetic_container_args:
            _publish_hermetic_container_convenience_symlinks(config, docker_instance, env)

    if macos_host_test_plan is not None:
        if not macos_host_test_plan.run_host_tests:
            _info("skipping macOS cross host test execution because --nobuild was requested")
            return 0
        return _run_macos_cross_host_tests(bazel_real, macos_host_test_plan, env)
    if macos_host_run_plan is not None:
        return _run_macos_cross_host_binary(macos_host_run_plan)
    if windows_host_run_plan is not None:
        return _run_windows_cross_host_binary(windows_host_run_plan)

    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bazel_real")
    parser.add_argument("bazel_args", nargs=argparse.REMAINDER)
    parsed = parser.parse_args(argv)
    try:
        return run_hermetic_container(parsed.bazel_real, parsed.bazel_args)
    except KeyboardInterrupt:
        print("ERROR: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
