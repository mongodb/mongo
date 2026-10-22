"""Windows cross-compilation: sysroot setup, env, and host runs."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import pathlib
import platform
import shlex
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence

from bazel.toolchains.cc.mongo_windows_cross import (
    windows_container_action_wrapper as windows_action_runner,
)

from .. import container_actions
from ..bazelrc import (
    _bazel_output_base,
    _config_requested,
    _remote_link_requested,
    _replace_bazel_startup_option,
    _repo_env_overrides_from_args,
)
from ..constants import (
    HERMETIC_CONTAINER_GIT_LAYER_ENV,
    REPO_ROOT,
    WINDOWS_CROSS_PASSTHROUGH_ENVS,
    WINDOWS_CROSS_PATH_ENVS,
    WINDOWS_CROSS_SDK_ROOT_ENVS,
    WINDOWS_TOOLCHAIN_PIN_ENVS,
)
from ..distro import (
    DockerImage,
    _safe_name,
    load_remote_execution_containers,
    parse_docker_image,
    select_distro,
)
from ..docker import (
    WSL_DOCKER_API_VERSION_DEFAULT,
    WSL_DOCKER_API_VERSION_ENV,
    WSL_DOCKER_HOST_ENV,
    WSL_DRIVE_MOUNT_PREFIX_ENV,
    _use_wsl_docker,
)
from ..env import (
    _append_bazel_command_options,
    _append_bazel_command_options_last,
    _bazel_command,
    _bazel_command_index,
    _bazel_run_target,
    _env_is_false,
    _env_is_true,
    _info,
    _is_cross_default_run_target,
    _platforms_requested,
)
from ..fsutil import (
    _container_user,
    _hardlink_or_copy,
    _hermetic_container_state_dir,
    _is_relative_to,
    _linux_action_sandbox_base,
    _linux_shared_install_dir,
    _parse_repo_env_assignment,
    _symlink_target,
)
from .common import (
    MacOSCrossHostRunPlan,
    _cross_host_run_plan,
    _host_binary_env,
    _label_to_host_executable,
    _macos_cross_linux_python_options,
)
from .macos import _macos_cross_remote_execution_disabled

WINDOWS_CROSS_CONFIG = "windows-cross-x86_64"


WINDOWS_CROSS_DEFAULT_CONFIG_ENV = "MONGO_WINDOWS_CROSS_DEFAULT_CONFIG"


WINDOWS_CROSS_ACTION_WRAPPER_ENV = "MONGO_WINDOWS_CROSS_ACTION_WRAPPER"


WINDOWS_CROSS_RBE_CONTAINER_IMAGE_ENV = "MONGO_WINDOWS_CROSS_RBE_CONTAINER_IMAGE"


WINDOWS_CROSS_RBE_POOL_ENV = "MONGO_WINDOWS_CROSS_RBE_POOL"


WINDOWS_CROSS_REMOTE_EXECUTOR = "grpcs://sodalite.cluster.engflow.com"


WINDOWS_CROSS_RBE_POOL = "x86_64"


# The pinned LLVM archive requires GLIBC_2.34 and GLIBCXX_3.4.30.
WINDOWS_CROSS_DEFAULT_EXEC_DISTRO = "ubuntu24"


WINDOWS_CROSS_DEFAULT_COMMANDS = frozenset(
    [
        "aquery",
        "build",
        "cquery",
        "fetch",
    ]
)


WINDOWS_CONTAINER_ACTION_WRAPPER_SCRIPT = (
    REPO_ROOT / "bazel/toolchains/cc/mongo_windows_cross/windows_container_action_wrapper.py"
)
WINDOWS_CONTAINER_ACTIONS_LAYOUT_VERSION = "v1"
WINDOWS_CROSS_UNIX_ROOT = "C:/bazel-linux"
WINDOWS_LOCAL_CONTAINER_MNEMONICS = ("CppLink",)
WINDOWS_CONTAINER_PREFLIGHT_TIMEOUT_SECONDS = 900
WINDOWS_CONTAINER_RUNNER_ENVIRONMENT_KEYS = (
    "APPDATA",
    "DOCKER_API_VERSION",
    "DOCKER_CERT_PATH",
    "DOCKER_CONFIG",
    "DOCKER_TLS_VERIFY",
    "HOMEDRIVE",
    "HOMEPATH",
    "LOCALAPPDATA",
    "PROGRAMDATA",
    "SYSTEMDRIVE",
    "SYSTEMROOT",
    "USERPROFILE",
    windows_action_runner.engine.WSL_DISTRIBUTION_ENV,
    windows_action_runner.engine.WSL_DOCKER_SOCKET_ENV,
)


@dataclasses.dataclass(frozen=True)
class WindowsCrossSysrootSpec:
    """Pinned host inputs used to build a Windows cross-compilation sysroot."""

    vc_path: pathlib.Path
    vc_full_version: str
    winsdk_root: pathlib.Path
    winsdk_full_version: str
    sources: dict[str, pathlib.Path]


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


def _windows_cross_config_requested(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return _config_requested(args, WINDOWS_CROSS_CONFIG, env=env, repo_root=repo_root)


def _windows_cross_remote_execution_disabled(
    args: Sequence[str],
    env: Mapping[str, str],
) -> bool:
    if _env_is_true(env.get("MONGO_WINDOWS_CROSS_LOCAL_CONTAINER_ONLY")):
        return True

    return _macos_cross_remote_execution_disabled(args, env)


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
    prepared_env.setdefault("MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE", "")
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


def _windows_cross_execution_arch(env: Mapping[str, str]) -> str:
    arch = env.get("MONGO_WINDOWS_CROSS_EXEC_ARCH", "x86_64").lower()
    if arch not in {"x86_64", "amd64"}:
        raise RuntimeError(
            "Windows host cross actions require MONGO_WINDOWS_CROSS_EXEC_ARCH=x86_64"
        )
    pool = env.get(WINDOWS_CROSS_RBE_POOL_ENV, WINDOWS_CROSS_RBE_POOL)
    if pool.lower() in {"arm64", "aarch64", "default"}:
        raise RuntimeError(f"Windows cross RBE pool {pool} is incompatible with x86_64 execution")
    return "x86_64"


def _windows_cross_rbe_exec_properties(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    _windows_cross_execution_arch(env)
    _, image = _windows_cross_action_image(env, containers)

    return [
        f"container-image=docker://{image.full_name}",
        "dockerNetwork=standard",
        f"Pool={env.get(WINDOWS_CROSS_RBE_POOL_ENV, WINDOWS_CROSS_RBE_POOL)}",
    ]


def _windows_cross_action_container_env_options(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    distro, image = _windows_cross_action_image(env, containers)
    values = {
        "MONGO_WINDOWS_CROSS_ACTION_WRAPPER": "1",
        "MONGO_WINDOWS_CROSS_ACTION_CONTAINER_PREFIX": _windows_cross_action_container_prefix(
            distro, image.full_name
        ),
        "MONGO_WINDOWS_CROSS_ACTION_IMAGE": image.full_name,
    }
    if env.get("MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"):
        values["MONGO_WINDOWS_CROSS_ACTION_PLATFORM"] = env[
            "MONGO_HERMETIC_CONTAINER_DOCKER_PLATFORM"
        ]
    return [f"--action_env={key}={value}" for key, value in sorted(values.items())]


def _windows_cross_host_wrapper_action_args(
    args: Sequence[str],
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> list[str]:
    command_index = _bazel_command_index(args)
    if command_index is not None:
        unix_root_option = f"--host_jvm_args=-Dbazel.windows_unix_root={WINDOWS_CROSS_UNIX_ROOT}"
        startup = [arg for arg in args[:command_index] if arg != unix_root_option]
        args = [*startup, unix_root_option, *args[command_index:]]
    selection_env = {**env, **_repo_env_overrides_from_args(args)}
    arch = _windows_cross_execution_arch(selection_env)
    exec_properties = _windows_cross_rbe_exec_properties(selection_env, containers=containers)
    platform_properties = dict(prop.split("=", 1) for prop in exec_properties)
    output_base = _bazel_output_base(args, env)
    if _remote_link_requested(args, env=env, repo_root=REPO_ROOT):
        link_options = ["--//bazel/config:remote_link=True", "--strategy=CppLink=remote"]
    else:
        link_options = _windows_container_action_options(output_base)
    host_args = _append_bazel_command_options(
        args,
        [
            "--//bazel/config:windows_cross_local_container_actions=True",
            "--//bazel/config:idl_use_linux_python=True",
            "--//bazel/config:disable_warnings_as_errors=True",
            *_macos_cross_linux_python_options(arch),
            f"--repo_env=MONGO_WINDOWS_CROSS_EXEC_ARCH={arch}",
            f"--repo_env=MONGO_WINDOWS_CROSS_RBE_POOL={selection_env.get(WINDOWS_CROSS_RBE_POOL_ENV, WINDOWS_CROSS_RBE_POOL)}",
            f"--repo_env=MONGO_WINDOWS_CROSS_RBE_PLATFORM_IMAGE={platform_properties['container-image']}",
            f"--repo_env=MONGO_WINDOWS_CROSS_RBE_PLATFORM_DISTRO={_windows_cross_rbe_distro(selection_env, containers)}",
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
            "--enable_runfiles",
            "--action_env=MSYS=winsymlinks:nativestrict",
            "--@rules_python//python/config_settings:build_python_zip=false",
            f"--host_platform={_windows_cross_rbe_execution_platform(selection_env, containers)}",
            "--extra_execution_platforms="
            + ",".join(
                [
                    _windows_cross_rbe_execution_platform(selection_env, containers),
                    "//bazel/platforms:windows_x86_64_cross",
                ]
            ),
            "--repo_env=MONGO_BAZEL_TOOLCHAIN_OS=linux",
            "--repo_env=MONGO_BAZEL_TOOLCHAIN_ARCH=x86_64",
            f"--repo_env=MONGO_BAZEL_TOOLCHAIN_DISTRO={_windows_cross_rbe_distro(selection_env, containers)}",
            "--shell_executable=/bin/bash",
            f"--action_env=PATH={_windows_cross_action_path(env)}",
            f"--host_action_env=PATH={_windows_cross_action_path(env)}",
            "--strategy=MongoInstallRule=local",
            "--spawn_strategy=local",
            "--strategy=CppCompile=remote",
            *link_options,
            "--strategy=CppArchive=remote",
            "--strategy=SolibSymlink=local",
            "--strategy=ExtractDebugInfo=local",
            "--strategy=StripDebugInfo=local",
            "--strategy=CcGenerateIntermediateDwp=local",
            "--strategy=CcGenerateDwp=local",
            "--strategy=ConfigHeaderGen=remote",
            "--strategy=IdlcGenerator=remote",
            "--strategy=WindowsRC=remote",
            "--features=-thin_archive",
            "--test_strategy=standalone",
            "--strategy=TestRunner=standalone",
            *_windows_cross_action_container_env_options(selection_env, containers=containers),
        ],
    )
    return _append_bazel_command_options_last(
        host_args,
        [
            # Dependency aliases must match the JVM root even when the caller supplied
            # an old repository override. Both values are owned by this execution route.
            f"--repo_env=MONGO_BAZEL_WINDOWS_UNIX_ROOT={WINDOWS_CROSS_UNIX_ROOT}",
            "--noenable_platform_specific_config",
        ],
    )


def _windows_cross_host_run_plan(args: Sequence[str]) -> MacOSCrossHostRunPlan:
    return _cross_host_run_plan(args, "Windows")


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


def _windows_cross_action_image(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> tuple[str, DockerImage]:
    """Select the same pinned image for remote and persistent local actions."""
    if _env_is_true(env.get(HERMETIC_CONTAINER_GIT_LAYER_ENV)):
        raise RuntimeError(
            "Windows cross actions require a published container image for both RBE and "
            "local linking; disable MONGO_HERMETIC_CONTAINER_GIT_LAYER and use "
            "MONGO_WINDOWS_CROSS_RBE_CONTAINER_IMAGE to select a published image"
        )
    containers = containers or load_remote_execution_containers()
    _windows_cross_execution_arch(env)
    distro = _windows_cross_rbe_distro(env, containers)
    image_override = env.get(WINDOWS_CROSS_RBE_CONTAINER_IMAGE_ENV) or env.get(
        "MONGO_HERMETIC_CONTAINER_IMAGE"
    )
    container_url = image_override or containers[distro]["container-url"]
    docker_image = parse_docker_image(container_url)
    return distro, docker_image


def _windows_cross_action_container_prefix(distro: str, image: str) -> str:
    image_hash = hashlib.sha256(image.encode()).hexdigest()[:12]
    return _safe_name(f"mongo_windows_cross_action_{distro}_x86_64_{image_hash}")


def _windows_container_action_options(output_base: pathlib.Path) -> list[str]:
    """Bazel options that route Windows link actions through the persistent Linux container.

    These mirror _linux_host_container_action_args: Bazel sandboxes each action's declared
    inputs on the host and invokes the Windows runner, which executes the tool at the same
    location inside the container built from the pinned RBE image.
    """
    return [
        *container_actions.persistent_container_options(
            output_base, WINDOWS_CONTAINER_ACTION_WRAPPER_SCRIPT
        ),
        *[
            f"--strategy={mnemonic}=persistent-container"
            for mnemonic in WINDOWS_LOCAL_CONTAINER_MNEMONICS
        ],
    ]


def _windows_container_actions_requested(args: Sequence[str]) -> bool:
    """Returns whether the planned Bazel arguments route any action to the local container."""
    strategies: dict[str, str] = {}
    tokens = iter(args)
    for arg in tokens:
        if arg == "--":
            break
        assignment = None
        if arg == "--strategy":
            assignment = next(tokens, "")
            if assignment == "--":
                break
        elif arg.startswith("--strategy="):
            assignment = arg.removeprefix("--strategy=")
        if assignment is not None:
            mnemonic, separator, strategy = assignment.partition("=")
            if separator:
                strategies[mnemonic] = strategy
    return any(
        "persistent-container" in strategies.get(mnemonic, "").split(",")
        for mnemonic in WINDOWS_LOCAL_CONTAINER_MNEMONICS
    )


def _windows_container_actions_config(
    args: Sequence[str],
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
    repo_root: pathlib.Path = REPO_ROOT,
) -> dict:
    """Returns the persistent-container config consumed by the Windows action runner.

    It has the shape of the Linux runner's config plus the Windows host details the runner
    needs: how to reach the Linux Docker Engine, where the WSL distribution mounts the host
    drives, and the toolchain locations that must be visible inside the container.
    """
    selection_env = {**env, **_repo_env_overrides_from_args(args)}
    distro, docker_image = _windows_cross_action_image(selection_env, containers=containers)
    image = docker_image.full_name
    docker_command = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND") or "docker"
    docker_argv = shlex.split(docker_command)
    if len(docker_argv) == 1 and "/" not in docker_argv[0] and "\\" not in docker_argv[0]:
        docker_executable = shutil.which(docker_argv[0], path=env.get("PATH"))
        if docker_executable:
            # Windows installs Docker below "Program Files"; the runner splits the command
            # with shlex, so keep the path a single token.
            docker_command = shlex.quote(docker_executable)
    output_base = _bazel_output_base(args, env, repo_root=repo_root)
    runner_environment = {
        key: env[key] for key in WINDOWS_CONTAINER_RUNNER_ENVIRONMENT_KEYS if env.get(key)
    }
    if _use_wsl_docker(env):
        runner_environment["DOCKER_HOST"] = (
            env.get(WSL_DOCKER_HOST_ENV) or env.get("DOCKER_HOST") or "tcp://127.0.0.1:2375"
        )
        runner_environment.setdefault(
            "DOCKER_API_VERSION",
            env.get(WSL_DOCKER_API_VERSION_ENV) or WSL_DOCKER_API_VERSION_DEFAULT,
        )
    else:
        for key in ("DOCKER_HOST", "DOCKER_CONTEXT"):
            if env.get(key):
                runner_environment[key] = env[key]
    return {
        "image": image,
        "docker_command": docker_command,
        # The interpreter Bazel starts the runner with; compile-command replays reuse it.
        "runner_python": sys.executable,
        "network": env.get("HERMETIC_CONTAINER_NETWORK", "host"),
        "user": _container_user("Windows"),
        "repo_root": str(repo_root),
        "state_dir": str(container_actions.state_dir(repo_root)),
        "container_layout_version": WINDOWS_CONTAINER_ACTIONS_LAYOUT_VERSION,
        "container_prefix": _windows_cross_action_container_prefix(distro, image),
        "host_system": "Windows",
        "docker_host_mode": env.get("MONGO_HERMETIC_CONTAINER_DOCKER_HOST_MODE", ""),
        "drive_mount_prefix": env.get(WSL_DRIVE_MOUNT_PREFIX_ENV, "/mnt"),
        "mount_paths": [
            selection_env[name] for name in WINDOWS_CROSS_PATH_ENVS if selection_env.get(name)
        ],
        "runner_environment": runner_environment,
        # Only used to locate a bare `docker` command; never forwarded into the container.
        "host_path": env.get("PATH", ""),
        "sandbox_base": str(_linux_action_sandbox_base(output_base)),
        # Writable sibling of the output base, as on Linux hosts; compile-command replays
        # (evergreen/validate_compile_commands.py) write their outputs there.
        "shared_install_dir": str(_linux_shared_install_dir(output_base)),
    }


def _ensure_windows_action_container(config_path: pathlib.Path) -> tuple[bool, str]:
    """Starts the Windows action container and verifies its bind mounts before Bazel runs."""
    return container_actions.preflight_container_runner(
        WINDOWS_CONTAINER_ACTION_WRAPPER_SCRIPT,
        config_path,
        timeout=WINDOWS_CONTAINER_PREFLIGHT_TIMEOUT_SECONDS,
        report_output=True,
    )


def _prepare_windows_container_actions(
    host_args: Sequence[str], env: Mapping[str, str]
) -> tuple[list[str], int]:
    """Publishes the runner config and starts the action container for a Windows cross build.

    Returns the (possibly adjusted) Bazel arguments and a non-zero status when the container
    cannot be used. The build must then fail rather than run Linux tools natively on Windows.
    """
    host_args = list(host_args)
    from ..runner import _prepare_hermetic_container_process_env

    _prepare_hermetic_container_process_env(env)
    config = _windows_container_actions_config(host_args, env)
    try:
        config = windows_action_runner.prepare_runner_config(config)
    except (OSError, subprocess.TimeoutExpired, SystemExit):
        _info("could not select a Linux engine for Windows link actions")
        return host_args, 1
    if not container_actions.ensure_container_image(
        str(config["docker_command"]), str(config["image"])
    ):
        _info(
            f"could not pull build container image {config['image']}; "
            "refusing to run Windows link actions natively"
        )
        return host_args, 1

    output_base = _bazel_output_base(host_args, env)
    # Publish the action config and establish the container under the output-base lock, then
    # release it before Bazel runs so nested Bazel invocations cannot deadlock.
    with container_actions.lock(output_base):
        generation, _ = container_actions.output_base_generation(output_base)
        config_path, config = container_actions.publish_container_actions_config(
            config, output_base, generation, "mongo_windows_cross_action"
        )
        host_args = _replace_bazel_startup_option(host_args, "--output_base", str(output_base))
        container_ready, detail = _ensure_windows_action_container(config_path)
    if not container_ready:
        # The runner's own report was already echoed above; keep this line short.
        detail_suffix = "" if detail else " (the runner produced no output)"
        _info(
            f"could not start or verify the Windows action container "
            f"{config['container_name']}{detail_suffix}; refusing to run link actions natively"
        )
        return host_args, 1

    image_identifier = str(config["image"]).rsplit("@", 1)[-1]
    _info(
        f"Container image {image_identifier} is running in the background to execute "
        "Windows link actions; compile actions execute on RBE in the same image."
    )
    return host_args, 0


def _windows_cross_action_path(env: Mapping[str, str]) -> str:
    """Returns an action PATH usable by both Linux RBE and native Windows actions.

    Bazel computes the action PATH from the client OS (see BazelRuleClassProvider
    pathOrDefault), so a Windows client bakes semicolon-separated Windows entries
    into actions that run on Linux RBE workers, which breaks PATH lookups there.
    POSIX entries come first for Linux workers; the Windows system directories
    mirror Bazel's own Windows defaults so local host actions keep working.
    """
    system_root = (env.get("SYSTEMROOT") or env.get("SystemRoot") or "C:\\Windows").rstrip("\\")
    return ";".join(
        (
            "/bin:/usr/bin:/usr/local/bin",
            system_root,
            system_root + "\\System32",
            system_root + "\\System32\\WindowsPowerShell\\v1.0",
        )
    )


def _windows_cross_rbe_distro(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> str:
    containers = containers or load_remote_execution_containers()
    if (
        not env.get("MONGO_HERMETIC_CONTAINER_DISTRO")
        and WINDOWS_CROSS_DEFAULT_EXEC_DISTRO in containers
    ):
        return WINDOWS_CROSS_DEFAULT_EXEC_DISTRO
    return select_distro(containers, env=env, arch="x86_64")


def _windows_cross_rbe_execution_platform(
    env: Mapping[str, str],
    containers: Mapping[str, Mapping[str, str]] | None = None,
) -> str:
    return "@internal_platforms_do_not_use//windows_cross:exec"
