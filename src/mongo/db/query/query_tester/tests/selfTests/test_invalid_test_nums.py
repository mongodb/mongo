from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

exit_code, output = run_mongotest(
    ("bad_test_nums", "decreasing_test_nums", "dupe_test_nums"), Mode.RUN
)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, "testNum must be written in monotonically increasing order.")
