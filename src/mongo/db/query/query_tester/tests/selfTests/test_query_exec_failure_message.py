from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

exit_code, output = run_mongotest(("invalidPipeline",), Mode.RUN)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, """Failed to execute test number 0.""")
assert_output_contains(
    output,
    '''"Unrecognized pipeline stage name: '$nonExistentStage'"''',
)
