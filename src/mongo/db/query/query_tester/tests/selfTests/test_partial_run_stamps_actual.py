import pathlib

from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    run_mongotest,
)
from testlib.test_utils import _discover_test_file as discover_test_file

# A failing partial compare run must stamp the leftover .actual file with the partial-run
# sentinel so that it cannot be silently promoted to a .results baseline.
# Covers both -n (single test) and -r (range) partial-run modes.
#
# Fixture: partial_run_fail has two tests. Test 0's expected result is intentionally wrong
# ({"_id":999} which does not exist in the collection), so a partial run covering only
# test 0 always fails comparison and triggers the sentinel stamp.

test_file = pathlib.Path(discover_test_file("partial_run_fail.test"))
actual_file = test_file.with_suffix(".actual")


def assert_actual_is_stamped(actual_file: pathlib.Path) -> None:
    assert actual_file.exists(), f"Expected .actual file at {actual_file}"
    first_line = actual_file.read_text().splitlines()[0]
    assert first_line.startswith(
        "// PARTIAL_RUN"
    ), f"Expected .actual to start with partial-run sentinel, got: {first_line!r}"


# --- -n (single test number) ---
actual_file.unlink(missing_ok=True)
try:
    exit_code, output = run_mongotest(("partial_run_fail",), Mode.COMPARE, partial_n=0)
    assert_exit_code(exit_code, ExitCode.FAILURE, output)
    assert_actual_is_stamped(actual_file)
finally:
    actual_file.unlink(missing_ok=True)

# --- -r (inclusive range) ---
actual_file.unlink(missing_ok=True)
try:
    exit_code, output = run_mongotest(("partial_run_fail",), Mode.COMPARE, partial_r=(0, 0))
    assert_exit_code(exit_code, ExitCode.FAILURE, output)
    assert_actual_is_stamped(actual_file)
finally:
    actual_file.unlink(missing_ok=True)
