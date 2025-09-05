"""Test resmoke's handling of test/task timeouts and archival."""

import datetime
import io
import json
import logging
import os
import os.path
import re
import subprocess
import sys
import time
import unittest
from shutil import rmtree
from typing import List

import yaml

from buildscripts.ciconfig.evergreen import parse_evergreen_file
from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_off_by_default
from buildscripts.resmokelib import config, core, suitesconfig
from buildscripts.resmokelib.hang_analyzer.attach_core_analyzer_task import (
    matches_generated_task_pattern,
)
from buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks import get_generated_task_name
from buildscripts.resmokelib.utils.dictionary import get_dict_value


class _ResmokeSelftest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.end2end_root = "buildscripts/tests/resmoke_end2end"
        cls.test_dir = os.path.normpath("/data/db/selftest")
        cls.resmoke_const_args = [
            "run",
            "--dbpathPrefix={}".format(cls.test_dir),
            "--skipSymbolization",
        ]
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

    def execute_resmoke(self, resmoke_args, **kwargs):
        resmoke_process = core.programs.make_process(
            self.logger,
            [sys.executable, "buildscripts/resmoke.py"] + self.resmoke_const_args + resmoke_args,
        )
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
        # The --originSuite argument is to trick the resmoke local invocation into passing
        # because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
        # and cannot normally find an evergreen task associated with
        # buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure.yml
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure.yml",
            "--originSuite=resmoke_end2end_tests",
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
        # The --originSuite argument is to trick the resmoke local invocation into passing
        # because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
        # and cannot normally find an evergreen task associated with
        # buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure_no_passthrough.yml
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_failure_no_passthrough.yml",
            "--taskId=123",
            "--originSuite=resmoke_end2end_tests",
            "--internalParam=test_archival",
            "--repeatTests=2",
            "--jobs=2",
        ]
        self.execute_resmoke(resmoke_args)
        self.resmoke_process.wait()

        # test archival
        archival_dirs_to_expect = 8  # (2 tests + 2 stacktrace files) * 2 nodes
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
        hang_analyzer_options = (
            f"-o=file -o=stdout -m=contains -p=python -d={self.resmoke_process.pid}"
        )
        signal_resmoke_process = core.programs.make_process(
            self.logger,
            [sys.executable, "buildscripts/resmoke.py", "hang-analyzer"]
            + hang_analyzer_options.split(),
        )
        signal_resmoke_process.start()

        # Wait for resmoke_process to be killed by 'run-timeout' so this doesn't hang.
        self.resmoke_process.wait()

        return_code = signal_resmoke_process.wait()
        if return_code != 0:
            self.resmoke_process.stop()
        self.assertEqual(return_code, 0)

    def execute_resmoke(self, resmoke_args, sentinel_file, **kwargs):
        # Since this test is designed to start remoke, wait for it to be up-and-running, and then
        # kill resmoke, we use a sentinel file to accomplish this.

        # Form sentinel path, remove any leftover version, and create the file that will be removed by the jstest.
        sentinel_path = f"{os.environ.get('TMPDIR') or os.environ.get('TMP_DIR') or '/tmp'}/{sentinel_file}.js.sentinel"
        if os.path.isfile(sentinel_path):
            os.remove(sentinel_path)
        open(sentinel_path, "w").close()

        # Spawn resmoke (async):
        super(TestTimeout, self).execute_resmoke(resmoke_args, **kwargs)

        # Wait for sentinel file to disappear; bail if it takes too long:
        started_polling_datetime = datetime.datetime.now()
        while os.path.isfile(sentinel_path):
            time.sleep(0.1)
            if datetime.datetime.now() - started_polling_datetime > datetime.timedelta(minutes=5):
                self.fail("SUT is not available within 5 minutes; aborting test")

        # Kill resmoke:
        self.signal_resmoke()

    def test_task_timeout(self):
        # The --originSuite argument is to trick the resmoke local invocation into passing
        # because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
        # and cannot normally find an evergreen task associated with
        # buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout.yml
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout.yml",
            "--taskId=123",
            "--originSuite=resmoke_end2end_tests",
            "--internalParam=test_archival",
            "--internalParam=test_analysis",
        ]
        self.execute_resmoke(resmoke_args, sentinel_file="timeout0")

        archival_dirs_to_expect = 2  # 2 mongod nodes
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

        analysis_pids_to_expect = 3  # 2 mongod + 1 mongo
        self.assert_dir_file_count(self.test_dir, self.analysis_file, analysis_pids_to_expect)

    def test_task_timeout_no_passthrough(self):
        # The --originSuite argument is to trick the resmoke local invocation into passing
        # because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
        # and cannot normally find an evergreen task associated with
        # buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout_no_passthrough.yml
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout_no_passthrough.yml",
            "--taskId=123",
            "--originSuite=resmoke_end2end_tests",
            "--internalParam=test_archival",
            "--internalParam=test_analysis",
        ]
        self.execute_resmoke(resmoke_args, sentinel_file="timeout1")

        archival_dirs_to_expect = 4  # 2 mongod nodes + 2 stacktrace files
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)

        analysis_pids_to_expect = 3  # 2 mongod + 1 mongo
        self.assert_dir_file_count(self.test_dir, self.analysis_file, analysis_pids_to_expect)

    # Test scenarios where an resmoke-launched process launches resmoke.
    def test_nested_timeout(self):
        # The --originSuite argument is to trick the resmoke local invocation into passing
        # because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
        # and cannot normally find an evergreen task associated with
        # buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_nested_timeout.yml
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_nested_timeout.yml",
            "--taskId=123",
            "--originSuite=resmoke_end2end_tests",
            "--internalParam=test_archival",
            "--internalParam=test_analysis",
            "jstests/resmoke_selftest/end2end/timeout/nested/top_level_timeout.js",
        ]

        self.execute_resmoke(resmoke_args, sentinel_file="inner_level_timeout")

        archival_dirs_to_expect = (
            4  # ((2 tests + 2 stacktrace files) * 2 nodes) / 2 data_file directories
        )
        self.assert_dir_file_count(self.test_dir, self.archival_file, archival_dirs_to_expect)
        self.assert_dir_file_count(self.test_dir_inner, self.archival_file, archival_dirs_to_expect)

        analysis_pids_to_expect = 6  # 2 tests * (2 mongod + 1 mongo)
        self.assert_dir_file_count(self.test_dir, self.analysis_file, analysis_pids_to_expect)


class TestTestTimeout(_ResmokeSelftest):
    def test_individual_test_timeout(self):
        # The --originSuite argument is to trick the resmoke local invocation into passing
        # because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
        # and cannot normally find an evergreen task associated with
        # buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_task_timeout.yml
        reportFile = os.path.join(self.test_dir, "report.json")
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_test_timeout.yml",
            "--taskId=123",
            "--originSuite=resmoke_end2end_tests",
            "--testTimeout=5",
            "--internalParam=test_analysis",
            "--continueOnFailure",
            f"--reportFile={reportFile}",
        ]
        self.execute_resmoke(resmoke_args)
        self.resmoke_process.wait()

        analysis_pids_to_expect = 2  # 1 tests * (1 mongod + 1 mongo)
        self.assert_dir_file_count(self.test_dir, "test_analysis.txt", analysis_pids_to_expect)

        with open(reportFile, "r") as f:
            report = json.load(f)
        timeout = [test for test in report["results"] if test["status"] == "timeout"]
        passed = [test for test in report["results"] if test["status"] == "pass"]
        self.assertEqual(len(timeout), 1, f"Expected one timed out test. Got {timeout}")  # one jstest
        self.assertEqual(
            len(passed), 3, f"Expected 3 passing tests. Got {passed}"
        )  # one jstest, one fixture setup, one fixture teardown

class TestTestSelection(_ResmokeSelftest):
    def parse_reports_json(self):
        with open(self.report_file) as fd:
            return json.load(fd)

    def execute_resmoke(self, resmoke_args):
        resmoke_process = core.programs.make_process(
            self.logger, [sys.executable, "buildscripts/resmoke.py", "run"] + resmoke_args
        )
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

    def test_missing_excluded_file(self):
        # Tests a suite that excludes a missing file
        self.assertEqual(
            0,
            self.execute_resmoke(
                [
                    f"--reportFile={self.report_file}",
                    "--repeatTests=2",
                    f"--suites={self.suites_root}/resmoke_missing_test.yml",
                    f"{self.testfiles_root}/one.js",
                    f"{self.testfiles_root}/one.js",
                    f"{self.testfiles_root}/one.js",
                ]
            ).wait(),
        )

        self.assertEqual(6 * [f"{self.testfiles_root}/one.js"], self.get_tests_run())

    def test_positional_arguments(self):
        self.assertEqual(
            0,
            self.execute_resmoke(
                [
                    f"--reportFile={self.report_file}",
                    "--repeatTests=2",
                    f"--suites={self.suites_root}/resmoke_no_mongod.yml",
                    f"{self.testfiles_root}/one.js",
                    f"{self.testfiles_root}/one.js",
                    f"{self.testfiles_root}/one.js",
                ]
            ).wait(),
        )

        self.assertEqual(6 * [f"{self.testfiles_root}/one.js"], self.get_tests_run())

    def test_replay_file(self):
        self.create_file_in_test_dir("replay", f"{self.testfiles_root}/two.js\n" * 3)

        self.assertEqual(
            0,
            self.execute_resmoke(
                [
                    f"--reportFile={self.report_file}",
                    "--repeatTests=2",
                    f"--suites={self.suites_root}/resmoke_no_mongod.yml",
                    f"--replay={self.test_dir}/replay",
                ]
            ).wait(),
        )

        self.assertEqual(6 * [f"{self.testfiles_root}/two.js"], self.get_tests_run())

    def test_suite_file(self):
        self.assertEqual(
            0,
            self.execute_resmoke(
                [
                    f"--reportFile={self.report_file}",
                    "--repeatTests=2",
                    f"--suites={self.suites_root}/resmoke_no_mongod.yml",
                ]
            ).wait(),
        )

        self.assertEqual(
            2 * [f"{self.testfiles_root}/one.js", f"{self.testfiles_root}/two.js"],
            self.get_tests_run(),
        )

    def test_at_sign_as_replay_file(self):
        self.create_file_in_test_dir("replay", f"{self.testfiles_root}/two.js\n" * 3)

        self.assertEqual(
            0,
            self.execute_resmoke(
                [
                    f"--reportFile={self.report_file}",
                    "--repeatTests=2",
                    f"--suites={self.suites_root}/resmoke_no_mongod.yml",
                    f"@{self.test_dir}/replay",
                ]
            ).wait(),
        )

        self.assertEqual(6 * [f"{self.testfiles_root}/two.js"], self.get_tests_run())

    def test_disallow_mixing_replay_and_positional(self):
        self.create_file_in_test_dir("replay", f"{self.testfiles_root}/two.js\n" * 3)

        # Additionally can assert on the error message.
        self.assertEqual(
            2,
            self.execute_resmoke(
                [f"--replay={self.test_dir}/replay", f"{self.testfiles_root}/one.js"]
            ).wait(),
        )

        # When multiple positional arguments are presented, they're all treated as test files.
        self.assertEqual(
            2,
            self.execute_resmoke(
                [f"@{self.test_dir}/replay", f"{self.testfiles_root}/one.js"]
            ).wait(),
        )
        self.assertEqual(
            2,
            self.execute_resmoke(
                [f"{self.testfiles_root}/one.js", f"@{self.test_dir}/replay"]
            ).wait(),
        )


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

        with open(os.path.normpath(template_file), "r", encoding="utf8") as template_suite_fd:
            suite = yaml.safe_load(template_suite_fd)

        try:
            os.remove(suite_output_path)
        except FileNotFoundError:
            pass

        suite["executor"]["config"]["shell_options"]["global_vars"]["TestData"][
            "outputLocation"
        ] = self.shell_output_file
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
            suite_template,
            suite_file,
            resmoke_args,
            self.shell_output_file,
        )

        resmoke_process = core.programs.make_process(
            self.logger,
            [sys.executable, "buildscripts/resmoke.py", "run", f"--suites={suite_file}"]
            + resmoke_args,
        )
        resmoke_process.start()

        return resmoke_process

    def test_suite_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml", []
        ).wait()

        set_params = self.parse_output_json()
        self.assertEqual("1", set_params["enableTestCommands"])
        self.assertEqual("false", set_params["testingDiagnosticsEnabled"])
        self.assertEqual("{'storage': 2}", set_params["logComponentVerbosity"])

    def test_cli_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
            ["""--mongodSetParameter={"enableFlowControl": false, "flowControlMaxSamples": 500}"""],
        ).wait()

        set_params = self.parse_output_json()
        self.assertEqual("1", set_params["enableTestCommands"])
        self.assertEqual("false", set_params["enableFlowControl"])
        self.assertEqual("500", set_params["flowControlMaxSamples"])

    def test_override_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
            ["""--mongodSetParameter={"testingDiagnosticsEnabled": true}"""],
        ).wait()

        set_params = self.parse_output_json()
        self.assertEqual("true", set_params["testingDiagnosticsEnabled"])
        self.assertEqual("{'storage': 2}", set_params["logComponentVerbosity"])

    def test_merge_cli_set_parameters(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
            [
                """--mongodSetParameter={"enableFlowControl": false}""",
                """--mongodSetParameter={"flowControlMaxSamples": 500}""",
            ],
        ).wait()

        set_params = self.parse_output_json()
        self.assertEqual("false", set_params["testingDiagnosticsEnabled"])
        self.assertEqual("{'storage': 2}", set_params["logComponentVerbosity"])
        self.assertEqual("false", set_params["enableFlowControl"])
        self.assertEqual("500", set_params["flowControlMaxSamples"])

    def test_merge_error_cli_set_parameters(self):
        self.assertEqual(
            2,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
                [
                    """--mongodSetParameter={"enableFlowControl": false}""",
                    """--mongodSetParameter={"enableFlowControl": true}""",
                ],
            ).wait(),
        )

    def test_mongos_set_parameter(self):
        self.generate_suite_and_execute_resmoke(
            f"{self.suites_root}/resmoke_selftest_set_parameters_sharding.yml",
            [
                """--mongosSetParameter={"maxTimeMSForHedgedReads": 100}""",
                """--mongosSetParameter={"mongosShutdownTimeoutMillisForSignaledShutdown": 1000}""",
            ],
        ).wait()

        set_params = self.parse_output_json()
        self.assertEqual("100", set_params["maxTimeMSForHedgedReads"])
        self.assertEqual("1000", set_params["mongosShutdownTimeoutMillisForSignaledShutdown"])

    def test_merge_error_cli_mongos_set_parameter(self):
        self.assertEqual(
            2,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters_sharding.yml",
                [
                    """--mongosSetParameter={"maxTimeMSForHedgedReads": 100}""",
                    """--mongosSetParameter={"maxTimeMSForHedgedReads": 1000}""",
                ],
            ).wait(),
        )

    def test_allow_duplicate_set_parameter_values(self):
        self.assertEqual(
            0,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
                [
                    """--mongodSetParameter={"enableFlowControl": false}""",
                    """--mongodSetParameter={"enableFlowControl": false}""",
                ],
            ).wait(),
        )

        self.assertEqual(
            0,
            self.generate_suite_and_execute_resmoke(
                f"{self.suites_root}/resmoke_selftest_set_parameters.yml",
                [
                    """--mongodSetParameter={"mirrorReads": {samplingRate: 1.0}}""",
                    """--mongodSetParameter={"mirrorReads": {samplingRate: 1.0}}""",
                ],
            ).wait(),
        )


class TestDiscovery(_ResmokeSelftest):
    def setUp(self):
        super(TestDiscovery, self).setUp()
        self.output = io.StringIO()
        handler = logging.StreamHandler(self.output)
        handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        self.logger.addHandler(handler)

        self.assertIn(
            "featureFlagToaster",
            get_all_feature_flags_turned_off_by_default(),
            "TestDiscovery tests use featureFlagToaster with the assumption that it is turned off by default.",
        )

        with open(
            "buildscripts/resmokeconfig/fully_disabled_feature_flags.yml", encoding="utf8"
        ) as fully_disabled_ffs:
            self.assertIn(
                "featureFlagFryer",
                yaml.safe_load(fully_disabled_ffs),
                "TestDiscovery tests use featureFlagFryer with the assumption that it is present in fully_disabled_feature_flags.yml.",
            )

    def execute_resmoke(self, resmoke_args):
        resmoke_process = core.programs.make_process(
            self.logger,
            [sys.executable, "buildscripts/resmoke.py", "test-discovery"] + resmoke_args,
        )
        resmoke_process.start()
        return resmoke_process

    def test_exclude_fully_disabled(self):
        self.execute_resmoke(
            ["--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_jstest_tagged.yml"]
        ).wait()
        self.assertIn(
            "buildscripts/tests/resmoke_end2end/testfiles/tagged_with_disabled_feature.js",
            self.output.getvalue(),
            "tagged_with_disabled_feature.js should have been included in the discovered tests.",
        )
        self.assertNotIn(
            "buildscripts/tests/resmoke_end2end/testfiles/tagged_with_fully_disabled_feature.js",
            self.output.getvalue(),
            "tagged_with_fully_disabled_feature.js should not have been included in the discovered tests.",
        )

    def test_include_fully_disabled(self):
        self.execute_resmoke(
            [
                "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_jstest_tagged.yml",
                "--includeFullyDisabledFeatureTests",
            ]
        ).wait()
        self.assertIn(
            "buildscripts/tests/resmoke_end2end/testfiles/tagged_with_fully_disabled_feature.js",
            self.output.getvalue(),
            "tagged_with_fully_disabled_feature.js should have been included in the discovered tests since --includeFullyDisabledFeatureTests is used.",
        )


def execute_resmoke(resmoke_args: List[str], subcommand: str="run"):
    return subprocess.run(
        [sys.executable, "buildscripts/resmoke.py", subcommand] + resmoke_args,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


class TestExceptionExtraction(unittest.TestCase):
    def test_resmoke_python_exception(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_failing_python.yml",
        ]
        output = execute_resmoke(resmoke_args).stdout

        expected = "The following tests failed (with exit code):\n        buildscripts/tests/resmoke_end2end/failtestfiles/python_failure.py (1 DB Exception)\n            [LAST Part of Exception]"
        assert expected in output

    def test_resmoke_javascript_exception(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_failing_javascript.yml",
        ]
        output = execute_resmoke(resmoke_args).stdout

        expected = "The following tests failed (with exit code):\n        buildscripts/tests/resmoke_end2end/failtestfiles/js_failure.js (253 Failure executing JS file)\n            uncaught exception: Error: [true] and [false] are not equal"
        assert expected in output

    def test_resmoke_fixture_error(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_fixture_error.yml",
        ]
        output = execute_resmoke(resmoke_args).stdout

        expected = "The following tests had errors:\n    job0_fixture_setup_0\n        Traceback (most recent call last):\n"
        assert expected in output

    def test_resmoke_hook_error(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_hook_error.yml",
        ]
        output = execute_resmoke(resmoke_args).stdout

        expected = "The following tests had errors:\n    buildscripts/tests/resmoke_end2end/failtestfiles/js_failure.js\n        Traceback (most recent call last):\n"
        assert expected in output


class TestForceExcludedTest(unittest.TestCase):
    def test_no_force_exclude(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_suite_with_excludes.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/one.js",
        ]

        result = execute_resmoke(resmoke_args)

        expected = (
            "Cannot run excluded test in suite config. Use '--force-excluded-tests' to override:"
        )
        assert expected in result.stdout
        assert result.returncode == 1

    def test_with_force_exclude(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_suite_with_excludes.yml",
            "--force-excluded-tests",
            "--dryRun",
            "tests",
            "buildscripts/tests/resmoke_end2end/testfiles/one.js",
        ]

        result = execute_resmoke(resmoke_args)

        assert result.returncode == 0


class TestSetShellSeed(unittest.TestCase):
    def execute_resmoke_and_get_seed(self, resmoke_args):
        process = execute_resmoke(resmoke_args)
        self.assertEqual(process.returncode, 0)
        match = re.search("setting random seed: ([0-9]+)", process.stdout)
        if not match:
            self.fail(
                "No random seed message found in resmoke output. Was the message changed or the test altered?"
            )

        return match.group(1)

    def test_set_shell_seed(self):
        test_seed = "5000"

        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_set_shellseed.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/random_with_seed.js",
            f"--shellSeed={test_seed}",
        ]

        seed = self.execute_resmoke_and_get_seed(resmoke_args)

        self.assertEqual(
            seed,
            test_seed,
            msg="The found random seed does not match the seed passed with the --shellSeed resmoke argument.",
        )

    def test_random_shell_seed(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_set_shellseed.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/random_with_seed.js",
        ]

        random_seeds = set()

        for _ in range(10):
            seed = self.execute_resmoke_and_get_seed(resmoke_args)
            random_seeds.add(seed)

        self.assertTrue(
            len(random_seeds) > 1, msg="Resmoke generated the same random seed 10 times in a row."
        )


# In resmoke we expect certain parts of the evergreen config to be a certain way
# These tests will fail if something is not as expected and also needs to change somewhere else in resmoke
class TestEvergreenYML(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evg_conf = parse_evergreen_file("etc/evergreen.yml")
        config.CONFIG_DIR = "buildscripts/resmokeconfig"

    def validate_jstestfuzz_selector(self, suite_names):
        for suite_name in suite_names:
            if not suite_name.startswith(
                "//"
            ):  # Ignore suites that are run via bazel. TODO: SERVER-104460
                suite_config = suitesconfig.get_suite(suite_name).get_config()
                expected_selector = ["jstestfuzz/out/*.js"]
                self.assertEqual(
                    suite_config["selector"]["roots"],
                    expected_selector,
                    msg=f"The jstestfuzz selector for {suite_name} did not match 'jstestfuzz/out/*.js'",
                )

    # This test asserts that the jstestfuzz tasks uploads the the URL we expect it to
    # If the remote url changes, also change it in the _log_local_resmoke_invocation method
    # before fixing this test to the correct url
    def test_jstestfuzz_download_url(self):
        functions = self.evg_conf.functions
        run_jstestfuzz = functions["run jstestfuzz"]
        contains_correct_url = False
        for item in run_jstestfuzz:
            if item["command"] != "s3.put":
                continue

            remote_url = item["params"]["remote_file"]
            if (
                remote_url
                == "${project}/${build_variant}/${revision}/jstestfuzz/${task_id}-${execution}.tgz"
            ):
                contains_correct_url = True
                break

        self.assertTrue(
            contains_correct_url,
            msg="The 'run jstestfuzz' function in evergreen did not contain the remote_url that was expected",
        )

    # This tasks asserts that the way implicit multiversion tasks are defined has not changed
    # If this fails, you will need to correct the _log_local_resmoke_invocation method before fixing
    # this test
    def test_implicit_multiversion_tasks(self):
        multiverson_task_names = self.evg_conf.get_task_names_by_tag("multiversion")
        implicit_multiversion_count = 0
        for multiversion_task_name in multiverson_task_names:
            task_config = self.evg_conf.get_task(multiversion_task_name)
            func = task_config.find_func_command("initialize multiversion tasks")
            if func is not None:
                implicit_multiversion_count += 1

        self.assertNotEqual(
            0,
            implicit_multiversion_count,
            msg="Could not find any implicit multiversion tasks in evergreen",
        )

    # This tasks asserts that the way jstestfuzz tasks are defined has not changed
    # It also asserts that the selector for jstestfuzz tasks always points to jstestfuzz/out/*.js
    # If this fails, you will need to correct the _log_local_resmoke_invocation method before fixing
    # this test
    def test_jstestfuzz_tasks(self):
        jstestfuzz_count = 0
        for task in self.evg_conf.tasks:
            generate_func = task.find_func_command("generate resmoke tasks")
            if (
                generate_func is None
                or get_dict_value(generate_func, ["vars", "is_jstestfuzz"]) is not True
            ):
                continue

            jstestfuzz_count += 1

            self.validate_jstestfuzz_selector(task.get_suite_names())

        self.assertNotEqual(0, jstestfuzz_count, msg="Could not find any jstestfuzz tasks")

    # our unittest names need to be correct for the spawnhost script to work.
    # If you change the unittest names please also change them in the spawnhost script.
    def test_unittest_name(self):
        tasks = self.evg_conf.tasks
        unit_test_string = "unit_test_group"
        unit_test_tasks = []
        for task in tasks:
            # We found at least one unit test task that matched!
            if unit_test_string in task.name:
                unit_test_tasks.append(task.name)

        # as of the time of writing this there are 16 different "unit_test_group" task definitions
        self.assertGreaterEqual(
            len(unit_test_tasks),
            16,
            "Something changed about unittest tasks, please make sure the spawnhost is also updated and update this test after.",
        )


class TestMultiversionConfig(unittest.TestCase):
    def test_valid_yaml(self):
        file_name = "multiversion-config.yml"
        subprocess.run(
            [
                sys.executable,
                "buildscripts/resmoke.py",
                "multiversion-config",
                "--config-file-output",
                file_name,
            ],
            check=True,
        )
        with open(file_name, "r", encoding="utf8") as file:
            file_contents = file.read()

        try:
            yaml.safe_load(file_contents)
        except Exception:
            self.fail(msg="`resmoke.py multiversion-config` does not output valid yaml.")

        os.remove(file_name)


class TestCoreAnalyzerFunctions(unittest.TestCase):
    def test_generated_task_name(self):
        task_name = "test_tast_name"
        execution = "0"
        generated_task_name = get_generated_task_name(task_name, execution)
        self.assertEquals(matches_generated_task_pattern(task_name, generated_task_name), execution)
        self.assertIsNone(matches_generated_task_pattern("not_same_task", generated_task_name))


class TestValidateCollections(unittest.TestCase):
    def test_validate_collections_passing(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_validate_collections.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/validatecollections/test_pass.js",
        ]

        result = execute_resmoke(resmoke_args)

        expected = "Collection validation passed on collection test_validate_passes"
        # Both nodes in the replica set should be checked and pass
        self.assertEqual(result.stdout.count(expected), 2)
        self.assertEqual(result.returncode, 0)

    def test_validate_collections_failing(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_validate_collections.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/validatecollections/test_fail.js",
        ]

        result = execute_resmoke(resmoke_args)

        expected = "collection validation failed"
        self.assertIn(expected, result.stdout)
        self.assertNotEqual(result.returncode, 0)

class TestModules(unittest.TestCase):
    def test_files_included(self):
        # this suite uses a fixture and hook from the module so it will fail if they are not loaded
        # it also uses a 
        resmoke_args = [
            "--resmokeModulesPath=buildscripts/tests/resmoke_end2end/test_resmoke_modules.yml",
            "--suite=resmoke_test_module_worked",
        ]

        result = execute_resmoke(resmoke_args)
        self.assertEqual(result.returncode, 0)
        
    def test_jstests_excluded(self):
        # this first command should not include any of the tests from the module
        resmoke_args = [
            "--resmokeModulesPath=buildscripts/tests/resmoke_end2end/test_resmoke_modules.yml",
            "--modules=none",
            "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_test_module_jstests.yml",
            "--dryRun=included-tests",
        ]
        
        result_without_module = execute_resmoke(resmoke_args)
        self.assertEqual(result_without_module.returncode, 0)
        
        # this second invocartion should include all of the base jstests and all of the module jstests.
        resmoke_args = [
            "--resmokeModulesPath=buildscripts/tests/resmoke_end2end/test_resmoke_modules.yml",
            "--modules=default",
            "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_test_module_jstests.yml",
            "--dryRun=included-tests",
        ]
        
        result_with_module = execute_resmoke(resmoke_args)
        self.assertEqual(result_with_module.returncode, 0)
        
        # assert the test is in the list of tests when the module is included
        self.assertIn("buildscripts/tests/resmoke_end2end/testfiles/one.js", result_with_module.stdout)
        # assert the test is not in the list of tests when the module is excluded
        self.assertNotIn("buildscripts/tests/resmoke_end2end/testfiles/one.js", result_without_module.stdout)
