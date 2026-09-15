"""Top-level orchestration: integration mode selection and bazel dispatch."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import json
import os
import pathlib
import platform
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterator, Mapping, Sequence

from .bazelrc import (
    _bazel_output_base,
    _config_requested,
    _credential_helper_requested,
    _remote_execution_disabled_by_args,
    _remote_link_requested,
    _replace_bazel_startup_option,
    _repo_env_overrides_from_args,
    _workspace_status_command_requested,
)
from .cleanup import (
    _clean_hermetic_container_outputs,
    _clean_host_outputs,
    _clean_requested_expunge,
    _expunge_hermetic_container_outputs,
)
from .config import (
    IntegrationMode,
    _config_fingerprint,
    _run_file_matches_config,
    build_hermetic_container_config,
)
from .constants import (
    HERMETIC_CONTAINER_REPOSITORY_PATH,
    HERMETIC_CONTAINER_SOURCE_ROOT,
    HERMETIC_CONTAINER_SYMLINK_PREFIX,
    HERMETIC_CONTAINER_WORKSPACE_STATUS_COMMAND,
    LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS,
    REPO_ROOT,
)
from .cross.common import _write_cross_host_action_config
from .cross.linux import (
    LINUX_CONTAINER_ACTIONS_ENV,
    LINUX_DYNAMIC_SCHEDULING_ENV,
    LINUX_HOST_CONTAINER_COMMANDS,
    NATIVE_TOOLCHAIN_CONFIG,
    _ensure_linux_action_container,
    _ensure_linux_container_image,
    _linux_container_actions_lock,
    _linux_cross_local_container_env,
    _linux_cross_local_process_env,
    _linux_cross_module_override_args,
    _linux_cross_rbe_config,
    _linux_cross_rbe_host_args,
    _linux_cross_rbe_local_host_args,
    _linux_cross_rbe_process_env,
    _linux_host_container_action_args,
    _linux_host_container_config,
    _write_linux_container_actions_config_unlocked,
)
from .cross.macos import (
    MACOS_CROSS_SPLIT_TEST_RUNNER_ENV,
    _bazel_args_with_default_macos_cross_config,
    _macos_cross_config_requested,
    _macos_cross_host_bazel_test_args,
    _macos_cross_host_bazel_test_candidate,
    _macos_cross_host_bazel_test_requested,
    _macos_cross_host_run_plan,
    _macos_cross_host_run_requested,
    _macos_cross_host_test_plan,
    _macos_cross_host_test_requested,
    _macos_cross_local_container_action_args,
    _macos_cross_local_container_action_candidate,
    _macos_cross_local_container_action_requested,
    _macos_cross_remote_execution_disabled,
    _run_macos_cross_host_binary,
    _run_macos_cross_host_tests,
    _should_default_macos_cross_config,
)
from .cross.windows import (
    _bazel_args_with_default_windows_cross_config,
    _prepare_windows_cross_env,
    _run_windows_cross_host_binary,
    _should_default_windows_hermetic_container,
    _windows_cross_config_requested,
    _windows_cross_host_run_plan,
    _windows_cross_host_run_requested,
    _windows_cross_host_wrapper_action_args,
    _windows_cross_host_wrapper_action_candidate,
    _windows_cross_host_wrapper_action_requested,
)
from .distro import (
    _linux_host_container_distro,
    _linux_host_container_supported,
    detect_host_distro,
    normalize_arch,
)
from .docker import (
    WSL_DOCKER_API_VERSION_DEFAULT,
    WSL_DOCKER_API_VERSION_ENV,
    WSL_DOCKER_HOST_ENV,
    WSL_DRIVE_MOUNT_PREFIX_ENV,
    _docker_daemon_status,
    _print_docker_daemon_error,
    _select_linux_container_runtime,
    _use_wsl_docker,
)
from .engflow import ENGFLOW_AUTH_CLUSTER
from .env import (
    _append_bazel_command_options,
    _append_bazel_command_options_before_release_suffix,
    _bazel_command,
    _bazel_command_index,
    _env_is_false,
    _env_is_true,
    _info,
    _is_running_in_container,
    _warn_native_fallback,
)
from .fsutil import (
    _hermetic_container_lock,
    _hermetic_container_state_dir,
    _linux_native_shared_install_dir,
    _linux_shared_install_dir,
    _macos_shared_install_dir,
    _remove_path,
)
from .symlinks import (
    _bazel_args_with_hermetic_container_symlink_prefix,
    _publish_hermetic_container_convenience_symlinks,
    _publish_linux_host_convenience_symlinks,
    _publish_linux_shared_install_symlink,
    _publish_macos_shared_install_symlink,
)
from .volumes import _container_cert_env_values, _container_path

CONTAINERIZED_BES_KEYWORD = "MONGO_BUILD_CONTAINERIZED"


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


def _load_hermetic_container_module():
    if not (HERMETIC_CONTAINER_SOURCE_ROOT / "hermetic_container.py").exists():
        raise RuntimeError(
            f"Vendored hermetic_container checkout not found at {HERMETIC_CONTAINER_SOURCE_ROOT}"
        )
    sys.path.insert(0, str(HERMETIC_CONTAINER_SOURCE_ROOT))
    import hermetic_container  # pylint: disable=import-error

    return hermetic_container


def _run_direct(
    bazel_real: str,
    args: Sequence[str],
    env: Mapping[str, str] | None = None,
) -> int:
    return subprocess.run([bazel_real, *args], check=False, env=env).returncode


def _bazel_args_with_native_install_strategy(args: Sequence[str]) -> list[str]:
    """Keep the shared install convenience tree outside native action sandboxes."""
    if _bazel_command(args) not in {"build", "coverage", "run", "test"}:
        return list(args)
    return _append_bazel_command_options(
        args,
        ["--strategy=MongoInstallRule=local"],
    )


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
        cross_config = _linux_cross_rbe_config(args, env=env, repo_root=REPO_ROOT)
        if cross_config is None:
            raise RuntimeError("Linux cross-RBE mode requires a linux-*-cross-rbe config")
        release_local = _remote_execution_disabled_by_args(args, env=env, repo_root=REPO_ROOT)
        if release_local:
            # public-release-local is a complete local build policy. Do not first add
            # cross-RBE options: repository hydration and Bazel's last-option semantics
            # can otherwise leave a foreign selector active in a supposedly local build.
            cross_process_env = _linux_cross_local_process_env(env)
            host_args = _linux_cross_rbe_local_host_args(args, cross_config)
            local_container_env = _linux_cross_local_container_env(env, use_cross_image=False)
            host_args = _linux_host_container_action_args(
                host_args,
                local_container_env,
                remote_compile_only=False,
                force_local=True,
            )
        else:
            cross_process_env = _linux_cross_rbe_process_env(cross_config, env)
            # Keep the checked-in protobuf/gRPC source distributions pristine. The
            # cross-only Bazel rule changes are applied to output-base overlays before
            # repository hydration and selected with --override_module.
            cross_module_args = _linux_cross_module_override_args(args, env, repo_root=REPO_ROOT)
            host_args = _linux_cross_rbe_host_args(args, env, repo_root=REPO_ROOT)
            host_args = _append_bazel_command_options_before_release_suffix(
                host_args, cross_module_args
            )
            # IBM cross-RBE always keeps output/link/test actions in the host-native
            # hermetic container; the foreign image is reserved for RBE workers.  The
            # only remote work is compilation and execution-platform tool generation.
            local_container_env = _linux_cross_local_container_env(env, use_cross_image=False)
            host_args = _linux_host_container_action_args(
                host_args,
                local_container_env,
                remote_compile_only=True,
            )

        local_link = True
        if local_link and _bazel_command(args) not in LINUX_HOST_CONTAINER_NO_RUNTIME_COMMANDS:
            if env.get("MONGO_HERMETIC_CONTAINER_DRY_RUN") == "1":
                print(
                    json.dumps(
                        {
                            "linux_cross_host_rbe": {
                                "args": host_args,
                                "config": dataclasses.asdict(
                                    _linux_cross_rbe_config(args, env=env, repo_root=REPO_ROOT)
                                ),
                                "local_link": True,
                                "container": _linux_host_container_config(local_container_env),
                            },
                        },
                        sort_keys=True,
                    )
                )
                return 0

            container_command, runtime_detail = _select_linux_container_runtime(local_container_env)
            if container_command is None:
                detail = f" ({runtime_detail})" if runtime_detail else ""
                _info(
                    "No usable Linux container runtime is available for native cross linking"
                    f"{detail}; refusing to run the link locally."
                )
                return 1

            container_config = _linux_host_container_config(
                local_container_env,
                container_command=container_command,
            )
            if not _ensure_linux_container_image(container_command, container_config["image"]):
                _info(
                    f"could not pull build container image {container_config['image']}; "
                    "refusing to run the cross link locally"
                )
                return 1

            output_base = _bazel_output_base(args, env)
            with _linux_container_actions_lock(output_base):
                # The runtime selected above may be the real Podman executable even when
                # ``docker`` is installed as Podman's compatibility shim. Preserve that
                # selection in the persistent-container config; otherwise the wrapper falls
                # back to ``docker`` and loses the task-scoped Podman storage/runtime env.
                local_container_env = {
                    **local_container_env,
                    "HERMETIC_CONTAINER_DOCKER_COMMAND": container_command,
                }
                config_path, container_config = _write_linux_container_actions_config_unlocked(
                    args,
                    local_container_env,
                )
                host_args = _replace_bazel_startup_option(
                    host_args,
                    "--output_base",
                    str(config_path.parent),
                )
                container_ready, container_detail = _ensure_linux_action_container(config_path)
                if not container_ready:
                    detail_suffix = f" ({container_detail})" if container_detail else ""
                    _info(
                        "could not start or reuse build container for native cross linking"
                        f"{detail_suffix}; refusing to run the link locally"
                    )
                    return 1

            image_identifier = container_config["image"].rsplit("@", 1)[-1]
            if release_local:
                _info(
                    f"IBM release actions use the native hermetic container {image_identifier}; "
                    "remote execution and cache are disabled."
                )
            else:
                _info(
                    f"Cross compilation remains on RBE; native link/actions use container "
                    f"{image_identifier}."
                )
            rc = _run_direct(bazel_real, host_args, env=cross_process_env)
            _publish_linux_shared_install_symlink(container_config)
            return rc

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

        if release_local:
            _info("running IBM release actions locally through the native hermetic container")
        else:
            _info("running IBM cross compile actions on RBE through host Bazel")
        return _run_direct(bazel_real, host_args, env=cross_process_env)

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
