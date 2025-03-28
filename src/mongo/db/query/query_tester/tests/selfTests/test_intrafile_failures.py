import pathlib

from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)
from testlib.test_utils import _discover_test_file as discover_test_file

test_file = pathlib.Path(discover_test_file("output_error.test"))
exit_code, output = run_mongotest(("output_error",), Mode.RUN, out_result=True)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, '''codeName: "ConversionFailure"''')

expected_file = test_file.with_suffix(".expected")
results_file = test_file.with_suffix(".results")

with open(expected_file, "r") as ein:
    with open(results_file, "r") as rin:
        assert ein.read() == rin.read(), "Computed results file does not match expected results."
