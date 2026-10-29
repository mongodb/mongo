"""Typed models shared by the quality-checks runner."""

from __future__ import annotations

import time
from collections.abc import Callable, Iterator, Mapping
from contextlib import contextmanager
from dataclasses import dataclass, field
from enum import StrEnum
from types import MappingProxyType


class ChangeStatus(StrEnum):
    """Git status for a candidate path."""

    ADDED = "A"
    COPIED = "C"
    DELETED = "D"
    MODIFIED = "M"
    RENAMED = "R"
    TYPE_CHANGED = "T"
    UNMERGED = "U"
    UNTRACKED = "?"


@dataclass(frozen=True, slots=True)
class CandidateFile:
    """One repository path which can select quality checks."""

    path: str
    status: ChangeStatus
    old_path: str | None = None
    exists: bool = True

    @property
    def selection_paths(self) -> tuple[str, ...]:
        """Paths which should participate in trigger matching."""

        if self.old_path and self.old_path != self.path:
            return (self.old_path, self.path)
        return (self.path,)


class CheckPhase(StrEnum):
    """Scheduling phase for a check."""

    PREFLIGHT_SERIAL = "preflight_serial"
    MUTATING_SERIAL = "mutating_serial"
    SAFE_PARALLEL = "safe_parallel"
    SHARED_STATE_SERIAL = "shared_state_serial"


class CheckStatus(StrEnum):
    """Normalized outcome of a check."""

    PASS = "PASS"
    FAIL = "FAIL"
    SKIPPED = "SKIPPED"


@dataclass(frozen=True, slots=True)
class CheckSpec:
    """Stable metadata and scheduling policy for one check."""

    check_id: str
    display_name: str
    group: str
    phase: CheckPhase
    description: str = ""
    dependencies: tuple[str, ...] = ()
    supports_fix: bool = False

    def __post_init__(self) -> None:
        if not self.check_id.strip():
            raise ValueError("check_id must not be empty")
        if not self.display_name.strip():
            raise ValueError("display_name must not be empty")
        if not self.group.strip():
            raise ValueError("group must not be empty")
        if self.check_id in self.dependencies:
            raise ValueError(f"{self.check_id} cannot depend on itself")


@dataclass(frozen=True, slots=True)
class CheckResult:
    """Completed outcome for one execution of a check."""

    spec: CheckSpec
    status: CheckStatus
    duration_seconds: float
    finished_time_ns: int
    exit_code: int | None = None
    matched_file_count: int | None = None
    matched_files: tuple[str, ...] = ()
    operation: str = "check"
    detail: str = field(default="", repr=False)

    def __post_init__(self) -> None:
        if self.duration_seconds < 0:
            raise ValueError("duration_seconds must be nonnegative")
        if self.matched_file_count is not None and self.matched_file_count < 0:
            raise ValueError("matched_file_count must be nonnegative")
        if self.operation not in {"fix", "check"}:
            raise ValueError("operation must be 'fix' or 'check'")

    @property
    def check_id(self) -> str:
        return self.spec.check_id

    @property
    def display_name(self) -> str:
        return self.spec.display_name

    @property
    def phase(self) -> CheckPhase:
        return self.spec.phase


class PhaseTimer:
    """Accumulates monotonic elapsed time by named phase."""

    def __init__(self, *, clock: Callable[[], float] | None = None) -> None:
        self._clock = clock or time.monotonic
        self._timings: dict[str, float] = {}

    def record_elapsed(self, phase_name: str, elapsed_seconds: float) -> None:
        if elapsed_seconds < 0:
            raise ValueError("elapsed_seconds must be nonnegative")
        self._timings[phase_name] = self._timings.get(phase_name, 0.0) + elapsed_seconds

    @contextmanager
    def record(self, phase_name: str) -> Iterator[None]:
        start = self._clock()
        try:
            yield
        finally:
            self.record_elapsed(phase_name, self._clock() - start)

    @property
    def timings(self) -> Mapping[str, float]:
        return MappingProxyType(dict(self._timings))
