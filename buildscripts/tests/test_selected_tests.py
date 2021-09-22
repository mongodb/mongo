"""Unit tests for the selected_tests script."""
import json
import sys
import unittest
from datetime import datetime, timedelta
from typing import Dict, Any

import inject
from mock import MagicMock, patch
from evergreen import EvergreenApi

# pylint: disable=wrong-import-position
import buildscripts.ciconfig.evergreen as _evergreen
from buildscripts.burn_in_tests import TaskInfo
from buildscripts.patch_builds.selected_tests.selected_tests_client import SelectedTestsClient, \
    TestMappingsResponse, TestMapping, TestFileInstance, TaskMappingsResponse, TaskMapInstance, \
    TaskMapping
from buildscripts.selected_tests import EvgExpansions
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, greedy_division, \
    FallbackStrategy, round_robin_fallback
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.tests.test_burn_in_tests import get_evergreen_config, mock_changed_git_files
from buildscripts import selected_tests as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,protected-access,no-value-for-parameter

NS = "buildscripts.selected_tests"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def empty_build_variant(variant_name: str) -> Dict[str, Any]:
    return {
        "buildvariants": [{
            "name": variant_name,
            "tasks": [],
        }],
        "tasks": [],
    }


def configure_dependencies(evg_api, evg_expansions, evg_project_config, selected_test_client,
                           test_suites_dir=under_test.DEFAULT_TEST_SUITE_DIR):
    start_date = datetime.utcnow()
    end_date = start_date - timedelta(weeks=2)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(EvgExpansions, evg_expansions)
        binder.bind(_evergreen.EvergreenProjectConfig, evg_project_config)
        binder.bind(SuiteSplitConfig, evg_expansions.build_suite_split_config(start_date, end_date))
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, evg_expansions.build_gen_task_options())
        binder.bind(EvergreenApi, evg_api)
        binder.bind(GenerationConfiguration, GenerationConfiguration.from_yaml_file())
        binder.bind(ResmokeProxyConfig, ResmokeProxyConfig(resmoke_suite_dir=test_suites_dir))
        binder.bind(SelectedTestsClient, selected_test_client)

    inject.clear_and_configure(dependencies)


class TestAcceptance(unittest.TestCase):
    """A suite of Acceptance tests for selected_tests."""

    @staticmethod
    def _mock_evg_api():
        evg_api_mock = MagicMock()
        task_mock = evg_api_mock.task_by_id.return_value
        task_mock.execution = 0
        return evg_api_mock

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_when_no_mappings_are_found_for_changed_files(self):
        mock_evg_api = self._mock_evg_api()
        mock_evg_config = get_evergreen_config("etc/evergreen.yml")
        mock_evg_expansions = under_test.EvgExpansions(
            task_id="task_id",
            task_name="selected_tests_gen",
            build_variant="selected-tests",
            build_id="my_build_id",
            project="mongodb-mongo-master",
            revision="abc123",
            version_id="my_version",
        )
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_test_mappings.return_value = TestMappingsResponse(
            test_mappings=[])
        configure_dependencies(mock_evg_api, mock_evg_expansions, mock_evg_config,
                               mock_selected_tests_client)
        repos = [mock_changed_git_files([])]

        selected_tests = under_test.SelectedTestsOrchestrator()
        changed_files = selected_tests.find_changed_files(repos, "task_id")
        generated_config = selected_tests.generate_version(changed_files)

        # assert that config_dict does not contain keys for any generated task configs
        self.assertEqual(len(generated_config.file_list), 1)
        self.assertEqual(generated_config.file_list[0].file_name, "selected_tests_config.json")

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_when_test_mappings_are_found_for_changed_files(self):
        mock_evg_api = self._mock_evg_api()
        mock_evg_config = get_evergreen_config("etc/evergreen.yml")
        mock_evg_expansions = under_test.EvgExpansions(
            task_id="task_id",
            task_name="selected_tests_gen",
            build_variant="selected-tests",
            build_id="my_build_id",
            project="mongodb-mongo-master",
            revision="abc123",
            version_id="my_version",
        )
        mock_test_mapping = TestMapping(
            branch="master", project="mongodb-mongo-master", repo="mongodb/mongo",
            source_file="src/file1.cpp", source_file_seen_count=8,
            test_files=[TestFileInstance(name="jstests/auth/auth1.js", test_file_seen_count=3)])
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_test_mappings.return_value = TestMappingsResponse(
            test_mappings=[mock_test_mapping])
        configure_dependencies(mock_evg_api, mock_evg_expansions, mock_evg_config,
                               mock_selected_tests_client)
        repos = [mock_changed_git_files(["src/file1.cpp"])]

        selected_tests = under_test.SelectedTestsOrchestrator()
        changed_files = selected_tests.find_changed_files(repos, "task_id")
        generated_config = selected_tests.generate_version(changed_files)

        files_to_generate = {gen_file.file_name for gen_file in generated_config.file_list}
        self.assertIn("selected_tests_config.json", files_to_generate)

        # assert that generated suite files have the suite name and the variant name in the
        # filename, to prevent tasks on different variants from using the same suite file
        self.assertIn("auth_enterprise-rhel-80-64-bit-dynamic-required_0.yml", files_to_generate)

        generated_evg_config_raw = [
            gen_file.content for gen_file in generated_config.file_list
            if gen_file.file_name == "selected_tests_config.json"
        ][0]

        generated_evg_config = json.loads(generated_evg_config_raw)
        build_variants_with_generated_tasks = generated_evg_config["buildvariants"]
        # jstests/auth/auth1.js belongs to two suites, auth and auth_audit,
        rhel_80_with_generated_tasks = next(
            (variant for variant in build_variants_with_generated_tasks
             if variant["name"] == "enterprise-rhel-80-64-bit-dynamic-required"), None)
        self.assertEqual(len(rhel_80_with_generated_tasks["tasks"]), 2)

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_when_task_mappings_are_found_for_changed_files(self):
        mock_evg_api = self._mock_evg_api()
        mock_evg_config = get_evergreen_config("etc/evergreen.yml")
        mock_evg_expansions = under_test.EvgExpansions(
            task_id="task_id",
            task_name="selected_tests_gen",
            build_variant="selected-tests",
            build_id="my_build_id",
            project="mongodb-mongo-master",
            revision="abc123",
            version_id="my_version",
        )
        mock_task_mapping = TaskMapping(
            branch="master", project="mongodb-mongo-master", repo="mongodb/mongo",
            source_file="src/file1.cpp", source_file_seen_count=8,
            tasks=[TaskMapInstance(name="auth", variant="enterprise-rhel-80", flip_count=5)])
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_task_mappings.return_value = TaskMappingsResponse(
            task_mappings=[mock_task_mapping])
        configure_dependencies(mock_evg_api, mock_evg_expansions, mock_evg_config,
                               mock_selected_tests_client)
        repos = [mock_changed_git_files(["src/file1.cpp"])]

        selected_tests = under_test.SelectedTestsOrchestrator()
        changed_files = selected_tests.find_changed_files(repos, "task_id")
        generated_config = selected_tests.generate_version(changed_files)

        files_to_generate = {gen_file.file_name for gen_file in generated_config.file_list}
        self.assertIn("selected_tests_config.json", files_to_generate)

        generated_evg_config_raw = [
            gen_file.content for gen_file in generated_config.file_list
            if gen_file.file_name == "selected_tests_config.json"
        ][0]
        generated_evg_config = json.loads(generated_evg_config_raw)
        # the auth task's generator task, max_sub_suites is 3,
        # resulting in 3 subtasks being generated, plus a _misc task, hence 4
        # tasks total
        build_variants_with_generated_tasks = generated_evg_config["buildvariants"]
        rhel_80_with_generated_tasks = next(
            (variant for variant in build_variants_with_generated_tasks
             if variant["name"] == "enterprise-rhel-80-64-bit-dynamic-required"), None)
        self.assertEqual(len(rhel_80_with_generated_tasks["tasks"]), 5)


class TestExcludeTask(unittest.TestCase):
    def test_task_should_not_be_excluded(self):
        task = _evergreen.Task({"name": "regular_task"})

        self.assertEqual(under_test._exclude_task(task), False)

    def test_task_should_be_excluded(self):
        excluded_task = under_test.EXCLUDE_TASK_LIST[0]
        task = _evergreen.Task({"name": excluded_task})

        self.assertEqual(under_test._exclude_task(task), True)

    def test_task_matches_excluded_pattern(self):
        task_that_matches_exclude_pattern = "compile_all"
        task = _evergreen.Task({"name": task_that_matches_exclude_pattern})

        self.assertEqual(under_test._exclude_task(task), True)


def build_mock_evg_task(name, cmd_func="generate resmoke tasks",
                        resmoke_args="--storageEngine=wiredTiger"):
    return _evergreen.Task({
        "name": name,
        "commands": [{
            "func": cmd_func,
            "vars": {"resmoke_args": resmoke_args, },
        }],
    })


class TestGetEvgTaskConfig(unittest.TestCase):
    def test_task_is_a_generate_resmoke_task(self):
        build_variant_conf = MagicMock()
        build_variant_conf.name = "variant"
        task = build_mock_evg_task("auth_gen")

        task_config_service = under_test.TaskConfigService()
        evg_task_config = task_config_service.get_evg_task_config(task, build_variant_conf)

        self.assertEqual(evg_task_config["task_name"], "auth")
        self.assertEqual(evg_task_config["build_variant"], "variant")
        self.assertIsNone(evg_task_config.get("suite"))
        self.assertEqual(
            evg_task_config["resmoke_args"],
            "--storageEngine=wiredTiger",
        )

    def test_task_is_not_a_generate_resmoke_task(self):
        build_variant_conf = MagicMock()
        build_variant_conf.name = "variant"
        task = build_mock_evg_task("jsCore_auth", "run tests",
                                   "--suites=core_auth --storageEngine=wiredTiger")

        task_config_service = under_test.TaskConfigService()
        evg_task_config = task_config_service.get_evg_task_config(task, build_variant_conf)

        self.assertEqual(evg_task_config["task_name"], "jsCore_auth")
        self.assertEqual(evg_task_config["build_variant"], "variant")
        self.assertEqual(evg_task_config["suite"], "core_auth")
        self.assertEqual(
            evg_task_config["resmoke_args"],
            "--storageEngine=wiredTiger",
        )


class TestGetTaskConfigsForTestMappings(unittest.TestCase):
    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_test_mapping(self, find_task_mock, exclude_task_mock):
        find_task_mock.side_effect = [
            build_mock_evg_task("jsCore_auth", "run tests"),
            build_mock_evg_task("auth_gen", "run tests",
                                "--suites=core_auth --storageEngine=wiredTiger"),
        ]
        exclude_task_mock.return_value = False
        tests_by_task = {
            "jsCore_auth":
                TaskInfo(
                    display_task_name="task 1",
                    tests=[
                        "jstests/core/currentop_waiting_for_latch.js",
                        "jstests/core/latch_analyzer.js",
                    ],
                    resmoke_args="",
                    require_multiversion=None,
                    distro="",
                ),
            "auth_gen":
                TaskInfo(
                    display_task_name="task 2",
                    tests=["jstests/auth/auth3.js"],
                    resmoke_args="",
                    require_multiversion=None,
                    distro="",
                ),
        }

        task_config_service = under_test.TaskConfigService()
        task_configs = task_config_service.get_task_configs_for_test_mappings(
            tests_by_task, MagicMock())

        self.assertEqual(task_configs["jsCore_auth"]["resmoke_args"], "--storageEngine=wiredTiger")
        self.assertEqual(
            task_configs["jsCore_auth"]["selected_tests_to_run"],
            {"jstests/core/currentop_waiting_for_latch.js", "jstests/core/latch_analyzer.js"})
        self.assertEqual(task_configs["auth_gen"]["suite"], "core_auth")
        self.assertEqual(task_configs["auth_gen"]["selected_tests_to_run"],
                         {'jstests/auth/auth3.js'})

    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_test_mapping_when_task_should_be_excluded(self, find_task_mock,
                                                                      exclude_task_mock):
        find_task_mock.return_value = build_mock_evg_task(
            "jsCore_auth", "run tests", "--suites=core_auth --storageEngine=wiredTiger")
        exclude_task_mock.return_value = True
        tests_by_task = {
            "jsCore_auth":
                TaskInfo(
                    display_task_name="task 1",
                    tests=[
                        "jstests/core/currentop_waiting_for_latch.js",
                        "jstests/core/latch_analyzer.js",
                    ],
                    resmoke_args="",
                    require_multiversion=None,
                    distro="",
                ),
        }

        task_config_service = under_test.TaskConfigService()
        task_configs = task_config_service.get_task_configs_for_test_mappings(
            tests_by_task, MagicMock())

        self.assertEqual(task_configs, {})

    @patch(ns("_find_task"))
    def test_get_config_for_test_mapping_when_task_does_not_exist(self, find_task_mock):
        find_task_mock.return_value = None
        tests_by_task = {
            "jsCore_auth":
                TaskInfo(
                    display_task_name="task 1",
                    tests=[
                        "jstests/core/currentop_waiting_for_latch.js",
                        "jstests/core/latch_analyzer.js",
                    ],
                    resmoke_args="",
                    require_multiversion=None,
                    distro="",
                ),
        }

        task_config_service = under_test.TaskConfigService()
        task_configs = task_config_service.get_task_configs_for_test_mappings(
            tests_by_task, MagicMock())

        self.assertEqual(task_configs, {})


class TestGetTaskConfigsForTaskMappings(unittest.TestCase):
    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_task_mapping(self, find_task_mock, exclude_task_mock):
        find_task_mock.side_effect = [build_mock_evg_task("task_1"), build_mock_evg_task("task_2")]
        exclude_task_mock.return_value = False
        tasks = ["task_1", "task_2"]

        task_config_service = under_test.TaskConfigService()
        task_configs = task_config_service.get_task_configs_for_task_mappings(tasks, MagicMock())

        self.assertEqual(task_configs["task_1"]["resmoke_args"], "--storageEngine=wiredTiger")
        self.assertEqual(task_configs["task_2"]["resmoke_args"], "--storageEngine=wiredTiger")

    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_task_mapping_when_task_should_be_excluded(self, find_task_mock,
                                                                      exclude_task_mock):
        find_task_mock.return_value = build_mock_evg_task("task_1")
        exclude_task_mock.return_value = True
        tasks = ["task_1"]

        task_config_service = under_test.TaskConfigService()
        task_configs = task_config_service.get_task_configs_for_task_mappings(tasks, MagicMock())

        self.assertEqual(task_configs, {})

    @patch(ns("_find_task"))
    def test_get_config_for_task_mapping_when_task_does_not_exist(self, find_task_mock):
        find_task_mock.return_value = None
        tasks = ["task_1"]

        task_config_service = under_test.TaskConfigService()
        task_configs = task_config_service.get_task_configs_for_task_mappings(tasks, MagicMock())

        self.assertEqual(task_configs, {})


class TestRemoveRepoPathPrefix(unittest.TestCase):
    def test_file_is_in_enterprise_modules(self):
        filepath = under_test._remove_repo_path_prefix(
            "src/mongo/db/modules/enterprise/src/file1.cpp")

        self.assertEqual(filepath, "src/file1.cpp")

    def test_file_is_not_in_enterprise_modules(self):
        filepath = under_test._remove_repo_path_prefix("other_directory/src/file1.cpp")

        self.assertEqual(filepath, "other_directory/src/file1.cpp")
