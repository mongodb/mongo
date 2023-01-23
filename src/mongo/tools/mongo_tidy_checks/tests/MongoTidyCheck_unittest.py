import unittest
import subprocess
import sys
import tempfile
import os
import textwrap
import argparse


class MongoTidyTests(unittest.TestCase):
    TIDY_BIN = None
    TIDY_MODULE = None
    COMPILE_COMMANDS_FILES = []

    def write_config(self, config_str: str):
        self.config_file = tempfile.NamedTemporaryFile(mode='w', delete=False)
        self.config_file.write(config_str)
        self.config_file.close()
        self.cmd += [f'--clang-tidy-cfg={self.config_file.name}']
        return self.config_file.name

    def run_clang_tidy(self):
        p = subprocess.run(self.cmd, capture_output=True, text=True)
        passed = self.expected_output is not None and self.expected_output in p.stdout
        with open(self.config_file.name) as f:
            msg = '\n'.join([
                '>' * 80,
                f"Mongo Tidy Unittest {self._testMethodName}: {'PASSED' if passed else 'FAILED'}",
                "",
                "Command:",
                ' '.join(self.cmd),
                "",
                "With config:",
                f.read(),
                "",
                f"Exit code was: {p.returncode}",
                "",
                f"Output expected in stdout: {self.expected_output}",
                "",
                "stdout was:",
                p.stdout,
                "",
                "stderr was:",
                p.stderr,
                "",
                '<' * 80,
            ])

            if passed:
                sys.stderr.write(msg)
            else:
                print(msg)
                self.fail()
                
            with open(f'{os.path.splitext(self.compile_db)[0]}.results', 'w') as results:
                results.write(msg)

    def setUp(self):
        self.config_file = None
        self.expected_output = None
        for compiledb in self.COMPILE_COMMANDS_FILES:
            if compiledb.endswith("/" + self._testMethodName + ".json"):
                self.compile_db = compiledb
        if self.compile_db:
            self.cmd = [
                sys.executable,
                'buildscripts/clang_tidy.py',
                f'--check-module={self.TIDY_MODULE}',
                f'--output-dir={os.path.join(os.path.dirname(compiledb), self._testMethodName + "_out")}',
                f'--compile-commands={self.compile_db}',
            ]
        else:
            raise(f"ERROR: did not findh matching compiledb for {self._testMethodName}")

    def tearDown(self):
        if self.config_file:
            self.config_file.close()
            os.unlink(self.config_file.name)

    def test_MongoTestCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-test-check'
                WarningsAsErrors: '*'
                """))
        
        self.expected_output = "ran mongo-test-check!"

        self.run_clang_tidy()


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument('--clang-tidy-path', default='/opt/mongodbtoolchain/v4/bin/clang-tidy',
                        help="Path to clang-tidy binary.")
    parser.add_argument('--mongo-tidy-module', default='build/install/lib/libmongo_tidy_checks.so',
                        help="Path to mongo tidy check library.")
    parser.add_argument(
        '--test-compiledbs', action='append', default=[],
        help="Used multiple times. Each use adds a test compilation database to use. " +
        "The compilation database name must match the unittest method name.")
    parser.add_argument('unittest_args', nargs='*')

    args = parser.parse_args()

    MongoTidyTests.TIDY_BIN = args.clang_tidy_path
    MongoTidyTests.TIDY_MODULE = args.mongo_tidy_module
    MongoTidyTests.COMPILE_COMMANDS_FILES = args.test_compiledbs

    # We need to validate the toolchain can support the load operation for our module.
    cmd = [MongoTidyTests.TIDY_BIN, f'-load', MongoTidyTests.TIDY_MODULE, '--list-checks']
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode != 0:
        print(f"Could not validate toolchain was able to load module {cmd}.")
        sys.exit(1)

    # Workaround to allow use to use argparse on top of unittest module.
    sys.argv[1:] = args.unittest_args

    unittest.main()
