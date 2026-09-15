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

from ..bazelrc import _config_requested, _repo_env_overrides_from_args
from ..constants import (
    HERMETIC_CONTAINER_GIT_LAYER_ENV,
    REPO_ROOT,
    WINDOWS_CROSS_PASSTHROUGH_ENVS,
    WINDOWS_CROSS_SDK_ROOT_ENVS,
    WINDOWS_TOOLCHAIN_PIN_ENVS,
)
from ..distro import _safe_name, load_remote_execution_containers, parse_docker_image, select_distro
from ..docker import WSL_DOCKER_HOST_MODE
from ..env import (
    _append_bazel_command_options,
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
    _hardlink_or_copy,
    _hermetic_container_state_dir,
    _is_relative_to,
    _parse_repo_env_assignment,
    _symlink_target,
)
from ..volumes import _hermetic_container_image_with_git_layer
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


WINDOWS_CROSS_DEFAULT_COMMANDS = frozenset(
    [
        "aquery",
        "build",
        "cquery",
        "fetch",
    ]
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
