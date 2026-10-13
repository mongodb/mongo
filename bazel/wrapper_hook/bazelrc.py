"""Resolve release configurations from Bazel arguments and rc files."""

import os
import pathlib
import shlex
from collections.abc import Mapping, Sequence

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

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


def _host_home(env: Mapping[str, str]) -> pathlib.Path | None:
    if env.get("HOME"):
        return pathlib.Path(env["HOME"])
    if env.get("USERPROFILE"):
        return pathlib.Path(env["USERPROFILE"])

    try:
        return pathlib.Path.home()
    except RuntimeError:
        return None


def _startup_args(args: Sequence[str]) -> list[str]:
    """Returns the startup option section of args with @argfile arguments expanded.

    Bazel expands `@<path>` arguments (including in the startup option section), so an
    argfile can legally carry --output_base or --output_user_root. The wrapper's mirror must
    see those values to key its state on the same output base Bazel will use. Expansion is
    not recursive, matching Bazel.
    """
    command_index = _bazel_command_index(args)
    startup_args = list(args) if command_index is None else list(args[:command_index])
    expanded = []
    for arg in startup_args:
        if len(arg) < 2 or not arg.startswith("@"):
            expanded.append(arg)
            continue
        try:
            contents = pathlib.Path(arg[1:]).read_text(encoding="utf-8")
        except OSError:
            # Bazel reports unreadable argfiles itself; keep the literal token.
            expanded.append(arg)
            continue
        expanded.extend(shlex.split(contents))
    return expanded


def _startup_option_values(args: Sequence[str], name: str) -> list[str]:
    """Returns a Bazel startup option's command-line values in precedence order."""
    startup_args = _startup_args(args)
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


def _startup_boolean_option(args: Sequence[str], name: str, default: bool) -> bool:
    """Returns the effective value of a --[no]NAME startup option."""
    startup_args = _startup_args(args)
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
