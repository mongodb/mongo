from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

# A .results file that begins with the partial-run sentinel should be rejected in
# compare mode before any comparison is attempted.
exit_code, output = run_mongotest(("partial_run_sentinel",), Mode.COMPARE)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, "was generated from a partial run (-n/-r)")
