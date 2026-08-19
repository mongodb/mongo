"""Dispatch the quality-check pseudo-targets before invoking Bazel.

``checks``, ``format``, ``lint``, and ``codeowners`` look like ordinary
``bazel run`` targets to callers, but they are implemented by the repository
Python environment.  Handling them here avoids starting a second Bazel
invocation merely to launch the quality-check runner.
"""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NamedTuple

from bazel.wrapper_hook.install_modules import _target_venv, _venv_python
from bazel.wrapper_hook.wrapper_util import get_terminal_stream

REPO_ROOT = Path(__file__).resolve().parents[2]
_INVOCATIONS = ("checks", "format", "lint", "codeowners")
_TARGETS = {
    spelling: invocation
    for invocation in _INVOCATIONS
    for spelling in (invocation, f":{invocation}", f"//:{invocation}")
}
_BAZEL_COMMANDS = frozenset(
    (REPO_ROOT / "bazel" / "wrapper_hook" / "bazel_commands.commands")
    .read_text(encoding="utf-8")
    .splitlines()
)
_COMMAND_OPTIONS_WITH_VALUES = frozenset(
    {
        "--aspects",
        "--build_tag_filters",
        "--compilation_mode",
        "--config",
        "--define",
        "--features",
        "--lockfile_mode",
        "--local_resources",
        "--jobs",
        "--output_groups",
        "--platforms",
        "--remote_download_regex",
        "--target_pattern_file",
        "--test_tag_filters",
        "--ui_event_filters",
        "-c",
        "-j",
    }
)


@dataclass(frozen=True, slots=True)
class QualityChecksResponse:
    """The response returned to the platform Bazel wrapper."""

    handled: bool
    exit_code: int = 0


class QualityChecksInvocation(NamedTuple):
    """Arguments split between Bazel and the quality-check CLI."""

    invocation_name: str
    program_args: list[str]
    startup_options: list[str]
    bazel_options: list[str]


def _split_legacy_tail(tokens: Sequence[str]) -> tuple[list[str], list[str]]:
    """Split Bazel options from alias arguments when ``--`` was omitted."""

    program_args: list[str] = []
    bazel_options: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in _COMMAND_OPTIONS_WITH_VALUES and index + 1 < len(tokens):
            bazel_options.extend((token, tokens[index + 1]))
            index += 2
            continue
        if any(token.startswith(f"{option}=") for option in _COMMAND_OPTIONS_WITH_VALUES):
            bazel_options.append(token)
        else:
            program_args.append(token)
        index += 1
    return program_args, bazel_options


def _quality_checks_invocation(
    args: Sequence[str],
) -> QualityChecksInvocation | None:
    """Split wrapper arguments before forwarding them to the quality-check CLI.

    Bazel consumes startup and command options on the left side of ``--`` while
    the quality-check CLI consumes its own arguments on the right. The wrapper
    has to perform this small split before launching the repository Python
    runner; using an ordinary argparse parser here would lose that boundary.
    """

    command = next(
        ((index, arg) for index, arg in enumerate(args) if arg in _BAZEL_COMMANDS),
        None,
    )
    if command is None or command[1] != "run":
        return None
    run_index = command[0]

    has_separator = True
    try:
        separator_index = args.index("--", run_index + 1)
    except ValueError:
        separator_index = len(args)
        has_separator = False

    target_index = run_index + 1
    while target_index < separator_index:
        token = args[target_index]
        if token in _COMMAND_OPTIONS_WITH_VALUES:
            target_index += 2
            continue
        if token.startswith("-"):
            target_index += 1
            continue
        break
    if target_index >= separator_index or args[target_index] not in _TARGETS:
        return None
    invocation = _TARGETS[args[target_index]]
    if has_separator:
        tail = list(args[separator_index + 1 :])
        trailing_bazel_options = list(args[target_index + 1 : separator_index])
    else:
        # Compatibility aliases historically accepted tool arguments without
        # Bazel's `--` separator (for example `bazel run lint --fix`).
        tail, trailing_bazel_options = _split_legacy_tail(args[target_index + 1 :])
    startup_options = list(args[:run_index])
    bazel_options = [*args[run_index + 1 : target_index], *trailing_bazel_options]
    return QualityChecksInvocation(invocation, tail, startup_options, bazel_options)


def _invocation_may_run_lint(
    invocation: QualityChecksInvocation,
) -> bool:
    """Return whether an invocation can select any lint checks."""

    invocation_name, program_args, _startup_options, _bazel_options = invocation
    if any(flag in program_args for flag in {"--list", "--help", "-h"}):
        return False

    groups: set[str] = set()
    only_ids: set[str] = set()
    index = 0
    while index < len(program_args):
        arg = program_args[index]
        if arg in {"--group", "--only"} and index + 1 < len(program_args):
            values = {
                value.strip() for value in program_args[index + 1].split(",") if value.strip()
            }
            (groups if arg == "--group" else only_ids).update(values)
            index += 2
            continue
        if arg.startswith("--group="):
            groups.update(
                value.strip() for value in arg.split("=", 1)[1].split(",") if value.strip()
            )
        elif arg.startswith("--only="):
            only_ids.update(
                value.strip() for value in arg.split("=", 1)[1].split(",") if value.strip()
            )
        index += 1

    if only_ids:
        return any(check_id.startswith("lint.") for check_id in only_ids)
    if groups:
        return "lint" in groups
    return invocation_name in {"checks", "lint"}


def run_quality_checks(
    bazel_real: str,
    args: Sequence[str],
    *,
    extra_bazel_options: Sequence[str] = (),
    runner: Callable[..., Any] = subprocess.run,
) -> QualityChecksResponse:
    """Run a quality-check pseudo-target and return whether it was handled."""

    invocation = _quality_checks_invocation(args)
    if invocation is None:
        return QualityChecksResponse(handled=False)

    invocation_name, program_args, startup_options, bazel_options = invocation
    command = [
        str(_venv_python(_target_venv())),
        str(REPO_ROOT / "buildscripts" / "quality_checks" / "cli.py"),
        "--invocation-name",
        invocation_name,
        "--bazel-real",
        bazel_real,
        *(f"--bazel-startup-option={option}" for option in startup_options),
        *(f"--bazel-option={option}" for option in bazel_options),
        *(f"--bazel-option={option}" for option in extra_bazel_options),
        *program_args,
    ]

    stdout = get_terminal_stream("MONGO_WRAPPER_STDOUT_FD") or sys.stdout
    stderr = get_terminal_stream("MONGO_WRAPPER_STDERR_FD") or sys.stderr
    try:
        completed = runner(command, cwd=REPO_ROOT, stdout=stdout, stderr=stderr, check=False)
        exit_code = int(completed.returncode)
    except KeyboardInterrupt:
        return QualityChecksResponse(handled=True, exit_code=130)
    except (OSError, ValueError) as exc:
        print(f"Unable to start the quality-check runner: {exc}", file=stderr)
        return QualityChecksResponse(handled=True, exit_code=2)

    if exit_code < 0:
        exit_code = 128 - exit_code
    # The legacy `lint` alias historically returned 3 for a lint finding.
    # Keep that mapping for callers of the compatibility alias; `checks`
    # exposes the unified runner's native exit code.
    if invocation_name == "lint" and exit_code == 1:
        exit_code = 3
    return QualityChecksResponse(handled=True, exit_code=exit_code)
