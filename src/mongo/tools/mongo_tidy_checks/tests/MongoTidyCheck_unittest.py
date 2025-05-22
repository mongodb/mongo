import os
import subprocess
import sys
import unittest


class MongoTidyTests(unittest.TestCase):
    def run_clang_tidy(self):
        cmd = [
            "bazel",
            "build",
            "--config=clang-tidy",
            "--skip_archive=False",
            "--build_tag_filters=mongo-tidy-tests",
            "--@bazel_clang_tidy//:clang_tidy_config=//src/mongo/tools/mongo_tidy_checks/tests:"
            + self._testMethodName
            + "_tidy_config",
            "//src/mongo/tools/mongo_tidy_checks/tests:" + self._testMethodName + "_with_debug",
        ]
        p = subprocess.run(
            cmd, cwd=os.environ.get("BUILD_WORKSPACE_DIRECTORY"), capture_output=True, text=True
        )

        if isinstance(self.expected_output, list):
            passed = all([expected_output in p.stdout for expected_output in self.expected_output])
            print_expected_output = "\n".join(self.expected_output)
        else:
            passed = self.expected_output is not None and self.expected_output in p.stdout
            print_expected_output = self.expected_output

        msg = "\n".join(
            [
                ">" * 80,
                f"Mongo Tidy Unittest {self._testMethodName}: {'PASSED' if passed else 'FAILED'}",
                "",
                "Command:",
                " ".join(cmd),
                "",
                f"Exit code was: {p.returncode}",
                "",
                "Output expected in stdout: ",
                f"{print_expected_output}",
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
            if os.environ.get("CI"):
                print(msg)
        else:
            sys.stderr.write(msg)
            self.fail()

    def test_MongoHeaderBracketCheck(self):
        self.expected_output = [
            "error: non-mongo include 'cctype' should use angle brackets",
            "error: mongo include 'test_MongoHeaderBracketCheck.h' should use double quotes",
            "error: third_party include 'third_party/s2/hash.h' should not start with 'third_party/'",
        ]

        self.run_clang_tidy()

    def test_MongoUninterruptibleLockGuardCheck(self):
        self.expected_output = (
            "Potentially incorrect use of UninterruptibleLockGuard, "
            "the programming model inside MongoDB requires that all operations be interruptible. "
            "Review with care and if the use is warranted, add NOLINT and a comment explaining why."
        )

        self.run_clang_tidy()

    def test_MongoUninterruptibleLockGuardCheckForOpCtxMember(self):
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
        self.expected_output = [
            'Use of prohibited "cctype" header, use "mongo/util/ctype.h"',
            'Use of prohibited <ctype.h> header, use "mongo/util/ctype.h"',
        ]

        self.run_clang_tidy()

    def test_MongoCxx20BannedIncludesCheck(self):
        self.expected_output = [
            "Use of prohibited <syncstream> header.",
            "Use of prohibited <ranges> header.",
            "Use of prohibited <barrier> header.",
            "Use of prohibited <latch> header.",
            "Use of prohibited <semaphore> header.",
        ]

        self.run_clang_tidy()

    def test_MongoCxx20StdChronoCheck(self):
        prohibited_types = ["day", "day", "month", "year", "month_day", "month", "day", "day"]
        self.expected_output = [
            f"Illegal use of prohibited type 'std::chrono::{t}'." for t in prohibited_types
        ]
        self.run_clang_tidy()

    def test_MongoStdOptionalCheck(self):
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
        self.expected_output = [
            "error: Illegal use of the bare assert macro, use a macro function from assert_util.h instead",
        ]

        self.run_clang_tidy()

    def test_MongoFCVConstantCheck(self):
        self.expected_output = [
            "error: Illegal use of FCV constant in FCV comparison check functions. FCV gating should be done through feature flags instead.",
        ]

        self.run_clang_tidy()

    def test_MongoUnstructuredLogCheck(self):
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
        self.expected_output = [
            "Missing #undef 'MONGO_LOGV2_DEFAULT_COMPONENT'",
        ]

        self.run_clang_tidy()

    def test_MongoNoUniqueAddressCheck(self):
        self.expected_output = [
            "Illegal use of [[no_unique_address]]",
        ]

        self.run_clang_tidy()

    def test_MongoPolyFillCheck(self):
        test_names = [
            "std::cv_status wait_result{std::cv_status::timeout}",
            "std::cv_status::timeout",
            "std::cv_status::timeout",
            "std::this_thread::get_id",
            "std::get_terminate()",
            "std::chrono::seconds(1)",
            "std::future<int> myFuture",
            "std::condition_variable cv",
            "std::unordered_map<int, int> myMap",
            "boost::unordered_map<int, int> boostMap",
        ]

        self.expected_output = [
            "error: Illegal use of banned name from std::/boost:: for "
            + name
            + ". Consider using alternatives such as the polyfills from the mongo::stdx:: namespace."
            for name in test_names
        ]

        self.run_clang_tidy()

    def test_MongoRandCheck(self):
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
        self.expected_output = [
            "Prefer passing StringData by value.",
        ]

        self.run_clang_tidy()

    def test_MongoStringDataConstRefCheck2(self):
        self.expected_output = [
            "Prefer passing StringData by value.",
        ]

        self.run_clang_tidy()

    def test_MongoStringDataConstRefCheck3(self):
        self.expected_output = [
            "",
        ]

        self.run_clang_tidy()

    def test_MongoStringDataStringViewApiCheck(self):
        self.expected_output = [
            "replace 'rawData' with 'data'",
            "replace 'startsWith' with 'starts_with'",
            "replace 'endsWith' with 'ends_with'",
        ]

        self.run_clang_tidy()

    def test_MongoInvariantStatusIsOKCheck(self):
        self.expected_output = [
            "Found invariant(status.isOK()) or dassert(status.isOK()), use invariant(status) for better diagnostics",
        ]

        self.run_clang_tidy()


if __name__ == "__main__":
    unittest.main()
