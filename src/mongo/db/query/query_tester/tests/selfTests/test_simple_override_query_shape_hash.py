from testlib.test_utils import ExitCode, Mode, assert_exit_code, run_mongotest

test_file_names = (
    "escapedQuotes",
    "getMoreTest",
    "indexOptions",
    "multiline",
    "norm",
    "testA",
    "testNums",
)
modes = [Mode.RUN, Mode.COMPARE]

# Ensure simple_run and simple_compare are run in serial so that a testfile isn't processed with
# --mode run at the same time that it's processed with --mode compare. This can lead to race
# conditions between deleting a .actual file (in --mode run) and reading it (in --mode compare).
for mode in modes:
    exit_code, output = run_mongotest(test_file_names, mode, override_type="queryShapeHash")
    assert_exit_code(exit_code, ExitCode.SUCCESS, output)
