import os
import re
import subprocess
import sys
import unittest


def _extract_extra_bazel_args(argv):
    if "--" not in argv:
        return [], argv

    separator_index = argv.index("--")
    return argv[:separator_index], argv[separator_index + 1 :]


class MongoTidyTests(unittest.TestCase):
    extra_bazel_args = []

    def setUp(self):
        m = re.search(r"\.test_(.*)$", self.id())
        if not m:
            self.fail(f"Unexpected test id format: {self.id()}")
        self._name = m[1]

    def run_clang_tidy(self, expected: list[str]):
        base_path = f"//src/mongo/tools/mongo_tidy_checks/tests:test_{self._name}"
        cmd = [
            "bazel",
            "build",
            "--config=clang-tidy",
            "--skip_archive=False",
            *self.extra_bazel_args,
            "--build_tag_filters=mongo-tidy-tests",
            f"--@bazel_clang_tidy//:clang_tidy_config={base_path}_tidy_config",
            f"{base_path}_with_debug",
        ]
        p = subprocess.run(
            cmd,
            cwd=os.environ.get("BUILD_WORKSPACE_DIRECTORY"),
            capture_output=True,
            text=True,
            check=False,
        )
        passed = all(s in p.stdout for s in expected)

        msg = "\n".join(
            [
                ">" * 80,
                f"Mongo Tidy Unittest {self._name}: {'PASSED' if passed else 'FAILED'}",
                "",
                "Command:",
                " ".join(cmd),
                "",
                f"Exit code was: {p.returncode}",
                "",
                "Output expected in stdout: ",
                "\n".join(f"EXPECT: {s}" for s in expected),
                "",
                "stdout was:",
                "\n".join(f"STDOUT: {s}" for s in p.stdout.splitlines()),
                "",
                "stderr was:",
                "\n".join(f"STDERR: {s}" for s in p.stderr.splitlines()),
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

    def test_MongoBypassDatabaseMetadataAccessCheck(self):
        self.run_clang_tidy(
            [
                "Potentially incorrect use of BypassDatabaseMetadataAccessCheck: operations that "
                "modify the database metadata must acquire the critical section, and operations "
                "that read the database metadata must ensure that no one else is holding the "
                "critical section. Review carefully, and if the use is warranted, add NOLINT "
                "with a comment explaining why."
            ]
        )

    def test_MongoHeaderBracketCheck(self):
        self.run_clang_tidy(
            [
                f"error: {e}"
                for e in (
                    "non-mongo include 'cctype' should use angle brackets",
                    "mongo include 'test_MongoHeaderBracketCheck.h' should use double quotes",
                    "third_party include 'third_party/s2/hash.h' should not start with "
                    "'third_party/'",
                )
            ]
        )

    def test_MongoHeaderIncludePathCheck(self):
        self.run_clang_tidy(
            [
                f"mongo include '{h}' should be referenced from 'src/' as 'mongo/{h}'"
                for h in ("base/data_range.h", "base/data_type.h")
            ]
        )

    def test_MongoUninterruptibleLockGuardCheck(self):
        self.run_clang_tidy(
            [
                "Potentially incorrect use of UninterruptibleLockGuard, "
                "the programming model inside MongoDB requires that all "
                "operations be interruptible. Review with care and if the "
                "use is warranted, add NOLINT and a comment explaining why."
            ]
        )

    def test_MongoUninterruptibleLockGuardCheckForOpCtxMember(self):
        self.run_clang_tidy(
            [
                "Potentially incorrect use of "
                "OperationContext::uninterruptibleLocksRequested_DO_NOT_USE, this is a legacy "
                "interruption mechanism that makes lock acquisition ignore interrupts. "
                "Please ensure this use is warranted and if so add a NOLINT comment "
                "explaining why. Please also add Service Arch to the PR so that we can "
                "verify this is necessary and there are no alternative workarounds."
            ]
        )

    def test_MongoCctypeCheck(self):
        self.run_clang_tidy(
            [
                f'Use of prohibited {h} header, use "mongo/util/ctype.h"'
                for h in ('"cctype"', "<ctype.h>")
            ]
        )

    def test_MongoCxx20BannedIncludesCheck(self):
        self.run_clang_tidy(
            [
                f"Use of prohibited {h} header."
                for h in [
                    "<syncstream>",
                    "<ranges>",
                    "<barrier>",
                    "<latch>",
                    "<semaphore>",
                ]
            ]
        )

    def test_MongoCxx20StdChronoCheck(self):
        self.run_clang_tidy(
            [
                f"Illegal use of prohibited type 'std::chrono::{t}'."
                for t in [
                    "day",
                    "day",
                    "month",
                    "year",
                    "month_day",
                    "month",
                    "day",
                    "day",
                ]
            ]
        )

    def test_MongoVolatileCheck(self):
        msg = (
            "Illegal use of the volatile storage keyword, "
            'use Atomic instead from "mongo/platform/atomic.h"'
        )
        opt = "mongo-volatile-check,-warnings-as-errors"
        self.run_clang_tidy(
            [
                f"{msg} [{opt}]\n{n:5} | {src}"
                for n, src in [
                    (3, "volatile int varVolatileTest;"),
                    (5, "    volatile int fieldVolatileTest;"),
                    (7, "void functionName(volatile int varVolatileTest) {}"),
                ]
            ]
        )

    def test_MongoTraceCheck(self):
        msg = (
            "Illegal use of prohibited tracing support, "
            "this is only for local development use and should not be committed."
        )
        opt = "mongo-trace-check,-warnings-as-errors"
        self.run_clang_tidy(
            [
                f"{msg} [{opt}]\n{n:5} | {src}"
                for n, src in [
                    (12, "    TracerProvider::initialize();"),
                    (13, "    TracerProvider provider = TracerProvider::get();"),
                ]
            ]
        )

    def test_MongoAssertCheck(self):
        self.run_clang_tidy(
            [
                "error: Illegal use of the bare assert macro, "
                "use a macro function from assert_util.h instead",
            ]
        )

    def test_MongoFCVConstantCheck(self):
        self.run_clang_tidy(
            [
                "error: Illegal use of FCV constant in FCV comparison check functions. "
                "FCV gating should be done through feature flags instead.",
            ]
        )

    def test_MongoUnstructuredLogCheck(self):
        msg = (
            "error: Illegal use of unstructured logging, this is only for local "
            "development use and should not be committed"
        )
        opt = "mongo-unstructured-log-check,-warnings-as-errors"
        self.run_clang_tidy(
            [
                f"{msg} [{opt}]\n{n:5} | {src}"
                for n, src in [
                    (11, "    logd();"),
                    (12, "    doUnstructuredLogImpl();"),
                ]
            ]
        )

    def test_MongoConfigHeaderCheck(self):
        msg = "error: MONGO_CONFIG define used without prior inclusion of config.h"
        opt = "mongo-config-header-check,-warnings-as-errors"
        self.run_clang_tidy(
            [
                f"{msg} [{opt}]\n{n:5} | {src}"
                for n, src in [
                    (4, "#define MONGO_CONFIG_TEST1 1"),
                    (6, "#ifdef MONGO_CONFIG_TEST1"),
                    (9, "#if MONGO_CONFIG_TEST1 == 1"),
                    (12, "#ifndef MONGO_CONFIG_TEST2"),
                    (15, "#if defined(MONGO_CONFIG_TEST1)"),
                ]
            ]
        )

    def test_MongoMacroDefinitionLeaksCheck(self):
        self.run_clang_tidy(
            [
                "Missing #undef 'MONGO_LOGV2_DEFAULT_COMPONENT'",
            ]
        )

    def test_MongoNoUniqueAddressCheck(self):
        self.run_clang_tidy(["Illegal use of [[no_unique_address]]"])

    def test_MongoBannedNamesCheck(self):
        stdx_note = (
            "Consider using alternatives such as the polyfills from the mongo::stdx:: namespace."
        )

        test_names = [
            ("std::get_terminate()", stdx_note),
            ("std::future<int> myFuture", "Consider using mongo::Future instead."),
            (
                "std::recursive_mutex recursiveMut",
                "Do not use. A recursive mutex is often an indication of a design problem "
                "and is prone to deadlocks because you don't know what code you are calling "
                "while holding the lock.",
            ),
            ("const std::condition_variable cv", stdx_note),
            ("static std::unordered_map<int, int> myMap", stdx_note),
            ("boost::unordered_map<int, int> boostMap", stdx_note),
            (
                'std::regex_search(std::string(""), std::regex(""))',
                "Consider using mongo::pcre::Regex instead.",
            ),
            (
                "std::jthread t([] {})",
                "Do not use. Consider stdx::thread with an explicit join in production or "
                "unittest::JoinThread in unit tests.",
            ),
            ("std::atomic<int> atomicVar", "Consider using mongo::Atomic<T> instead."),
            (
                "std::optional<std::string> strOpt",
                "Consider using boost::optional instead.",
            ),
            ("std::atomic<int> fieldDecl", "Consider using mongo::Atomic<T> instead."),
            (
                "std::optional<T> templateDecl",
                "Consider using boost::optional instead.",
            ),
            ("using std::optional", "Consider using boost::optional instead."),
        ]

        self.run_clang_tidy(
            [
                f"error: Forbidden use of banned name in {name}. {msg} "
                "Use '//  NOLINT' if usage is absolutely necessary. Be especially "
                "careful doing so outside of test code."
                for (name, msg) in test_names
            ]
        )

    def test_MongoRandCheck(self):
        msg = "error: Use of rand or srand, use <random> or PseudoRandom instead."
        opt = "mongo-rand-check,-warnings-as-errors"
        self.run_clang_tidy(
            [
                f"{msg} [{opt}]\n{n:5} | {src}"
                for n, src in [
                    (5, "    srand(time(0));"),
                    (6, "    int random_number = rand();"),
                ]
            ]
        )

    def test_MongoRWMutexCheck(self):
        msg = "error: Prefer using other mutex types over `WriteRarelyRWMutex`."
        opt = "mongo-rwmutex-check,-warnings-as-errors"
        self.run_clang_tidy(
            [
                f"{msg} [{opt}]\n{n:5} | {src}"
                for n, src in [
                    (5, "WriteRarelyRWMutex mutex_vardecl;"),
                    (8, "    WriteRarelyRWMutex mutex_fielddecl;"),
                ]
            ]
        )

    def test_MongoInvariantStatusIsOKCheck(self):
        self.run_clang_tidy(
            [
                "Found invariant(status.isOK()) or dassert(status.isOK()), use "
                "invariant(status) for better diagnostics",
            ]
        )

    def test_MongoInvariantShardingCoordinatorCheck(self):
        errmsg = (
            "Use 'tassert' instead of 'invariant' in sharding coordinator code. "
            "Invariants in sharding coordinators are prone to crash loops."
        )
        self.run_clang_tidy(
            [
                f"{file}:{line}:{column}: error: {errmsg}"
                for file, line, column in (
                    (".cpp", 5, 5),
                    (".cpp", 12, 5),
                    (".h", 9, 5),
                )
            ]
        )

    def test_MongoBannedCatalogAccessFromQueryCodeCheck(self):
        self.run_clang_tidy(
            [
                f"{t} is not allowed to be used from the query modules. "
                "Use ShardRole CollectionAcquisitions instead."
                for t in ("AutoGetCollection", "CollectionCatalog")
            ]
        )


class MongoTidyArgParsingTests(unittest.TestCase):
    def test_extract_extra_bazel_args_without_separator(self):
        self.assertEqual(
            _extract_extra_bazel_args(["MongoTidyTests.test_MongoHeaderBracketCheck"]),
            ([], ["MongoTidyTests.test_MongoHeaderBracketCheck"]),
        )

    def test_extract_extra_bazel_args_with_separator(self):
        self.assertEqual(
            _extract_extra_bazel_args(
                [
                    "--remote_execution_priority=2",
                    "--",
                    "MongoTidyTests.test_MongoHeaderBracketCheck",
                ]
            ),
            (
                ["--remote_execution_priority=2"],
                ["MongoTidyTests.test_MongoHeaderBracketCheck"],
            ),
        )

    def test_extract_extra_bazel_args_with_only_separator(self):
        self.assertEqual(_extract_extra_bazel_args(["--"]), ([], []))


if __name__ == "__main__":
    MongoTidyTests.extra_bazel_args, unittest_args = _extract_extra_bazel_args(sys.argv[1:])
    unittest.main(argv=[sys.argv[0], *unittest_args])
