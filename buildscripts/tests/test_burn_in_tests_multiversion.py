"""Unit tests for buildscripts/burn_in_tests_multiversion.py."""

from __future__ import absolute_import

import datetime
import os
import sys
import unittest

from mock import MagicMock, patch

from shrub.v2 import BuildVariant, ShrubProject

import buildscripts.burn_in_tests_multiversion as under_test
from buildscripts.burn_in_tests import _gather_task_info, create_generate_tasks_config
from buildscripts.ciconfig.evergreen import parse_evergreen_file
import buildscripts.resmokelib.parser as _parser
import buildscripts.evergreen_gen_multiversion_tests as gen_multiversion
_parser.set_options()

MONGO_4_2_HASH = "d94888c0d0a8065ca57d354ece33b3c2a1a5a6d6"

# pylint: disable=missing-docstring,protected-access,too-many-lines,no-self-use


def create_tests_by_task_mock(n_tasks, n_tests):
    return {
        f"task_{i}_gen": {
            "display_task_name": f"task_{i}", "resmoke_args": f"--suites=suite_{i}",
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
BURN_IN_TESTS = "buildscripts.burn_in_tests"
NUM_REPL_MIXED_VERSION_CONFIGS = len(gen_multiversion.REPL_MIXED_VERSION_CONFIGS)
NUM_SHARDED_MIXED_VERSION_CONFIGS = len(gen_multiversion.SHARDED_MIXED_VERSION_CONFIGS)

NS = "buildscripts.burn_in_tests_multiversion"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def bit_ns(relative_name):
    return BURN_IN_TESTS + "." + relative_name


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


def create_variant_task_mock(task_name, suite_name, distro="distro"):
    variant_task = MagicMock()
    variant_task.name = task_name
    variant_task.generated_task_name = task_name
    variant_task.resmoke_suite = suite_name
    variant_task.get_vars_suite_name.return_value = suite_name
    variant_task.combined_resmoke_args = f"--suites={suite_name}"
    variant_task.multiversion_path = None
    variant_task.run_on = [distro]
    return variant_task


class TestCreateMultiversionGenerateTasksConfig(unittest.TestCase):
    def tests_no_tasks_given(self):
        gen_config = MagicMock(run_build_variant="variant", fallback_num_sub_suites=1,
                               project="project", build_variant="build_variant", task_id="task_id",
                               target_resmoke_time=60)
        evg_api = MagicMock()
        build_variant = under_test.create_multiversion_generate_tasks_config({}, evg_api,
                                                                             gen_config)
        evg_config_dict = build_variant.as_dict()
        self.assertEqual(0, len(evg_config_dict["tasks"]))

    def test_tasks_not_in_multiversion_suites(self):
        n_tasks = 1
        n_tests = 1
        gen_config = MagicMock(run_build_variant="variant", fallback_num_sub_suites=1,
                               project="project", build_variant="build_variant", task_id="task_id",
                               target_resmoke_time=60)
        evg_api = MagicMock()

        # Create a tests_by_tasks dict that doesn't contain any multiversion suites.
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)
        build_variant = under_test.create_multiversion_generate_tasks_config(
            tests_by_task, evg_api, gen_config)
        evg_config_dict = build_variant.as_dict()

        # We should not generate any tasks that are not part of the burn_in_multiversion suite.
        self.assertEqual(0, len(evg_config_dict["tasks"]))

    @patch("buildscripts.evergreen_gen_multiversion_tests.get_backports_required_last_stable_hash")
    def test_one_task_one_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 1
        n_tests = 1
        gen_config = MagicMock(run_build_variant="variant", fallback_num_sub_suites=1,
                               project="project", build_variant="build_variant", task_id="task_id",
                               target_resmoke_time=60)
        evg_api = MagicMock()

        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)
        build_variant = under_test.create_multiversion_generate_tasks_config(
            tests_by_task, evg_api, gen_config)
        evg_config_dict = build_variant.as_dict()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(len(tasks), NUM_REPL_MIXED_VERSION_CONFIGS * n_tests)

    @patch("buildscripts.evergreen_gen_multiversion_tests.get_backports_required_last_stable_hash")
    def test_n_task_one_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 2
        n_tests = 1
        gen_config = MagicMock(run_build_variant="variant", fallback_num_sub_suites=1,
                               project="project", build_variant="build_variant", task_id="task_id",
                               target_resmoke_time=60)
        evg_api = MagicMock()

        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)
        build_variant = under_test.create_multiversion_generate_tasks_config(
            tests_by_task, evg_api, gen_config)
        evg_config_dict = build_variant.as_dict()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(
            len(tasks),
            (NUM_REPL_MIXED_VERSION_CONFIGS + NUM_SHARDED_MIXED_VERSION_CONFIGS) * n_tests)

    @patch("buildscripts.evergreen_gen_multiversion_tests.get_backports_required_last_stable_hash")
    def test_one_task_n_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 1
        n_tests = 2
        gen_config = MagicMock(run_build_variant="variant", fallback_num_sub_suites=1,
                               project="project", build_variant="build_variant", task_id="task_id",
                               target_resmoke_time=60)
        evg_api = MagicMock()

        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)
        build_variant = under_test.create_multiversion_generate_tasks_config(
            tests_by_task, evg_api, gen_config)
        evg_config_dict = build_variant.as_dict()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(len(tasks), NUM_REPL_MIXED_VERSION_CONFIGS * n_tests)

    @patch("buildscripts.evergreen_gen_multiversion_tests.get_backports_required_last_stable_hash")
    def test_n_task_m_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 2
        n_tests = 3
        gen_config = MagicMock(run_build_variant="variant", fallback_num_sub_suites=1,
                               project="project", build_variant="build_variant", task_id="task_id",
                               target_resmoke_time=60)
        evg_api = MagicMock()

        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)
        build_variant = under_test.create_multiversion_generate_tasks_config(
            tests_by_task, evg_api, gen_config)
        evg_config_dict = build_variant.as_dict()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(
            len(tasks),
            (NUM_REPL_MIXED_VERSION_CONFIGS + NUM_SHARDED_MIXED_VERSION_CONFIGS) * n_tests)


class TestRepeatConfig(unittest.TestCase):
    def test_get_resmoke_repeat_options_use_multiversion(self):
        repeat_config = under_test.RepeatConfig()

        self.assertEqual(repeat_config, repeat_config.validate())


class TestGenerateConfig(unittest.TestCase):
    def test_validate_use_multiversion(self):
        evg_conf_mock = MagicMock()

        gen_config = under_test.GenerateConfig("build_variant", "project")

        gen_config.validate(evg_conf_mock)


class TestCreateGenerateTasksConfig(unittest.TestCase):
    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_multiversion_path_is_used(self):
        n_tasks = 1
        n_tests = 1
        build_variant = BuildVariant("variant")
        gen_config = MagicMock(run_build_variant="variant", distro=None)
        repeat_config = MagicMock()
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)
        first_task = "task_0_gen"
        multiversion_path = "multiversion_path"
        tests_by_task[first_task]["use_multiversion"] = multiversion_path

        create_generate_tasks_config(build_variant, tests_by_task, gen_config, repeat_config, None)

        shrub_project = ShrubProject.empty().add_build_variant(build_variant)
        evg_config_dict = shrub_project.as_dict()
        tasks = evg_config_dict["tasks"]
        self.assertEqual(n_tasks * n_tests, len(tasks))
        self.assertEqual(multiversion_path, tasks[0]["commands"][2]["vars"]["task_path_suffix"])


class TestGatherTaskInfo(unittest.TestCase):
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

        task_info = _gather_task_info(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info["resmoke_args"])
        for test in test_list:
            self.assertIn(test, task_info["tests"])
        self.assertEqual(task_mock.multiversion_path, task_info["use_multiversion"])
        self.assertEqual(distro_name, task_info["distro"])
