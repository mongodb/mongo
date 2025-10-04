from testlib.test_utils import (
    ExitCode,
    Mode,
    assert_exit_code,
    assert_output_contains,
    run_mongotest,
)

exit_code, output = run_mongotest(("bad_queries", "bad_index", "bad_document"), Mode.RUN)
assert_exit_code(exit_code, ExitCode.FAILURE, output)
assert_output_contains(output, '''"Unrecognized pipeline stage name: '$firstN'"''')
assert_output_contains(
    output,
    """Bad character is in this snippet: "a:{bad:do". Full input: 
{a: {bad: document}}""",
)
assert_output_contains(
    output,
    '''Bad character is in this snippet: "{$firstN.". Full input: 
{$firstN.obj: "no"''',
)
