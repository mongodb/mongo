from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

exit_code, output = run_mongotest(("no_matching_res",), Mode.COMPARE)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, "A corresponding .results file must exist in compare mode")
