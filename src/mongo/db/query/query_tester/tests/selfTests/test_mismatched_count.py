from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

exit_code, output = run_mongotest(("mismatched_countA", "mismatched_countB"), Mode.COMPARE)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(
    output, ':sortResults {aggregate: "fuzzer_coll", pipeline: [{$limit: 5}], cursor: {}}'
)
