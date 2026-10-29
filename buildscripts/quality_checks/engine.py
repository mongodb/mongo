"""Selection and dependency-aware execution for MongoDB quality checks."""

from __future__ import annotations

import time
from collections.abc import Callable, Iterable, Mapping
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass

from buildscripts.quality_checks.adapters import CheckContext, RunOutcome
from buildscripts.quality_checks.manifest import CHECKS, RegisteredCheck, validate_manifest
from buildscripts.quality_checks.models import (
    CandidateFile,
    CheckPhase,
    CheckResult,
    CheckStatus,
    PhaseTimer,
)
from buildscripts.quality_checks.progress import TerminalUI
from buildscripts.quality_checks.telemetry import TelemetryClient


@dataclass(frozen=True, slots=True)
class CheckSelection:
    """Checks selected for execution and checks excluded with an explanation."""

    checks: tuple[RegisteredCheck, ...]
    skipped: tuple[tuple[RegisteredCheck, str], ...] = ()
    unavailable_dependencies: Mapping[str, tuple[str, ...]] | None = None


@dataclass(frozen=True, slots=True)
class EngineResult:
    """Normalized result of one invocation."""

    exit_code: int
    results: tuple[CheckResult, ...]
    phase_timings: Mapping[str, float]


def _split_values(values: Iterable[str]) -> set[str]:
    return {item.strip() for value in values for item in value.split(",") if item.strip()}


def _topological_order(
    selected_ids: set[str], checks: tuple[RegisteredCheck, ...]
) -> tuple[RegisteredCheck, ...]:
    by_id = {check.spec.check_id: check for check in checks}
    ordered: list[RegisteredCheck] = []
    visited: set[str] = set()

    def visit(check_id: str) -> None:
        if check_id in visited:
            return
        visited.add(check_id)
        for dependency in by_id[check_id].spec.dependencies:
            if dependency in selected_ids:
                visit(dependency)
        ordered.append(by_id[check_id])

    for check in checks:
        if check.spec.check_id in selected_ids:
            visit(check.spec.check_id)
    return tuple(ordered)


def select_checks(
    candidates: tuple[CandidateFile, ...],
    *,
    groups: Iterable[str] = (),
    only: Iterable[str] = (),
    force: Iterable[str] = (),
    skip: Iterable[str] = (),
    all_files: bool = False,
    checks: tuple[RegisteredCheck, ...] = CHECKS,
) -> CheckSelection:
    """Select applicable checks and automatically include their dependencies."""

    validate_manifest(checks)
    by_id = {check.spec.check_id: check for check in checks}
    known_groups = {check.spec.group for check in checks}
    requested_groups = _split_values(groups)
    only_ids = _split_values(only)
    forced_ids = _split_values(force)
    skipped_ids = _split_values(skip)

    unknown_groups = requested_groups - known_groups
    if unknown_groups:
        raise ValueError(f"unknown quality-check groups: {', '.join(sorted(unknown_groups))}")
    unknown_ids = (only_ids | forced_ids | skipped_ids) - set(by_id)
    if unknown_ids:
        raise ValueError(f"unknown quality-check ids: {', '.join(sorted(unknown_ids))}")
    overlap = (only_ids | forced_ids) & skipped_ids
    if overlap:
        raise ValueError(
            f"checks cannot be both selected and skipped: {', '.join(sorted(overlap))}"
        )

    scope = requested_groups or known_groups
    skipped_reasons: dict[str, str] = {}
    if only_ids:
        selected_ids = set(only_ids)
    else:
        selected_ids = set()
        for check in checks:
            if check.spec.group not in scope:
                continue
            if all_files or any(check.matcher(candidate) for candidate in candidates):
                selected_ids.add(check.spec.check_id)
            else:
                skipped_reasons[check.spec.check_id] = "no matching changed files"
        for check_id in forced_ids:
            selected_ids.add(check_id)
            skipped_reasons.pop(check_id, None)

    # Dependencies are part of the execution graph even when they do not have a
    # matching candidate of their own. This is especially important for --only.
    pending = list(selected_ids)
    while pending:
        check_id = pending.pop()
        for dependency in by_id[check_id].spec.dependencies:
            if dependency not in selected_ids:
                selected_ids.add(dependency)
                pending.append(dependency)
                skipped_reasons.pop(dependency, None)

    unavailable: dict[str, tuple[str, ...]] = {}
    for check_id in tuple(selected_ids):
        missing = tuple(dep for dep in by_id[check_id].spec.dependencies if dep in skipped_ids)
        if missing:
            unavailable[check_id] = missing

    for check_id in skipped_ids:
        if check_id in selected_ids:
            selected_ids.remove(check_id)
        skipped_reasons[check_id] = "excluded by --skip"

    ordered = _topological_order(selected_ids, checks)
    skipped_checks = tuple(
        (check, skipped_reasons[check.spec.check_id])
        for check in checks
        if check.spec.check_id in skipped_reasons
        and (check.spec.group in scope or check.spec.check_id in skipped_ids)
    )
    return CheckSelection(ordered, skipped_checks, unavailable)


def matched_candidates(
    check: RegisteredCheck, candidates: tuple[CandidateFile, ...], *, all_files: bool
) -> tuple[CandidateFile, ...]:
    del all_files  # --all controls selection, not a check's matched-path telemetry.
    return tuple(candidate for candidate in candidates if check.matcher(candidate))


def runnable_paths(candidates: Iterable[CandidateFile]) -> tuple[str, ...]:
    """Return current paths only; deleted and old rename paths merely trigger checks."""

    return tuple(dict.fromkeys(candidate.path for candidate in candidates if candidate.exists))


def telemetry_paths(candidates: Iterable[CandidateFile]) -> tuple[str, ...]:
    return tuple(
        dict.fromkeys(path for candidate in candidates for path in candidate.selection_paths)
    )


class QualityChecksEngine:
    """Run each selected check exactly once in fix or verification mode."""

    _CHECK_PHASES = (
        CheckPhase.PREFLIGHT_SERIAL,
        CheckPhase.MUTATING_SERIAL,
        CheckPhase.SAFE_PARALLEL,
        CheckPhase.SHARED_STATE_SERIAL,
    )

    def __init__(
        self,
        *,
        context: CheckContext,
        selection: CheckSelection,
        ui: TerminalUI,
        telemetry: TelemetryClient,
        jobs: int = 4,
        verbose: bool = False,
        rediscover: Callable[[], tuple[CandidateFile, ...]] | None = None,
        reselect: Callable[[tuple[CandidateFile, ...]], CheckSelection] | None = None,
    ) -> None:
        self.context = context
        self.selection = selection
        self.ui = ui
        self.telemetry = telemetry
        self.jobs = max(1, min(jobs, 64))
        self.verbose = verbose
        self.rediscover = rediscover
        self.reselect = reselect
        self.results: list[CheckResult] = []
        self.timer = PhaseTimer()
        self._fatal = False
        self._check_status: dict[str, CheckStatus] = {}
        self._fix_status: dict[str, CheckStatus] = {}

    def _blocked_dependencies(self, check: RegisteredCheck, operation: str) -> tuple[str, ...]:
        explicitly_unavailable = (self.selection.unavailable_dependencies or {}).get(
            check.spec.check_id, ()
        )
        if operation == "fix":
            # Preflight checks run before fixers and are recorded as checks,
            # so a failed preflight must also block a dependent fixer.
            statuses = {**self._check_status, **self._fix_status}
        else:
            # Fixers are not run again in check mode. Their fix result still
            # determines whether dependent verification checks can run.
            statuses = {**self._fix_status, **self._check_status}
        failed = tuple(
            dependency
            for dependency in check.spec.dependencies
            if statuses.get(dependency) in {CheckStatus.FAIL, CheckStatus.SKIPPED}
        )
        return tuple(dict.fromkeys((*explicitly_unavailable, *failed)))

    def _record_skipped(
        self,
        check: RegisteredCheck,
        reason: str,
        *,
        operation: str = "check",
        count_toward_dependencies: bool = True,
    ) -> CheckResult:
        result = CheckResult(
            check.spec,
            CheckStatus.SKIPPED,
            0.0,
            time.time_ns(),
            matched_file_count=0,
            operation=operation,
            detail=reason,
        )
        self.results.append(result)
        if count_toward_dependencies and operation == "check":
            self._check_status[check.spec.check_id] = CheckStatus.SKIPPED
        elif count_toward_dependencies and operation == "fix":
            self._fix_status[check.spec.check_id] = CheckStatus.SKIPPED
        self.ui.record_result(result)
        if self.ui.show_skipped and reason:
            self.ui.print_details(reason)
        try:
            self.telemetry.record_result(result)
        except Exception:
            # Reporting must never alter the quality-check result.
            pass
        return result

    def _execute(self, check: RegisteredCheck, operation: str) -> CheckResult:
        candidates = matched_candidates(
            check, self.context.candidates, all_files=self.context.all_files
        )
        paths = runnable_paths(candidates)
        sampled_paths = telemetry_paths(candidates)
        started = time.monotonic()
        self.ui.record_check_started(check.spec, operation=operation)
        try:
            outcome = check.runner(self.context, paths)
        except BaseException as exc:  # An adapter exception is an orchestration failure.
            if isinstance(exc, (KeyboardInterrupt, SystemExit)):
                raise
            outcome = RunOutcome(
                2,
                f"quality-check adapter {check.spec.check_id} raised "
                f"{type(exc).__name__}: {exc}\n",
                fatal=True,
            )
        duration = time.monotonic() - started
        if outcome.skipped_reason is not None:
            status = CheckStatus.SKIPPED
        else:
            status = CheckStatus.PASS if outcome.returncode == 0 else CheckStatus.FAIL
        result = CheckResult(
            check.spec,
            status,
            duration,
            time.time_ns(),
            exit_code=outcome.returncode,
            matched_file_count=len(candidates),
            matched_files=sampled_paths,
            operation=operation,
            detail=outcome.skipped_reason or outcome.output,
        )
        if outcome.fatal:
            self._fatal = True
        return result

    def _publish(self, result: CheckResult) -> None:
        self.results.append(result)
        if result.operation == "check":
            self._check_status[result.check_id] = result.status
        elif result.operation == "fix":
            self._fix_status[result.check_id] = result.status
        self.ui.record_result(result)
        if result.detail and (
            self.verbose
            or result.status == CheckStatus.FAIL
            or (result.status == CheckStatus.SKIPPED and self.ui.show_skipped)
        ):
            self.ui.print_details(result.detail)
        try:
            self.telemetry.record_result(result)
        except Exception:
            # Reporting must never alter the quality-check result.
            pass

    def _run_serial(self, checks: Iterable[RegisteredCheck], operation: str) -> None:
        for check in checks:
            blocked = self._blocked_dependencies(check, operation)
            if blocked:
                self._record_skipped(
                    check,
                    f"blocked by failed or skipped dependencies: {', '.join(blocked)}",
                    operation=operation,
                )
                continue
            self._publish(self._execute(check, operation))

    def _run_parallel(self, checks: tuple[RegisteredCheck, ...]) -> None:
        remaining = list(checks)
        phase_ids = {check.spec.check_id for check in checks}
        with ThreadPoolExecutor(max_workers=self.jobs, thread_name_prefix="quality-check") as pool:
            while remaining:
                remaining_ids = {check.spec.check_id for check in remaining}
                ready = [
                    check
                    for check in remaining
                    if not any(
                        dependency in phase_ids and dependency in remaining_ids
                        for dependency in check.spec.dependencies
                    )
                ]
                if not ready:
                    # Manifest cycle validation should make this unreachable.
                    self._fatal = True
                    for check in remaining:
                        self._record_skipped(check, "unschedulable parallel dependency graph")
                    return

                for check in ready:
                    remaining.remove(check)

                runnable: list[RegisteredCheck] = []
                for check in ready:
                    blocked = self._blocked_dependencies(check, "check")
                    if blocked:
                        self._record_skipped(
                            check,
                            f"blocked by failed or skipped dependencies: {', '.join(blocked)}",
                        )
                    else:
                        runnable.append(check)
                if not runnable:
                    continue

                # Start and publish each dependency level in manifest order so
                # redirected output remains deterministic.
                futures: list[tuple[RegisteredCheck, Future[CheckResult]]] = []
                for check in runnable:
                    candidates = matched_candidates(
                        check, self.context.candidates, all_files=self.context.all_files
                    )
                    paths = runnable_paths(candidates)
                    sampled_paths = telemetry_paths(candidates)
                    self.ui.record_check_started(check.spec)

                    def invoke(
                        selected: RegisteredCheck = check,
                        selected_paths: tuple[str, ...] = paths,
                        selected_samples: tuple[str, ...] = sampled_paths,
                        matched_count: int = len(candidates),
                    ) -> CheckResult:
                        started = time.monotonic()
                        try:
                            outcome = selected.runner(self.context, selected_paths)
                        except BaseException as exc:
                            if isinstance(exc, (KeyboardInterrupt, SystemExit)):
                                raise
                            outcome = RunOutcome(
                                2,
                                f"quality-check adapter {selected.spec.check_id} raised "
                                f"{type(exc).__name__}: {exc}\n",
                                fatal=True,
                            )
                        duration = time.monotonic() - started
                        status = (
                            CheckStatus.SKIPPED
                            if outcome.skipped_reason is not None
                            else CheckStatus.PASS
                            if outcome.returncode == 0
                            else CheckStatus.FAIL
                        )
                        if outcome.fatal:
                            self._fatal = True
                        return CheckResult(
                            selected.spec,
                            status,
                            duration,
                            time.time_ns(),
                            exit_code=outcome.returncode,
                            matched_file_count=matched_count,
                            matched_files=selected_samples,
                            detail=outcome.skipped_reason or outcome.output,
                        )

                    futures.append((check, pool.submit(invoke)))
                for _check, future in futures:
                    self._publish(future.result())

    def run(self, *, fix: bool) -> EngineResult:
        original_fix = self.context.fix
        completed_check_ids: set[str] = set()
        try:
            self.ui.start()
            self.context.fix = False
            preflight_checks = tuple(
                check
                for check in self.selection.checks
                if check.spec.phase == CheckPhase.PREFLIGHT_SERIAL
            )
            if preflight_checks:
                with self.timer.record(CheckPhase.PREFLIGHT_SERIAL.value):
                    self._run_serial(preflight_checks, "check")
                completed_check_ids.update(check.spec.check_id for check in preflight_checks)
            if fix:
                self.context.fix = True
                fixers = tuple(
                    check
                    for check in self.selection.checks
                    if check.spec.supports_fix and check.spec.check_id not in completed_check_ids
                )
                with self.timer.record("fix"):
                    self._run_serial(fixers, "fix")
                completed_check_ids.update(check.spec.check_id for check in fixers)
                if self.rediscover is not None:
                    with self.timer.record("rediscovery"):
                        self.context.candidates = self.rediscover()
                        if self.reselect is not None:
                            self.selection = self.reselect(self.context.candidates)
                remaining_checks = sum(
                    check.spec.check_id not in completed_check_ids
                    for check in self.selection.checks
                )
                remaining_skipped = sum(
                    check.spec.check_id not in completed_check_ids
                    for check, _reason in self.selection.skipped
                )
                self.ui.set_total_checks(len(self.results) + remaining_checks + remaining_skipped)

            for check, reason in self.selection.skipped:
                if check.spec.check_id in completed_check_ids:
                    continue
                self._record_skipped(check, reason, count_toward_dependencies=False)

            self.context.fix = False
            for phase in self._CHECK_PHASES:
                if phase == CheckPhase.PREFLIGHT_SERIAL:
                    continue
                phase_checks = tuple(
                    check
                    for check in self.selection.checks
                    if check.spec.phase == phase and check.spec.check_id not in completed_check_ids
                )
                if not phase_checks:
                    continue
                with self.timer.record(phase.value):
                    if phase == CheckPhase.SAFE_PARALLEL:
                        self._run_parallel(phase_checks)
                    else:
                        self._run_serial(phase_checks, "check")
                completed_check_ids.update(check.spec.check_id for check in phase_checks)
        finally:
            self.context.fix = original_fix
            self.ui.finish()

        failed = sum(result.status == CheckStatus.FAIL for result in self.results)
        exit_code = 2 if self._fatal else 1 if failed else 0
        return EngineResult(exit_code, tuple(self.results), self.timer.timings)
