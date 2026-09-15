"""Linux cross-compilation: RBE args, env, and host container actions."""

from __future__ import annotations

import contextlib
import dataclasses
import errno
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid
from collections.abc import Mapping, Sequence

from ..bazelrc import (
    _bazel_output_base,
    _effective_config_values,
    _remote_execution_disabled_by_args,
    _remote_link_requested,
)
from ..constants import (
    DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
    HERMETIC_CONTAINER_DISABLED_VALUES,
    LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS,
    REPO_ROOT,
)
from ..distro import (
    _is_digest_pinned_container_url,
    _linux_host_container_distro,
    _safe_name,
    load_remote_execution_containers,
    normalize_arch,
)
from ..env import (
    _append_bazel_command_options,
    _append_bazel_command_options_before_release_suffix,
    _append_bazel_command_options_last,
    _bazel_command,
    _env_is_false,
    _env_is_true,
    _info,
)
from ..fsutil import (
    _container_user,
    _linux_action_sandbox_base,
    _linux_native_shared_install_dir,
    _linux_shared_install_dir,
    _run_container_network_command,
    _sha256_file,
)
from ..podman import (
    PODMAN_AUTH_FILE_ENV,
    PODMAN_CONFIG_ENV,
    PODMAN_REPOSITORY_ENV_VARS,
    PODMAN_RUNTIME_DIR_PREFIX,
    PODMAN_TASK_ID_ENV,
    _compact_runtime_detail,
    _container_runtime_env,
    _ensure_owned_podman_directory,
    _is_podman_command,
    _podman_anonymous_runtime_env,
    _podman_auth_file,
    _podman_authentication_failure,
    _podman_containers_config,
    _podman_storage_config,
    _podman_task_root,
    _print_container_command_output,
)
from ..podman_common import (
    PODMAN_REQUIRED_ENV,
)
from ..podman_common import (
    is_podman_docker_shim as _is_podman_docker_shim,
)
from .common import _macos_cross_linux_python_options

LINUX_CONTAINER_ACTIONS_ENV = "MONGO_LINUX_CONTAINER_ACTIONS"


LINUX_DYNAMIC_SCHEDULING_ENV = "MONGO_LINUX_DYNAMIC_SCHEDULING"


NATIVE_TOOLCHAIN_CONFIG = "native_toolchain"


LINUX_DYNAMIC_LOCAL_LOAD_FACTOR = "0.125"


LINUX_CONTAINER_ACTIONS_CONFIG_FILENAME = "mongo_linux_container_actions.json"


LINUX_CONTAINER_ACTIONS_LOCK_FILENAME = "mongo_linux_container_actions.lock"


LINUX_CONTAINER_ACTIONS_GENERATION_FILENAME = "mongo_linux_output_base_generation"


# Keep this in lockstep with the action wrapper.  The layout version is included
# in the container identity and invalidates containers created with an older mount
# topology when the wrapper changes its bind mounts. Bazel 9 mounts the repo
# contents cache and install base (siblings of the output base), so the wrapper
# bumped this to v7.
LINUX_CONTAINER_ACTIONS_LAYOUT_VERSION = "v7"


LINUX_CONTAINER_ACTION_WRAPPER_SCRIPT = (
    REPO_ROOT / "bazel" / "toolchains" / "cc" / "mongo_linux" / "linux_container_action_wrapper.py"
)


LINUX_DYNAMIC_CONTAINER_MNEMONICS = ("CppCompile", "Rustc", "RustcMetadata")


LINUX_LOCAL_CONTAINER_MNEMONICS = ("HistoricRuntime",)


LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS = (
    "CppLink",
    "CppArchive",
    "CppArchiveDist",
    "SolibSymlink",
    "ExtractDebugInfo",
    "StripDebugInfo",
    "ObjcopyEmbedData",
    "CcGenerateIntermediateDwp",
    "CcGenerateDwp",
    "GdbGenerateIndex",
    "GdbApplyIndex",
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
    "ExtractCargoTomlEnvVars",
    "ExtractCertificateGenerationYear",
    "GenProto",
    "GenProtoDescriptorSet",
    "Genrule",
    "EmbedEditionDefaults",
    "IdlcGenerator",
    "MdBookBuild",
    "MongoPrettyPrinterTestCreation",
    "ProstGenProto",
    "PyCompile",
    "PyO3StubGen",
    "PyWriteBuildData",
    "ProtoCompile",
    "ProtocInvocation",
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
    "UpbAmalgamation",
    "WasmAotCompile",
    "WheelInstall",
    "WitBindgenC",
)


LINUX_LOCAL_TEST_MNEMONICS = ("CoverageReport", "TestRunner")


LINUX_LOCAL_JAVA_MNEMONICS = ("Javac", "JavaToolchainCompileBootClasspath", "Turbine")


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


LINUX_CROSS_RBE_CONFIG_RE = re.compile(
    r"^linux-(?P<target_arch>ppc64le|s390x)"
    r"(?:-(?P<target_distro>rhel8|rhel9|rhel10))?"
    r"-cross-rbe"
    r"(?:-(?P<exec_arch>x86_64|arm64|aarch64))?$"
)


LINUX_CROSS_RBE_DEFAULT_TARGET_DISTRO = "rhel9"


LINUX_CROSS_RBE_DEFAULT_EXEC_DISTRO = "rhel9"


LINUX_CROSS_RBE_DEFAULT_EXEC_ARCH = "x86_64"


LINUX_CROSS_RBE_REMOTE_EXECUTOR = "grpcs://sodalite.cluster.engflow.com"


LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV = "MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE"


LINUX_CROSS_RBE_POOL_ENV = "MONGO_LINUX_CROSS_RBE_POOL"


LINUX_CROSS_TOOLCHAIN_ENV = "MONGO_LINUX_CROSS_TOOLCHAIN"


WASI_SDK_EXEC_ARCH_ENV = "MONGO_WASI_SDK_EXEC_ARCH"


# These actions consume the execution-platform toolchain or are themselves compiler and
# code-generation actions. They stay remote for IBM cross builds because their binaries
# are supplied by the x86_64/aarch64 cross archive and cannot execute in an IBM host
# container. Keep packaging, release/provenance output, and test execution in the local
# list below. Signing and public-key generation are also local: they are release/provenance
# actions and their helpers must use the host-native IBM toolchain.
# PyWriteBuildData is intentionally omitted: a target's no-remote tag
# propagates to all of its actions, and the no-remote rules_pkg host tar helper
# py_binary carries its own PyWriteBuildData action, which a remote-only
# strategy would reject. Keeping it local also matches the compile-only policy:
# build data writes are cheap local metadata generation.
# WheelBuild is included even though it is not in the general container-tool list:
# rules_pycross invokes an execution-platform Python to build target-platform wheels.
# Running that action in the local IBM host container can resolve the target Python
# for both roles and produces an Exec format error; the cross execution platform
# supplies the native execution interpreter while retaining the IBM target runtime.
# ProtoCompile and ProtocInvocation cover third-party rules that call protoc
# directly instead of using GenProto/GenProtoDescriptorSet.
# CppLTOIndexing is intentionally not remote: Bazel resolves the LTO indexing
# tool from the cpp-link-* action configs, so in composed cross toolchains it
# runs the host-native linker driver just like CppLink and must stay in the
# native IBM container. DTLTO's CcLtoBackendCompile uses the
# execution-architecture compiler and stays remote.
LINUX_CROSS_REMOTE_COMPILE_MNEMONICS = (
    "CcLtoBackendCompile",
    "CargoBuildScriptRun",
    "CargoLints",
    "Clippy",
    "ConfigHeaderGen",
    "ExtractCargoTomlEnvVars",
    "GenProto",
    "GenProtoDescriptorSet",
    "Genrule",
    "EmbedEditionDefaults",
    "IdlcGenerator",
    "ProstGenProto",
    "PyCompile",
    "PyO3StubGen",
    "PycrossTargetEnvironment",
    "ProtoCompile",
    "ProtocInvocation",
    "RustBindgen",
    "RustProtocGen",
    "RustUnpretty",
    "RustWasmBindgen",
    "Rustdoc",
    "RustdocTestWriter",
    "RustdocZip",
    "Rustfmt",
    "TemplateRenderer",
    "UpbAmalgamation",
    "WheelBuild",
    "WitBindgenC",
    "WheelInstall",
)


# WasmAotCompile uses the target-built s390x Wasmtime CLI and runs natively.
# Bindgen actions remain remote because their output is architecture-independent.
LINUX_CROSS_LOCAL_TOOL_MNEMONICS = tuple(
    dict.fromkeys(
        (
            *(
                mnemonic
                for mnemonic in LINUX_CONTAINER_TOOL_MNEMONICS
                if mnemonic not in LINUX_CROSS_REMOTE_COMPILE_MNEMONICS
            ),
            # These actions intentionally do not belong to the general tool list because
            # they are release-only, but they still need the native persistent container
            # in IBM cross mode.
            "EmbedPublicKeyHeader",
            "GpgExportArmored",
            "GpgSign",
        )
    )
)


LINUX_CROSS_LOCAL_RELEASE_ENV_VARS = (
    LINUX_CROSS_TOOLCHAIN_ENV,
    WASI_SDK_EXEC_ARCH_ENV,
    "MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON",
    "MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH",
    LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV,
    LINUX_CROSS_RBE_POOL_ENV,
)


# The vendored protobuf and gRPC trees are source distributions and must not carry
# Mongo-specific execution-platform changes. Cross-RBE builds use an output-base
# overlay instead (see ``_linux_cross_module_override_args``), while native and
# release-local builds continue to consume the untouched trees.
LINUX_CROSS_MODULE_OVERRIDES = (
    (
        "protobuf",
        pathlib.Path("src/third_party/protobuf/dist"),
        pathlib.Path("bazel/third_party/protobuf_cross_rbe.patch"),
    ),
    (
        "grpc",
        pathlib.Path("src/third_party/grpc/dist"),
        pathlib.Path("bazel/third_party/grpc_cross_rbe.patch"),
    ),
)


LINUX_CROSS_MODULE_OVERRIDE_DIRNAME = "mongo-linux-cross-module-overrides"


# Bzlmod gives repositories created by this extension a stable canonical prefix.
# The cross toolchain's target archive is kept below ``target/`` in the composed
# repository. Keep the prefixes here so native IBM test execution can find the
# target C++ runtime. Bazel 9 canonicalizes extension repositories as
# ``+<extension>+<repo>`` while Bazel 7 used ``_main~<extension>~<repo>``; emit
# both spellings so the dynamic loader finds the runtime either way (missing
# entries are harmless).
LINUX_CROSS_TOOLCHAIN_REPOSITORY_PREFIXES = (
    "+setup_mongo_linux_cross_toolchains_extension+mongo_linux_cross_toolchain_v5_",
    "_main~setup_mongo_linux_cross_toolchains_extension~mongo_linux_cross_toolchain_v5_",
)


LINUX_CROSS_RBE_X86_POOL = "x86_64"


LINUX_CROSS_RBE_ARM_POOL = "default"


@dataclasses.dataclass(frozen=True)
class LinuxCrossRBEConfig:
    """Linux target and execution platform selected by a cross-RBE config."""

    target_arch: str
    target_distro: str
    exec_arch: str
    exec_distro: str


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
    # Cross-only execution platforms carry a marker constraint used by
    # execution-tool rules (WheelBuild and protobuf) to avoid selecting the
    # native IBM host platform.  They retain the same container image and
    # execution properties as the ordinary RHEL platform.
    return f"//bazel/platforms:{config.exec_distro}_{exec_arch}_cross"


def _linux_cross_toolchain_selector(config: LinuxCrossRBEConfig) -> str:
    return (
        f"{config.target_distro}_{config.target_arch}"
        f"_on_{config.exec_distro}_{config.exec_arch}"
    )


def _linux_cross_rbe_process_env(
    config: LinuxCrossRBEConfig,
    env: Mapping[str, str],
) -> dict[str, str]:
    """Make cross execution selection visible during Bazel repository hydration.

    ``--repo_env`` is applied after Bazel has started.  A repository rule can be
    evaluated by the client/bootstrap phase first, however, and on IBM hosts
    that phase otherwise sees the host architecture (for example, s390x) while
    the action execution platform is amd64.  Export the same resolved values in
    the child Bazel process so both phases select the execution toolchain.
    """
    process_env = dict(os.environ)
    process_env.update(env)
    # Repository rules can hydrate before Bazel applies command-line
    # ``--repo_env`` values.  Keep their Podman invocations on the same
    # task-scoped storage/runtime instead of falling back to the host's shared
    # overlay store.  Do not, however, put Podman's temporary directory in the
    # Bazel client/server environment: local host actions inherit that
    # environment, and Bazel's linux-sandbox will try to bind-mount the path.
    # The path can disappear when Podman recovery/reset runs, which makes an
    # otherwise unrelated local action fail.  The complete set is still passed
    # with --repo_env by _linux_cross_rbe_host_args for repository rules, while
    # the action wrappers set it only on actual Podman subprocesses.
    podman_repo_env = _linux_cross_rbe_podman_repo_env(env)
    process_env.update(
        {
            name: podman_repo_env[name]
            for name in ("CONTAINERS_STORAGE_CONF", PODMAN_CONFIG_ENV, "XDG_RUNTIME_DIR")
            if name in podman_repo_env
        }
    )
    for name in ("TMPDIR", "TMP", "TEMP"):
        if podman_repo_env.get(name) == process_env.get(name):
            process_env.pop(name, None)
    process_env[LINUX_CROSS_TOOLCHAIN_ENV] = _linux_cross_toolchain_selector(config)
    process_env[WASI_SDK_EXEC_ARCH_ENV] = config.exec_arch
    return process_env


def _linux_cross_local_process_env(env: Mapping[str, str]) -> dict[str, str]:
    """Remove cross-execution selectors from a native IBM Bazel process.

    The cross repository and WASI rules read these values during repository hydration,
    before action-level command-line options are necessarily available.  Leaving the
    selectors in the child environment can therefore make a local release invocation
    hydrate an x86_64/aarch64 executable and later produce ``Exec format error`` on the
    IBM host.
    """
    process_env = dict(os.environ)
    process_env.update(env)
    for name in LINUX_CROSS_LOCAL_RELEASE_ENV_VARS:
        process_env.pop(name, None)
    return process_env


def _linux_cross_target_runtime_test_env(
    config: LinuxCrossRBEConfig,
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> str:
    """Return the target runtime search path for native IBM test execution.

    Cross test binaries are linked with the target GCC runtime from the composed
    cross-toolchain repository.  The IBM host's system libstdc++ can be older
    than that runtime, so standalone TestRunner actions must prefer the target
    archive.  Include both the composed ``target/`` layout and the optional
    complete-archive layout; nonexistent entries are harmless to the dynamic
    loader and keep the complete-archive override usable.
    """
    output_base = _bazel_output_base(args, env, repo_root=repo_root)
    selector = _linux_cross_toolchain_selector(config)
    runtime_directories: list[pathlib.Path] = []
    for prefix in LINUX_CROSS_TOOLCHAIN_REPOSITORY_PREFIXES:
        repository_root = output_base / "external" / (prefix + selector)
        runtime_directories.extend(
            (
                repository_root / "target/stow/gcc-v5/lib64",
                repository_root / "target/stow/gcc-v5/lib",
                repository_root / "target/v5/lib64",
                repository_root / "target/v5/lib",
                repository_root / "stow/gcc-v5/lib64",
                repository_root / "stow/gcc-v5/lib",
                repository_root / "v5/lib64",
                repository_root / "v5/lib",
            )
        )
    values = [str(path) for path in runtime_directories]
    inherited = env.get("LD_LIBRARY_PATH")
    if inherited:
        values.append(inherited)
    return os.pathsep.join(values)


def _linux_cross_rbe_podman_repo_env(env: Mapping[str, str]) -> dict[str, str]:
    """Return task-scoped Podman state for Bazel and sysroot repository rules."""
    # Evergreen exports a task id for every Linux task so cleanup can be scoped.
    # It is not evidence that this invocation selected Podman: honor the generic
    # container-action opt-out unless an IBM variant explicitly requires Podman.
    if env.get(
        LINUX_CONTAINER_ACTIONS_ENV, ""
    ).strip().lower() in HERMETIC_CONTAINER_DISABLED_VALUES and not _env_is_true(
        env.get(PODMAN_REQUIRED_ENV)
    ):
        return {}
    runtime_env = {name: env[name] for name in PODMAN_REPOSITORY_ENV_VARS if env.get(name)}
    if PODMAN_AUTH_FILE_ENV not in runtime_env:
        auth_file = _podman_auth_file(env)
        if auth_file is not None:
            runtime_env[PODMAN_AUTH_FILE_ENV] = str(auth_file)

    # An explicit storage configuration is authoritative. Otherwise only create
    # the standard task-scoped configuration when the repository rule will pick
    # Podman (directly, because it is required, or through the distro's Docker
    # compatibility shim).
    if "CONTAINERS_STORAGE_CONF" not in runtime_env:
        if not env.get(PODMAN_TASK_ID_ENV):
            return runtime_env

        docker_command = shutil.which("docker")
        podman_command = shutil.which("podman")
        podman_required = _env_is_true(env.get(PODMAN_REQUIRED_ENV))
        docker_is_podman = bool(
            not podman_required and docker_command and _is_podman_docker_shim(docker_command)
        )
        if podman_required and not podman_command:
            return runtime_env
        if not podman_required and not docker_is_podman and (docker_command or not podman_command):
            return runtime_env

        uid = os.getuid()
        runtime_dir = _podman_task_root(env) / f"{PODMAN_RUNTIME_DIR_PREFIX}{uid}"
        _ensure_owned_podman_directory(runtime_dir, uid)
        storage_config = _podman_storage_config(runtime_dir)
        containers_config = _podman_containers_config(runtime_dir)
        runtime_env.update(
            {
                "CONTAINERS_STORAGE_CONF": str(storage_config),
                PODMAN_CONFIG_ENV: str(containers_config),
                "XDG_RUNTIME_DIR": str(runtime_dir),
                "TMPDIR": str(runtime_dir),
                "TMP": str(runtime_dir),
                "TEMP": str(runtime_dir),
            }
        )

    return runtime_env


def _linux_cross_rbe_podman_repo_env_args(env: Mapping[str, str]) -> list[str]:
    """Forward the task-scoped Podman state to sysroot repository rules."""
    runtime_env = _linux_cross_rbe_podman_repo_env(env)
    return [
        f"--repo_env={name}={runtime_env[name]}"
        for name in PODMAN_REPOSITORY_ENV_VARS
        if name in runtime_env
    ]


def _linux_cross_local_container_env(
    env: Mapping[str, str], *, use_cross_image: bool = False
) -> dict[str, str]:
    """Return the container environment for local actions in a cross invocation.

    The cross execution image is an execution-platform image for RBE workers.  Local
    link, archive, debug, and test actions must run in the host-native IBM image; using
    the foreign image makes the local wrapper select x86/aarch64 binaries and produces
    Exec format errors.  ``use_cross_image`` is retained for callers that explicitly
    need the historical override, but normal IBM cross invocations opt out of it.
    """
    local_env = dict(env)
    if (
        use_cross_image
        and env.get(LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV)
        and not env.get("MONGO_HERMETIC_CONTAINER_IMAGE")
    ):
        local_env["MONGO_HERMETIC_CONTAINER_IMAGE"] = env[LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV]
    if not use_cross_image:
        local_env.pop(LINUX_CROSS_RBE_CONTAINER_IMAGE_ENV, None)
    return local_env


def _linux_cross_rbe_local_host_args(
    args: Sequence[str],
    config: LinuxCrossRBEConfig,
) -> list[str]:
    """Force an IBM release invocation onto the native hermetic toolchain.

    The selected ``linux-*-cross-rbe`` config remains in argv so the wrapper can discover
    the target distro/architecture, but all of its foreign execution settings are reset
    after config expansion. Empty list values clear the cross execution platform/toolchain
    lists before the host-native platform is added.
    """
    native_platform = f"//bazel/platforms:{config.target_distro}_{config.target_arch}"
    return _append_bazel_command_options_before_release_suffix(
        args,
        [
            f"--platforms={native_platform}",
            "--extra_execution_platforms=",
            f"--extra_execution_platforms={native_platform}",
            "--extra_toolchains=",
            "--remote_executor=",
            # The cross configs set toplevel to keep RBE worker outputs lean;
            # restore the repository default for the all-local release build.
            "--remote_download_outputs=all",
            "--remote_upload_local_results=false",
            "--noremote_accept_cached",
            # Keep the gRPC cache endpoint selected by public-release-local so
            # Bazel's remote downloader remains valid.  The all-action
            # no-cache annotation below prevents release actions from reading
            # or writing the remote action cache.
            "--modify_execution_info=.*=+no-cache",
            "--define=MONGO_IBM_CROSS=0",
            "--repo_env=MONGO_LINUX_CROSS_TOOLCHAIN=",
            "--repo_env=MONGO_WASI_SDK_EXEC_ARCH=",
            "--repo_env=MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON=",
            "--repo_env=MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH=",
            "--//bazel/config:idl_use_linux_python=False",
            "--//bazel/config:remote_link=False",
            # The cross config selects lld, which is unsupported by the native s390x
            # toolchain and is not required for native PPC64LE either.
            "--linker=auto",
        ],
    )


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
    # IBM cross-RBE is intentionally compile-only. In particular, remote_test and
    # remote_link configs must not send native target links or provenance outputs to RBE.
    remote_link = False
    host_args = _append_bazel_command_options(
        args,
        [
            f"--remote_executor={LINUX_CROSS_RBE_REMOTE_EXECUTOR}",
            *[
                f"--remote_default_exec_properties={exec_property}"
                for exec_property in exec_properties
            ],
            f"--extra_execution_platforms={_linux_cross_rbe_execution_platform(config)}",
            f"--repo_env={LINUX_CROSS_TOOLCHAIN_ENV}={_linux_cross_toolchain_selector(config)}",
            f"--repo_env={WASI_SDK_EXEC_ARCH_ENV}={config.exec_arch}",
            # Keep the IBM marker explicit in the final Bazel invocation.  The
            # cross config normally supplies this through .bazelrc, but task
            # generated rc files can omit common-config expansions.  The
            # marker gates the dedicated pycross C++ toolchain; local release
            # mode appends the corresponding =0 override below.
            "--define=MONGO_IBM_CROSS=1",
            *_linux_cross_rbe_podman_repo_env_args(env),
            *_macos_cross_linux_python_options(config.exec_arch),
            "--//bazel/config:idl_use_linux_python=True",
            *(["--//bazel/config:remote_link=True"] if remote_link else []),
            # The execution platform is foreign to the host on s390x/ppc64le. Keep all
            # otherwise-unspecified actions on RBE so their execution tools do not run locally,
            # but allow actions explicitly marked no-remote (such as install/package actions)
            # to fall back to a usable local strategy.
            "--spawn_strategy=remote,local",
            "--strategy=CppCompile=remote",
            "--strategy=IdlcGenerator=remote",
        ],
    )
    if not remote_link:
        # Evergreen's generated bazelrc enables this globally. Append after all
        # user/config options so the local-output policy cannot be overridden by
        # a later --config expansion. The build setting is also consumed by the
        # cross toolchain's selects: leaving it true while running output
        # actions in the native IBM container would select the execution
        # architecture's ar/objcopy/strip binaries and produce Exec format
        # errors on s390x/ppc64le. Override the generated Evergreen remote-link
        # strategies as well; setting only the build flag still leaves
        # ExtractDebugInfo/StripDebugInfo (and their objcopy tools) on RBE.
        host_args = _append_bazel_command_options_before_release_suffix(
            host_args,
            [
                "--//bazel/config:remote_link=False",
                "--remote_upload_local_results=false",
                *[
                    f"--strategy={mnemonic}=local"
                    for mnemonic in LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS
                ],
                "--modify_execution_info=^("
                + "|".join(LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS)
                + ")=+no-cache",
            ],
        )
    if _bazel_command(args) in {"coverage", "test"}:
        # A later --config=remote_test otherwise changes TestRunner back to remote.
        # Cross-built test binaries can only execute on the native IBM host.
        host_args = _append_bazel_command_options_last(
            host_args,
            [
                "--test_strategy=standalone",
                "--strategy=TestRunner=standalone",
                "--test_env=LD_LIBRARY_PATH="
                + _linux_cross_target_runtime_test_env(config, args, env, repo_root),
            ],
        )
    return host_args


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
    auth_file = _podman_auth_file(env)
    if auth_file is not None:
        # The action wrapper may run in a long-lived Bazel server whose process
        # environment predates this task. Preserve only the credential-file path;
        # Podman reads the file on the host and its contents never enter Bazel's
        # action environment or command line.
        config["podman_auth_file"] = str(auth_file)
    return config


def _linux_cross_module_tree_fingerprint(source: pathlib.Path, patch_file: pathlib.Path) -> str:
    """Return a content fingerprint for a patched local module overlay."""
    digest = hashlib.sha256()
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source).as_posix().encode()
        try:
            file_stat = path.lstat()
        except OSError as exc:
            raise RuntimeError(f"could not inspect cross module input {path}: {exc}") from exc
        digest.update(relative)
        digest.update(b"\0")
        if path.is_symlink():
            digest.update(b"symlink\0")
            digest.update(os.readlink(path).encode())
        elif stat.S_ISREG(file_stat.st_mode):
            digest.update(b"file\0")
            digest.update(_sha256_file(path).encode())
        else:
            # The source distributions should only contain regular files and
            # symlinks. Include other entries in the fingerprint so an unexpected
            # input cannot silently reuse an old overlay.
            digest.update(f"mode={file_stat.st_mode}\0".encode())
    digest.update(patch_file.read_bytes())
    return digest.hexdigest()


def _linux_cross_module_override_args(
    args: Sequence[str],
    env: Mapping[str, str],
    repo_root: pathlib.Path = REPO_ROOT,
) -> list[str]:
    """Create patched external-module copies for an IBM cross-RBE invocation.

    ``local_path_override`` deliberately points at the checked-in source
    distributions. Bzlmod has no patch attribute for that override, so using
    ``--override_module`` is the least invasive way to apply the small
    cross-execution fixes without modifying vendored files. The copies live under
    the Bazel output base, are content-addressed, and are never written into the
    workspace. Release-local mode does not call this function.
    """
    if _bazel_command(args) not in {"build", "coverage", "run", "test"}:
        return []

    output_base = _bazel_output_base(args, env, repo_root=repo_root).resolve()
    overlay_root = output_base / LINUX_CROSS_MODULE_OVERRIDE_DIRNAME
    overlay_root.mkdir(parents=True, exist_ok=True)
    patch_command = shutil.which("patch")
    if patch_command is None:
        raise RuntimeError(
            "IBM cross-RBE builds require the `patch` utility to materialize the "
            "protobuf and gRPC execution-platform overlays"
        )

    override_args: list[str] = []
    for repository, source_relative, patch_relative in LINUX_CROSS_MODULE_OVERRIDES:
        source = repo_root / source_relative
        patch_file = repo_root / patch_relative
        if not source.is_dir():
            raise RuntimeError(f"cross module source directory is missing: {source}")
        if not patch_file.is_file():
            raise RuntimeError(f"cross module patch file is missing: {patch_file}")

        fingerprint = _linux_cross_module_tree_fingerprint(source, patch_file)
        destination = overlay_root / f"{repository}-{fingerprint[:24]}"
        if not destination.is_dir():
            temporary = overlay_root / f".{repository}-{uuid.uuid4().hex}.tmp"
            try:
                shutil.copytree(source, temporary, symlinks=True)
                result = subprocess.run(
                    [
                        patch_command,
                        "--batch",
                        "--forward",
                        "--fuzz=0",
                        "-p1",
                        "-i",
                        str(patch_file),
                    ],
                    cwd=temporary,
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                if result.returncode:
                    detail = (result.stderr or result.stdout).strip()
                    raise RuntimeError(
                        f"could not apply {patch_file} to {source}: "
                        f"{detail or f'exit code {result.returncode}'}"
                    )
                try:
                    os.rename(temporary, destination)
                except OSError as e:
                    # Another Bazel client may have materialized the same
                    # content-addressed overlay concurrently. Renaming a
                    # directory onto an existing non-empty directory raises
                    # EEXIST or ENOTEMPTY; FileExistsError alone only covers
                    # the empty-directory case.
                    if e.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                        raise
                    shutil.rmtree(temporary)
            except Exception:
                if temporary.exists():
                    shutil.rmtree(temporary)
                raise

        override_args.append(f"--override_module={repository}={destination}")

    return override_args


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
    # Keep the output base in the trusted descriptor so post-build publication can recover the
    # action-private install tree even when Bazel could not create its workspace convenience
    # symlinks (for example, when a persistent container leaves a dangling bazel-bin link).
    config["output_base"] = str(output_base)
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
    remote_compile_only: bool = False,
    force_local: bool = False,
) -> list[str]:
    # These commands only analyze the graph or fetch repositories. They do not run
    # local actions, and Bazel's query-family commands reject action-only options
    # (including --sandbox_base and persistent-container strategy flags).
    if _bazel_command(args) in LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS:
        return list(args)

    remote_link_requested = (
        False
        if remote_compile_only or force_local
        else _remote_link_requested(args, env=env, repo_root=repo_root)
    )
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
        *[f"--strategy={mnemonic}=local" for mnemonic in LINUX_LOCAL_JAVA_MNEMONICS],
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
    remote_execution_disabled = force_local or _remote_execution_disabled_by_args(
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
                # Signing and provenance actions are release-only and not in the
                # general container-tool list, but release-local mode must run
                # them in the native container exactly like cross-RBE mode does
                # rather than falling through to the default sandboxed strategy.
                *[
                    f"--strategy={mnemonic}=persistent-container,local"
                    for mnemonic in LINUX_CROSS_LOCAL_TOOL_MNEMONICS
                    if mnemonic in ("EmbedPublicKeyHeader", "GpgExportArmored", "GpgSign")
                ],
                "--strategy=PyCompile=worker",
            ]
        )
    elif remote_compile_only:
        # Linux IBM cross builds use the native toolchain for link/output actions.
        # Compile and execution-platform tools remain remote because their
        # binaries come from the x86_64/aarch64 cross archive and cannot execute
        # on the native IBM host.
        options.extend(
            [
                # Keep the default strategy local. Only the explicit compiler and
                # execution-tool mnemonics below may use RBE in IBM compile-only mode.
                "--spawn_strategy=local",
                "--strategy=CppCompile=remote",
                "--strategy=Rustc=remote",
                "--strategy=RustcMetadata=remote",
                *[
                    f"--strategy={mnemonic}=remote"
                    for mnemonic in LINUX_CROSS_REMOTE_COMPILE_MNEMONICS
                    if mnemonic not in ("IdlcGenerator", "Genrule")
                ],
                # Genrule is remote-first, but genrules can be explicitly tagged
                # no-remote (the resmoke TSS test list genrule runs on the
                # Evergreen host for its credentials and network). Those must
                # fall back to the native container like they do in the
                # non-cross dynamic mode instead of failing strategy selection.
                "--strategy=Genrule=remote,persistent-container,local",
                # These actions invoke execution-platform tools (notably the
                # WASI clang used by ConfigHeaderGen) and must not fall back to
                # the native IBM host when an RBE attempt is unavailable.
                # IdlcGenerator is added by _linux_cross_rbe_host_args.
                # Bazel gives tool-link actions the CppLink mnemonic too. They use
                # the execution-platform toolchain and therefore cannot run in the
                # native target linker container on an IBM host. The progress
                # description is ``Linking ... [for tool]`` (it does not contain
                # the CppLink mnemonic), so match that description explicitly
                # while leaving unrelated execution-platform actions on their
                # normal strategy.
                r"--strategy_regexp=.*Linking .*\[for tool\].*=remote",
                # WASI links use the execution-platform clang and produce a normal CppLink
                # action whose progress text contains the .wasm output. Keep those links remote
                # rather than trying to execute the foreign clang in the native IBM container.
                r"--strategy_regexp=.*Linking .*\.wasm.*=remote",
                *[
                    f"--strategy={mnemonic}=persistent-container,local"
                    for mnemonic in LINUX_CROSS_LOCAL_TOOL_MNEMONICS
                ],
                *[f"--strategy={mnemonic}=local" for mnemonic in LINUX_LOCAL_TEST_MNEMONICS],
                "--modify_execution_info=^(MongoInstallRule|"
                + "|".join(
                    [
                        *LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS,
                        *LINUX_CROSS_LOCAL_TOOL_MNEMONICS,
                        *LINUX_LOCAL_TEST_MNEMONICS,
                    ]
                )
                + ")=+no-cache",
                # Keep these overrides at the end of the generated action policy.
                # Evergreen's `evg`/`remote_link` configs can otherwise append a
                # remote CppLink/CppArchive strategy after the wrapper's first
                # local-output declaration.  Regular links, archives, and debug
                # extraction must run with the host-native IBM toolchain; only
                # the explicit `[for tool]` and WASI link regexps remain remote.
                "--//bazel/config:remote_link=False",
                "--remote_upload_local_results=false",
                *[
                    f"--strategy={mnemonic}=persistent-container,local"
                    for mnemonic in LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS
                ],
                "--modify_execution_info=^("
                + "|".join(LINUX_LOCAL_OUTPUT_CONTAINER_MNEMONICS)
                + ")=+no-cache",
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
    if remote_compile_only or force_local:
        # Cross-RBE arguments are generated before this helper runs. Put the action
        # policy after those generated/config options so a later cross config cannot
        # re-enable remote links or the remote default spawn strategy.
        return _append_bazel_command_options_before_release_suffix(args, options)
    return _append_bazel_command_options(args, options)
