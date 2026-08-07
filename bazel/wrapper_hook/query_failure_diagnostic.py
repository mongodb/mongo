"""Add actionable diagnostics for known Bazel query failures."""

from __future__ import annotations

import errno
import os
import shlex
import subprocess
import sys
from collections.abc import Callable, Sequence
from typing import TextIO

_CONDITIONS_PACKAGE_ERROR = b"no such package 'conditions'"
_CONDITIONS_PACKAGE_ERRORS = (
    _CONDITIONS_PACKAGE_ERROR,
    b'no such package "conditions"',
)
_QUERY_ERROR_CONTEXT = (
    b"evaluation of query",
    b"preloading transitive closure",
)
_DIAGNOSTIC = """WARNING: Known Bazel query limitation detected.

The legacy `bazel query` command cannot evaluate configuration-dependent
`select()` branches. In this graph, Bazel is reporting the special
`//conditions:default` select label as if the `conditions` package were
missing.

Try the configured query instead:

  {cquery_command}

`cquery` evaluates one concrete configuration, so its results can differ
from the all-configurations graph that `query` attempts to load.
"""


class _ChunkMatcher:
    """Find a byte sequence even when a subprocess splits it across reads."""

    def __init__(self, needles: Sequence[bytes]) -> None:
        self._needles = tuple(needles)
        self._tail = b""
        self.found = False
        self._tail_size = max((len(needle) for needle in self._needles), default=1) - 1

    def feed(self, chunk: bytes) -> None:
        if self.found:
            return

        haystack = self._tail + chunk.lower()
        self.found = any(needle in haystack for needle in self._needles)
        self._tail = haystack[-self._tail_size :] if self._tail_size else b""


def _query_command_index(command: Sequence[str]) -> int | None:
    """Return the index of the query command, excluding the executable."""
    return next(
        (index for index, argument in enumerate(command[1:], start=1) if argument == "query"),
        None,
    )


def _format_cquery_command(command: Sequence[str]) -> str | None:
    """Convert the failed query invocation into a copy-pasteable cquery."""
    query_index = _query_command_index(command)
    if query_index is None:
        return None

    cquery_command = ["bazel", *command[1:]]
    cquery_command[query_index] = "cquery"
    if os.name == "nt":
        return subprocess.list2cmdline(cquery_command)
    return shlex.join(cquery_command)


def _write_stderr(stderr: TextIO, chunk: bytes) -> None:
    """Forward subprocess bytes without requiring stderr to be a real terminal."""
    buffer = getattr(stderr, "buffer", None)
    if buffer is not None:
        buffer.write(chunk)
        buffer.flush()
    else:
        stderr.write(chunk.decode(errors="replace"))
        stderr.flush()


def _exit_code(returncode: int) -> int:
    """Translate Python's negative signal return codes to shell conventions."""
    return returncode if returncode >= 0 else 128 - returncode


def _start_bazel(
    command: Sequence[str], terminal_stderr: TextIO, *, stdout: int | None, stderr: int | None
) -> subprocess.Popen | int:
    """Start Bazel, returning a shell-style error code if it cannot be launched."""
    try:
        return subprocess.Popen(command, stdout=stdout, stderr=stderr)
    except PermissionError as error:
        print(f"{command[0]}: {error}", file=terminal_stderr)
        return 126
    except OSError as error:
        print(f"{command[0]}: {error}", file=terminal_stderr)
        return 127


def _stream_and_scan(read_chunk: Callable[[], bytes], terminal_stderr: TextIO) -> tuple[bool, bool]:
    """Forward stderr while retaining only bounded state for signature matching."""
    conditions_package_error = _ChunkMatcher(_CONDITIONS_PACKAGE_ERRORS)
    query_error_context = _ChunkMatcher(_QUERY_ERROR_CONTEXT)
    while chunk := read_chunk():
        _write_stderr(terminal_stderr, chunk)
        conditions_package_error.feed(chunk)
        query_error_context.feed(chunk)
    return conditions_package_error.found, query_error_context.found


def _print_diagnostic(
    command: Sequence[str],
    returncode: int,
    terminal_stderr: TextIO,
    conditions_package_error: bool,
    query_error_context: bool,
) -> None:
    if (
        returncode == 0
        or _query_command_index(command) is None
        or not conditions_package_error
        or not query_error_context
    ):
        return

    cquery_command = _format_cquery_command(command)
    if cquery_command is not None:
        terminal_stderr.write(_DIAGNOSTIC.format(cquery_command=cquery_command))
        terminal_stderr.flush()


def _run_with_pipe(command: Sequence[str], terminal_stderr: TextIO) -> int:
    started = _start_bazel(command, terminal_stderr, stdout=None, stderr=subprocess.PIPE)
    if isinstance(started, int):
        return started

    assert started.stderr is not None
    conditions_package_error, query_error_context = _stream_and_scan(
        started.stderr.read, terminal_stderr
    )
    returncode = _exit_code(started.wait())
    _print_diagnostic(
        command,
        returncode,
        terminal_stderr,
        conditions_package_error,
        query_error_context,
    )
    return returncode


def _read_pty(master_fd: int) -> bytes:
    try:
        return os.read(master_fd, 16 * 1024)
    except OSError as error:
        # Linux reports EIO when the slave side of a pseudo-terminal closes.
        if error.errno == errno.EIO:
            return b""
        raise


def _run_with_pty(command: Sequence[str], terminal_stderr: TextIO) -> int:
    import pty

    master_fd, slave_fd = pty.openpty()
    try:
        started = _start_bazel(command, terminal_stderr, stdout=None, stderr=slave_fd)
    finally:
        os.close(slave_fd)

    if isinstance(started, int):
        os.close(master_fd)
        return started

    try:
        conditions_package_error, query_error_context = _stream_and_scan(
            lambda: _read_pty(master_fd), terminal_stderr
        )
    finally:
        os.close(master_fd)

    returncode = _exit_code(started.wait())
    _print_diagnostic(
        command,
        returncode,
        terminal_stderr,
        conditions_package_error,
        query_error_context,
    )
    return returncode


def _is_tty(stream: TextIO) -> bool:
    try:
        return stream.isatty()
    except (AttributeError, OSError):
        return False


def run_bazel(command: Sequence[str], stderr: TextIO | None = None) -> int:
    """Run Bazel, forwarding output and adding the targeted diagnostic if needed."""
    terminal_stderr = stderr if stderr is not None else sys.stderr
    if os.name != "nt" and _is_tty(terminal_stderr):
        # A PTY keeps Bazel's stderr isatty() result intact, preserving its
        # interactive colors and progress formatting while we inspect the bytes.
        return _run_with_pty(command, terminal_stderr)

    return _run_with_pipe(command, terminal_stderr)


def main(argv: Sequence[str] | None = None) -> int:
    command = list(sys.argv[1:] if argv is None else argv)
    if not command:
        print("query_failure_diagnostic.py requires a Bazel executable", file=sys.stderr)
        return 2
    return run_bazel(command)


if __name__ == "__main__":
    sys.exit(main())
