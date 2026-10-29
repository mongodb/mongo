"""Helpers shared by the cross-compilation host modules."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence

from ..bazelrc import _bazel_output_base
from ..constants import REPO_ROOT
from ..distro import _safe_name
from ..env import (
    CROSS_HOST_FLAGS_WITH_SEPARATE_VALUE,
    RUN_FLAG_PREFIXES,
    RUN_FLAGS_WITH_SEPARATE_VALUE,
    _bazel_command_index,
    _consume_option_value,
    _info,
)
from ..fsutil import _container_user, _hermetic_container_home_dir, _hermetic_container_state_dir

CROSS_HOST_ACTION_CONFIG_FILENAME = "mongo_cross_host_action.json"


RESMOKE_DEPS_PATH_SUFFIX = "_resmoke_deps_path.txt"


RESMOKE_DEPS_PATH_MAP_ENV = "DEPS_PATH_MAP_FILE"


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


def _host_binary_env(executable: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    runfiles_dir = pathlib.Path(str(executable) + ".runfiles")
    if runfiles_dir.is_dir():
        env["RUNFILES_DIR"] = str(runfiles_dir)
        env["TEST_SRCDIR"] = str(runfiles_dir)
    return env
