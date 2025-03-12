import argparse
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest


class MongoTidyTests(unittest.TestCase):
    TIDY_BIN = None
    TIDY_MODULE = None
    COMPILE_COMMANDS_FILES = []

    def write_config(self, config_str: str):
        self.config_file = tempfile.NamedTemporaryFile(mode="w", delete=False)
        self.config_file.write(config_str)
        self.config_file.close()
        self.cmd += [f"--clang-tidy-cfg={self.config_file.name}"]
        return self.config_file.name

    def run_clang_tidy(self):
        p = subprocess.run(self.cmd, capture_output=True, text=True)

        if isinstance(self.expected_output, list):
            passed = all([expected_output in p.stdout for expected_output in self.expected_output])
        else:
            passed = self.expected_output is not None and self.expected_output in p.stdout

        with open(self.config_file.name) as f:
            msg = "\n".join(
                [
                    ">" * 80,
                    f"Mongo Tidy Unittest {self._testMethodName}: {'PASSED' if passed else 'FAILED'}",
                    "",
                    "Command:",
                    " ".join(self.cmd),
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
                    "<" * 80,
                ]
            )

            if passed:
                sys.stderr.write(msg)
            else:
                print(msg)
                self.fail()

            with open(f"{os.path.splitext(self.compile_db)[0]}.results", "w") as results:
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
                "buildscripts/clang_tidy.py",
                "--disable-reporting",
                "--clang-tidy-test",
                f"--check-module={self.TIDY_MODULE}",
                f'--output-dir={os.path.join(os.path.dirname(self.compile_db), self._testMethodName + "_out")}',
                f"--compile-commands={self.compile_db}",
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
                """)
        )

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
                """)
        )

        self.expected_output = (
            "Potentially incorrect use of UninterruptibleLockGuard, "
            "the programming model inside MongoDB requires that all operations be interruptible. "
            "Review with care and if the use is warranted, add NOLINT and a comment explaining why."
        )

        self.run_clang_tidy()

    def test_MongoUninterruptibleLockGuardCheckForOpCtxMember(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-uninterruptible-lock-guard-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = (
            "Potentially incorrect use of "
            "OperationContext::uninterruptibleLocksRequested_DO_NOT_USE, this is a legacy "
            "interruption mechanism that makes lock acquisition ignore interrupts. Please ensure "
            "this use is warranted and if so add a NOLINT comment explaining why. Please also add "
            "Service Arch to the PR so that we can verify this is necessary and there are no "
            "alternative workarounds."
        )

        self.run_clang_tidy()

    def test_MongoCctypeCheck(self):
        self.write_config(
            textwrap.dedent("""\
                    Checks: '-*,mongo-cctype-check'
                    WarningsAsErrors: '*'
                    HeaderFilterRegex: '(mongo/.*)'
                    """)
        )

        self.expected_output = [
            'Use of prohibited "cctype" header, use "mongo/util/ctype.h"',
            'Use of prohibited <ctype.h> header, use "mongo/util/ctype.h"',
        ]

        self.run_clang_tidy()

    def test_MongoCxx20BannedIncludesCheck(self):
        self.write_config(
            textwrap.dedent("""\
                    Checks: '-*,mongo-cxx20-banned-includes-check'
                    WarningsAsErrors: '*'
                    HeaderFilterRegex: '(mongo/.*)'
                    """)
        )

        self.expected_output = [
            "Use of prohibited <syncstream> header.",
            "Use of prohibited <ranges> header.",
            "Use of prohibited <barrier> header.",
            "Use of prohibited <latch> header.",
            "Use of prohibited <semaphore> header.",
        ]

        self.run_clang_tidy()

    def test_MongoCxx20StdChronoCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-cxx20-std-chrono-check'
                WarningsAsErrors: '*'
                """)
        )
        prohibited_types = ["day", "day", "month", "year", "month_day", "month", "day", "day"]
        self.expected_output = [
            f"Illegal use of prohibited type 'std::chrono::{t}'." for t in prohibited_types
        ]
        self.run_clang_tidy()

    def test_MongoStdOptionalCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-std-optional-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = "Use of std::optional, use boost::optional instead."
        opt = "mongo-std-optional-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg}  [{opt}]\n{n:5} | {src}"
            for n, src in [
                (36, "void f(std::optional<std::string> parameterDeclTest) {"),
                (37, "    std::optional<std::string> variableDeclTest;"),
                (42, "    std::optional<int> fieldDeclTest = 5;"),
                (46, "void functionName(const std::optional<int>& referenceDeclTest) {"),
                (52, "std::optional<std::string> functionReturnTypeDeclTest(StringData name);"),
                (55, "std::optional<T> templateDeclTest;"),
                (57, "using std::optional;"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoVolatileCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-volatile-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = 'Illegal use of the volatile storage keyword, use Atomic instead from "mongo/platform/atomic.h"'
        opt = "mongo-volatile-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (31, "volatile int varVolatileTest;"),
                (33, "    volatile int fieldVolatileTest;"),
                (35, "void functionName(volatile int varVolatileTest) {}"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoTraceCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-trace-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = "Illegal use of prohibited tracing support, this is only for local development use and should not be committed."
        opt = "mongo-trace-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (12, "    TracerProvider::initialize();"),
                (13, "    TracerProvider provider = TracerProvider::get();"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoStdAtomicCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-std-atomic-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = 'Illegal use of prohibited std::atomic<T>, use Atomic<T> or other types from "mongo/platform/atomic.h"'
        opt = "mongo-std-atomic-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (6, "std::atomic<int> atomic_var;"),
                (10, "    std::atomic<int> field_decl;"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoAssertCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-assert-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "error: Illegal use of the bare assert macro, use a macro function from assert_util.h instead",
        ]

        self.run_clang_tidy()

    def test_MongoFCVConstantCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-fcv-constant-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "error: Illegal use of FCV constant in FCV comparison check functions. FCV gating should be done through feature flags instead.",
        ]

        self.run_clang_tidy()

    def test_MongoUnstructuredLogCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-unstructured-log-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = "error: Illegal use of unstructured logging, this is only for local development use and should not be committed"
        opt = "mongo-unstructured-log-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (11, "    logd();"),
                (12, "    doUnstructuredLogImpl();"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoConfigHeaderCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-config-header-check'
                WarningsAsErrors: '*'
                HeaderFilterRegex: '(mongo/.*)'
                """)
        )

        msg = "error: MONGO_CONFIG define used without prior inclusion of config.h"
        opt = "mongo-config-header-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (4, "#define MONGO_CONFIG_TEST1 1"),
                (6, "#ifdef MONGO_CONFIG_TEST1"),
                (9, "#if MONGO_CONFIG_TEST1 == 1"),
                (12, "#ifndef MONGO_CONFIG_TEST2"),
                (15, "#if defined(MONGO_CONFIG_TEST1)"),
            ]
        ]
        self.run_clang_tidy()

    def test_MongoCollectionShardingRuntimeCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-collection-sharding-runtime-check'
                WarningsAsErrors: '*'
                CheckOptions:
                    - key:             mongo-collection-sharding-runtime-check.exceptionDirs
                      value:           'src/mongo/db/s'
                """)
        )

        msg = (
            "error: Illegal use of CollectionShardingRuntime outside of mongo/db/s/; "
            "use CollectionShardingState instead; see src/mongo/db/s/collection_sharding_state.h for details."
        )
        opt = "mongo-collection-sharding-runtime-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (21, '    CollectionShardingRuntime csr(5, "Test");'),
                (24, '    int result = CollectionShardingRuntime::functionTest(7, "Test");'),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoMacroDefinitionLeaksCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-macro-definition-leaks-check'
                WarningsAsErrors: '*'
                HeaderFilterRegex: '(mongo/.*)'
                """)
        )

        self.expected_output = [
            "Missing #undef 'MONGO_LOGV2_DEFAULT_COMPONENT'",
        ]

        self.run_clang_tidy()

    def test_MongoNoUniqueAddressCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-no-unique-address-check'
                WarningsAsErrors: '*'
                HeaderFilterRegex: '(mongo/.*)'
                """)
        )

        self.expected_output = [
            "Illegal use of [[no_unique_address]]",
        ]

        self.run_clang_tidy()

    def test_MongoPolyFillCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-polyfill-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "error: Illegal use of banned name from std::/boost:: for std::mutex, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for std::future, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for std::condition_variable, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for std::unordered_map, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for boost::unordered_map, use mongo::stdx:: variant instead",
        ]

        self.run_clang_tidy()

    def test_MongoRandCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-rand-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = "error: Use of rand or srand, use <random> or PseudoRandom instead."
        opt = "mongo-rand-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (5, "    srand(time(0));"),
                (6, "    int random_number = rand();"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoRWMutexCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-rwmutex-check'
                WarningsAsErrors: '*'
                """)
        )

        msg = "error: Prefer using other mutex types over `WriteRarelyRWMutex`."
        opt = "mongo-rwmutex-check,-warnings-as-errors"
        self.expected_output = [
            f"{msg} [{opt}]\n{n:5} | {src}"
            for n, src in [
                (5, "WriteRarelyRWMutex mutex_vardecl;"),
                (8, "    WriteRarelyRWMutex mutex_fielddecl;"),
            ]
        ]

        self.run_clang_tidy()

    def test_MongoStringDataConstRefCheck1(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-stringdata-const-ref-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "Prefer passing StringData by value.",
        ]

        self.run_clang_tidy()

    def test_MongoStringDataConstRefCheck2(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-stringdata-const-ref-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "Prefer passing StringData by value.",
        ]

        self.run_clang_tidy()

    def test_MongoStringDataConstRefCheck3(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-stringdata-const-ref-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "",
        ]

        self.run_clang_tidy()

    def test_MongoInvariantStatusIsOKCheck(self):
        self.write_config(
            textwrap.dedent("""\
                Checks: '-*,mongo-invariant-status-is-ok-check'
                WarningsAsErrors: '*'
                """)
        )

        self.expected_output = [
            "Found invariant(status.isOK()) or dassert(status.isOK()), use invariant(status) for better diagnostics",
        ]

        self.run_clang_tidy()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--clang-tidy-path",
        default="/opt/mongodbtoolchain/v4/bin/clang-tidy",
        help="Path to clang-tidy binary.",
    )
    parser.add_argument(
        "--mongo-tidy-module",
        default="build/install/lib/libmongo_tidy_checks.so",
        help="Path to mongo tidy check library.",
    )
    parser.add_argument(
        "--test-compiledbs",
        action="append",
        default=[],
        help="Used multiple times. Each use adds a test compilation database to use. "
        + "The compilation database name must match the unittest method name.",
    )
    parser.add_argument("unittest_args", nargs="*")

    args = parser.parse_args()

    MongoTidyTests.TIDY_BIN = args.clang_tidy_path
    MongoTidyTests.TIDY_MODULE = args.mongo_tidy_module
    MongoTidyTests.COMPILE_COMMANDS_FILES = args.test_compiledbs

    # We need to validate the toolchain can support the load operation for our module.
    cmd = [MongoTidyTests.TIDY_BIN, "-load", MongoTidyTests.TIDY_MODULE, "--list-checks"]
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode != 0:
        print(f"Could not validate toolchain was able to load module {cmd}.")
        sys.exit(1)

    # Workaround to allow use to use argparse on top of unittest module.
    sys.argv[1:] = args.unittest_args

    unittest.main()
