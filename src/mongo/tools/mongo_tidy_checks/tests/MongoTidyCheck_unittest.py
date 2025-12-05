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
            "Use of prohibited \"cctype\" header, use \"mongo/util/ctype.h\"",
            "Use of prohibited <ctype.h> header, use \"mongo/util/ctype.h\"",
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

        self.expected_output = [
            "Illegal use of the volatile storage keyword, use AtomicWord instead from \"mongo/platform/atomic_word.h\" [mongo-volatile-check,-warnings-as-errors]\nvolatile int varVolatileTest;",
            "Illegal use of the volatile storage keyword, use AtomicWord instead from \"mongo/platform/atomic_word.h\" [mongo-volatile-check,-warnings-as-errors]\n    volatile int fieldVolatileTest;",
            "Illegal use of the volatile storage keyword, use AtomicWord instead from \"mongo/platform/atomic_word.h\" [mongo-volatile-check,-warnings-as-errors]\nvoid functionName(volatile int varVolatileTest) {}",
        ]

        self.run_clang_tidy()

    def test_MongoTraceCheck(self):

        self.expected_output = [
            "Illegal use of prohibited tracing support, this is only for local development use and should not be committed. [mongo-trace-check,-warnings-as-errors]\n    TracerProvider::initialize();",
            "Illegal use of prohibited tracing support, this is only for local development use and should not be committed. [mongo-trace-check,-warnings-as-errors]\n    TracerProvider provider = TracerProvider::get();",
        ]

        self.run_clang_tidy()

    def test_MongoStdAtomicCheck(self):

        self.expected_output = [
            "Illegal use of prohibited std::atomic<T>, use AtomicWord<T> or other types from \"mongo/platform/atomic_word.h\" [mongo-std-atomic-check,-warnings-as-errors]\nstd::atomic<int> atomic_var;",
            "Illegal use of prohibited std::atomic<T>, use AtomicWord<T> or other types from \"mongo/platform/atomic_word.h\" [mongo-std-atomic-check,-warnings-as-errors]\n    std::atomic<int> field_decl;",
        ]

        self.run_clang_tidy()

    def test_MongoMutexCheck(self):

        self.expected_output = [
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\nstdx::mutex stdxmutex_vardecl;",
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\nstd::mutex stdmutex_vardecl;",
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\n    std::mutex stdmutex_fileddecl;",
            "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h instead. [mongo-mutex-check,-warnings-as-errors]\n    stdx::mutex stdxmutex_fileddecl;",
        ]

        self.run_clang_tidy()

    def test_MongoAssertCheck(self):

        self.expected_output = [
            "error: Illegal use of the bare assert function, use a function from assert_util.h instead",
        ]

        self.run_clang_tidy()

    def test_MongoFCVConstantCheck(self):

        self.expected_output = [
            "error: Illegal use of FCV constant in FCV comparison check functions. FCV gating should be done through feature flags instead.",
        ]

        self.run_clang_tidy()

    def test_MongoUnstructuredLogCheck(self):

        self.expected_output = [
            "error: Illegal use of unstructured logging, this is only for local development use and should not be committed [mongo-unstructured-log-check,-warnings-as-errors]\n    logd();",
            "error: Illegal use of unstructured logging, this is only for local development use and should not be committed [mongo-unstructured-log-check,-warnings-as-errors]\n    doUnstructuredLogImpl();",
        ]

        self.run_clang_tidy()

    def test_MongoConfigHeaderCheck(self):

        self.expected_output = [
            "error: MONGO_CONFIG define used without prior inclusion of config.h [mongo-config-header-check,-warnings-as-errors]\n#define MONGO_CONFIG_TEST1 1",
            "error: MONGO_CONFIG define used without prior inclusion of config.h [mongo-config-header-check,-warnings-as-errors]\n#ifdef MONGO_CONFIG_TEST1",
            "error: MONGO_CONFIG define used without prior inclusion of config.h [mongo-config-header-check,-warnings-as-errors]\n#if MONGO_CONFIG_TEST1 == 1",
            "error: MONGO_CONFIG define used without prior inclusion of config.h [mongo-config-header-check,-warnings-as-errors]\n#ifndef MONGO_CONFIG_TEST2",
            "error: MONGO_CONFIG define used without prior inclusion of config.h [mongo-config-header-check,-warnings-as-errors]\n#if defined(MONGO_CONFIG_TEST1)",
        ]
        self.run_clang_tidy()

    def test_MongoCollectionShardingRuntimeCheck(self):

        self.expected_output = [
            "error: Illegal use of CollectionShardingRuntime outside of mongo/db/s/; use CollectionShardingState instead; see src/mongo/db/s/collection_sharding_state.h for details. [mongo-collection-sharding-runtime-check,-warnings-as-errors]\n    CollectionShardingRuntime csr(5, \"Test\");",
            "error: Illegal use of CollectionShardingRuntime outside of mongo/db/s/; use CollectionShardingState instead; see src/mongo/db/s/collection_sharding_state.h for details. [mongo-collection-sharding-runtime-check,-warnings-as-errors]\n    int result = CollectionShardingRuntime::functionTest(7, \"Test\");",
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

        self.expected_output = [
            "error: Illegal use of banned name from std::/boost:: for std::mutex, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for std::future, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for std::condition_variable, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for std::unordered_map, use mongo::stdx:: variant instead",
            "error: Illegal use of banned name from std::/boost:: for boost::unordered_map, use mongo::stdx:: variant instead",
        ]

        self.run_clang_tidy()

    def test_MongoRandCheck(self):

        self.expected_output = [
            "error: Use of rand or srand, use <random> or PseudoRandom instead. [mongo-rand-check,-warnings-as-errors]\n    srand(time(0));",
            "error: Use of rand or srand, use <random> or PseudoRandom instead. [mongo-rand-check,-warnings-as-errors]\n    int random_number = rand();",
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

if __name__ == "__main__":
    unittest.main()