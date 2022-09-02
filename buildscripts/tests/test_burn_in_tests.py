"""Unit tests for buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import collections
import datetime
from io import StringIO
import os
import sys
import subprocess
import unittest

from mock import Mock, patch, MagicMock
import yaml

import buildscripts.burn_in_tests as under_test
from buildscripts.ciconfig.evergreen import parse_evergreen_file, VariantTask
import buildscripts.resmokelib.parser as _parser
_parser.set_run_options()

# pylint: disable=protected-access


def create_tests_by_task_mock(n_tasks, n_tests):
    return {
        f"task_{i}_gen": under_test.TaskInfo(display_task_name=f"task_{i}", resmoke_args="", tests=[
            f"jstests/tests_{j}" for j in range(n_tests)
        ], require_multiversion_setup=False, distro=f"distro_{i}", suite=f"suite_{i}",
                                             build_variant="dummy_variant")
        for i in range(n_tasks)
    }


MV_MOCK_SUITES = ["replica_sets_jscore_passthrough", "sharding_jscore_passthrough"]

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
    repo.working_dir = "."
    return repo


def get_evergreen_config(config_file_path):
    evergreen_home = os.path.expanduser(os.path.join("~", "evergreen"))
    if os.path.exists(evergreen_home):
        return parse_evergreen_file(config_file_path, evergreen_home)
    return parse_evergreen_file(config_file_path)


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

        self.assertEqual(repeat_options.strip(), "--repeatSuites=5")

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
        expected_resmoke_cmd = [sys.executable, 'buildscripts/resmoke.py', 'run'
                                ] + resmoke_args + ['--repeatSuites=2']

        self.assertListEqual(expected_resmoke_cmd, resmoke_cmd)

    def test__set_resmoke_cmd(self):
        repeat_config = under_test.RepeatConfig(repeat_tests_num=3)
        resmoke_args = ["arg1", "arg2"]

        resmoke_cmd = under_test._set_resmoke_cmd(repeat_config, resmoke_args)
        expected_resmoke_cmd = [sys.executable, 'buildscripts/resmoke.py', 'run'
                                ] + resmoke_args + ['--repeatSuites=3']

        self.assertListEqual(expected_resmoke_cmd, resmoke_cmd)


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
        self.assertEqual(mock_suite_class.call_count, 1)


def create_variant_task_mock(task_name, suite_name, distro="distro"):
    variant_task = MagicMock()
    variant_task.name = task_name
    variant_task.generated_task_name = task_name
    variant_task.get_suite_name.return_value = suite_name
    variant_task.resmoke_args = f"--suites={suite_name}"
    variant_task.require_multiversion_setup.return_value = False
    variant_task.run_on = [distro]
    return variant_task


class TestTaskInfo(unittest.TestCase):
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

        task_info = under_test.TaskInfo.from_task(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info.resmoke_args)
        for test in test_list:
            self.assertIn(test, task_info.tests)
        self.assertFalse(task_info.require_multiversion_setup)
        self.assertEqual(distro_name, task_info.distro)

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

        task_info = under_test.TaskInfo.from_task(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info.resmoke_args)
        for test in test_list:
            self.assertIn(test, task_info.tests)
        self.assertFalse(task_info.require_multiversion_setup)
        self.assertEqual(distro_name, task_info.distro)

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

        task_info = under_test.TaskInfo.from_task(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info.resmoke_args)
        for test in test_list:
            self.assertIn(test, task_info.tests)
        self.assertFalse(task_info.require_multiversion_setup)
        self.assertEqual(distro_name, task_info.distro)

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

        task_info = under_test.TaskInfo.from_task(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info.resmoke_args)
        for test in test_list:
            self.assertIn(test, task_info.tests)
        self.assertFalse(task_info.require_multiversion_setup)
        self.assertEqual(large_distro_name, task_info.distro)


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
        self.assertIn("suite_1", task_info.resmoke_args)
        for i in range(3):
            self.assertIn(f"test{i}.js", task_info.tests)
        self.assertFalse(task_info.require_multiversion_setup)
        self.assertEqual("distro 1", task_info.distro)

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


class TestCreateTestsByTask(unittest.TestCase):
    def test_build_variant_not_in_evg_project_config(self):
        variant = "novariant"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value = None

        with self.assertRaises(ValueError):
            under_test.create_tests_by_task(variant, evg_conf_mock, set(), "install-dir/bin")


class TestLocalFileChangeDetector(unittest.TestCase):
    @patch(ns("find_changed_files_in_repos"))
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

        file_change_detector = under_test.LocalFileChangeDetector(None)
        found_tests = file_change_detector.find_changed_tests([repo_mock])

        self.assertIn(file_list[0], found_tests)
        self.assertIn(file_list[2], found_tests)
        self.assertNotIn(file_list[1], found_tests)

    @patch(ns("find_changed_files_in_repos"))
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

        file_change_detector = under_test.LocalFileChangeDetector(None)
        found_tests = file_change_detector.find_changed_tests([repo_mock])

        self.assertEqual(0, len(found_tests))

    @patch(ns("find_changed_files_in_repos"))
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

        file_change_detector = under_test.LocalFileChangeDetector(None)
        found_tests = file_change_detector.find_changed_tests([repo_mock])

        self.assertIn(file_list[0], found_tests)
        self.assertIn(file_list[2], found_tests)
        self.assertNotIn(file_list[1], found_tests)
        self.assertEqual(2, len(found_tests))


class TestYamlBurnInExecutor(unittest.TestCase):
    @patch('sys.stdout', new_callable=StringIO)
    def test_found_tasks_should_be_reported_as_yaml(self, stdout):
        n_tasks = 5
        n_tests = 3
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)

        yaml_executor = under_test.YamlBurnInExecutor()
        yaml_executor.execute(tests_by_task)

        yaml_raw = stdout.getvalue()
        results = yaml.safe_load(yaml_raw)
        self.assertEqual(n_tasks, len(results["discovered_tasks"]))
        self.assertEqual(n_tests, len(results["discovered_tasks"][0]["test_list"]))
