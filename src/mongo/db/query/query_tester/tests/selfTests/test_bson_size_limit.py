from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

exit_code, output = run_mongotest(("bson_size_limit",), Mode.RUN, out_result=True)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, "ExceededMemoryLimit")
