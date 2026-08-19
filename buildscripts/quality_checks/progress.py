"""TTY and non-TTY progress reporting for quality checks."""

from __future__ import annotations

import sys
import threading
import time
from collections.abc import Callable
from typing import Literal, TextIO

from buildscripts.quality_checks.models import CheckResult, CheckSpec, CheckStatus

_SPINNER = ("⠋", "⠙", "⠹", "⠸")
_CLEAR_LINE = "\r\x1b[2K"
_HIDE_CURSOR = "\x1b[?25l"
_SHOW_CURSOR = "\x1b[?25h"


class TerminalUI:
    """Render concise check progress without mixing successful tool chatter."""

    def __init__(
        self,
        total_checks: int,
        color_mode: Literal["auto", "always", "never"] = "auto",
        *,
        stream: TextIO | None = None,
        clock: Callable[[], float] | None = None,
        show_skipped: bool = False,
    ) -> None:
        self.total_checks = total_checks
        self.stream = stream or sys.stderr
        self.clock = clock or time.monotonic
        self.show_skipped = show_skipped
        self._start_time = self.clock()
        self._running: dict[str, tuple[str, float]] = {}
        self._completed = 0
        self._failed = 0
        self._skipped = 0
        self._spinner_index = 0
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._finished = False
        self.is_tty = bool(getattr(self.stream, "isatty", lambda: False)())
        self.color = color_mode == "always" or (color_mode == "auto" and self.is_tty)

    def _paint(self, text: str, color: str) -> str:
        return f"\x1b[{color}m{text}\x1b[0m" if self.color else text

    def start(self) -> None:
        if not self.is_tty or self._thread is not None:
            return
        self.stream.write(_HIDE_CURSOR)
        self.stream.flush()
        self._thread = threading.Thread(target=self._spinner_loop, daemon=True)
        self._thread.start()

    def _spinner_loop(self) -> None:
        try:
            while not self._stop.wait(0.1):
                self.tick()
        finally:
            with self._lock:
                self.stream.write(_CLEAR_LINE + _SHOW_CURSOR)
                self.stream.flush()

    def _footer(self) -> str:
        elapsed = self.clock() - self._start_time
        active = ""
        if self._running:
            name, started = min(self._running.values(), key=lambda item: item[1])
            label = "Longest-running" if len(self._running) > 1 else "Running"
            active = f" · {label}: {name} ({self.clock() - started:.1f}s)"
        spinner = _SPINNER[self._spinner_index % len(_SPINNER)]
        return f"{spinner} {self._completed}/{self.total_checks} checks · {elapsed:.1f}s{active}"

    def tick(self) -> None:
        if not self.is_tty or self._finished:
            return
        with self._lock:
            self._spinner_index += 1
            self.stream.write(_CLEAR_LINE + self._footer())
            self.stream.flush()

    def _erase_footer(self) -> None:
        if self.is_tty:
            self.stream.write(_CLEAR_LINE)

    def _restore_footer(self) -> None:
        if self.is_tty and not self._finished:
            self.stream.write(self._footer())
        self.stream.flush()

    def set_total_checks(self, total_checks: int) -> None:
        """Adjust the total after fix-mode candidate reselection."""

        with self._lock:
            self.total_checks = max(self._completed, total_checks)

    def record_check_started(self, check: CheckSpec, *, operation: str = "check") -> None:
        display = check.display_name + (" [fix]" if operation == "fix" else "")
        with self._lock:
            self._running[f"{check.check_id}:{operation}"] = (display, self.clock())
            if not self.is_tty:
                self.stream.write(f"RUNNING: {display}\n")
                self.stream.flush()

    def record_result(self, result: CheckResult) -> None:
        display = result.display_name + (" [fix]" if result.operation == "fix" else "")
        suffix = f"{result.duration_seconds:.1f}s"
        if result.matched_file_count is not None:
            suffix += f", {result.matched_file_count} files"
        line = f"{result.status.value}: {display} ({suffix})"
        if result.status == CheckStatus.FAIL:
            # Keep a conventional error token in non-TTY logs for automated log scanners while
            # retaining the quality-check status in the same line.
            line = f"ERROR: {line}"
        color = {
            CheckStatus.PASS: "32",
            CheckStatus.FAIL: "31",
            CheckStatus.SKIPPED: "36",
        }[result.status]
        with self._lock:
            self._running.pop(f"{result.check_id}:{result.operation}", None)
            self._completed += 1
            self._failed += result.status == CheckStatus.FAIL
            self._skipped += result.status == CheckStatus.SKIPPED
            if result.status != CheckStatus.SKIPPED or self.show_skipped:
                self._erase_footer()
                self.stream.write(self._paint(line, color) + "\n")
                self._restore_footer()

    def record_skipped_check(self, check: CheckSpec, reason: str) -> None:
        self.record_result(
            CheckResult(check, CheckStatus.SKIPPED, 0.0, time.time_ns(), detail=reason)
        )
        if self.show_skipped and reason:
            self.print_details(reason)

    def print_details(self, details: str) -> None:
        if not details:
            return
        with self._lock:
            self._erase_footer()
            self.stream.write(details.rstrip() + "\n")
            self._restore_footer()

    def print_banner(self, text: str) -> None:
        with self._lock:
            self._erase_footer()
            self.stream.write(text.rstrip() + "\n")
            self._restore_footer()

    def finish(self) -> None:
        if self._finished:
            return
        self._finished = True
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1)
        elif self.is_tty:
            self.stream.write(_CLEAR_LINE + _SHOW_CURSOR)
        self.stream.flush()

    def print_final_summary(self) -> None:
        elapsed = self.clock() - self._start_time
        ran = self._completed - self._skipped
        if ran == 0:
            self.stream.write("No applicable checks.\n")
            self.stream.flush()
            return
        noun = "check" if ran == 1 else "checks"
        summary = f"Ran {ran} {noun} in {elapsed:.1f}s"
        if self._failed:
            summary += f"; {self._failed} failed"
        if self._skipped:
            summary += f"; {self._skipped} skipped"
        self.stream.write(summary + "\n")
        self.stream.flush()
