from testlib.test_utils import ExitCode, Mode, assert_exit_code, run_mongotest

exit_code, output = run_mongotest(("optOff",), Mode.RUN, opt_off=True)
assert_exit_code(exit_code, ExitCode.SUCCESS, output)
