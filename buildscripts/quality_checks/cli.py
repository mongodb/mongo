#!/usr/bin/env python3
"""Command-line entry point for MongoDB repository quality checks."""

from __future__ import annotations

import argparse
import contextlib
import os
import sys
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import TextIO

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from buildscripts.quality_checks.adapters import CheckContext, SharedTools
from buildscripts.quality_checks.engine import CheckSelection, QualityChecksEngine, select_checks
from buildscripts.quality_checks.manifest import validate_manifest
from buildscripts.quality_checks.models import CandidateFile, CheckPhase, CheckStatus
from buildscripts.quality_checks.progress import TerminalUI
from buildscripts.quality_checks.selection import (
    DiscoveryResult,
    discover_changed_files,
    explicit_files,
)
from buildscripts.quality_checks.telemetry import TelemetryClient


def _telemetry_call(callback: Callable[..., object], /, *args: object, **kwargs: object) -> object:
    """Invoke telemetry without allowing it to influence command behavior."""

    try:
        return callback(*args, **kwargs)
    except Exception:
        return None


def _duplicate_progress_stream(stream: TextIO) -> TextIO | None:
    """Keep progress visible while in-process legacy tools redirect fd 1/2."""

    try:
        duplicate_fd = os.dup(stream.fileno())
    except (AttributeError, OSError, ValueError):
        return None
    try:
        return os.fdopen(
            duplicate_fd,
            "w",
            encoding=getattr(stream, "encoding", None) or "utf-8",
            errors="replace",
            buffering=1,
        )
    except Exception:
        os.close(duplicate_fd)
        return None


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bazel run checks --",
        description=(
            "Run applicable MongoDB formatting, lint, and CODEOWNERS checks through one "
            "changed-file-aware entry point."
        ),
    )
    parser.add_argument(
        "--invocation-name",
        choices=("checks", "format", "lint", "codeowners"),
        default="checks",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--bazel-real", default=os.environ.get("BAZEL_REAL", "bazel"), help=argparse.SUPPRESS
    )
    parser.add_argument(
        "--bazel-startup-option", action="append", default=[], help=argparse.SUPPRESS
    )
    parser.add_argument("--bazel-option", action="append", default=[], help=argparse.SUPPRESS)

    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--fix",
        dest="fix",
        action="store_true",
        help="Apply available fixes; run checks without fix support",
    )
    mode.add_argument(
        "--check",
        dest="fix",
        action="store_false",
        help="Only check; never modify repository files",
    )
    parser.set_defaults(fix=None)

    scope = parser.add_mutually_exclusive_group()
    scope.add_argument(
        "--all", action="store_true", help="Run selected checks over all repository files"
    )
    scope.add_argument(
        "--files",
        action="extend",
        nargs="+",
        metavar="PATH",
        help="Use exactly these repository-relative candidate paths; repeatable",
    )
    parser.add_argument(
        "--file", action="append", default=[], metavar="PATH", help=argparse.SUPPRESS
    )
    parser.add_argument(
        "--group",
        action="append",
        default=[],
        metavar="GROUP",
        help="Select a check group; repeatable (format, lint, codeowners, bazel)",
    )
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="CHECK_ID",
        help="Force stable check IDs; comma-separated or repeatable",
    )
    parser.add_argument(
        "--skip",
        action="append",
        default=[],
        metavar="CHECK_ID",
        help="Exclude stable check IDs; comma-separated or repeatable",
    )
    parser.add_argument(
        "--origin-branch",
        default="auto",
        help="Branch/ref used to find the merge base (default: auto)",
    )
    parser.add_argument(
        "--jobs", type=int, default=4, help="Maximum independent read-only checks (default: 4)"
    )
    parser.add_argument("--list", action="store_true", help="List stable check IDs and exit")
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Show successful check output and telemetry diagnostics",
    )
    parser.add_argument(
        "--show-skipped",
        action="store_true",
        help="Show non-applicable and dependency-blocked checks",
    )

    # Compatibility options formerly parsed by the lint pseudo-target.
    parser.add_argument("--lint-yaml-project", "--lint-yaml", dest="lint_yaml_project")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--fail-on-validation", action="store_true")
    parser.add_argument("--large-files", action="store_true")
    parser.add_argument("--keep-going", action="store_true", help=argparse.SUPPRESS)

    # Compatibility options accepted by the CODEOWNERS generator.
    parser.add_argument("--expansions-file")
    parser.add_argument("--branch", dest="codeowners_branch")
    parser.add_argument("--output-file")
    parser.add_argument("--repo-dir")
    parser.add_argument("--run-validation", action="store_true")
    parser.add_argument("--check-new-files", action="store_true")

    parser.add_argument("lint_labels", nargs="*", help=argparse.SUPPRESS)
    return parser


def _csv(values: Sequence[str]) -> set[str]:
    return {item.strip() for value in values for item in value.split(",") if item.strip()}


def _truthy(value: str | None) -> bool:
    return bool(value and value.lower() not in {"0", "false", "no", "off"})


def _alias_default_groups(invocation_name: str, groups: list[str]) -> list[str]:
    if groups:
        return groups
    if invocation_name in {"format", "lint", "codeowners"}:
        return [invocation_name]
    return []


def _lint_arguments(args: argparse.Namespace, unknown: list[str]) -> tuple[str, ...]:
    lint_args: list[str] = []
    if args.lint_yaml_project:
        lint_args.append(f"--lint-yaml-project={args.lint_yaml_project}")
    if args.dry_run:
        lint_args.append("--dry-run")
    if args.fail_on_validation:
        lint_args.append("--fail-on-validation")
    if args.large_files:
        lint_args.append("--large-files")
    if args.keep_going:
        lint_args.append("--keep-going")
    lint_args.extend(args.lint_labels)
    lint_args.extend(unknown)
    return tuple(lint_args)


def _codeowners_arguments(args: argparse.Namespace) -> tuple[str, ...]:
    result: list[str] = []
    if args.output_file:
        result.extend(("--output-file", args.output_file))
    if args.repo_dir:
        result.extend(("--repo-dir", args.repo_dir))
    if args.run_validation:
        result.append("--run-validation")
    if args.check_new_files:
        result.append("--check-new-files")
    return tuple(result)


def _print_checks(args: argparse.Namespace, groups: list[str], stdout: TextIO) -> int:
    try:
        selection = select_checks((), groups=groups, only=args.only, skip=args.skip, all_files=True)
    except ValueError as exc:
        print(f"error: {exc}", file=stdout)
        return 2
    for check in selection.checks:
        fix = " [fix]" if check.spec.supports_fix else ""
        print(
            f"{check.spec.check_id:<28} {check.spec.group:<10} {check.spec.display_name}{fix}",
            file=stdout,
        )
    return 0


def _discover(args: argparse.Namespace, *, force_all: bool = False) -> DiscoveryResult:
    requested_files = [*(args.files or []), *args.file]
    if (args.all or force_all) and requested_files:
        raise ValueError("--all and --files/--file are mutually exclusive")
    if requested_files:
        origin = "HEAD" if args.origin_branch == "auto" else args.origin_branch
        return DiscoveryResult(explicit_files(REPO_ROOT, requested_files), origin)
    return discover_changed_files(
        REPO_ROOT, origin_branch=args.origin_branch, force_all=args.all or force_all
    )


def _selection_needs_buildozer(selected_ids: set[str], discovery: DiscoveryResult) -> bool:
    if selected_ids & {
        "lint.clang-tidy-config",
        "lint.bazel-groups",
        "lint.private-headers",
    }:
        return True
    return "lint.duplicate-library" in selected_ids


def _parse_args(
    argv: list[str], stdout: TextIO, stderr: TextIO
) -> tuple[argparse.Namespace | None, list[str], int | None]:
    parser = _parser()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        try:
            args, unknown = parser.parse_known_args(argv)
        except SystemExit as exc:
            return None, [], int(exc.code or 0)
    if args.jobs < 1:
        print("error: --jobs must be at least 1", file=stderr)
        return None, [], 2
    return args, unknown, None


def main(
    argv: list[str] | None = None,
    *,
    stdout: TextIO | None = None,
    stderr: TextIO | None = None,
) -> int:
    stdout = stdout or sys.stdout
    stderr = stderr or sys.stderr
    args, unknown, parse_exit = _parse_args(
        list(sys.argv[1:] if argv is None else argv), stdout, stderr
    )
    if parse_exit is not None:
        return parse_exit
    assert args is not None

    groups = _alias_default_groups(args.invocation_name, args.group)
    selected_group_names = _csv(groups)
    only_ids = _csv(args.only)
    lint_all_label = any(label in {"...", "//..."} for label in args.lint_labels)
    lint_compatibility = (
        args.invocation_name == "lint"
        or selected_group_names == {"lint"}
        or bool(only_ids)
        and all(check_id.startswith("lint.") for check_id in only_ids)
    )
    unexpected = [*unknown, *([] if lint_compatibility else args.lint_labels)]
    if unexpected:
        print(f"error: unrecognized arguments: {' '.join(unexpected)}", file=stderr)
        return 2
    if args.list:
        return _print_checks(args, groups, stdout)

    fix = args.fix
    if fix is None:
        fix = args.invocation_name in {"format", "codeowners"}

    telemetry = TelemetryClient.create(REPO_ROOT)
    preparation_started = time.monotonic()
    shared_tools = SharedTools()
    try:
        validate_manifest()
        discovery = _discover(args, force_all=lint_all_label)
        if discovery.warning:
            print(f"WARNING: {discovery.warning}", file=stderr)
        lint_args = _lint_arguments(args, unknown)
        only = list(args.only)
        if args.invocation_name == "codeowners" and not only:
            only = [
                "codeowners.local"
                if "codeowners.github" in _csv(args.skip)
                else "codeowners.github"
            ]
        # An explicit lint label adds rules_lint even when there are no
        # changed files matching its predicates, while preserving other
        # applicable lint checks.
        forced = []
        if not only:
            if args.lint_labels:
                forced.append("lint.rules-lint")
            if args.large_files and (not selected_group_names or "lint" in selected_group_names):
                forced.append("lint.file-size")
        selection = select_checks(
            discovery.candidates,
            groups=groups,
            only=only,
            force=forced,
            skip=args.skip,
            all_files=discovery.selected_all,
        )
        selected_ids = {check.spec.check_id for check in selection.checks}
        if _selection_needs_buildozer(selected_ids, discovery):
            _ = shared_tools.buildozer
    except Exception as exc:
        duration = time.monotonic() - preparation_started
        _telemetry_call(
            telemetry.record_preparation,
            duration_seconds=duration,
            status="FAIL",
            # Exception messages can contain absolute paths. Keep telemetry
            # diagnostic-only without leaking local filesystem details.
            detail=type(exc).__name__,
        )
        _telemetry_call(
            telemetry.finish,
            completed_checks=0,
            failed_checks=1,
            serial_checks=0,
            parallel_checks=0,
            wallclock_seconds=duration,
            phase_timings={"preparation": duration},
            verbose=args.verbose,
        )
        print(f"quality-check preparation failed: {exc}", file=stderr)
        return 2

    preparation_duration = time.monotonic() - preparation_started
    _telemetry_call(
        telemetry.set_invocation_attributes,
        {
            "mongo.quality_checks.invocation_name": args.invocation_name,
            "mongo.quality_checks.mode": "fix" if fix else "check",
            "mongo.quality_checks.origin_branch": discovery.origin_branch,
            "mongo.quality_checks.groups": sorted(
                selected_group_names or {check.spec.group for check in selection.checks}
            ),
            "mongo.quality_checks.selected_ids": [
                check.spec.check_id for check in selection.checks
            ],
        },
    )
    _telemetry_call(
        telemetry.record_candidate_files,
        (path for candidate in discovery.candidates for path in candidate.selection_paths),
        trigger_mode="all"
        if discovery.selected_all
        else "files"
        if (args.files or args.file)
        else "changed",
        candidate_count=len(discovery.candidates),
    )
    _telemetry_call(telemetry.record_preparation, duration_seconds=preparation_duration)

    context = CheckContext(
        repo_root=REPO_ROOT,
        bazel_real=args.bazel_real,
        candidates=discovery.candidates,
        all_files=discovery.selected_all,
        fix=fix,
        origin_branch=discovery.origin_branch,
        invocation_name=args.invocation_name,
        explicit_groups=frozenset(
            selected_group_names
            | (
                {"codeowners"}
                if any(item.startswith("codeowners.") for item in _csv(only))
                else set()
            )
        ),
        lint_args=lint_args,
        codeowners_args=_codeowners_arguments(args),
        bazel_startup_options=tuple(args.bazel_startup_option),
        bazel_options=tuple(args.bazel_option),
        expansions_file=args.expansions_file,
        codeowners_branch=args.codeowners_branch,
        is_ci=_truthy(os.environ.get("CI")),
        shared_tools=shared_tools,
    )

    def rediscover() -> tuple[CandidateFile, ...]:
        shared_tools.clear_lint_state()
        refreshed = _discover(args, force_all=lint_all_label)
        return refreshed.candidates

    def reselect(candidates: tuple[CandidateFile, ...]) -> CheckSelection:
        return select_checks(
            candidates,
            groups=groups,
            only=only,
            force=forced,
            skip=args.skip,
            all_files=discovery.selected_all,
        )

    total = len(selection.checks) + len(selection.skipped)
    protected_progress_stream = _duplicate_progress_stream(stderr)
    ui = TerminalUI(
        total,
        stream=protected_progress_stream or stderr,
        show_skipped=args.show_skipped,
    )
    engine = QualityChecksEngine(
        context=context,
        selection=selection,
        ui=ui,
        telemetry=telemetry,
        jobs=args.jobs,
        verbose=args.verbose,
        rediscover=rediscover,
        reselect=reselect,
    )
    started = time.monotonic()
    try:
        outcome = engine.run(fix=fix)
    except KeyboardInterrupt:
        ui.finish()
        if protected_progress_stream is not None:
            protected_progress_stream.close()
        return 130
    except Exception as exc:
        ui.finish()
        wallclock = time.monotonic() - started
        print(f"quality-check orchestration failed: {type(exc).__name__}: {exc}", file=stderr)
        _telemetry_call(
            telemetry.finish,
            completed_checks=len(engine.results),
            failed_checks=1,
            serial_checks=len(engine.results),
            parallel_checks=0,
            wallclock_seconds=wallclock + preparation_duration,
            phase_timings={"preparation": preparation_duration, **engine.timer.timings},
            verbose=args.verbose,
        )
        if protected_progress_stream is not None:
            protected_progress_stream.close()
        return 2
    wallclock = time.monotonic() - started
    ui.print_final_summary()
    if protected_progress_stream is not None:
        protected_progress_stream.close()

    failed = sum(result.status == CheckStatus.FAIL for result in outcome.results)
    parallel = sum(result.phase == CheckPhase.SAFE_PARALLEL for result in outcome.results)
    phase_timings = {"preparation": preparation_duration, **outcome.phase_timings}
    if args.verbose:
        timings = ", ".join(f"{name}={elapsed:.3f}s" for name, elapsed in phase_timings.items())
        print(f"Phase timings: {timings}", file=stderr)
    log_path = _telemetry_call(
        telemetry.finish,
        completed_checks=len(outcome.results),
        failed_checks=failed,
        serial_checks=len(outcome.results) - parallel,
        parallel_checks=parallel,
        wallclock_seconds=wallclock + preparation_duration,
        phase_timings=phase_timings,
        verbose=args.verbose,
    )
    if args.verbose and log_path is not None:
        print(f"Telemetry exporter log: {log_path}", file=stderr)
    return outcome.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
