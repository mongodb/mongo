import argparse
import enum
import os
import subprocess
import sys


class ExitCode(enum.IntEnum):
    SUCCESS = 0
    FAILURE = 1
    TIMEOUT = 2


class Mode(enum.Enum):
    """
    QueryTester execution modes.

    """

    COMPARE = "compare"
    RUN = "run"
    NORMALIZE = "normalize"


def assert_output_contains(actual: bytes, expected: str):
    assert expected.encode("utf-8") in actual, f"Expected '{expected}' in output but got '{actual}'"


def assert_exit_code(actual: ExitCode, expected: ExitCode, output: str):
    assert expected == actual, f"Expected {expected} but got '{actual}' with output: {output}"


def _get_mongotest_args() -> tuple[str, str]:
    """
    Parse command-line arguments for the QueryTester self-tests.

    It expects two arguments: the path to the mongotest binary and the URI of the `mongod` instance.
    Returns:
        Tuple containing the path to the mongotest binary and the URI of the `mongod` instance.
    Raises:
        SystemExit: If the required arguments are not provided.
    """
    parser = argparse.ArgumentParser(description="Run QueryTester Self Tests")
    parser.add_argument("-b", "--bin", required=True, help="Path to mongotest binary")
    parser.add_argument("-u", "--uri", required=True, help="URI of `mongod` instance")
    args = parser.parse_args()
    return (args.bin, args.uri)


def _discover_test_file(test_file_name: str):
    """
    Search for a specified test file in the parent and cousin directories.

    Parameters:
        test_file_name: The name of the test file to search for.
    Returns:
        The full path to the found test file.
    Exits:
        SystemExit: If the specified test file is not found.
    """
    current_dir = os.path.dirname(os.path.abspath(__file__))
    parent_dir = os.path.abspath(os.path.join(current_dir, os.pardir))
    for dirpath, _, filenames in os.walk(parent_dir):
        if test_file_name in filenames:
            return os.path.join(dirpath, test_file_name)
    sys.exit(f"Unable to find {test_file_name}")


def run_mongotest(
    test_file_names: tuple[str, ...],
    mode: Mode,
    drop: bool = True,
    load: bool = True,
    minimal_index: bool = False,
    opt_off: bool = False,
    out_result: bool = False,
    extract_features: bool = False,
    override_type: str = None,
) -> tuple[ExitCode, bytes]:
    """
    Execute mongotest binary with specified test files and options. It constructs the command-line invocation of mongotest based on the input parameters and executes the command. It returns the output of the mongotest process to the calling function.

    Params:
        test_file_names: A list of test file names to be executed.
        mode: The mode in which to run the tests (e.g. COMPARE, RUN and NORMALIZE).
            COMPARE: Compare results between mongod output and .results. Cannot be run with out_result set
            RUN: Run *.test files. Can be used to record output results in a .result file. Can overwrite .results file if out_result is set.
            NORMALIZE: Check that results in a given .results file are normalized. Can overwrite .results file if out_result is set.
        drop: If True mongotest will drop the test collection before running. Defaults to True.
        load: If True, mongotest will load data into test collection. Defaults to True.
        minimal_index: If True, mongotest will only load a minimal set of indices. Defaults to False.
        out_result: If True, mongotest will produce an output .result file. It can only be true if mode is RUN or NORMALIZE. Defaults to False.
    Returns:
        Tuple containing the exit code and output (as bytes) from the mongotest execution.
    """
    if out_result and mode == Mode.COMPARE:
        sys.exit("Mode must be set to RUN or NORMALIZE if we want to produce output .result files.")

    # Retrieve mongotest binary path and mongod uri from command line arguments
    mongotest, uri = _get_mongotest_args()
    test_file_names = tuple(f"{test_file_name}.test" for test_file_name in test_file_names)
    test_files = map(_discover_test_file, test_file_names)

    cmd = [mongotest, "--uri", uri]
    cmd.extend((test_arg for test_file in test_files for test_arg in ("-t", test_file)))

    if drop:
        cmd.append("--drop")
    if load:
        cmd.append("--load")
    cmd.extend(("--mode", mode.value))
    if opt_off:
        cmd.append("--opt-off")
    if out_result:
        cmd.extend(("--out", "result"))
    if minimal_index:
        cmd.append("--minimal-index")
    if extract_features:
        cmd.extend(("-v", "--extractFeatures"))
    if override_type:
        cmd.extend(("--override", override_type))

    try:
        output = subprocess.run(
            cmd,
            timeout=60 if extract_features else 10,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        return ExitCode.SUCCESS, output.stdout
    except subprocess.TimeoutExpired as e:
        return ExitCode.TIMEOUT, e.stdout
    except subprocess.CalledProcessError as e:
        return ExitCode.FAILURE, e.stdout
