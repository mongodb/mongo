#!/usr/bin/env python3
"""Linter that flags bare duration constructors used as timeouts in streams exec tests.

Timeout durations under
``src/mongo/db/modules/enterprise/src/streams/exec/tests`` must go through the ``test::duration``
namespace (see ``test_utils.h``), which scales durations by a build-overhead factor under
sanitizers so tests do not flake.  Bare ``mongo::Seconds(...)`` / ``Milliseconds(...)`` /
``Microseconds(...)`` / ``Minutes(...)`` / ``Hours(...)`` calls used *as timeouts* are flagged.

Non-timeout uses of the same constructors (mock-clock advancement like ``tickSource.advance(...)``,
assertion comparisons like ``ASSERT_EQ(..., Seconds(1))``) are left alone: a line is only flagged
when it carries a timeout-context signal (a timeout-related identifier, or a ``while``/``for``
poll loop bounded by ``elapsed``).

Add ``// NOLINT`` (or ``// NO LINT``) to a line to suppress.

Usage:
    python3 buildscripts/streams_test_timeout_linter.py            # scan streams/exec/tests
    python3 buildscripts/streams_test_timeout_linter.py <file>...  # scan specific files
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

TESTS_DIR = (
    REPO_ROOT
    / "src"
    / "mongo"
    / "db"
    / "modules"
    / "enterprise"
    / "src"
    / "streams"
    / "exec"
    / "tests"
)

# A duration constructor call with an optional qualifier. Group 1 is the qualifier (ending in
# ``::``), group 2 is the constructor name. A call is "bare" (flagged) when the qualifier does
# not end in ``test::duration::`` -- so ``test::duration::Seconds(...)``,
# ``::streams::test::duration::Seconds(...)`` and ``mongo::test::duration::Seconds(...)`` are all
# accepted, while ``Seconds(...)``, ``mongo::Seconds(...)`` and ``::mongo::Seconds(...)`` are
# flagged.
DURATION_RE = re.compile(
    r"([A-Za-z_:][A-Za-z0-9_:]*::)?"
    r"(Seconds|Milliseconds|Microseconds|Minutes|Hours)\s*(?:\(|\{)"
)

# Identifiers that indicate a line is dealing with a timeout rather than a deterministic
# duration. Matched case-insensitively as substrings.
TIMEOUT_KEYWORDS: tuple[str, ...] = (
    "sleepfor",  # sleepFor
    "sleep_for",
    "waitfor",  # waitFor / WaitForCondition
    "wait_for",
    "waitms",  # waitMs / WaitMs
    "assert_eventually",  # ASSERT_EVENTUALLY macro
    "assert_within",  # ASSERT_WITHIN macro (duration is the first argument)
    "eventually",
    "deadline",
    "timeout",  # Timeout / timeout / connectTimeout / requestTimeout / stopTimeout
    "maxwait",  # maxWaitMs
    "basewait",  # baseWaitMs
    "entryttl",  # cacheEntryTTL (time-to-live is a timeout)
)

# A poll loop bounded by elapsed time, e.g. ``while (timer.elapsed() < Minutes(1))``. This is a
# timeout pattern that does not contain any of the keywords above.
POLL_LOOP_RE = re.compile(r"\b(?:while|for)\b.*\belapsed\b.*[<>]")

# Assertion-comparison macros compare values deterministically; they never wait, so a duration
# constructor inside one is an expected/actual value (e.g. asserting a config field equals 2000ms)
# and must NOT be scaled under sanitizers. Such lines are never timeouts.
ASSERTION_RE = re.compile(
    r"\bASSERT_(?:EQ|NE|LESS_THAN|GREATER_THAN|LESS_THAN_OR_EQUALS|GREATER_THAN_OR_EQUALS|APPROX)\b"
)

# A duration constructor is qualified (and thus accepted) when its qualifier ends in
# test::duration::.
_QUALIFIED_SUFFIX = "test::duration::"


def is_suppressed(line: str) -> bool:
    """Return True when the line carries a NOLINT suppression comment."""
    return "NOLINT" in line or "NO LINT" in line


def has_timeout_signal(line: str) -> bool:
    """Return True when the line looks like it is expressing a timeout."""
    lowered = line.lower()
    if any(keyword in lowered for keyword in TIMEOUT_KEYWORDS):
        return True
    return bool(POLL_LOOP_RE.search(line))


def find_violations_in_text(text: str) -> list[tuple[int, str]]:
    """Return (line number, line text) for each line with a bare timeout duration constructor."""
    violations: list[tuple[int, str]] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if is_suppressed(line):
            continue
        if ASSERTION_RE.search(line):
            continue
        if not has_timeout_signal(line):
            continue
        for match in DURATION_RE.finditer(line):
            qualifier = match.group(1) or ""
            if qualifier.endswith(_QUALIFIED_SUFFIX):
                continue
            violations.append((lineno, line.rstrip()))
            break
    return violations


def check_file(path: pathlib.Path) -> list[tuple[str, int, str]]:
    """Return (file path, line number, line text) tuples for each violation in ``path``."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []
    return [(str(path), lineno, line) for lineno, line in find_violations_in_text(text)]


def find_test_files() -> list[pathlib.Path]:
    """Return all ``.cpp``/``.h``/``.hpp``/``.inl`` files under the streams exec tests directory."""
    if not TESTS_DIR.is_dir():
        return []
    files: list[pathlib.Path] = []
    for root, _dirs, names in os.walk(TESTS_DIR):
        for name in names:
            if name.endswith((".cpp", ".h", ".hpp", ".inl")):
                files.append(pathlib.Path(root) / name)
    return sorted(files)


_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def changed_line_numbers(base_branch: str, repo_path: str) -> set[int]:
    """Return the new-file line numbers of lines added/modified in ``repo_path`` vs ``base_branch``.

    ``repo_path`` is relative to the repository root. Returns an empty set if the file is new or
    unchanged relative to the base in a way that produces no diff (a wholly new file returns every
    line, since every line is an addition).
    """
    result = subprocess.run(
        ["git", "diff", "--unified=0", "--no-color", base_branch, "--", repo_path],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
    )
    changed: set[int] = set()
    new_line: int | None = None
    for line in result.stdout.splitlines():
        hunk = _HUNK_RE.match(line)
        if hunk:
            new_line = int(hunk.group(1))
            continue
        if new_line is None or line.startswith("+++"):
            continue
        if line.startswith("+"):
            changed.add(new_line)
            new_line += 1
        elif line.startswith("-"):
            continue  # removed line; does not advance the new-file line counter
        else:
            new_line += 1  # context line
    return changed


def main() -> int:
    """Execute main entry point. Returns 0 on success, 1 on violations."""
    parser = argparse.ArgumentParser(
        description="Linter that flags timeout durations not using the `test::duration::` namespace."
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Scan every file under streams/exec/tests (the default with no --file).",
    )
    parser.add_argument(
        "--base-branch",
        default=None,
        help="When set with --file, only check lines added/modified relative to this branch.",
    )
    parser.add_argument(
        "--file",
        action="append",
        dest="files",
        default=[],
        help="A specific file to check (may be repeated).",
    )
    parser.add_argument(
        "--root",
        default=str(REPO_ROOT),
        help="Repository root used for rendering relative paths in diagnostics.",
    )
    args = parser.parse_args()

    if args.files:
        paths = [pathlib.Path(f) for f in args.files]
    else:
        paths = find_test_files()

    root = pathlib.Path(args.root)
    has_violations = False
    for path in paths:
        violations = check_file(path)
        if args.base_branch is not None:
            try:
                rel = str(path.relative_to(root))
            except ValueError:
                rel = str(path)
            changed = changed_line_numbers(args.base_branch, rel)
            if not changed:
                continue
            violations = [(f, n, t) for (f, n, t) in violations if n in changed]
        for filename, lineno, line in violations:
            has_violations = True
            try:
                display = str(pathlib.Path(filename).relative_to(root))
            except ValueError:
                display = filename
            print(
                f"Error: {display}:{lineno} - streams-test-timeout"
                f" - timeout duration uses a bare duration constructor instead of the"
                f" `test::duration::` namespace from test_utils.h."
                f" Use test::duration::Seconds/Milliseconds/Microseconds/Minutes/Hours(...)"
                f" or add `// NOLINT`: {line.strip()}"
            )

    if has_violations:
        print(
            "ERROR: Found timeout durations not using the `test::duration::` namespace."
            " Please migrate them or add `// NOLINT`."
        )
        return 1

    print("All timeout durations use the `test::duration::` namespace (or are NOLINT-suppressed).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
