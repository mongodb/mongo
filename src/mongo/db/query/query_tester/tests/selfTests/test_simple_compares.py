from testlib.test_utils import ExitCode, Mode, assert_exit_code, run_mongotest

test_file_names = ("escapedQuotes", "getMoreTest", "indexOptions", "multiline", "norm", "testA")
exit_code, output = run_mongotest(test_file_names, Mode.COMPARE)
assert_exit_code(exit_code, ExitCode.SUCCESS, output)
