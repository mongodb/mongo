#!/usr/bin/env python3
# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ##########################################################################

import argparse
import contextlib
import copy
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile
import typing


ZSTD_SYMLINKS = [
    "zstd",
    "zstdmt",
    "unzstd",
    "zstdcat",
    "zcat",
    "gzip",
    "gunzip",
    "gzcat",
    "lzma",
    "unlzma",
    "xz",
    "unxz",
    "lz4",
    "unlz4",
]


EXCLUDED_DIRS = {
    "bin",
    "common",
    "scratch",
}


EXCLUDED_BASENAMES = {
    "setup",
    "setup_once",
    "teardown",
    "teardown_once",
    "README.md",
    "run.py",
    ".gitignore",
}

EXCLUDED_SUFFIXES = [
    ".exact",
    ".glob",
    ".ignore",
    ".exit",
]


def exclude_dir(dirname: str) -> bool:
    """
    Should files under the directory :dirname: be excluded from the test runner?
    """
    if dirname in EXCLUDED_DIRS:
        return True
    return False


def exclude_file(filename: str) -> bool:
    """Should the file :filename: be excluded from the test runner?"""
    if filename in EXCLUDED_BASENAMES:
        return True
    for suffix in EXCLUDED_SUFFIXES:
        if filename.endswith(suffix):
            return True
    return False

def read_file(filename: str) -> bytes:
    """Reads the file :filename: and returns the contents as bytes."""
    with open(filename, "rb") as f:
        return f.read()


def diff(a: bytes, b: bytes) -> str:
    """Returns a diff between two different byte-strings :a: and :b:."""
    assert a != b
    with tempfile.NamedTemporaryFile("wb") as fa:
        fa.write(a)
        fa.flush()
        with tempfile.NamedTemporaryFile("wb") as fb:
            fb.write(b)
            fb.flush()

            diff_bytes = subprocess.run(["diff", fa.name, fb.name], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
            return diff_bytes.decode("utf8")


def pop_line(data: bytes) -> typing.Tuple[typing.Optional[bytes], bytes]:
    """
    Pop the first line from :data: and returns the first line and the remainder
    of the data as a tuple. If :data: is empty, returns :(None, data):. Otherwise
    the first line always ends in a :\n:, even if it is the last line and :data:
    doesn't end in :\n:.
    """
    NEWLINE = b"\n"

    if data == b'':
        return (None, data)

    parts = data.split(NEWLINE, maxsplit=1)
    line = parts[0] + NEWLINE
    if len(parts) == 1:
        return line, b''

    return line, parts[1]


def glob_line_matches(actual: bytes, expect: bytes) -> bool:
    """
    Does the `actual` line match the expected glob line `expect`?
    """
    return fnmatch.fnmatchcase(actual.strip(), expect.strip())


def glob_diff(actual: bytes, expect: bytes) -> bytes:
    """
    Returns None if the :actual: content matches the expected glob :expect:,
    otherwise returns the diff bytes.
    """
    diff = b''
    actual_line, actual = pop_line(actual)
    expect_line, expect = pop_line(expect)
    while True:
        # Handle end of file conditions - allow extra newlines
        while expect_line is None and actual_line == b"\n":
            actual_line, actual = pop_line(actual)
        while actual_line is None and expect_line == b"\n":
            expect_line, expect = pop_line(expect)

        if expect_line is None and actual_line is None:
            if diff == b'':
                return None
            return diff
        elif expect_line is None:
            diff += b"---\n"
            while actual_line != None:
                diff += b"> "
                diff += actual_line
                actual_line, actual = pop_line(actual)
            return diff
        elif actual_line is None:
            diff += b"---\n"
            while expect_line != None:
                diff += b"< "
                diff += expect_line
                expect_line, expect = pop_line(expect)
            return diff

        assert expect_line is not None
        assert actual_line is not None

        if expect_line == b'...\n':
            next_expect_line, expect = pop_line(expect)
            if next_expect_line is None:
                if diff == b'':
                    return None
                return diff
            while not glob_line_matches(actual_line, next_expect_line):
                actual_line, actual = pop_line(actual)
                if actual_line is None:
                    diff += b"---\n"
                    diff += b"< "
                    diff += next_expect_line
                    return diff
            expect_line = next_expect_line
            continue

        if not glob_line_matches(actual_line, expect_line):
            diff += b'---\n'
            diff += b'< ' + expect_line
            diff += b'> ' + actual_line

        actual_line, actual = pop_line(actual)
        expect_line, expect = pop_line(expect)


class Options:
    """Options configuring how to run a :TestCase:."""
    def __init__(
        self,
        env: typing.Dict[str, str],
        timeout: typing.Optional[int],
        verbose: bool,
        preserve: bool,
        scratch_dir: str,
        test_dir: str,
        set_exact_output: bool,
    ) -> None:
        self.env = env
        self.timeout = timeout
        self.verbose = verbose
        self.preserve = preserve
        self.scratch_dir = scratch_dir
        self.test_dir = test_dir
        self.set_exact_output = set_exact_output


class TestCase:
    """
    Logic and state related to running a single test case.

    1. Initialize the test case.
    2. Launch the test case with :TestCase.launch():.
       This will start the test execution in a subprocess, but
       not wait for completion. So you could launch multiple test
       cases in parallel. This will now print any test output.
    3. Analyze the results with :TestCase.analyze():. This will
       join the test subprocess, check the results against the
       expectations, and print the results to stdout.

    :TestCase.run(): is also provided which combines the launch & analyze
    steps for single-threaded use-cases.

    All other methods, prefixed with _, are private helper functions.
    """
    def __init__(self, test_filename: str, options: Options) -> None:
        """
        Initialize the :TestCase: for the test located in :test_filename:
        with the given :options:.
        """
        self._opts = options
        self._test_file = test_filename
        self._test_name = os.path.normpath(
            os.path.relpath(test_filename, start=self._opts.test_dir)
        )
        self._success = {}
        self._message = {}
        self._test_stdin = None
        self._scratch_dir = os.path.abspath(os.path.join(self._opts.scratch_dir, self._test_name))

    @property
    def name(self) -> str:
        """Returns the unique name for the test."""
        return self._test_name

    def launch(self) -> None:
        """
        Launch the test case as a subprocess, but do not block on completion.
        This allows users to run multiple tests in parallel. Results aren't yet
        printed out.
        """
        self._launch_test()

    def analyze(self) -> bool:
        """
        Must be called after :TestCase.launch():. Joins the test subprocess and
        checks the results against expectations. Finally prints the results to
        stdout and returns the success.
        """
        self._join_test()
        self._check_exit()
        self._check_stderr()
        self._check_stdout()
        self._analyze_results()
        return self._succeeded

    def run(self) -> bool:
        """Shorthand for combining both :TestCase.launch(): and :TestCase.analyze():."""
        self.launch()
        return self.analyze()

    def _log(self, *args, **kwargs) -> None:
        """Logs test output."""
        print(file=sys.stdout, *args, **kwargs)

    def _vlog(self, *args, **kwargs) -> None:
        """Logs verbose test output."""
        if self._opts.verbose:
            print(file=sys.stdout, *args, **kwargs)

    def _test_environment(self) -> typing.Dict[str, str]:
        """
        Returns the environment to be used for the
        test subprocess.
        """
        # We want to omit ZSTD cli flags so tests will be consistent across environments
        env = {k: v for k, v in os.environ.items() if not k.startswith("ZSTD")}
        for k, v in self._opts.env.items():
            self._vlog(f"${k}='{v}'")
            env[k] = v
        return env

    def _launch_test(self) -> None:
        """Launch the test subprocess, but do not join it."""
        args = [os.path.abspath(self._test_file)]
        stdin_name = f"{self._test_file}.stdin"
        if os.path.exists(stdin_name):
            self._test_stdin = open(stdin_name, "rb")
            stdin = self._test_stdin
        else:
            stdin = subprocess.DEVNULL
        cwd = self._scratch_dir
        env = self._test_environment()
        self._test_process = subprocess.Popen(
            args=args,
            stdin=stdin,
            cwd=cwd,
            env=env,
            stderr=subprocess.PIPE,
            stdout=subprocess.PIPE
        )

    def _join_test(self) -> None:
        """Join the test process and save stderr, stdout, and the exit code."""
        (stdout, stderr) = self._test_process.communicate(timeout=self._opts.timeout)
        self._output = {}
        self._output["stdout"] = stdout
        self._output["stderr"] = stderr
        self._exit_code = self._test_process.returncode
        self._test_process = None
        if self._test_stdin is not None:
            self._test_stdin.close()
            self._test_stdin = None

    def _check_output_exact(self, out_name: str, expected: bytes, exact_name: str) -> None:
        """
        Check the output named :out_name: for an exact match against the :expected: content.
        Saves the success and message.
        """
        check_name = f"check_{out_name}"
        actual = self._output[out_name]
        if actual == expected:
            self._success[check_name] = True
            self._message[check_name] = f"{out_name} matches!"
        else:
            self._success[check_name] = False
            self._message[check_name] = f"{out_name} does not match!\n> diff expected actual\n{diff(expected, actual)}"

            if self._opts.set_exact_output:
                with open(exact_name, "wb") as f:
                    f.write(actual)

    def _check_output_glob(self, out_name: str, expected: bytes) -> None:
        """
        Check the output named :out_name: for a glob match against the :expected: glob.
        Saves the success and message.
        """
        check_name = f"check_{out_name}"
        actual = self._output[out_name]
        diff = glob_diff(actual, expected)
        if diff is None:
            self._success[check_name] = True
            self._message[check_name] = f"{out_name} matches!"
        else:
            utf8_diff = diff.decode('utf8')
            self._success[check_name] = False
            self._message[check_name] = f"{out_name} does not match!\n> diff expected actual\n{utf8_diff}"

    def _check_output(self, out_name: str) -> None:
        """
        Checks the output named :out_name: for a match against the expectation.
        We check for a .exact, .glob, and a .ignore file. If none are found we
        expect that the output should be empty.

        If :Options.preserve: was set then we save the scratch directory and
        save the stderr, stdout, and exit code to the scratch directory for
        debugging.
        """
        if self._opts.preserve:
            # Save the output to the scratch directory
            actual_name = os.path.join(self._scratch_dir, f"{out_name}")
            with open(actual_name, "wb") as f:
                    f.write(self._output[out_name])

        exact_name = f"{self._test_file}.{out_name}.exact"
        glob_name = f"{self._test_file}.{out_name}.glob"
        ignore_name = f"{self._test_file}.{out_name}.ignore"

        if os.path.exists(exact_name):
            return self._check_output_exact(out_name, read_file(exact_name), exact_name)
        elif os.path.exists(glob_name):
            return self._check_output_glob(out_name, read_file(glob_name))
        else:
            check_name = f"check_{out_name}"
            self._success[check_name] = True
            self._message[check_name] = f"{out_name} ignored!"

    def _check_stderr(self) -> None:
        """Checks the stderr output against the expectation."""
        self._check_output("stderr")

    def _check_stdout(self) -> None:
        """Checks the stdout output against the expectation."""
        self._check_output("stdout")

    def _check_exit(self) -> None:
        """
        Checks the exit code against expectations. If a .exit file
        exists, we expect that the exit code matches the contents.
        Otherwise we expect the exit code to be zero.

        If :Options.preserve: is set we save the exit code to the
        scratch directory under the filename "exit".
        """
        if self._opts.preserve:
            exit_name = os.path.join(self._scratch_dir, "exit")
            with open(exit_name, "w") as f:
                f.write(str(self._exit_code) + "\n")
        exit_name = f"{self._test_file}.exit"
        if os.path.exists(exit_name):
            exit_code: int = int(read_file(exit_name))
        else:
            exit_code: int = 0
        if exit_code == self._exit_code:
            self._success["check_exit"] = True
            self._message["check_exit"] = "Exit code matches!"
        else:
            self._success["check_exit"] = False
            self._message["check_exit"] = f"Exit code mismatch! Expected {exit_code} but got {self._exit_code}"

    def _analyze_results(self) -> None:
        """
        After all tests have been checked, collect all the successes
        and messages, and print the results to stdout.
        """
        STATUS = {True: "PASS", False: "FAIL"}
        checks = sorted(self._success.keys())
        self._succeeded = all(self._success.values())
        self._log(f"{STATUS[self._succeeded]}: {self._test_name}")

        if not self._succeeded or self._opts.verbose:
            for check in checks:
                if self._opts.verbose or not self._success[check]:
                    self._log(f"{STATUS[self._success[check]]}: {self._test_name}.{check}")
                    self._log(self._message[check])

        self._log("----------------------------------------")


class TestSuite:
    """
    Setup & teardown test suite & cases.
    This class is intended to be used as a context manager.

    TODO: Make setup/teardown failure emit messages, not throw exceptions.
    """
    def __init__(self, test_directory: str, options: Options) -> None:
        self._opts = options
        self._test_dir = os.path.abspath(test_directory)
        rel_test_dir = os.path.relpath(test_directory, start=self._opts.test_dir)
        assert not rel_test_dir.startswith(os.path.sep)
        self._scratch_dir = os.path.normpath(os.path.join(self._opts.scratch_dir, rel_test_dir))

    def __enter__(self) -> 'TestSuite':
        self._setup_once()
        return self

    def __exit__(self, _exc_type, _exc_value, _traceback) -> None:
        self._teardown_once()

    @contextlib.contextmanager
    def test_case(self, test_basename: str) -> TestCase:
        """
        Context manager for a test case in the test suite.
        Pass the basename of the test relative to the :test_directory:.
        """
        assert os.path.dirname(test_basename) == ""
        try:
            self._setup(test_basename)
            test_filename = os.path.join(self._test_dir, test_basename)
            yield TestCase(test_filename, self._opts)
        finally:
            self._teardown(test_basename)

    def _remove_scratch_dir(self, dir: str) -> None:
        """Helper to remove a scratch directory with sanity checks"""
        assert "scratch" in dir
        assert dir.startswith(self._scratch_dir)
        assert os.path.exists(dir)
        shutil.rmtree(dir)

    def _setup_once(self) -> None:
        if os.path.exists(self._scratch_dir):
            self._remove_scratch_dir(self._scratch_dir)
        os.makedirs(self._scratch_dir)
        setup_script = os.path.join(self._test_dir, "setup_once")
        if os.path.exists(setup_script):
            self._run_script(setup_script, cwd=self._scratch_dir)

    def _teardown_once(self) -> None:
        assert os.path.exists(self._scratch_dir)
        teardown_script = os.path.join(self._test_dir, "teardown_once")
        if os.path.exists(teardown_script):
            self._run_script(teardown_script, cwd=self._scratch_dir)
        if not self._opts.preserve:
            self._remove_scratch_dir(self._scratch_dir)

    def _setup(self, test_basename: str) -> None:
        test_scratch_dir = os.path.join(self._scratch_dir, test_basename)
        assert not os.path.exists(test_scratch_dir)
        os.makedirs(test_scratch_dir)
        setup_script = os.path.join(self._test_dir, "setup")
        if os.path.exists(setup_script):
            self._run_script(setup_script, cwd=test_scratch_dir)

    def _teardown(self, test_basename: str) -> None:
        test_scratch_dir = os.path.join(self._scratch_dir, test_basename)
        assert os.path.exists(test_scratch_dir)
        teardown_script = os.path.join(self._test_dir, "teardown")
        if os.path.exists(teardown_script):
            self._run_script(teardown_script, cwd=test_scratch_dir)
        if not self._opts.preserve:
            self._remove_scratch_dir(test_scratch_dir)

    def _run_script(self, script: str, cwd: str) -> None:
        env = copy.copy(os.environ)
        for k, v in self._opts.env.items():
            env[k] = v
        try:
            subprocess.run(
                args=[script],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=cwd,
                env=env,
                check=True,
            )
        except subprocess.CalledProcessError as e:
            print(f"{script} failed with exit code {e.returncode}!")
            print(f"stderr:\n{e.stderr}")
            print(f"stdout:\n{e.stdout}")
            raise

TestSuites = typing.Dict[str, typing.List[str]]

def get_all_tests(options: Options) -> TestSuites:
    """
    Find all the test in the test directory and return the test suites.
    """
    test_suites = {}
    for root, dirs, files in os.walk(options.test_dir, topdown=True):
        dirs[:] = [d for d in dirs if not exclude_dir(d)]
        test_cases = []
        for file in files:
            if not exclude_file(file):
                test_cases.append(file)
        assert root == os.path.normpath(root)
        test_suites[root] = test_cases
    return test_suites


def resolve_listed_tests(
    tests: typing.List[str], options: Options
) -> TestSuites:
    """
    Resolve the list of tests passed on the command line into their
    respective test suites. Tests can either be paths, or test names
    relative to the test directory.
    """
    test_suites = {}
    for test in tests:
        if not os.path.exists(test):
            test = os.path.join(options.test_dir, test)
            if not os.path.exists(test):
                raise RuntimeError(f"Test {test} does not exist!")

        test = os.path.normpath(os.path.abspath(test))
        assert test.startswith(options.test_dir)
        test_suite = os.path.dirname(test)
        test_case = os.path.basename(test)
        test_suites.setdefault(test_suite, []).append(test_case)

    return test_suites

def run_tests(test_suites: TestSuites, options: Options) -> bool:
    """
    Runs all the test in the :test_suites: with the given :options:.
    Prints the results to stdout.
    """
    tests = {}
    for test_dir, test_files in test_suites.items():
        with TestSuite(test_dir, options) as test_suite:
            test_files = sorted(set(test_files))
            for test_file in test_files:
                with test_suite.test_case(test_file) as test_case:
                    tests[test_case.name] = test_case.run()

    successes = 0
    for test, status in tests.items():
        if status:
            successes += 1
        else:
            print(f"FAIL: {test}")
    if successes == len(tests):
        print(f"PASSED all {len(tests)} tests!")
        return True
    else:
        print(f"FAILED {len(tests) - successes} / {len(tests)} tests!")
        return False


def setup_zstd_symlink_dir(zstd_symlink_dir: str, zstd: str) -> None:
    assert os.path.join("bin", "symlinks") in zstd_symlink_dir
    if not os.path.exists(zstd_symlink_dir):
        os.makedirs(zstd_symlink_dir)
    for symlink in ZSTD_SYMLINKS:
        path = os.path.join(zstd_symlink_dir, symlink)
        if os.path.exists(path):
            os.remove(path)
        os.symlink(zstd, path)

if __name__ == "__main__":
    CLI_TEST_DIR = os.path.dirname(sys.argv[0])
    REPO_DIR = os.path.join(CLI_TEST_DIR, "..", "..")
    PROGRAMS_DIR = os.path.join(REPO_DIR, "programs")
    TESTS_DIR = os.path.join(REPO_DIR, "tests")
    ZSTD_PATH = os.path.join(PROGRAMS_DIR, "zstd")
    ZSTDGREP_PATH = os.path.join(PROGRAMS_DIR, "zstdgrep")
    ZSTDLESS_PATH = os.path.join(PROGRAMS_DIR, "zstdless")
    DATAGEN_PATH = os.path.join(TESTS_DIR, "datagen")

    parser = argparse.ArgumentParser(
        (
            "Runs the zstd CLI tests. Exits nonzero on failure. Default arguments are\n"
            "generally correct. Pass --preserve to preserve test output for debugging,\n"
            "and --verbose to get verbose test output.\n"
        )
    )
    parser.add_argument(
        "--preserve",
        action="store_true",
        help="Preserve the scratch directory TEST_DIR/scratch/ for debugging purposes."
    )
    parser.add_argument("--verbose", action="store_true", help="Verbose test output.")
    parser.add_argument("--timeout", default=200, type=int, help="Test case timeout in seconds. Set to 0 to disable timeouts.")
    parser.add_argument(
        "--exec-prefix",
        default=None,
        help="Sets the EXEC_PREFIX environment variable. Prefix to invocations of the zstd CLI."
    )
    parser.add_argument(
        "--zstd",
        default=ZSTD_PATH,
        help="Sets the ZSTD_BIN environment variable. Path of the zstd CLI."
    )
    parser.add_argument(
        "--zstdgrep",
        default=ZSTDGREP_PATH,
        help="Sets the ZSTDGREP_BIN environment variable. Path of the zstdgrep CLI."
    )
    parser.add_argument(
        "--zstdless",
        default=ZSTDLESS_PATH,
        help="Sets the ZSTDLESS_BIN environment variable. Path of the zstdless CLI."
    )
    parser.add_argument(
        "--datagen",
        default=DATAGEN_PATH,
        help="Sets the DATAGEN_BIN environment variable. Path to the datagen CLI."
    )
    parser.add_argument(
        "--test-dir",
        default=CLI_TEST_DIR,
        help=(
            "Runs the tests under this directory. "
            "Adds TEST_DIR/bin/ to path. "
            "Scratch directory located in TEST_DIR/scratch/."
        )
    )
    parser.add_argument(
        "--set-exact-output",
        action="store_true",
        help="Set stderr.exact and stdout.exact for all failing tests, unless .ignore or .glob already exists"
    )
    parser.add_argument(
        "tests",
        nargs="*",
        help="Run only these test cases. Can either be paths or test names relative to TEST_DIR/"
    )
    args = parser.parse_args()

    if args.timeout <= 0:
        args.timeout = None

    args.test_dir = os.path.normpath(os.path.abspath(args.test_dir))
    bin_dir = os.path.abspath(os.path.join(args.test_dir, "bin"))
    zstd_symlink_dir = os.path.join(bin_dir, "symlinks")
    scratch_dir = os.path.join(args.test_dir, "scratch")

    setup_zstd_symlink_dir(zstd_symlink_dir, os.path.abspath(args.zstd))

    env = {}
    if args.exec_prefix is not None:
        env["EXEC_PREFIX"] = args.exec_prefix
    env["ZSTD_SYMLINK_DIR"] = zstd_symlink_dir
    env["ZSTD_REPO_DIR"] = os.path.abspath(REPO_DIR)
    env["DATAGEN_BIN"] = os.path.abspath(args.datagen)
    env["ZSTDGREP_BIN"] = os.path.abspath(args.zstdgrep)
    env["ZSTDLESS_BIN"] = os.path.abspath(args.zstdless)
    env["COMMON"] = os.path.abspath(os.path.join(args.test_dir, "common"))
    env["PATH"] = bin_dir + ":" + os.getenv("PATH", "")
    env["LC_ALL"] = "C"

    opts = Options(
        env=env,
        timeout=args.timeout,
        verbose=args.verbose,
        preserve=args.preserve,
        test_dir=args.test_dir,
        scratch_dir=scratch_dir,
        set_exact_output=args.set_exact_output,
    )

    if len(args.tests) == 0:
        tests = get_all_tests(opts)
    else:
        tests = resolve_listed_tests(args.tests, opts)

    success = run_tests(tests, opts)
    if success:
        sys.exit(0)
    else:
        sys.exit(1)
