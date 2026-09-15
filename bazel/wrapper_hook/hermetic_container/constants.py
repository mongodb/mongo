"""Module-level constants shared across the hermetic container integration."""

from __future__ import annotations

import pathlib
import re

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent.parent


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


LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS = frozenset(["aquery", "cquery", "fetch", "query"])


HERMETIC_CONTAINER_GIT_LAYER_ENV = "MONGO_HERMETIC_CONTAINER_GIT_LAYER"


HERMETIC_CONTAINER_REPOSITORY_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND = "/usr/bin/python3 bazel/workspace_status.py"


HERMETIC_CONTAINER_OUTPUT_ROOT_VERSION = "v2"


HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT_ENV = (
    "MONGO_HERMETIC_CONTAINER_MACOS_CASE_SENSITIVE_OUTPUT"
)


HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE_ENV = "MONGO_HERMETIC_CONTAINER_MACOS_OUTPUT_IMAGE_SIZE"


# Evergreen appends this exact sequence to release invocations and provenance validation
# requires it to remain the final command-line suffix. Cross-policy options are inserted
# immediately before it so they still override the selected config without weakening the
# release guard.
RELEASE_LOCAL_SAFETY_SUFFIX = (
    "--remote_executor=",
    "--noremote_accept_cached",
    "--remote_upload_local_results=false",
    "--modify_execution_info=.*=+no-cache",
)


MACOS_CROSS_DEFAULT_CONFIG_ENV = "MONGO_MACOS_CROSS_DEFAULT_CONFIG"


DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS = 60


HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS_ENV = "MONGO_HERMETIC_CONTAINER_CONVENIENCE_SYMLINKS"


HERMETIC_CONTAINER_SYMLINK_PREFIX = "bazel-"


HERMETIC_CONTAINER_ROOT_CONVENIENCE_SYMLINKS = (
    "bazel-bin",
    "bazel-out",
    "bazel-testlogs",
)


HERMETIC_CONTAINER_OUTPUT_BASE_PRESERVED_DIRS = frozenset(["action_cache", "execroot", "external"])


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
