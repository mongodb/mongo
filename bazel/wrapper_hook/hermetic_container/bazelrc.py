"""Bazelrc parsing and effective option computation."""

from __future__ import annotations

import hashlib
import os
import pathlib
import shlex
from collections.abc import Mapping, Sequence

from .constants import REPO_ROOT
from .env import _bazel_command, _bazel_command_index
from .fsutil import _default_bazel_user_output_root, _host_home, _parse_repo_env_assignment


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


def _config_requested(
    args: Sequence[str],
    config_name: str,
    env: Mapping[str, str] = os.environ,
    repo_root: pathlib.Path = REPO_ROOT,
) -> bool:
    return config_name in _effective_config_values(args, env=env, repo_root=repo_root)


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
