"""Test resmoke's handling of test/task timeouts and archival."""

import logging
import json
import os
import os.path
import sys
import time
import unittest
from shutil import rmtree

import yaml

from buildscripts.resmokelib import core


class _ResmokeSelftest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.end2end_root = "buildscripts/tests/resmoke_end2end"
        cls.test_dir = os.path.normpath("/data/db/selftest")
        cls.resmoke_const_args = ["run", "--dbpathPrefix={}".format(cls.test_dir)]
        cls.suites_root = os.path.join(cls.end2end_root, "suites")
        cls.testfiles_root = os.path.join(cls.end2end_root, "testfiles")
        cls.report_file = os.path.join(cls.test_dir, "reports.json")

        cls.resmoke_process = None

    def setUp(self):
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
        cls.analysis_file = "test_analysis.txt"

    def setUp(self):
        super(TestTimeout, self).setUp()
        self.logger.info("Cleaning temp directory %s", self.test_dir_inner)
        rmtree(self.test_dir_inner, ignore_errors=True)

    def signal_resmoke(self):
        hang_analyzer_options = f"-o=file -o=stdout -m=contains -p=python -d={self.resmoke_process.pid}"
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

    def execute_resmoke(self, resmoke_args, sleep_secs=30, **kwargs):  # pylint: disable=arguments-differ
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

        archival_dirs_to_expect = 4  # 2 tests * 2 mongod
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

        analysis_pids_to_expect = 6  # 2 tests * (2 mongod + 1 mongo)
        self.assert_dir_file_count(self.test_dir, self.analysis_file, analysis_pids_to_expect)

    def test_task_timeout_no_passthrough(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout_no_passthrough.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "--internalParam=test_analysis",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)

        archival_dirs_to_expect = 4  # 2 tests * 2 nodes
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

        analysis_pids_to_expect = 6  # 2 tests * (2 mongod + 1 mongo)
        self.assert_dir_file_count(self.test_dir, self.analysis_file, analysis_pids_to_expect)

    # Test scenarios where an resmoke-launched process launches resmoke.
    def test_nested_timeout(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_nested_timeout.yml",
            "--taskId=123",
            "--internalParam=test_archival",
            "--internalParam=test_analysis",
            "jstests/resmoke_selftest/end2end/timeout/nested/top_level_timeout.js",
        ]

        self.execute_resmoke(resmoke_args, sleep_secs=25)

        archival_dirs_to_expect = 2  # 2 tests * 2 nodes / 2 data_file directories
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)
        self.assert_dir_file_count(self.test_dir_inner, self.archival_file, archival_dirs_to_expect)

        analysis_pids_to_expect = 6  # 2 tests * (2 mongod + 1 mongo)
        self.assert_dir_file_count(self.test_dir, self.analysis_file, analysis_pids_to_expect)


class TestTestSelection(_ResmokeSelftest):
    def parse_reports_json(self):
        with open(self.report_file) as fd:
            return json.load(fd)

    def execute_resmoke(self, resmoke_args):  # pylint: disable=arguments-differ
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
        self.create_file_in_test_dir("replay", f"{self.testfiles_root}/two.js\n" * 3)

        # Additionally can assert on the error message.
        self.assertEqual(
            2,
            self.execute_resmoke([f"--replay={self.test_dir}/replay",
                                  "jstests/filename.js"]).wait())

        # When multiple positional arguments are presented, they're all treated as test files. Technically errors on file `@<testdir>/replay` not existing. It's not a requirement that this invocation errors in this less specific way.
        self.assertEqual(
            1,
            self.execute_resmoke([f"@{self.test_dir}/replay", "jstests/filename.js"]).wait())
        self.assertEqual(
            1,
            self.execute_resmoke([f"{self.testfiles_root}/one.js",
                                  f"@{self.test_dir}/replay"]).wait())


class TestSetParameters(_ResmokeSelftest):
    def setUp(self):
        self.shell_output_file = None
        super().setUp()

    def parse_output_json(self):
        # Parses the outputted json.
        with open(self.shell_output_file) as fd:
            return json.load(fd)

    def generate_suite(self, suite_output_path, template_file):
        """Read the template file, substitute the `outputLocation` and rewrite to the file `suite_output_path`."""

        with open(os.path.normpath(template_file), "r") as template_suite_fd:
            suite = yaml.safe_load(template_suite_fd)

        try:
            os.remove(suite_output_path)
        except FileNotFoundError:
            pass

        suite["executor"]["config"]["shell_options"]["global_vars"]["TestData"][
            "outputLocation"] = self.shell_output_file
        with open(os.path.normpath(suite_output_path), "w") as fd:
            yaml.dump(suite, fd, default_flow_style=False)

    def generate_suite_and_execute_resmoke(self, suite_template, resmoke_args):
        """Generates a resmoke suite with the appropriate `outputLocation` and runs resmoke against that suite with the `fixture_info` test. Input `resmoke_args` are appended to the run command."""

        self.shell_output_file = f"{self.test_dir}/output.json"
        try:
            os.remove(self.shell_output_file)
        except OSError:
            pass

        suite_file = f"{self.test_dir}/suite.yml"
        self.generate_suite(suite_file, suite_template)

        self.logger.info(
            "Running test. Template suite: %s Rewritten suite: %s Resmoke Args: %s Test output file: %s.",
            suite_template, suite_file, resmoke_args, self.shell_output_file)

        resmoke_process = core.programs.make_process(self.logger, [
            sys.executable, "buildscripts/resmoke.py", "run", f"--suites={suite_file}",
            f"{self.testfiles_root}/fixture_info.js"
        ] + resmoke_args)
        resmoke_process.start()

        return resmoke_process

    def test_suite_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml", []).wait()

        set_params = self.parse_output_json()
        self.assertEqual("1", set_params["enableTestCommands"])
        self.assertEqual("false", set_params["testingDiagnosticsEnabled"])
        self.assertEqual("{'storage': 2}", set_params["logComponentVerbosity"])

    def test_cli_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
            ["""--mongodSetParameter={"enableFlowControl": false, "flowControlMaxSamples": 500}"""
             ]).wait()

        set_params = self.parse_output_json()
        self.assertEqual("1", set_params["enableTestCommands"])
        self.assertEqual("false", set_params["enableFlowControl"])
        self.assertEqual("500", set_params["flowControlMaxSamples"])

    def test_override_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
            ["""--mongodSetParameter={"testingDiagnosticsEnabled": true}"""]).wait()

        set_params = self.parse_output_json()
        self.assertEqual("true", set_params["testingDiagnosticsEnabled"])
        self.assertEqual("{'storage': 2}", set_params["logComponentVerbosity"])

    def test_merge_cli_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml", [
                """--mongodSetParameter={"enableFlowControl": false}""",
                """--mongodSetParameter={"flowControlMaxSamples": 500}"""
            ]).wait()

        set_params = self.parse_output_json()
        self.assertEqual("false", set_params["testingDiagnosticsEnabled"])
        self.assertEqual("{'storage': 2}", set_params["logComponentVerbosity"])
        self.assertEqual("false", set_params["enableFlowControl"])
        self.assertEqual("500", set_params["flowControlMaxSamples"])

    def test_merge_error_cli_set_parameters(self):
        self.assertEqual(
            2,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters.yml", [
                    """--mongodSetParameter={"enableFlowControl": false}""",
                    """--mongodSetParameter={"enableFlowControl": true}"""
                ]).wait())

    def test_mongos_set_parameter(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters_sharding.yml", [
                """--mongosSetParameter={"maxTimeMSForHedgedReads": 100}""",
                """--mongosSetParameter={"mongosShutdownTimeoutMillisForSignaledShutdown": 1000}"""
            ]).wait()

        set_params = self.parse_output_json()
        self.assertEqual("100", set_params["maxTimeMSForHedgedReads"])
        self.assertEqual("1000", set_params["mongosShutdownTimeoutMillisForSignaledShutdown"])

    def test_merge_error_cli_mongos_set_parameter(self):
        self.assertEqual(
            2,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters_sharding.yml", [
                    """--mongosSetParameter={"maxTimeMSForHedgedReads": 100}""",
                    """--mongosSetParameter={"maxTimeMSForHedgedReads": 1000}"""
                ]).wait())

    def test_allow_duplicate_set_parameter_values(self):
        self.assertEqual(
            0,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters.yml", [
                    """--mongodSetParameter={"enableFlowControl": false}""",
                    """--mongodSetParameter={"enableFlowControl": false}"""
                ]).wait())

        self.assertEqual(
            0,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters.yml", [
                    """--mongodSetParameter={"mirrorReads": {samplingRate: 1.0}}""",
                    """--mongodSetParameter={"mirrorReads": {samplingRate: 1.0}}"""
                ]).wait())
