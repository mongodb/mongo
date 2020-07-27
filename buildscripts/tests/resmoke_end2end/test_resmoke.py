"""Test resmoke's handling of test/task timeouts and archival."""

import logging
import json
import os
import os.path
import sys
import time
import unittest

from buildscripts.resmokelib import core
from buildscripts.resmokelib.utils import rmtree

# pylint: disable=missing-docstring,protected-access


class _ResmokeSelftest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.test_dir = os.path.normpath("/data/db/selftest")
        cls.resmoke_const_args = ["run", "--dbpathPrefix={}".format(cls.test_dir)]

        cls.resmoke_process = None

    def setUp(self):
        #self.test_dir = os.path.normpath("/data/db/selftest")
        self.end2end_root = "buildscripts/tests/resmoke_end2end"
        self.suites_root = f"{self.end2end_root}/suites"
        self.testfiles_root = f"{self.end2end_root}/testfiles"
        self.report_file = os.path.join(self.test_dir, "reports.json")

        self.logger = logging.getLogger(self._testMethodName)
        self.logger.setLevel(logging.DEBUG)
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        self.logger.addHandler(handler)

        self.logger.info("Cleaning temp directory %s", self.test_dir)
        rmtree(self.test_dir, ignore_errors=True)
        os.makedirs(self.test_dir, mode=0o755, exist_ok=True)

    def execute_resmoke(self, resmoke_args, **kwargs):  # pylint: disable=unused-argument
        resmoke_process = core.programs.make_process(
            self.logger,
            [sys.executable, "buildscripts/resmoke.py"] + self.resmoke_const_args + resmoke_args)
        resmoke_process.start()
        self.resmoke_process = resmoke_process

    def assert_dir_file_count(self, test_dir, test_file, num_entries):
        file_path = os.path.join(test_dir, test_file)
        count = 0
        with open(file_path) as file:
            count = sum(1 for _ in file)
        self.assertEqual(count, num_entries)


class TestArchivalOnFailure(_ResmokeSelftest):
    @classmethod
    def setUpClass(cls):
        super(TestArchivalOnFailure, cls).setUpClass()

        cls.archival_file = "test_archival.txt"

    def test_archival_on_task_failure(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)
        self.resmoke_process.wait()

        # test archival
        archival_dirs_to_expect = 4  # 2 tests * 2 nodes
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

    def test_archival_on_task_failure_no_passthrough(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure_no_passthrough.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)
        self.resmoke_process.wait()

        # test archival
        archival_dirs_to_expect = 4  # 2 tests * 2 nodes
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

    def test_no_archival_locally(self):
        # archival should not happen if --taskId is not set.
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure_no_passthrough.yml",
            "--internalParam=test_archival",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)
        self.resmoke_process.wait()

        # test that archival file wasn't created.
        self.assertFalse(os.path.exists(self.archival_file))


class TestTimeout(_ResmokeSelftest):
    @classmethod
    def setUpClass(cls):
        super(TestTimeout, cls).setUpClass()

        cls.test_dir_inner = os.path.normpath("/data/db/selftest_inner")
        cls.archival_file = "test_archival.txt"
        cls.analysis_files = ["debugger_mongo.log", "debugger_mongod.log"]

    def setUp(self):
        super(TestTimeout, self).setUp()
        self.logger.info("Cleaning temp directory %s", self.test_dir_inner)
        rmtree(self.test_dir_inner, ignore_errors=True)
        self.logger.info("Cleaning hang analyzer files %s", str(self.analysis_files))
        for filename in self.analysis_files:
            if os.path.exists(filename):
                os.remove(filename)

    def signal_resmoke(self):
        hang_analyzer_options = f"-o=file -o=stdout -m=contains -d={self.resmoke_process.pid}"
        signal_resmoke_process = core.programs.make_process(
            self.logger, [sys.executable, "buildscripts/resmoke.py", "hang-analyzer"
                          ] + hang_analyzer_options.split())
        signal_resmoke_process.start()

        # Wait for resmoke_process to be killed by 'run-timeout' so this doesn't hang.
        self.resmoke_process.wait()

        return_code = signal_resmoke_process.wait()
        if return_code != 0:
            self.resmoke_process.stop()
        self.assertEqual(return_code, 0)

    def execute_resmoke(self, resmoke_args, sleep_secs=10, **kwargs):  # pylint: disable=arguments-differ
        super(TestTimeout, self).execute_resmoke(resmoke_args, **kwargs)

        time.sleep(sleep_secs
                   )  # TODO: Change to more durable way of ensuring the fixtures have been set up.

        self.signal_resmoke()

    def test_task_timeout(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "--internalParam=test_analysis",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)

        archival_dirs_to_expect = 4  # 2 tests * 2 nodes
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

        for filename in self.analysis_files:
            self.assertTrue(os.path.exists(os.path.join(os.getcwd(), filename)))

    def test_task_timeout_no_passthrough(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout_no_passthrough.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)

        archival_dirs_to_expect = 4  # 2 tests * 2 nodes
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

        for filename in self.analysis_files:
            self.assertTrue(os.path.exists(os.path.join(os.getcwd(), filename)))

    # Test scenarios where an resmoke-launched process launches resmoke.
    def test_nested_timeout(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_nested_timeout.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "jstests/resmoke_selftest/end2end/timeout/nested/top_level_timeout.js",
        ]

        self.execute_resmoke(resmoke_args, sleep_secs=25)

        archival_dirs_to_expect = 2  # 2 tests * 2 nodes / 2 data_file directories
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)
        self.assert_dir_file_count(self.test_dir_inner, self.archival_file, archival_dirs_to_expect)

        for filename in self.analysis_files:
            self.assertTrue(os.path.exists(os.path.join(os.getcwd(), filename)))


class TestTestSelection(_ResmokeSelftest):
    def parse_reports_json(self):
        with open(self.report_file) as fd:
            return json.load(fd)

    def execute_resmoke(self, resmoke_args, **kwargs):  # pylint: disable=unused-argument
        resmoke_process = core.programs.make_process(
            self.logger, [sys.executable, "buildscripts/resmoke.py", "run"] + resmoke_args)
        resmoke_process.start()

        return resmoke_process

    def create_file_in_test_dir(self, filename, contents):
        with open(os.path.normpath(f"{self.test_dir}/{filename}"), "w") as fd:
            fd.write(contents)

    def get_tests_run(self):
        tests_run = []
        for res in self.parse_reports_json()["results"]:
            if "fixture" not in res["test_file"]:
                tests_run.append(res["test_file"])

        return tests_run

    def test_positional_arguments(self):
        self.assertEqual(
            0,
            self.execute_resmoke([
                f"--reportFile={self.report_file}", "--repeatTests=2",
                f"--suites={self.suites_root}/resmoke_no_mongod.yml",
                f"{self.testfiles_root}/one.js", f"{self.testfiles_root}/one.js",
                f"{self.testfiles_root}/one.js"
            ]).wait())

        self.assertEqual(6 * [f"{self.testfiles_root}/one.js"], self.get_tests_run())

    def test_replay_file(self):
        self.create_file_in_test_dir("replay", f"{self.testfiles_root}/two.js\n" * 3)

        self.assertEqual(
            0,
            self.execute_resmoke([
                f"--reportFile={self.report_file}", "--repeatTests=2",
                f"--suites={self.suites_root}/resmoke_no_mongod.yml",
                f"--replay={self.test_dir}/replay"
            ]).wait())

        self.assertEqual(6 * [f"{self.testfiles_root}/two.js"], self.get_tests_run())

    def test_suite_file(self):
        self.assertEqual(
            0,
            self.execute_resmoke([
                f"--reportFile={self.report_file}", "--repeatTests=2",
                f"--suites={self.suites_root}/resmoke_no_mongod.yml"
            ]).wait())

        self.assertEqual(2 * [f"{self.testfiles_root}/one.js", f"{self.testfiles_root}/two.js"],
                         self.get_tests_run())

    def test_at_sign_as_replay_file(self):
        self.create_file_in_test_dir("replay", f"{self.testfiles_root}/two.js\n" * 3)

        self.assertEqual(
            0,
            self.execute_resmoke([
                f"--reportFile={self.report_file}", "--repeatTests=2",
                f"--suites={self.suites_root}/resmoke_no_mongod.yml", f"@{self.test_dir}/replay"
            ]).wait())

        self.assertEqual(6 * [f"{self.testfiles_root}/two.js"], self.get_tests_run())

    def test_disallow_mixing_replay_and_positional(self):
        # Additionally can assert on the error message.
        self.assertEqual(1, self.execute_resmoke(["--replay=foo", "jstests/filename.js"]).wait())
        self.assertEqual(1, self.execute_resmoke(["@foo", "jstests/filename.js"]).wait())

        # Technically errors on file `@foo` not existing. Only the first positional argument can be interpreted as a `@replay_file`. This is a limitation not a feature, this test just serves as documentation.
        self.assertEqual(1, self.execute_resmoke([f"{self.testfiles_root}/one.js", "@foo"]).wait())
