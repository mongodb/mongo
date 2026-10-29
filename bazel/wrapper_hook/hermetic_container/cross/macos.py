"""macOS cross-compilation: host test plans, container actions, host runs."""

from __future__ import annotations

import hashlib
import os
import pathlib
import platform
import subprocess
import sys
from collections.abc import Mapping, Sequence

from ..bazelrc import _config_requested, _remote_execution_disabled_by_args, _remote_link_requested
from ..constants import MACOS_CROSS_DEFAULT_CONFIG_ENV, REPO_ROOT
from ..distro import (
    _safe_name,
    load_remote_execution_containers,
    normalize_arch,
    parse_docker_image,
    select_distro,
)
from ..env import (
    CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE,
    TEST_FLAG_PREFIXES,
    TEST_FLAGS_WITH_SEPARATE_VALUE,
    _append_bazel_command_options,
    _bazel_bool_flag_value,
    _bazel_command,
    _bazel_command_index,
    _bazel_run_target,
    _consume_option_value,
    _env_is_false,
    _env_is_true,
    _info,
    _is_macos_cross_default_run_target,
    _parse_test_tag_filters,
)
from ..fsutil import _symlink_target
from ..volumes import (
    _hermetic_container_git_layer_enabled,
    _hermetic_container_image_with_git_layer,
)
from .common import (
    RESMOKE_DEPS_PATH_MAP_ENV,
    MacOSCrossHostRunPlan,
    MacOSCrossHostTestPlan,
    _add_test_env,
    _cross_host_run_plan,
    _expand_host_test_labels,
    _host_bazel_env,
    _host_binary_env,
    _host_test_base_env,
    _label_to_host_executable,
    _macos_cross_linux_python_options,
    _read_target_pattern_file,
    _resmoke_deps_path_file,
    _write_resmoke_deps_path_map,
)

MACOS_CROSS_CONFIGS = frozenset(
    [
        "macos-cross-arm64",
        "macos-cross-x86_64",
    ]
)


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


MACOS_CROSS_DEFAULT_COMMANDS = frozenset(["build", "test"])


def _macos_cross_config_requested(
    args: Sequence[str],
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return any(
        _config_requested(args, config, env=env, repo_root=repo_root)
        for config in MACOS_CROSS_CONFIGS
    )


def _macos_cross_remote_execution_disabled(
    args: Sequence[str],
    env: Mapping[str, str],
) -> bool:
    if _env_is_true(env.get("MONGO_MACOS_CROSS_LOCAL_CONTAINER_ONLY")):
        return True

    return _remote_execution_disabled_by_args(args, env=env)


def _default_macos_cross_config(machine: str | None = None) -> str:
    arch = normalize_arch(machine)
    if arch == "aarch64":
        return "macos-cross-arm64"
    if arch == "x86_64":
        return "macos-cross-x86_64"
    raise RuntimeError(f"Unsupported macOS cross-compilation architecture: {arch}")


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


def _macos_cross_host_run_plan(args: Sequence[str]) -> MacOSCrossHostRunPlan:
    return _cross_host_run_plan(args, "macOS")


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
