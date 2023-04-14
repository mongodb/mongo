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

        if isinstance(self.expected_output, list):
            passed = all([expected_output in p.stdout for expected_output in self.expected_output])
        else:
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
            if compiledb.endswith("/" + self._testMethodName + "/compile_commands.json"):
                self.compile_db = compiledb
        if self.compile_db:
            self.cmd = [
                sys.executable,
                'buildscripts/clang_tidy.py',
                '--disable-reporting',
                f'--check-module={self.TIDY_MODULE}',
                f'--output-dir={os.path.join(os.path.dirname(self.compile_db), self._testMethodName + "_out")}',
                f'--compile-commands={self.compile_db}',
            ]
        else:
            raise (f"ERROR: did not findh matching compiledb for {self._testMethodName}")

    def tearDown(self):
        if self.config_file:
            self.config_file.close()
            os.unlink(self.config_file.name)

    def test_MongoHeaderBracketCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-header-bracket-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "error: non-mongo include 'cctype' should use angle brackets",
            "error: mongo include 'test_MongoHeaderBracketCheck.h' should use double quotes",
            "error: third_party include 'third_party/s2/hash.h' should not start with 'third_party/'",
        ]

        self.run_clang_tidy()

    def test_MongoUninterruptibleLockGuardCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-uninterruptible-lock-guard-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = (
            "Potentially incorrect use of UninterruptibleLockGuard, "
            "the programming model inside MongoDB requires that all operations be interruptible. "
            "Review with care and if the use is warranted, add NOLINT and a comment explaining why."
        )

        self.run_clang_tidy()

    def test_MongoCctypeCheck(self):

        self.write_config(
            textwrap.dedent("""\
                    Checks: '-*,mongo-cctype-check'
                    WarningsAsErrors: '*'
                    HeaderFilterRegex: '(mongo/.*)'
                    """))

        self.expected_output = [
            "Use of prohibited \"cctype\" header, use \"mongo/util/ctype.h\"",
            "Use of prohibited <ctype.h> header, use \"mongo/util/ctype.h\"",
        ]

        self.run_clang_tidy()

    def test_MongoCxx20BannedIncludesCheck(self):

        self.write_config(
            textwrap.dedent("""\
                    Checks: '-*,mongo-cxx20-banned-includes-check'
                    WarningsAsErrors: '*'
                    HeaderFilterRegex: '(mongo/.*)'
                    """))

        self.expected_output = [
            "Use of prohibited <syncstream> header.",
            "Use of prohibited <ranges> header.",
            "Use of prohibited <barrier> header.",
            "Use of prohibited <latch> header.",
            "Use of prohibited <semaphore> header.",
        ]

        self.run_clang_tidy()

    def test_MongoStdOptionalCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-std-optional-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\nvoid f(std::optional<std::string> parameterDeclTest) {",
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\n    std::optional<std::string> variableDeclTest;",
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\n    std::optional<int> fieldDeclTest = 5;",
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\nvoid functionName(const std::optional<int>& referenceDeclTest) {",
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\nstd::optional<std::string> functionReturnTypeDeclTest(StringData name);",
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\nstd::optional<T> templateDeclTest;",
            "Use of std::optional, use boost::optional instead.  [mongo-std-optional-check,-warnings-as-errors]\nusing std::optional;",
        ]

        self.run_clang_tidy()

    def test_MongoVolatileCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-volatile-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "Illegal use of the volatile storage keyword, use AtomicWord instead from \"mongo/platform/atomic_word.h\" [mongo-volatile-check,-warnings-as-errors]\nvolatile int varVolatileTest;",
            "Illegal use of the volatile storage keyword, use AtomicWord instead from \"mongo/platform/atomic_word.h\" [mongo-volatile-check,-warnings-as-errors]\n    volatile int fieldVolatileTest;",
            "Illegal use of the volatile storage keyword, use AtomicWord instead from \"mongo/platform/atomic_word.h\" [mongo-volatile-check,-warnings-as-errors]\nvoid functionName(volatile int varVolatileTest) {}",
        ]

        self.run_clang_tidy()

    def test_MongoTraceCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-trace-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "Illegal use of prohibited tracing support, this is only for local development use and should not be committed. [mongo-trace-check,-warnings-as-errors]\n    TracerProvider::initialize();",
            "Illegal use of prohibited tracing support, this is only for local development use and should not be committed. [mongo-trace-check,-warnings-as-errors]\n    TracerProvider provider = TracerProvider::get();",
        ]

        self.run_clang_tidy()

    def test_MongoStdAtomicCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-std-atomic-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "Illegal use of prohibited std::atomic<T>, use AtomicWord<T> or other types from \"mongo/platform/atomic_word.h\" [mongo-std-atomic-check,-warnings-as-errors]\nstd::atomic<int> atomic_var;",
            "Illegal use of prohibited std::atomic<T>, use AtomicWord<T> or other types from \"mongo/platform/atomic_word.h\" [mongo-std-atomic-check,-warnings-as-errors]\n    std::atomic<int> field_decl;",
        ]

        self.run_clang_tidy()

    def test_MongoMutexCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-mutex-check,mongo-std-atomic-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\nstdx::mutex stdxmutex_vardecl;",
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\nstd::mutex stdmutex_vardecl;",
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\n    std::mutex stdmutex_fileddecl;",
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\n    stdx::mutex stdxmutex_fileddecl;",
        ]

        self.run_clang_tidy()

    def test_MongoAssertCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-assert-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "error: Illegal use of the bare assert function, use a function from assert_util.h instead",
        ]

        self.run_clang_tidy()

    def test_MongoFCVConstantCheck(self):

        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-fcv-constant-check'
                WarningsAsErrors: '*'
                """))

        self.expected_output = [
            "error: Illegal use of FCV constant in FCV comparison check functions. FCV gating should be done through feature flags instead.",
        ]

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
