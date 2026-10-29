"""Environment variable, CLI argument, and console messaging helpers."""

from __future__ import annotations

import os
import pathlib
import sys
from collections.abc import Mapping, Sequence

from .constants import (
    CONTAINER_CGROUP_PATH,
    CONTAINER_CGROUP_RE,
    CONTAINER_MARKER_PATHS,
    HERMETIC_CONTAINER_DISABLED_VALUES,
    HERMETIC_CONTAINER_ENABLED_VALUES,
    KUBERNETES_SERVICE_HOST_ENV,
    RELEASE_LOCAL_SAFETY_SUFFIX,
)

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


def _warning(message: str) -> None:
    print(f"WARNING: {message}", file=sys.stderr)


def _warn_native_fallback(reason: str) -> None:
    _warning(
        f"hermetic_container is unavailable because {reason}; running Bazel natively. "
        "Native build outputs may not be cache-compatible with hermetic builds."
    )


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


def _consume_option_value(args: Sequence[str], index: int) -> tuple[str | None, int]:
    arg = args[index]
    if "=" in arg:
        return arg.split("=", 1)[1], index + 1
    if index + 1 < len(args):
        return args[index + 1], index + 2
    return None, index + 1


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


def _append_bazel_command_options_last(args: Sequence[str], options: Sequence[str]) -> list[str]:
    """Appends mandatory command options after user options but before run arguments."""
    if not options or _bazel_command_index(args) is None:
        return list(args)

    try:
        run_args_index = args.index("--")
    except ValueError:
        run_args_index = len(args)

    return [*args[:run_args_index], *options, *args[run_args_index:]]


def _append_bazel_command_options_before_release_suffix(
    args: Sequence[str], options: Sequence[str]
) -> list[str]:
    """Append options while preserving Evergreen's required release suffix."""
    if not options:
        return list(args)

    suffix_length = len(RELEASE_LOCAL_SAFETY_SUFFIX)
    if len(args) >= suffix_length and tuple(args[-suffix_length:]) == RELEASE_LOCAL_SAFETY_SUFFIX:
        return [*args[:-suffix_length], *options, *RELEASE_LOCAL_SAFETY_SUFFIX]
    return _append_bazel_command_options_last(args, options)
