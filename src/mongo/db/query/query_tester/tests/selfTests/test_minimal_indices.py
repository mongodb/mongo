from testlib.test_utils import ExitCode, Mode, assert_exit_code, run_mongotest

exit_code, output = run_mongotest(("minimalIndices",), Mode.RUN, minimal_index=True)
assert_exit_code(exit_code, ExitCode.SUCCESS, output)
