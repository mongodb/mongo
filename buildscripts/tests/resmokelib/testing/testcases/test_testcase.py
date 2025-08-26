"""Tests for buildscripts.resmokelib.testing.testcases.interface"""

import logging
import sys
import threading
import time
import unittest
from unittest.mock import patch

from buildscripts.resmokelib import config
from buildscripts.resmokelib.core.programs import generic_program
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.testing.fixtures.interface import NoOpFixture

# Test cases
from buildscripts.resmokelib.testing.testcases.benchmark_test import BenchmarkTestCase
from buildscripts.resmokelib.testing.testcases.cpp_integration_test import CPPIntegrationTestCase
from buildscripts.resmokelib.testing.testcases.cpp_libfuzzer_test import CPPLibfuzzerTestCase
from buildscripts.resmokelib.testing.testcases.cpp_unittest import CPPUnitTestCase
from buildscripts.resmokelib.testing.testcases.dbtest import DBTestCase
from buildscripts.resmokelib.testing.testcases.interface import ProcessTestCase, TestCase
from buildscripts.resmokelib.testing.testcases.jstest import JSTestCase
from buildscripts.resmokelib.testing.testcases.magic_restore_js_test import MagicRestoreTestCase
from buildscripts.resmokelib.testing.testcases.mongos_test import MongosTestCase
from buildscripts.resmokelib.testing.testcases.multi_stmt_txn_test import MultiStmtTxnTestCase
from buildscripts.resmokelib.testing.testcases.pretty_printer_testcase import PrettyPrinterTestCase
from buildscripts.resmokelib.testing.testcases.pytest import PyTestCase
from buildscripts.resmokelib.testing.testcases.query_tester_self_test import QueryTesterSelfTestCase
from buildscripts.resmokelib.testing.testcases.query_tester_server_test import (
    QueryTesterServerTestCase,
)
from buildscripts.resmokelib.testing.testcases.sdam_json_test import SDAMJsonTestCase
from buildscripts.resmokelib.testing.testcases.server_selection_json_test import (
    ServerSelectionJsonTestCase,
)
from buildscripts.resmokelib.testing.testcases.sleeptest import SleepTestCase
from buildscripts.resmokelib.testing.testcases.tla_plus_test import TLAPlusTestCase


class SimpleTestCase(TestCase):
    def run_test(self):
        self.return_code = 0


class LongRunningSimpleTestCase(TestCase):
    def run_test(self):
        time.sleep(3)
        self.return_code = 0


class SimpleProcessTestCase(ProcessTestCase):
    def _make_process(self):
        return generic_program(self.logger, args=[sys.executable, "-c", "print('hello!')"])


class LongRunningProcessTestCase(ProcessTestCase):
    def _make_process(self):
        return generic_program(
            self.logger, args=[sys.executable, "-c", "import time; time.sleep(3)"]
        )


# Tests the python implementation of these test cases. In many cases, the system under test is replaced
# with the "true" binary, which always succeeds with return code 0.
# TestCase types are documented in buildscripts/resmokelib/testing/testcases/README.md
TESTCASES = [
    (SimpleTestCase, {"test_kind": "simple", "test_name": "mytest"}),
    (SimpleProcessTestCase, {"test_kind": "simple", "test_name": "mytest"}),
    (BenchmarkTestCase, {"program_executables": ["true"]}),
    (CPPIntegrationTestCase, {"program_executables": ["true"]}),
    (CPPLibfuzzerTestCase, {"program_executables": ["true"]}),
    (CPPUnitTestCase, {"program_executables": ["true"]}),
    (DBTestCase, {"dbtest_suites": ["foo"], "dbtest_executable": "true"}),
    (
        JSTestCase,
        {"js_filenames": ["buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.js"]},
    ),
    (
        MagicRestoreTestCase,
        {"js_filenames": ["buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.js"]},
    ),
    (MongosTestCase, {"mongos_options": [{}]}),
    (
        MultiStmtTxnTestCase,
        {
            "multi_stmt_txn_test_files": [
                "buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.js"
            ]
        },
    ),
    (PrettyPrinterTestCase, {"program_executables": ["true"]}),
    (
        PyTestCase,
        {"py_filenames": ["buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.py"]},
    ),
    (
        QueryTesterSelfTestCase,
        {
            "test_filenames": [
                "buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.py"
            ]
        },
    ),
    (
        QueryTesterServerTestCase,
        {
            "test_dir": ["buildscripts/tests/resmokelib/testing/testcases/testfiles/"],
            "wait_for_files": False,
        },
    ),
    (
        SDAMJsonTestCase,
        {
            "json_test_files": [
                "buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.json"
            ],
            "program_executable": "true",
        },
    ),
    (
        ServerSelectionJsonTestCase,
        {
            "json_test_files": [
                "buildscripts/tests/resmokelib/testing/testcases/testfiles/passing.json"
            ],
            "program_executable": "true",
        },
    ),
    (SleepTestCase, {"sleep_duration_secs": 0}),
    (
        TLAPlusTestCase,
        {
            "model_config_files": [
                "buildscripts/tests/resmokelib/testing/testcases/testfiles/passing/MCpassing.cfg"
            ],
            "java_binary": "true",
            "model_check_command": "true",
        },
    ),
]


@unittest.skipIf(
    sys.platform == "win32",
    reason="Mocks out many executables using `true`, which does not exist on Windows.",
)
class TestTestCases(unittest.TestCase):
    def setUp(self):
        # Set config values that may not exist that are needed by various test types.
        self.config_overrides = {
            "BASE_PORT": config.BASE_PORT if config.BASE_PORT else 20000,
            "DEFAULT_MONGOTEST_EXECUTABLE": "true",
            "MONGOS_EXECUTABLE": "true",
            "MONGO_EXECUTABLE": "true",
            "MONGOD_SET_PARAMETERS": None,
            "MONGOS_SET_PARAMETERS": None,
            "MONGO_SET_PARAMETERS": None,
            "MONGOCRYPTD_SET_PARAMETERS": None,
            "LOG_FORMAT": None,
            "LOG_LEVEL": None,
            "MOCHA_GREP": None,
            "REPEAT_SUITES": 0,
            "REPEAT_TESTS": 0,
            "TEST_TIMEOUT": 999,
        }

        self.config_original = {}
        for key in self.config_overrides:
            if hasattr(config, key):
                self.config_original[key] = getattr(config, key)
            setattr(config, key, self.config_overrides[key])

    def tearDown(self):
        for key in self.config_overrides:
            if key in self.config_original:
                setattr(config, key, self.config_original[key])
            else:
                delattr(config, key)

    def test_passing(self):
        logger = logging.getLogger("test_passing")
        logger.setLevel(logging.DEBUG)
        handler = logging.StreamHandler(sys.stdout)
        handler.setLevel(logging.DEBUG)
        logger.addHandler(handler)

        fixture = NoOpFixture(logger, 0, FixtureLib())
        with patch.object(fixture, "get_internal_connection_string", return_value=""):
            for cls, args in TESTCASES:
                print(f"Testing {cls.__name__} with {args}")

                formatter = logging.Formatter(f"[{cls.__name__}] %(message)s")
                handler.setFormatter(formatter)

                testcase = cls(logger, **args)
                testcase.configure(fixture)
                if hasattr(testcase, "configure_shell"):
                    testcase.configure_shell()
                testcase.run_test()

                self.assertEqual(testcase.return_code, 0)
                self.assertFalse(testcase.timed_out.is_set())


class TestTimeouts(unittest.TestCase):
    def setUp(self):
        self.logger = logging.getLogger("TestTimeouts")
        self.logger.setLevel(logging.DEBUG)
        handler = logging.StreamHandler(sys.stdout)
        handler.setLevel(logging.DEBUG)
        self.logger.addHandler(handler)

    def test_simple_timeout(self):
        kind = "LongRunningSimpleTestCase"
        testcase = LongRunningSimpleTestCase(self.logger, kind, "mytest")
        try:
            timer = threading.Timer(2, testcase.on_timeout)
            timer.start()
            testcase.run_test()
        finally:
            timer.cancel()
        self.assertTrue(testcase.timed_out.is_set())
        self.assertTrue(testcase.timed_out_processed.is_set())

    def test_process_timeout(self):
        kind = "LongRunningProcessTestCase"
        testcase = LongRunningProcessTestCase(self.logger, kind, "mytest")
        try:
            timer = threading.Timer(2, testcase.on_timeout)
            timer.start()
            testcase.run_test()
        except testcase.failureException:
            pass
        finally:
            timer.cancel()
        self.assertTrue(testcase.timed_out.is_set())
        self.assertTrue(testcase.timed_out_processed.is_set())
