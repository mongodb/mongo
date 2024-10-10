"""Script to be invoked by GDB for testing optimizer pretty printers."""

import difflib
import string

import gdb


def output_diff(actual, expected):
    str = ""
    for text in difflib.unified_diff(expected.split("\n"), actual.split("\n")):
        if text[:3] not in ("+++", "---", "@@ "):
            str += text + "\n"
    return str


def remove_whitespace(str):
    remove = string.whitespace
    mapping = {ord(c): None for c in remove}
    return str.translate(mapping)


# Asserts on the pretty printed string of the local 'variable' by comparing to 'expected'.
def assertPrintedOutput(variable, expected):
    actual = gdb.execute("print " + variable, to_string=True).split(" = ", 1)[1]
    assert remove_whitespace(actual) == remove_whitespace(expected), (
        "[case: '" + variable + "'] Diff:\n" + output_diff(actual, expected)
    )
    print("TEST PASSED - " + variable)


if __name__ == "__main__":
    try:
        gdb.execute("run")
        gdb.execute("frame function main")

        # These tests work in tandem with the test binary 'optimizer_gdb_test_program'. Each test
        # case inspects a local variable by invoking the appropriate pretty printer and comparing
        # to the expected output.

        # Test ABT containing a conjunction over the fields 'a' and 'b'.
        assertPrintedOutput(
            "testABT",
            '\nRoot["root"]\n'
            + "Filter\n"
            + "|   EvalFilter\n"
            + '|   |   Variable["root"], \n'
            + "|   PathComposeM\n"
            + '|   |   PathGet["b"]\n'
            + "|   |   PathCompare[Eq]\n"
            + '|   |   Constant["1"], \n'
            + '|   PathGet["a"]\n'
            + "|   PathCompare[Eq]\n"
            + '|   Constant["1"], \n'
            + 'Scan["coll", "root"]\n',
        )

        assertPrintedOutput("emptyProjectionMap", "{std::map with 0 elements}\n")

        assertPrintedOutput(
            "testProjectionMap",
            '{<root>: "test", std::map with 2 elements : (["a"] : "b", ["c"] : "d")}',
        )

        gdb.write("TEST PASSED\n")
    except Exception as err:
        gdb.write("TEST FAILED -- {!s}\n".format(err))
        gdb.execute("quit 1", to_string=True)
