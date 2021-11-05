"""Unit tests for buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import collections
import datetime
import os
import sys
import subprocess
import unittest

from math import ceil
from mock import Mock, patch, MagicMock

import requests

from shrub.config import Configuration

import buildscripts.burn_in_tests as under_test
from buildscripts.ciconfig.evergreen import parse_evergreen_file
import buildscripts.util.teststats as teststats_utils
import buildscripts.resmokelib.config as _config

# pylint: disable=missing-docstring,protected-access,too-many-lines,no-self-use


def create_tests_by_task_mock(n_tasks, n_tests):
    return {
        f"task_{i}": {
            "resmoke_args": f"--suites=suite_{i}",
            "tests": [f"jstests/tests_{j}" for j in range(n_tests)]
        }
        for i in range(n_tasks)
    }


MV_MOCK_SUITES = ["replica_sets_jscore_passthrough", "sharding_jscore_passthrough"]


def create_multiversion_tests_by_task_mock(n_tasks, n_tests):
    assert n_tasks <= len(MV_MOCK_SUITES)
    return {
        f"{MV_MOCK_SUITES[i % len(MV_MOCK_SUITES)]}": {
            "resmoke_args": f"--suites=suite_{i}",
            "tests": [f"jstests/tests_{j}" for j in range(n_tests)]
        }
        for i in range(n_tasks)
    }


_DATE = datetime.datetime(2018, 7, 15)
RESMOKELIB = "buildscripts.resmokelib"

NS = "buildscripts.burn_in_tests"


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
    return repo


def get_evergreen_config(config_file_path):
    evergreen_home = os.path.expanduser(os.path.join("~", "evergreen"))
    if os.path.exists(evergreen_home):
        return parse_evergreen_file(config_file_path, evergreen_home)
    return parse_evergreen_file(config_file_path)


class TestAcceptance(unittest.TestCase):
    @patch(ns("_write_json_file"))
    def test_no_tests_run_if_none_changed(self, write_json_mock):
        """
        Given a git repository with no changes,
        When burn_in_tests is run,
        Then no tests are discovered to run.
        """
        variant = "build_variant"
        repo = mock_changed_git_files([])
        repeat_config = under_test.RepeatConfig()
        gen_config = under_test.GenerateConfig(
            variant,
            "project",
        )  # yapf: disable

        under_test.burn_in(repeat_config, gen_config, "", "testfile.json", False, None, repo, None)

        write_json_mock.assert_called_once()
        written_config = write_json_mock.call_args[0][0]
        display_task = written_config["buildvariants"][0]["display_tasks"][0]
        self.assertEqual(1, len(display_task["execution_tasks"]))
        self.assertEqual(under_test.BURN_IN_TESTS_GEN_TASK, display_task["execution_tasks"][0])

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(ns("_write_json_file"))
    def test_tests_generated_if_a_file_changed(self, write_json_mock):
        """
        Given a git repository with no changes,
        When burn_in_tests is run,
        Then no tests are discovered to run.
        """
        # Note: this test is using actual tests and suites. So changes to those suites could
        # introduce failures and require this test to be updated.
        # You can see the test file it is using below. This test is used in the 'auth' and
        # 'auth_audit' test suites. It needs to be in at least one of those for the test to pass.
        _config.NAMED_SUITES = None
        variant = "enterprise-rhel-62-64-bit"
        repo = mock_changed_git_files(["jstests/auth/auth1.js"])
        repeat_config = under_test.RepeatConfig()
        gen_config = under_test.GenerateConfig(
            variant,
            "project",
        )  # yapf: disable
        evg_config = get_evergreen_config("etc/evergreen.yml")

        under_test.burn_in(repeat_config, gen_config, "", "testfile.json", False, evg_config, repo,
                           None)

        write_json_mock.assert_called_once()
        written_config = write_json_mock.call_args[0][0]
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


class TestRepeatConfig(unittest.TestCase):
    def test_validate_no_args(self):
        repeat_config = under_test.RepeatConfig()

        self.assertEqual(repeat_config, repeat_config.validate())

    def test_validate_with_both_repeat_options_specified(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=10, repeat_tests_num=5)

        with self.assertRaises(ValueError):
            repeat_config.validate()

    def test_validate_with_repeat_max_with_no_secs(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_max=10)

        with self.assertRaises(ValueError):
            repeat_config.validate()

    def test_validate_with_repeat_min_greater_than_max(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_max=10, repeat_tests_min=100,
                                                repeat_tests_secs=15)

        with self.assertRaises(ValueError):
            repeat_config.validate()

    def test_validate_with_repeat_min_with_no_secs(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_min=10)

        with self.assertRaises(ValueError):
            repeat_config.validate()

    def test_get_resmoke_repeat_options_num(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_num=5)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertEqual(repeat_options.strip(), f"--repeatSuites=5")

    def test_get_resmoke_repeat_options_secs(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=5)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertEqual(repeat_options.strip(), "--repeatTestsSecs=5")

    def test_get_resmoke_repeat_options_secs_min(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=5, repeat_tests_min=2)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertIn("--repeatTestsSecs=5", repeat_options)
        self.assertIn("--repeatTestsMin=2", repeat_options)
        self.assertNotIn("--repeatTestsMax", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_secs_max(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=5, repeat_tests_max=2)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertIn("--repeatTestsSecs=5", repeat_options)
        self.assertIn("--repeatTestsMax=2", repeat_options)
        self.assertNotIn("--repeatTestsMin", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_secs_min_max(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=5, repeat_tests_min=2,
                                                repeat_tests_max=2)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertIn("--repeatTestsSecs=5", repeat_options)
        self.assertIn("--repeatTestsMin=2", repeat_options)
        self.assertIn("--repeatTestsMax=2", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_min(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_min=2)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertEqual(repeat_options.strip(), "--repeatSuites=2")

    def test_get_resmoke_repeat_options_max(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_max=2)
        repeat_options = repeat_config.generate_resmoke_options()

        self.assertEqual(repeat_options.strip(), "--repeatSuites=2")


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

    def test_validate_use_multiversion(self):
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

        timeout_info = under_test._generate_timeouts(repeat_config, test_name, runtime_stats)

        self.assertEqual(timeout_info.exec_timeout, 1771)
        self.assertEqual(timeout_info.timeout, 1366)

    def test__generate_timeouts_no_results(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600)
        runtime_stats = []
        test_name = "dir/new_test.js"

        timeout_info = under_test._generate_timeouts(repeat_config, test_name, runtime_stats)

        self.assertIsNone(timeout_info.cmd)

    def test__generate_timeouts_avg_runtime_is_zero(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_secs=600)
        runtime_stats = [
            teststats_utils.TestRuntime(test_name="dir/test_with_zero_runtime.js", runtime=0)
        ]
        test_name = "dir/test_with_zero_runtime.js"

        timeout_info = under_test._generate_timeouts(repeat_config, test_name, runtime_stats)

        self.assertIsNone(timeout_info.cmd)


class TestGetTaskRuntimeHistory(unittest.TestCase):
    def test_get_task_runtime_history_with_no_api(self):
        self.assertListEqual([],
                             under_test._get_task_runtime_history(None, "project", "task",
                                                                  "variant"))

    def test__get_task_runtime_history(self):
        evergreen_api = Mock()
        evergreen_api.test_stats_by_project.return_value = [
            Mock(
                test_file="dir/test2.js",
                task_name="task1",
                variant="variant1",
                distro="distro1",
                date=_DATE,
                num_pass=1,
                num_fail=0,
                avg_duration_pass=10.1,
            )
        ]
        analysis_duration = under_test.AVG_TEST_RUNTIME_ANALYSIS_DAYS
        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=analysis_duration)

        result = under_test._get_task_runtime_history(evergreen_api, "project1", "task1",
                                                      "variant1")
        self.assertEqual(result, [("dir/test2.js", 10.1)])
        evergreen_api.test_stats_by_project.assert_called_with(
            "project1", after_date=start_date.strftime("%Y-%m-%d"),
            before_date=end_date.strftime("%Y-%m-%d"), group_by="test", group_num_days=14,
            tasks=["task1"], variants=["variant1"])

    def test__get_task_runtime_history_evg_degraded_mode_error(self):  # pylint: disable=invalid-name
        response = Mock()
        response.status_code = requests.codes.SERVICE_UNAVAILABLE
        evergreen_api = Mock()
        evergreen_api.test_stats_by_project.side_effect = requests.HTTPError(response=response)

        result = under_test._get_task_runtime_history(evergreen_api, "project1", "task1",
                                                      "variant1")
        self.assertEqual(result, [])


class TestGetTaskName(unittest.TestCase):
    def test__get_task_name(self):
        name = "mytask"
        task = Mock()
        task.is_generate_resmoke_task = False
        task.name = name
        self.assertEqual(name, under_test._get_task_name(task))

    def test__get_task_name_generate_resmoke_task(self):
        task_name = "mytask"
        task = Mock(is_generate_resmoke_task=True, generated_task_name=task_name)
        self.assertEqual(task_name, under_test._get_task_name(task))


class TestSetResmokeArgs(unittest.TestCase):
    def test__set_resmoke_args(self):
        resmoke_args = "--suites=suite1 test1.js"
        task = Mock()
        task.resmoke_args = resmoke_args
        task.is_generate_resmoke_task = False
        self.assertEqual(resmoke_args, under_test._set_resmoke_args(task))

    def test__set_resmoke_args_gen_resmoke_task(self):
        resmoke_args = "--suites=suite1 test1.js"
        new_suite = "suite2"
        new_resmoke_args = "--suites={} test1.js".format(new_suite)
        task = Mock()
        task.resmoke_args = resmoke_args
        task.is_generate_resmoke_task = True
        task.get_vars_suite_name = lambda cmd_vars: cmd_vars["suite"]
        task.generate_resmoke_tasks_command = {"vars": {"suite": new_suite}}
        self.assertEqual(new_resmoke_args, under_test._set_resmoke_args(task))

    def test__set_resmoke_args_gen_resmoke_task_no_suite(self):
        suite = "suite1"
        resmoke_args = "--suites={} test1.js".format(suite)
        task = Mock()
        task.resmoke_args = resmoke_args
        task.is_generate_resmoke_task = True
        task.get_vars_suite_name = lambda cmd_vars: cmd_vars["task"]
        task.generate_resmoke_tasks_command = {"vars": {"task": suite}}
        self.assertEqual(resmoke_args, under_test._set_resmoke_args(task))


class TestSetResmokeCmd(unittest.TestCase):
    def test__set_resmoke_cmd_no_opts_no_args(self):
        repeat_config = under_test.RepeatConfig()
        resmoke_cmds = under_test._set_resmoke_cmd(repeat_config, [])

        self.assertListEqual(resmoke_cmds,
                             [sys.executable, "buildscripts/resmoke.py", "run", '--repeatSuites=2'])

    def test__set_resmoke_cmd_no_opts(self):
        repeat_config = under_test.RepeatConfig()
        resmoke_args = ["arg1", "arg2"]

        resmoke_cmd = under_test._set_resmoke_cmd(repeat_config, resmoke_args)

        self.assertListEqual(resmoke_args + ['--repeatSuites=2'], resmoke_cmd)

    def test__set_resmoke_cmd(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_num=3)
        resmoke_args = ["arg1", "arg2"]

        resmoke_cmd = under_test._set_resmoke_cmd(repeat_config, resmoke_args)

        self.assertListEqual(resmoke_args + ['--repeatSuites=3'], resmoke_cmd)


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
    "taskmulti": {
        "resmoke_args": "--suites=suite4",
        "tests": ["jstests/multi1.js"],
        "use_multiversion": "/data/multi"},
}  # yapf: disable


class TestCreateGenerateTasksConfig(unittest.TestCase):
    def test_no_tasks_given(self):
        evg_config = Configuration()
        gen_config = MagicMock(run_build_variant="variant")
        repeat_config = MagicMock()

        evg_config = under_test.create_generate_tasks_config(evg_config, {}, gen_config,
                                                             repeat_config, None)

        evg_config_dict = evg_config.to_map()
        self.assertNotIn("tasks", evg_config_dict)

    def test_one_task_one_test(self):
        n_tasks = 1
        n_tests = 1
        resmoke_options = "options for resmoke"
        evg_config = Configuration()
        gen_config = MagicMock(run_build_variant="variant", distro=None)
        repeat_config = MagicMock()
        repeat_config.generate_resmoke_options.return_value = resmoke_options
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)

        evg_config = under_test.create_generate_tasks_config(evg_config, tests_by_task, gen_config,
                                                             repeat_config, None)

        evg_config_dict = evg_config.to_map()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(n_tasks * n_tests, len(tasks))
        cmd = tasks[0]["commands"]
        self.assertIn(resmoke_options, cmd[1]["vars"]["resmoke_args"])
        self.assertIn("--suites=suite_0", cmd[1]["vars"]["resmoke_args"])
        self.assertIn("tests_0", cmd[1]["vars"]["resmoke_args"])

    def test_n_task_m_test(self):
        n_tasks = 3
        n_tests = 5
        evg_config = Configuration()
        gen_config = MagicMock(run_build_variant="variant", distro=None)
        repeat_config = MagicMock()
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)

        evg_config = under_test.create_generate_tasks_config(evg_config, tests_by_task, gen_config,
                                                             repeat_config, None)

        evg_config_dict = evg_config.to_map()
        self.assertEqual(n_tasks * n_tests, len(evg_config_dict["tasks"]))

    def test_multiversion_path_is_used(self):
        n_tasks = 1
        n_tests = 1
        evg_config = Configuration()
        gen_config = MagicMock(run_build_variant="variant", distro=None)
        repeat_config = MagicMock()
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)
        first_task = "task_0"
        multiversion_path = "multiversion_path"
        tests_by_task[first_task]["use_multiversion"] = multiversion_path

        evg_config = under_test.create_generate_tasks_config(evg_config, tests_by_task, gen_config,
                                                             repeat_config, None)

        evg_config_dict = evg_config.to_map()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(n_tasks * n_tests, len(tasks))
        self.assertEqual(multiversion_path, tasks[0]["commands"][2]["vars"]["task_path_suffix"])


class TestCreateGenerateTasksFile(unittest.TestCase):
    @patch("buildscripts.burn_in_tests.create_generate_tasks_config")
    def test_gen_tasks_configuration_is_returned(self, gen_tasks_config_mock):
        evg_api = MagicMock()
        gen_config = MagicMock(use_multiversion=False)
        repeat_config = MagicMock()
        tests_by_task = MagicMock()

        task_list = [f"task_{i}" for i in range(10)]

        evg_config = MagicMock()
        evg_config.to_map.return_value = {
            "tasks": task_list,
        }

        gen_tasks_config_mock.return_value = evg_config

        config = under_test.create_generate_tasks_file(tests_by_task, gen_config, repeat_config,
                                                       evg_api)

        self.assertEqual(config, evg_config.to_map.return_value)

    @patch(ns("create_generate_tasks_config"))
    def test_gen_tasks_multiversion_configuration_is_returned(self, gen_tasks_config_mock):  # pylint: disable=invalid-name
        evg_api = MagicMock()
        gen_config = MagicMock(run_build_variant="variant", project="project",
                               build_variant="build_variant", task_id="task_id",
                               use_multiversion=True)
        repeat_config = MagicMock()
        tests_by_task = MagicMock()

        evg_config = MagicMock()
        evg_config.to_map.return_value = {
            'buildvariants': [
                {
                    'name': 'build_variant',
                    'display_tasks': [
                        {
                            'name': 'burn_in_tests_multiversion',
                            'execution_tasks': [
                                'burn_in_tests_multiversion_gen'
                            ]
                        }
                    ]
                }
            ]
        }  # yapf: disable

        gen_tasks_config_mock.return_value = evg_config

        config = under_test.create_generate_tasks_file(tests_by_task, gen_config, repeat_config,
                                                       evg_api)
        self.assertEqual(config, evg_config.to_map.return_value)

    @patch("buildscripts.burn_in_tests.sys.exit")
    @patch("buildscripts.burn_in_tests.create_generate_tasks_config")
    def test_cap_on_task_generate(self, gen_tasks_config_mock, exit_mock):
        evg_api = MagicMock()
        gen_config = MagicMock(use_multiversion=False)
        repeat_config = MagicMock()
        tests_by_task = MagicMock()

        task_list = [f"task_{i}" for i in range(1005)]

        evg_config = MagicMock()
        evg_config.to_map.return_value = {
            "tasks": task_list,
        }

        gen_tasks_config_mock.return_value = evg_config

        exit_mock.side_effect = ValueError("exiting")
        with self.assertRaises(ValueError):
            under_test.create_generate_tasks_file(tests_by_task, gen_config, repeat_config, evg_api)

        exit_mock.assert_called_once()


class RunTests(unittest.TestCase):
    @patch(ns('subprocess.check_call'))
    def test_run_tests_no_tests(self, check_call_mock):
        tests_by_task = {}
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "run", "--continueOnFailure"]

        under_test.run_tests(tests_by_task, resmoke_cmd)

        check_call_mock.assert_not_called()

    @patch(ns('subprocess.check_call'))
    def test_run_tests_some_test(self, check_call_mock):
        n_tasks = 3
        tests_by_task = create_tests_by_task_mock(n_tasks, 5)
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "run", "--continueOnFailure"]

        under_test.run_tests(tests_by_task, resmoke_cmd)

        self.assertEqual(n_tasks, check_call_mock.call_count)

    @patch(ns('sys.exit'))
    @patch(ns('subprocess.check_call'))
    def test_run_tests_tests_resmoke_failure(self, check_call_mock, exit_mock):
        error_code = 42
        n_tasks = 3
        tests_by_task = create_tests_by_task_mock(n_tasks, 5)
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "run", "--continueOnFailure"]
        check_call_mock.side_effect = subprocess.CalledProcessError(error_code, "err1")
        exit_mock.side_effect = ValueError('exiting')

        with self.assertRaises(ValueError):
            under_test.run_tests(tests_by_task, resmoke_cmd)

        self.assertEqual(1, check_call_mock.call_count)
        exit_mock.assert_called_with(error_code)


MEMBERS_MAP = {
    "test1.js": ["suite1", "suite2"], "test2.js": ["suite1", "suite3"], "test3.js": [],
    "test4.js": ["suite1", "suite2", "suite3"], "test5.js": ["suite2"]
}

SUITE1 = Mock()
SUITE1.tests = ["test1.js", "test2.js", "test4.js"]
SUITE2 = Mock()
SUITE2.tests = ["test1.js"]
SUITE3 = Mock()
SUITE3.tests = ["test2.js", "test4.js"]


def _create_executor_list(suites, exclude_suites):
    with patch(ns("create_test_membership_map"), return_value=MEMBERS_MAP):
        return under_test.create_executor_list(suites, exclude_suites)


class CreateExecutorList(unittest.TestCase):
    def test_create_executor_list_no_excludes(self):
        suites = [SUITE1, SUITE2]
        exclude_suites = []
        executor_list = _create_executor_list(suites, exclude_suites)
        self.assertEqual(executor_list["suite1"], SUITE1.tests)
        self.assertEqual(executor_list["suite2"], ["test1.js", "test4.js"])
        self.assertEqual(executor_list["suite3"], ["test2.js", "test4.js"])

    def test_create_executor_list_excludes(self):
        suites = [SUITE1, SUITE2]
        exclude_suites = ["suite3"]
        executor_list = _create_executor_list(suites, exclude_suites)
        self.assertEqual(executor_list["suite1"], SUITE1.tests)
        self.assertEqual(executor_list["suite2"], ["test1.js", "test4.js"])
        self.assertEqual(executor_list["suite3"], [])

    def test_create_executor_list_nosuites(self):
        executor_list = _create_executor_list([], [])
        self.assertEqual(executor_list, collections.defaultdict(list))

    @patch(RESMOKELIB + ".testing.suite.Suite")
    @patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_create_executor_list_runs_core_suite(self, mock_get_named_suites, mock_suite_class):
        mock_get_named_suites.return_value = ["core"]

        under_test.create_executor_list([], [])
        self.assertEqual(mock_suite_class.call_count, 1)

    @patch(RESMOKELIB + ".testing.suite.Suite")
    @patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_create_executor_list_ignores_dbtest_suite(self, mock_get_named_suites,
                                                       mock_suite_class):
        mock_get_named_suites.return_value = ["dbtest"]

        under_test.create_executor_list([], [])
        self.assertEqual(mock_suite_class.call_count, 0)


def create_variant_task_mock(task_name, suite_name, distro="distro"):
    variant_task = MagicMock()
    variant_task.name = task_name
    variant_task.generated_task_name = task_name
    variant_task.resmoke_suite = suite_name
    variant_task.get_vars_suite_name.return_value = suite_name
    variant_task.resmoke_args = f"--suites={suite_name}"
    variant_task.multiversion_path = None
    variant_task.run_on = [distro]
    return variant_task


class TestGatherTaskInfo(unittest.TestCase):
    def test_non_generated_task(self):
        suite_name = "suite_1"
        distro_name = "distro_1"
        variant = "build_variant"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_task.return_value.is_generate_resmoke_task = False
        task_mock = create_variant_task_mock("task 1", suite_name, distro_name)
        test_list = [f"test{i}.js" for i in range(3)]
        tests_by_suite = {
            suite_name: test_list,
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }

        task_info = under_test._gather_task_info(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info["resmoke_args"])
        for test in test_list:
            self.assertIn(test, task_info["tests"])
        self.assertIsNone(task_info["use_multiversion"])
        self.assertEqual(distro_name, task_info["distro"])

    def test_multiversion_task(self):
        suite_name = "suite_1"
        distro_name = "distro_1"
        variant = "build_variant"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_task.return_value.is_generate_resmoke_task = False
        task_mock = create_variant_task_mock("task 1", suite_name, distro_name)
        task_mock.multiversion_path = "/path/to/multiversion"
        test_list = [f"test{i}.js" for i in range(3)]
        tests_by_suite = {
            suite_name: test_list,
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }

        task_info = under_test._gather_task_info(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info["resmoke_args"])
        for test in test_list:
            self.assertIn(test, task_info["tests"])
        self.assertEqual(task_mock.multiversion_path, task_info["use_multiversion"])
        self.assertEqual(distro_name, task_info["distro"])

    def test_generated_task_no_large_on_task(self):
        suite_name = "suite_1"
        distro_name = "distro_1"
        variant = "build_variant"
        evg_conf_mock = MagicMock()
        task_def_mock = evg_conf_mock.get_task.return_value
        task_def_mock.is_generate_resmoke_task = True
        task_def_mock.generate_resmoke_tasks_command = {"vars": {}}
        task_mock = create_variant_task_mock("task 1", suite_name, distro_name)
        test_list = [f"test{i}.js" for i in range(3)]
        tests_by_suite = {
            suite_name: test_list,
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }

        task_info = under_test._gather_task_info(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info["resmoke_args"])
        for test in test_list:
            self.assertIn(test, task_info["tests"])
        self.assertIsNone(task_info["use_multiversion"])
        self.assertEqual(distro_name, task_info["distro"])

    def test_generated_task_no_large_on_build_variant(self):
        suite_name = "suite_1"
        distro_name = "distro_1"
        variant = "build_variant"
        evg_conf_mock = MagicMock()
        task_def_mock = evg_conf_mock.get_task.return_value
        task_def_mock.is_generate_resmoke_task = True
        task_def_mock.generate_resmoke_tasks_command = {"vars": {"use_large_distro": True}}
        task_mock = create_variant_task_mock("task 1", suite_name, distro_name)
        test_list = [f"test{i}.js" for i in range(3)]
        tests_by_suite = {
            suite_name: test_list,
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }

        task_info = under_test._gather_task_info(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info["resmoke_args"])
        for test in test_list:
            self.assertIn(test, task_info["tests"])
        self.assertIsNone(task_info["use_multiversion"])
        self.assertEqual(distro_name, task_info["distro"])

    def test_generated_task_large_distro(self):
        suite_name = "suite_1"
        distro_name = "distro_1"
        large_distro_name = "large_distro_1"
        variant = "build_variant"
        evg_conf_mock = MagicMock()
        task_def_mock = evg_conf_mock.get_task.return_value
        task_def_mock.is_generate_resmoke_task = True
        task_def_mock.generate_resmoke_tasks_command = {"vars": {"use_large_distro": True}}
        evg_conf_mock.get_variant.return_value.raw = {
            "expansions": {
                "large_distro_name": large_distro_name
            }
        }  # yapf: disable
        task_mock = create_variant_task_mock("task 1", suite_name, distro_name)
        test_list = [f"test{i}.js" for i in range(3)]
        tests_by_suite = {
            suite_name: test_list,
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }

        task_info = under_test._gather_task_info(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info["resmoke_args"])
        for test in test_list:
            self.assertIn(test, task_info["tests"])
        self.assertIsNone(task_info["use_multiversion"])
        self.assertEqual(large_distro_name, task_info["distro"])


class TestCreateTaskList(unittest.TestCase):
    def test_create_task_list_no_excludes(self):
        variant = "variant name"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value.tasks = [
            create_variant_task_mock("task 1", "suite 1"),
            create_variant_task_mock("task 2", "suite 2"),
            create_variant_task_mock("task 3", "suite 3"),
        ]
        tests_by_suite = {
            "suite 1": [f"test{i}.js" for i in range(3)],
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }
        exclude_tasks = []

        task_list = under_test.create_task_list(evg_conf_mock, variant, tests_by_suite,
                                                exclude_tasks)

        self.assertIn("task 1", task_list)
        self.assertIn("task 2", task_list)
        self.assertIn("task 3", task_list)
        self.assertEqual(3, len(task_list))

    def test_create_task_list_has_correct_task_info(self):
        variant = "variant name"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value.tasks = [
            create_variant_task_mock("task 1", "suite_1", "distro 1"),
        ]
        evg_conf_mock.get_task.return_value.run_on = ["distro 1"]
        tests_by_suite = {
            "suite_1": [f"test{i}.js" for i in range(3)],
        }
        exclude_tasks = []

        task_list = under_test.create_task_list(evg_conf_mock, variant, tests_by_suite,
                                                exclude_tasks)

        self.assertIn("task 1", task_list)
        task_info = task_list["task 1"]
        self.assertIn("suite_1", task_info["resmoke_args"])
        for i in range(3):
            self.assertIn(f"test{i}.js", task_info["tests"])
        self.assertIsNone(task_info["use_multiversion"])
        self.assertEqual("distro 1", task_info["distro"])

    def test_create_task_list_with_excludes(self):
        variant = "variant name"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value.tasks = [
            create_variant_task_mock("task 1", "suite 1"),
            create_variant_task_mock("task 2", "suite 2"),
            create_variant_task_mock("task 3", "suite 3"),
        ]
        tests_by_suite = {
            "suite 1": [f"test{i}.js" for i in range(3)],
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }
        exclude_tasks = ["task 2"]

        task_list = under_test.create_task_list(evg_conf_mock, variant, tests_by_suite,
                                                exclude_tasks)

        self.assertIn("task 1", task_list)
        self.assertNotIn("task 2", task_list)
        self.assertIn("task 3", task_list)
        self.assertEqual(2, len(task_list))

    def test_create_task_list_no_suites(self):
        variant = "variant name"
        evg_conf_mock = MagicMock()
        suite_dict = {}

        task_list = under_test.create_task_list(evg_conf_mock, variant, suite_dict, [])

        self.assertEqual(task_list, {})

    def test_build_variant_not_in_evg_project_config(self):
        variant = "novariant"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value = None
        suite_dict = {}

        with self.assertRaises(ValueError):
            under_test.create_task_list(evg_conf_mock, variant, suite_dict, [])


class TestFindChangedTests(unittest.TestCase):
    @patch(ns("find_changed_files"))
    def test_nothing_found(self, changed_files_mock):
        repo_mock = MagicMock()
        changed_files_mock.return_value = set()

        self.assertEqual(0, len(under_test.find_changed_tests(repo_mock)))

    @patch(ns("find_changed_files"))
    @patch(ns("os.path.isfile"))
    def test_non_js_files_filtered(self, is_file_mock, changed_files_mock):
        repo_mock = MagicMock()
        file_list = [
            os.path.join("jstests", "test1.js"),
            os.path.join("jstests", "test1.cpp"),
            os.path.join("jstests", "test2.js"),
        ]
        changed_files_mock.return_value = set(file_list)
        is_file_mock.return_value = True

        found_tests = under_test.find_changed_tests(repo_mock)

        self.assertIn(file_list[0], found_tests)
        self.assertIn(file_list[2], found_tests)
        self.assertNotIn(file_list[1], found_tests)

    @patch(ns("find_changed_files"))
    @patch(ns("os.path.isfile"))
    def test_missing_files_filtered(self, is_file_mock, changed_files_mock):
        repo_mock = MagicMock()
        file_list = [
            os.path.join("jstests", "test1.js"),
            os.path.join("jstests", "test2.js"),
            os.path.join("jstests", "test3.js"),
        ]
        changed_files_mock.return_value = set(file_list)
        is_file_mock.return_value = False

        found_tests = under_test.find_changed_tests(repo_mock)

        self.assertEqual(0, len(found_tests))

    @patch(ns("find_changed_files"))
    @patch(ns("os.path.isfile"))
    def test_non_jstests_files_filtered(self, is_file_mock, changed_files_mock):
        repo_mock = MagicMock()
        file_list = [
            os.path.join("jstests", "test1.js"),
            os.path.join("other", "test2.js"),
            os.path.join("jstests", "test3.js"),
        ]
        changed_files_mock.return_value = set(file_list)
        is_file_mock.return_value = True

        found_tests = under_test.find_changed_tests(repo_mock)

        self.assertIn(file_list[0], found_tests)
        self.assertIn(file_list[2], found_tests)
        self.assertNotIn(file_list[1], found_tests)
        self.assertEqual(2, len(found_tests))
