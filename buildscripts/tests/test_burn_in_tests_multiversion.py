"""Unit tests for buildscripts/burn_in_tests_multiversion.py."""

from __future__ import absolute_import

import json
from datetime import datetime
import os
import sys
import unittest
from mock import MagicMock, patch

import inject
from evergreen import EvergreenApi

import buildscripts.burn_in_tests_multiversion as under_test
from buildscripts.burn_in_tests import TaskInfo
from buildscripts.ciconfig.evergreen import parse_evergreen_file, EvergreenProjectConfig
import buildscripts.resmokelib.parser as _parser
from buildscripts.evergreen_burn_in_tests import EvergreenFileChangeDetector
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.multiversion_util import REPL_MIXED_VERSION_CONFIGS, \
    SHARDED_MIXED_VERSION_CONFIGS
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig
from buildscripts.task_generation.suite_split_strategies import greedy_division, SplitStrategy, \
    FallbackStrategy, round_robin_fallback
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions

_parser.set_run_options()

MONGO_4_2_HASH = "d94888c0d0a8065ca57d354ece33b3c2a1a5a6d6"

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access,no-value-for-parameter


def create_tests_by_task_mock(n_tasks, n_tests, multiversion_values=None):
    if multiversion_values is None:
        multiversion_values = [None for _ in range(n_tasks)]
    return {
        f"task_{i}_gen": TaskInfo(
            display_task_name=f"task_{i}",
            resmoke_args=f"--suites=suite_{i}",
            tests=[f"jstests/tests_{j}" for j in range(n_tests)],
            require_multiversion=multiversion_values[i],
            distro="",
        )
        for i in range(n_tasks)
    }


MV_MOCK_SUITES = ["replica_sets_jscore_passthrough", "sharding_jscore_passthrough"]
MV_MOCK_TESTS = {
    "replica_sets_jscore_passthrough": [
        "core/all.js",
        "core/andor.js",
        "core/apitest_db.js",
        "core/auth1.js",
        "core/auth2.js",
    ], "sharding_jscore_passthrough": [
        "core/basic8.js",
        "core/batch_size.js",
        "core/bson.js",
        "core/bulk_insert.js",
        "core/capped.js",
    ]
}


def create_multiversion_tests_by_task_mock(n_tasks, n_tests):
    assert n_tasks <= len(MV_MOCK_SUITES)
    assert n_tests <= len(MV_MOCK_TESTS[MV_MOCK_SUITES[0]])
    return {
        f"{MV_MOCK_SUITES[i]}": TaskInfo(
            display_task_name=f"task_{i}",
            resmoke_args=f"--suites=suite_{i}",
            tests=[f"jstests/{MV_MOCK_TESTS[MV_MOCK_SUITES[i]][j]}" for j in range(n_tests)],
            require_multiversion=None,
            distro="",
        )
        for i in range(n_tasks)
    }


_DATE = datetime(2018, 7, 15)
BURN_IN_TESTS = "buildscripts.burn_in_tests"
NUM_REPL_MIXED_VERSION_CONFIGS = len(REPL_MIXED_VERSION_CONFIGS)
NUM_SHARDED_MIXED_VERSION_CONFIGS = len(SHARDED_MIXED_VERSION_CONFIGS)

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
    variant_task.require_multiversion = None
    variant_task.run_on = [distro]
    return variant_task


def build_mock_gen_task_options():
    return GenTaskOptions(
        create_misc_suite=False,
        is_patch=True,
        generated_config_dir=under_test.DEFAULT_CONFIG_DIR,
        use_default_timeouts=False,
    )


def build_mock_split_task_config():
    return SuiteSplitConfig(
        evg_project="my project",
        target_resmoke_time=60,
        max_sub_suites=100,
        max_tests_per_suite=1,
        start_date=datetime.utcnow(),
        end_date=datetime.utcnow(),
        default_to_fallback=True,
    )


def configure_dependencies(evg_api, split_config):
    gen_task_options = build_mock_gen_task_options()

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(SuiteSplitConfig, split_config)
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, gen_task_options)
        binder.bind(EvergreenApi, evg_api)
        binder.bind(GenerationConfiguration, GenerationConfiguration.from_yaml_file())
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=under_test.DEFAULT_TEST_SUITE_DIR))
        binder.bind(EvergreenFileChangeDetector, None)
        binder.bind(EvergreenProjectConfig, MagicMock())
        binder.bind(
            under_test.BurnInConfig,
            under_test.BurnInConfig(build_id="build_id", build_variant="build variant",
                                    revision="revision"))

    inject.clear_and_configure(dependencies)


class TestCreateMultiversionGenerateTasksConfig(unittest.TestCase):
    def tests_no_tasks_given(self):
        target_file = "target_file.json"
        mock_evg_api = MagicMock()
        split_config = build_mock_split_task_config()
        configure_dependencies(mock_evg_api, split_config)

        orchestrator = under_test.MultiversionBurnInOrchestrator()
        generated_config = orchestrator.generate_configuration({}, target_file, "build_variant")

        evg_config = [
            config for config in generated_config.file_list if config.file_name == target_file
        ]
        self.assertEqual(1, len(evg_config))
        evg_config = evg_config[0]
        evg_config_dict = json.loads(evg_config.content)

        self.assertEqual(0, len(evg_config_dict["tasks"]))

    def test_tasks_not_in_multiversion_suites(self):
        n_tasks = 1
        n_tests = 1
        target_file = "target_file.json"
        mock_evg_api = MagicMock()
        split_config = build_mock_split_task_config()
        configure_dependencies(mock_evg_api, split_config)
        tests_by_task = create_tests_by_task_mock(n_tasks, n_tests)

        orchestrator = under_test.MultiversionBurnInOrchestrator()
        generated_config = orchestrator.generate_configuration(tests_by_task, target_file,
                                                               "build_variant")

        evg_config = [
            config for config in generated_config.file_list if config.file_name == target_file
        ]
        self.assertEqual(1, len(evg_config))
        evg_config = evg_config[0]
        evg_config_dict = json.loads(evg_config.content)

        self.assertEqual(0, len(evg_config_dict["tasks"]))

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(
        "buildscripts.evergreen_gen_multiversion_tests.get_backports_required_hash_for_shell_version"
    )
    def test_one_task_one_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 1
        n_tests = 1
        target_file = "target_file.json"
        mock_evg_api = MagicMock()
        split_config = build_mock_split_task_config()
        configure_dependencies(mock_evg_api, split_config)
        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)

        orchestrator = under_test.MultiversionBurnInOrchestrator()
        generated_config = orchestrator.generate_configuration(tests_by_task, target_file,
                                                               "build_variant")

        evg_config = [
            config for config in generated_config.file_list if config.file_name == target_file
        ]
        self.assertEqual(1, len(evg_config))
        evg_config = evg_config[0]
        evg_config_dict = json.loads(evg_config.content)
        tasks = evg_config_dict["tasks"]
        self.assertEqual(len(tasks), NUM_REPL_MIXED_VERSION_CONFIGS * n_tests)

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(
        "buildscripts.evergreen_gen_multiversion_tests.get_backports_required_hash_for_shell_version"
    )
    def test_n_task_one_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 2
        n_tests = 1
        target_file = "target_file.json"
        mock_evg_api = MagicMock()
        split_config = build_mock_split_task_config()
        configure_dependencies(mock_evg_api, split_config)
        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)

        orchestrator = under_test.MultiversionBurnInOrchestrator()
        generated_config = orchestrator.generate_configuration(tests_by_task, target_file,
                                                               "build_variant")

        evg_config = [
            config for config in generated_config.file_list if config.file_name == target_file
        ]
        self.assertEqual(1, len(evg_config))
        evg_config = evg_config[0]
        evg_config_dict = json.loads(evg_config.content)
        tasks = evg_config_dict["tasks"]
        self.assertEqual(
            len(tasks),
            (NUM_REPL_MIXED_VERSION_CONFIGS + NUM_SHARDED_MIXED_VERSION_CONFIGS) * n_tests)

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(
        "buildscripts.evergreen_gen_multiversion_tests.get_backports_required_hash_for_shell_version"
    )
    def test_one_task_n_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 1
        n_tests = 2
        target_file = "target_file.json"
        mock_evg_api = MagicMock()
        split_config = build_mock_split_task_config()
        configure_dependencies(mock_evg_api, split_config)
        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)

        orchestrator = under_test.MultiversionBurnInOrchestrator()
        generated_config = orchestrator.generate_configuration(tests_by_task, target_file,
                                                               "build_variant")

        evg_config = [
            config for config in generated_config.file_list if config.file_name == target_file
        ]
        self.assertEqual(1, len(evg_config))
        evg_config = evg_config[0]
        evg_config_dict = json.loads(evg_config.content)
        tasks = evg_config_dict["tasks"]
        self.assertEqual(len(tasks), NUM_REPL_MIXED_VERSION_CONFIGS * n_tests)

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(
        "buildscripts.evergreen_gen_multiversion_tests.get_backports_required_hash_for_shell_version"
    )
    def test_n_task_m_test(self, mock_hash):
        mock_hash.return_value = MONGO_4_2_HASH
        n_tasks = 2
        n_tests = 3
        target_file = "target_file.json"
        mock_evg_api = MagicMock()
        split_config = build_mock_split_task_config()
        configure_dependencies(mock_evg_api, split_config)
        tests_by_task = create_multiversion_tests_by_task_mock(n_tasks, n_tests)

        orchestrator = under_test.MultiversionBurnInOrchestrator()
        generated_config = orchestrator.generate_configuration(tests_by_task, target_file,
                                                               "build_variant")

        evg_config = [
            config for config in generated_config.file_list if config.file_name == target_file
        ]
        self.assertEqual(1, len(evg_config))
        evg_config = evg_config[0]
        evg_config_dict = json.loads(evg_config.content)
        tasks = evg_config_dict["tasks"]
        self.assertEqual(
            len(tasks),
            (NUM_REPL_MIXED_VERSION_CONFIGS + NUM_SHARDED_MIXED_VERSION_CONFIGS) * n_tests)


class TestGenerateConfig(unittest.TestCase):
    def test_validate_multiversion(self):
        evg_conf_mock = MagicMock()
        gen_config = under_test.GenerateConfig("build_variant", "project")
        gen_config.validate(evg_conf_mock)


class TestGatherTaskInfo(unittest.TestCase):
    def test_multiversion_task(self):
        suite_name = "suite_1"
        distro_name = "distro_1"
        variant = "build_variant"
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_task.return_value.is_generate_resmoke_task = False
        task_mock = create_variant_task_mock("task 1", suite_name, distro_name)
        task_mock.require_multiversion = True
        test_list = [f"test{i}.js" for i in range(3)]
        tests_by_suite = {
            suite_name: test_list,
            "suite 2": [f"test{i}.js" for i in range(1)],
            "suite 3": [f"test{i}.js" for i in range(2)],
        }

        task_info = TaskInfo.from_task(task_mock, tests_by_suite, evg_conf_mock, variant)

        self.assertIn(suite_name, task_info.resmoke_args)
        for test in test_list:
            self.assertIn(test, task_info.tests)
        self.assertEqual(task_mock.require_multiversion, task_info.require_multiversion)
        self.assertEqual(distro_name, task_info.distro)
