"""Unit tests for buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import json
import os
import sys
import unittest
from datetime import datetime, timedelta
from math import ceil

import requests
from mock import patch, MagicMock
from shrub.v2 import BuildVariant, ShrubProject

import buildscripts.evergreen_burn_in_tests as under_test
from buildscripts.ciconfig.evergreen import parse_evergreen_file
import buildscripts.resmokelib.parser as _parser
import buildscripts.resmokelib.config as _config
import buildscripts.util.teststats as teststats_utils
_parser.set_run_options()

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access

NS = "buildscripts.evergreen_burn_in_tests"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def mock_a_file(filename):
    change = MagicMock(a_path=filename)
    return change


def mock_git_diff(change_list):
    diff = MagicMock()
    diff.iter_change_type.return_value = change_list
    return diff


def mock_changed_git_files(add_files):
    repo = MagicMock()
    repo.index.diff.return_value = mock_git_diff([mock_a_file(f) for f in add_files])
    repo.working_dir = "."
    return repo


def get_evergreen_config(config_file_path):
    evergreen_home = os.path.expanduser(os.path.join("~", "evergreen"))
    if os.path.exists(evergreen_home):
        return parse_evergreen_file(config_file_path, evergreen_home)
    return parse_evergreen_file(config_file_path)


class TestAcceptance(unittest.TestCase):
    def tearDown(self):
        _parser.set_run_options()

    @patch(ns("write_file"))
    def test_no_tests_run_if_none_changed(self, write_json_mock):
        """
        Given a git repository with no changes,
        When burn_in_tests is run,
        Then no tests are discovered to run.
        """
        variant = "build_variant"
        repos = [mock_changed_git_files([])]
        repeat_config = under_test.RepeatConfig()
        gen_config = under_test.GenerateConfig(
            variant,
            "project",
        )  # yapf: disable
        mock_evg_conf = MagicMock()
        mock_evg_conf.get_task_names_by_tag.return_value = set()
        mock_evg_api = MagicMock()

        under_test.burn_in("task_id", variant, gen_config, repeat_config, mock_evg_api,
                           mock_evg_conf, repos, "testfile.json")

        write_json_mock.assert_called_once()
        written_config = json.loads(write_json_mock.call_args[0][1])
        display_task = written_config["buildvariants"][0]["display_tasks"][0]
        self.assertEqual(1, len(display_task["execution_tasks"]))
        self.assertEqual(under_test.BURN_IN_TESTS_GEN_TASK, display_task["execution_tasks"][0])

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(ns("write_file"))
    def test_tests_generated_if_a_file_changed(self, write_json_mock):
        """
        Given a git repository with changes,
        When burn_in_tests is run,
        Then tests are discovered to run.
        """
        # Note: this test is using actual tests and suites. So changes to those suites could
        # introduce failures and require this test to be updated.
        # You can see the test file it is using below. This test is used in the 'auth' and
        # 'auth_audit' test suites. It needs to be in at least one of those for the test to pass.
        _config.NAMED_SUITES = None
        variant = "enterprise-rhel-80-64-bit"
        repos = [mock_changed_git_files(["jstests/auth/auth1.js"])]
        repeat_config = under_test.RepeatConfig()
        gen_config = under_test.GenerateConfig(
            variant,
            "project",
        )  # yapf: disable
        mock_evg_conf = get_evergreen_config("etc/evergreen.yml")
        mock_evg_api = MagicMock()

        under_test.burn_in("task_id", variant, gen_config, repeat_config, mock_evg_api,
                           mock_evg_conf, repos, "testfile.json")

        write_json_mock.assert_called_once()
        written_config = json.loads(write_json_mock.call_args[0][1])
        n_tasks = len(written_config["tasks"])
        # Ensure we are generating at least one task for the test.
        self.assertGreaterEqual(n_tasks, 1)

        written_build_variant = written_config["buildvariants"][0]
        self.assertEqual(variant, written_build_variant["name"])
        self.assertEqual(n_tasks, len(written_build_variant["tasks"]))

        display_task = written_build_variant["display_tasks"][0]
        # The display task should contain all the generated tasks as well as 1 extra task for
        # the burn_in_test_gen task.
        self.assertEqual(n_tasks + 1, len(display_task["execution_tasks"]))


class TestGenerateConfig(unittest.TestCase):
    def test_run_build_variant_with_no_run_build_variant(self):
        gen_config = under_test.GenerateConfig("build_variant", "project")

        self.assertEqual(gen_config.build_variant, gen_config.run_build_variant)

    def test_run_build_variant_with_run_build_variant(self):
        gen_config = under_test.GenerateConfig("build_variant", "project", "run_build_variant")

        self.assertNotEqual(gen_config.build_variant, gen_config.run_build_variant)
        self.assertEqual(gen_config.run_build_variant, "run_build_variant")

    def test_validate_non_existing_build_variant(self):
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value = None

        gen_config = under_test.GenerateConfig("build_variant", "project", "run_build_variant")

        with self.assertRaises(ValueError):
            gen_config.validate(evg_conf_mock)

    def test_validate_existing_build_variant(self):
        evg_conf_mock = MagicMock()

        gen_config = under_test.GenerateConfig("build_variant", "project", "run_build_variant")
        gen_config.validate(evg_conf_mock)

    def test_validate_non_existing_run_build_variant(self):
        evg_conf_mock = MagicMock()

        gen_config = under_test.GenerateConfig("build_variant", "project")
        gen_config.validate(evg_conf_mock)


class TestParseAvgTestRuntime(unittest.TestCase):
    def test__parse_avg_test_runtime(self):
        task_avg_test_runtime_stats = [
            teststats_utils.TestRuntime(test_name="dir/test1.js", runtime=30.2),
            teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=455.1)
        ]
        result = under_test._parse_avg_test_runtime("dir/test2.js", task_avg_test_runtime_stats)
        self.assertEqual(result, 455.1)


class TestCalculateTimeout(unittest.TestCase):
    def test__calculate_timeout(self):
        avg_test_runtime = 455.1
        expected_result = ceil(avg_test_runtime * under_test.AVG_TEST_TIME_MULTIPLIER)
        self.assertEqual(expected_result, under_test._calculate_timeout(avg_test_runtime))

    def test__calculate_timeout_avg_is_less_than_min(self):
        avg_test_runtime = 10
        self.assertEqual(under_test.MIN_AVG_TEST_TIME_SEC,
                         under_test._calculate_timeout(avg_test_runtime))


class TestCalculateExecTimeout(unittest.TestCase):
    def test__calculate_exec_timeout(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600)
        avg_test_runtime = 455.1

        exec_timeout = under_test._calculate_exec_timeout(repeat_config, avg_test_runtime)

        self.assertEqual(1771, exec_timeout)

    def test_average_timeout_greater_than_execution_time(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600, repeat_tests_min=2)
        avg_test_runtime = 750

        exec_timeout = under_test._calculate_exec_timeout(repeat_config, avg_test_runtime)

        # The timeout needs to be greater than the number of the test * the minimum number of runs.
        minimum_expected_timeout = avg_test_runtime * repeat_config.repeat_tests_min

        self.assertGreater(exec_timeout, minimum_expected_timeout)


class TestGenerateTimeouts(unittest.TestCase):
    def test__generate_timeouts(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600)
        runtime_stats = [teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=455.1)]
        test_name = "dir/test2.js"

        task_generator = under_test.TaskGenerator(MagicMock(), repeat_config, MagicMock(),
                                                  runtime_stats)
        timeout_info = task_generator.generate_timeouts(test_name)

        self.assertEqual(timeout_info.exec_timeout, 1771)
        self.assertEqual(timeout_info.timeout, 1366)

    def test__generate_timeouts_no_results(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600)
        runtime_stats = []
        test_name = "dir/new_test.js"

        task_generator = under_test.TaskGenerator(MagicMock(), repeat_config, MagicMock(),
                                                  runtime_stats)
        timeout_info = task_generator.generate_timeouts(test_name)

        self.assertIsNone(timeout_info.cmd)

    def test__generate_timeouts_avg_runtime_is_zero(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600)
        runtime_stats = [
            teststats_utils.TestRuntime(test_name="dir/test_with_zero_runtime.js", runtime=0)
        ]
        test_name = "dir/test_with_zero_runtime.js"

        task_generator = under_test.TaskGenerator(MagicMock(), repeat_config, MagicMock(),
                                                  runtime_stats)
        timeout_info = task_generator.generate_timeouts(test_name)

        self.assertIsNone(timeout_info.cmd)


class TestGetTaskRuntimeHistory(unittest.TestCase):
    def test_get_task_runtime_history(self):
        mock_evg_api = MagicMock()
        mock_evg_api.test_stats_by_project.return_value = [
            MagicMock(
                test_file="dir/test2.js",
                task_name="task1",
                variant="variant1",
                distro="distro1",
                date=datetime.utcnow().date(),
                num_pass=1,
                num_fail=0,
                avg_duration_pass=10.1,
            )
        ]
        analysis_duration = under_test.AVG_TEST_RUNTIME_ANALYSIS_DAYS
        end_date = datetime.utcnow().replace(microsecond=0)
        start_date = end_date - timedelta(days=analysis_duration)
        mock_gen_config = MagicMock(project="project1", build_variant="variant1")

        executor = under_test.GenerateBurnInExecutor(mock_gen_config, MagicMock(), mock_evg_api,
                                                     history_end_date=end_date)
        result = executor.get_task_runtime_history("task1")

        self.assertEqual(result, [("dir/test2.js", 10.1)])
        mock_evg_api.test_stats_by_project.assert_called_with(
            "project1", after_date=start_date, before_date=end_date, group_by="test",
            group_num_days=14, tasks=["task1"], variants=["variant1"])

    def test_get_task_runtime_history_evg_degraded_mode_error(self):
        mock_response = MagicMock(status_code=requests.codes.SERVICE_UNAVAILABLE)
        mock_evg_api = MagicMock()
        mock_evg_api.test_stats_by_project.side_effect = requests.HTTPError(response=mock_response)
        mock_gen_config = MagicMock(project="project1", build_variant="variant1")

        executor = under_test.GenerateBurnInExecutor(mock_gen_config, MagicMock(), mock_evg_api)
        result = executor.get_task_runtime_history("task1")

        self.assertEqual(result, [])


TESTS_BY_TASK = {
    "task1": {
        "resmoke_args": "--suites=suite1",
        "tests": ["jstests/test1.js", "jstests/test2.js"]},
    "task2": {
        "resmoke_args": "--suites=suite1",
        "tests": ["jstests/test1.js", "jstests/test3.js"]},
    "task3": {
        "resmoke_args": "--suites=suite3",
        "tests": ["jstests/test4.js", "jstests/test5.js"]},
    "task4": {
        "resmoke_args": "--suites=suite4", "tests": []},
}  # yapf: disable


def create_tests_by_task_mock(n_tasks, n_tests):
    return {
        f"task_{i}_gen":
        under_test.TaskInfo(display_task_name=f"task_{i}", resmoke_args=f"--suites=suite_{i}",
                            tests=[f"jstests/tests_{j}" for j in range(n_tests)],
                            require_multiversion=None, distro=f"distro_{i}")
        for i in range(n_tasks)
    }


class TestCreateGenerateTasksConfig(unittest.TestCase):
    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_no_tasks_given(self):
        build_variant = BuildVariant("build variant")
        gen_config = MagicMock(run_build_variant="variant")
        repeat_config = MagicMock()
        mock_evg_api = MagicMock()

        executor = under_test.GenerateBurnInExecutor(gen_config, repeat_config, mock_evg_api)
        executor.add_config_for_build_variant(build_variant, {})

        evg_config_dict = build_variant.as_dict()
        self.assertEqual(0, len(evg_config_dict["tasks"]))

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_one_task_one_test(self):
        n_tasks = 1
        n_tests = 1
        resmoke_options = "options for resmoke"
        build_variant = BuildVariant("build variant")
        gen_config = MagicMock(run_build_variant="variant", distro=None)
        repeat_config = MagicMock()
        repeat_config.generate_resmoke_options.return_value = resmoke_options
        mock_evg_api = MagicMock()
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)

        executor = under_test.GenerateBurnInExecutor(gen_config, repeat_config, mock_evg_api)
        executor.add_config_for_build_variant(build_variant, tests_by_task)

        shrub_config = ShrubProject.empty().add_build_variant(build_variant)
        evg_config_dict = shrub_config.as_dict()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(n_tasks * n_tests, len(tasks))
        cmd = tasks[0]["commands"]
        self.assertIn(resmoke_options, cmd[1]["vars"]["resmoke_args"])
        self.assertIn("--suites=suite_0", cmd[1]["vars"]["resmoke_args"])
        self.assertIn("tests_0", cmd[1]["vars"]["resmoke_args"])

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_n_task_m_test(self):
        n_tasks = 3
        n_tests = 5
        build_variant = BuildVariant("build variant")
        gen_config = MagicMock(run_build_variant="variant", distro=None)
        repeat_config = MagicMock()
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)
        mock_evg_api = MagicMock()

        executor = under_test.GenerateBurnInExecutor(gen_config, repeat_config, mock_evg_api)
        executor.add_config_for_build_variant(build_variant, tests_by_task)

        evg_config_dict = build_variant.as_dict()
        self.assertEqual(n_tasks * n_tests, len(evg_config_dict["tasks"]))


class TestCreateGenerateTasksFile(unittest.TestCase):
    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(ns("sys.exit"))
    @patch(ns("validate_task_generation_limit"))
    def test_cap_on_task_generate(self, validate_mock, exit_mock):
        gen_config = MagicMock(require_multiversion=False)
        repeat_config = MagicMock()
        tests_by_task = MagicMock()
        mock_evg_api = MagicMock()

        validate_mock.return_value = False

        exit_mock.side_effect = ValueError("exiting")
        with self.assertRaises(ValueError):
            executor = under_test.GenerateBurnInExecutor(gen_config, repeat_config, mock_evg_api,
                                                         "gen_file.json")
            executor.execute(tests_by_task)

        exit_mock.assert_called_once()
