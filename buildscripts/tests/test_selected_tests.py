"""Unit tests for the selected_tests script."""
import json
import sys
import unittest
from typing import Dict, Any

from mock import MagicMock, patch
from shrub.v2 import BuildVariant, ShrubProject

# pylint: disable=wrong-import-position
import buildscripts.ciconfig.evergreen as _evergreen
from buildscripts.evergreen_generate_resmoke_tasks import Suite
from buildscripts.tests.test_burn_in_tests import get_evergreen_config, mock_changed_git_files
from buildscripts import selected_tests as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,protected-access

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


class TestAcceptance(unittest.TestCase):
    """A suite of Acceptance tests for selected_tests."""

    def setUp(self):
        Suite._current_index = 0

    @staticmethod
    def _mock_evg_api():
        evg_api_mock = MagicMock()
        task_mock = evg_api_mock.task_by_id.return_value
        task_mock.execution = 0
        return evg_api_mock

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_when_no_mappings_are_found_for_changed_files(self):
        evg_api_mock = self._mock_evg_api()
        evg_config = get_evergreen_config("etc/evergreen.yml")
        selected_tests_service_mock = MagicMock()
        selected_tests_service_mock.get_test_mappings.return_value = []
        selected_tests_variant_expansions = {
            "task_name": "selected_tests_gen", "build_variant": "selected-tests",
            "build_id": "my_build_id", "project": "mongodb-mongo-master"
        }
        repos = [mock_changed_git_files([])]
        origin_build_variants = ["enterprise-rhel-62-64-bit"]

        config_dict = under_test.run(evg_api_mock, evg_config, selected_tests_service_mock,
                                     selected_tests_variant_expansions, repos,
                                     origin_build_variants)

        self.assertEqual(
            json.loads(config_dict["selected_tests_config.json"]),
            empty_build_variant(origin_build_variants[0]))

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_when_test_mappings_are_found_for_changed_files(self):
        evg_api_mock = self._mock_evg_api()
        evg_config = get_evergreen_config("etc/evergreen.yml")
        selected_tests_service_mock = MagicMock()
        selected_tests_service_mock.get_test_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "test_files": [{"name": "jstests/auth/auth1.js"}],
            },
        ]
        selected_tests_variant_expansions = {
            "task_name": "selected_tests_gen", "build_variant": "selected-tests",
            "build_id": "my_build_id", "project": "mongodb-mongo-master"
        }
        repos = [mock_changed_git_files(["src/file1.cpp"])]
        origin_build_variants = ["enterprise-rhel-62-64-bit"]

        config_dict = under_test.run(evg_api_mock, evg_config, selected_tests_service_mock,
                                     selected_tests_variant_expansions, repos,
                                     origin_build_variants)

        self.assertIn("selected_tests_config.json", config_dict)
        # jstests/auth/auth1.js belongs to two suites, auth and auth_audit, each of which has
        # fallback_num_sub_suites = 4 in their resmoke args, resulting in 4 subtasks being generated
        # for each
        self.assertEqual(len(config_dict), 9)
        self.assertEqual(
            sorted(config_dict.keys()), [
                "auth_0.yml", "auth_1.yml", "auth_2.yml", "auth_3.yml", "auth_audit_4.yml",
                "auth_audit_5.yml", "auth_audit_6.yml", "auth_audit_7.yml",
                "selected_tests_config.json"
            ])

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    def test_when_task_mappings_are_found_for_changed_files(self):
        evg_api_mock = self._mock_evg_api()
        evg_config = get_evergreen_config("etc/evergreen.yml")
        selected_tests_service_mock = MagicMock()
        selected_tests_service_mock.get_task_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "tasks": [{"name": "auth"}],
            },
        ]
        selected_tests_variant_expansions = {
            "task_name": "selected_tests_gen", "build_variant": "selected-tests",
            "build_id": "my_build_id", "project": "mongodb-mongo-master"
        }
        repos = [mock_changed_git_files(["src/file1.cpp"])]
        origin_build_variants = ["enterprise-rhel-62-64-bit"]

        config_dict = under_test.run(evg_api_mock, evg_config, selected_tests_service_mock,
                                     selected_tests_variant_expansions, repos,
                                     origin_build_variants)

        self.assertIn("selected_tests_config.json", config_dict)
        # the auth task's generator task, auth_gen, has fallback_num_sub_suites = 4 in
        # its resmoke args, resulting in 4 subtasks being generated, plus a _misc task
        self.assertEqual(len(config_dict), 6)
        self.assertEqual(
            sorted(config_dict.keys()), [
                "auth_0.yml", "auth_1.yml", "auth_2.yml", "auth_3.yml", "auth_misc.yml",
                "selected_tests_config.json"
            ])


class TestSelectedTestsConfigOptions(unittest.TestCase):
    def test_overwrites_overwrite_filepath_config(self):
        origin_variant_expansions = {"key1": 0}
        selected_tests_variant_expansions = {"key1": 1}
        overwrites = {"key1": 2}
        required_keys = {"key1"}
        defaults = {}
        formats = {"key1": int}

        config_options = under_test.SelectedTestsConfigOptions.from_file(
            origin_variant_expansions, selected_tests_variant_expansions, overwrites, required_keys,
            defaults, formats)

        self.assertEqual(overwrites["key1"], config_options.key1)

    def test_overwrites_overwrite_defaults(self):
        origin_variant_expansions = {}
        selected_tests_variant_expansions = {"key1": 1}
        overwrites = {"key1": 2}
        required_keys = {"key1"}
        defaults = {"key1": 3}
        formats = {"key1": int}

        config_options = under_test.SelectedTestsConfigOptions.from_file(
            origin_variant_expansions, selected_tests_variant_expansions, overwrites, required_keys,
            defaults, formats)

        self.assertEqual(overwrites["key1"], config_options.key1)

    def test_selected_tests_config_overrides_origin_expansions(self):
        origin_variant_expansions = {"key1": 0}
        selected_tests_variant_expansions = {"key1": 1}
        overwrites = {}
        required_keys = {"key1"}
        defaults = {}
        formats = {"key1": int}

        config_options = under_test.SelectedTestsConfigOptions.from_file(
            origin_variant_expansions, selected_tests_variant_expansions, overwrites, required_keys,
            defaults, formats)

        self.assertEqual(selected_tests_variant_expansions["key1"], config_options.key1)

    def test_run_tests_task(self):
        config_options = under_test.SelectedTestsConfigOptions(
            {"name_of_generating_task": "my_task_gen"}, {}, {}, {})

        self.assertEqual(config_options.run_tests_task, "my_task")

    def test_run_tests_build_variant(self):
        config_options = under_test.SelectedTestsConfigOptions(
            {"name_of_generating_build_variant": "my-build-variant"}, {}, {}, {})

        self.assertEqual(config_options.run_tests_build_variant, "my-build-variant")

    def test_run_tests_build_id(self):
        config_options = under_test.SelectedTestsConfigOptions(
            {"name_of_generating_build_id": "my_build_id"}, {}, {}, {})

        self.assertEqual(config_options.run_tests_build_id, "my_build_id")

    def test_create_misc_suite_with_no_selected_tests_to_run(self):
        config_options = under_test.SelectedTestsConfigOptions({}, {}, {}, {})

        self.assertTrue(config_options.create_misc_suite)

    def test_create_misc_suite_with_selected_tests_to_run(self):
        config_options = under_test.SelectedTestsConfigOptions(
            {"selected_tests_to_run": {"my_test.js"}}, {}, {}, {})

        self.assertFalse(config_options.create_misc_suite)


class TestFindSelectedTestFiles(unittest.TestCase):
    @patch(ns("is_file_a_test_file"))
    @patch(ns("SelectedTestsService"))
    def test_related_files_returned_from_selected_tests_service(self, selected_tests_service_mock,
                                                                is_file_a_test_file_mock):
        is_file_a_test_file_mock.return_value = True
        changed_files = {"src/file1.cpp", "src/file2.js"}
        selected_tests_service_mock.get_test_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "test_files": [{"name": "jstests/file-1.js"}],
            },
            {
                "source_file": "src/file2.cpp",
                "test_files": [{"name": "jstests/file-3.js"}],
            },
        ]

        related_test_files = under_test._find_selected_test_files(selected_tests_service_mock,
                                                                  changed_files)

        self.assertEqual(related_test_files, {"jstests/file-1.js", "jstests/file-3.js"})

    @patch(ns("is_file_a_test_file"))
    @patch(ns("SelectedTestsService"))
    def test_related_files_returned_are_not_valid_test_files(self, selected_tests_service_mock,
                                                             is_file_a_test_file_mock):
        is_file_a_test_file_mock.return_value = False
        changed_files = {"src/file1.cpp", "src/file2.js"}
        selected_tests_service_mock.get_test_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "test_files": [{"name": "jstests/file-1.js"}],
            },
            {
                "source_file": "src/file2.cpp",
                "test_files": [{"name": "jstests/file-3.js"}],
            },
        ]

        related_test_files = under_test._find_selected_test_files(selected_tests_service_mock,
                                                                  changed_files)

        self.assertEqual(related_test_files, set())

    @patch(ns("SelectedTestsService"))
    def test_no_related_files_returned(self, selected_tests_service_mock):
        selected_tests_service_mock.get_test_mappings.return_value = set()
        changed_files = {"src/file1.cpp", "src/file2.js"}

        related_test_files = under_test._find_selected_test_files(selected_tests_service_mock,
                                                                  changed_files)

        self.assertEqual(related_test_files, set())


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


class TestFindSelectedTasks(unittest.TestCase):
    @patch(ns("SelectedTestsService"))
    def test_related_tasks_returned_from_selected_tests_service(self, selected_tests_service_mock):
        selected_tests_service_mock.get_task_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "tasks": [{"name": "my_task_1"}],
            },
            {
                "source_file": "src/file2.cpp",
                "tasks": [{"name": "my_task_2"}],
            },
        ]
        changed_files = {"src/file1.cpp", "src/file2.js"}

        related_tasks = under_test._find_selected_tasks(selected_tests_service_mock, changed_files)

        self.assertEqual(related_tasks, {"my_task_1", "my_task_2"})


class TestGetSelectedTestsTaskConfiguration(unittest.TestCase):
    def test_gets_values(self):
        selected_tests_variant_expansions = {
            "task_name": "my_task", "build_variant": "my-build-variant", "build_id": "my_build_id"
        }

        selected_tests_task_config = under_test._get_selected_tests_task_config(
            selected_tests_variant_expansions)

        self.assertEqual(selected_tests_task_config["name_of_generating_task"], "my_task")
        self.assertEqual(selected_tests_task_config["name_of_generating_build_variant"],
                         "my-build-variant")
        self.assertEqual(selected_tests_task_config["name_of_generating_build_id"], "my_build_id")


class TestGetEvgTaskConfig(unittest.TestCase):
    @patch(ns("_get_selected_tests_task_config"))
    def test_task_is_a_generate_resmoke_task(self, selected_tests_config_mock):
        selected_tests_config_mock.return_value = {"selected_tests_key": "selected_tests_value"}
        build_variant_conf = MagicMock()
        build_variant_conf.name = "variant"
        task = _evergreen.Task({
            "name":
                "auth_gen",
            "commands": [{
                "func": "generate resmoke tasks",
                "vars": {
                    "fallback_num_sub_suites": "4",
                    "resmoke_args": "--storageEngine=wiredTiger",
                },
            }],
        })

        evg_task_config = under_test._get_evg_task_config({}, task, build_variant_conf)

        self.assertEqual(evg_task_config["task_name"], "auth_gen")
        self.assertEqual(evg_task_config["build_variant"], "variant")
        self.assertIsNone(evg_task_config.get("suite"))
        self.assertEqual(
            evg_task_config["resmoke_args"],
            "--storageEngine=wiredTiger",
        )
        self.assertEqual(evg_task_config["fallback_num_sub_suites"], "4")
        self.assertEqual(evg_task_config["selected_tests_key"], "selected_tests_value")

    @patch(ns("_get_selected_tests_task_config"))
    def test_task_is_not_a_generate_resmoke_task(self, selected_tests_config_mock):
        build_variant_conf = MagicMock()
        build_variant_conf.name = "variant"
        task = _evergreen.Task({
            "name":
                "jsCore_auth",
            "commands": [{
                "func": "run tests",
                "vars": {"resmoke_args": "--suites=core_auth --storageEngine=wiredTiger"}
            }],
        })

        evg_task_config = under_test._get_evg_task_config({}, task, build_variant_conf)

        self.assertEqual(evg_task_config["task_name"], "jsCore_auth")
        self.assertEqual(evg_task_config["build_variant"], "variant")
        self.assertEqual(evg_task_config["suite"], "core_auth")
        self.assertEqual(
            evg_task_config["resmoke_args"],
            "--storageEngine=wiredTiger",
        )
        self.assertEqual(evg_task_config["fallback_num_sub_suites"], "1")


class TestUpdateConfigDictWithTask(unittest.TestCase):
    @patch(ns("SelectedTestsConfigOptions"))
    @patch(ns("GenerateSubSuites"))
    def test_no_suites_or_tasks_are_generated(self, generate_subsuites_mock,
                                              selected_tests_config_options_mock):
        generate_subsuites_mock.return_value.generate_suites_config.return_value = {}

        def generate_task_config(shrub_config, suites):
            pass

        generate_subsuites_mock.return_value.generate_task_config.side_effect = generate_task_config

        build_variant = BuildVariant("variant")
        config_dict_of_suites_and_tasks = {}
        under_test._update_config_with_task(
            MagicMock(), build_variant, config_options=MagicMock(),
            config_dict_of_suites_and_tasks=config_dict_of_suites_and_tasks)

        shrub_project = ShrubProject.empty().add_build_variant(build_variant)
        self.assertEqual(config_dict_of_suites_and_tasks, {})
        self.assertEqual(shrub_project.as_dict(), empty_build_variant("variant"))


class TestGetTaskConfigsForTestMappings(unittest.TestCase):
    @patch(ns("_get_evg_task_config"))
    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_test_mapping(self, find_task_mock, exclude_task_mock,
                                         get_evg_task_config_mock):
        find_task_mock.side_effect = [
            _evergreen.Task({"name": "jsCore_auth"}),
            _evergreen.Task({"name": "auth_gen"})
        ]
        exclude_task_mock.return_value = False
        tests_by_task = {
            "jsCore_auth": {
                "tests": [
                    "jstests/core/currentop_waiting_for_latch.js",
                    "jstests/core/latch_analyzer.js",
                ],
            },
            "auth_gen": {"tests": ["jstests/auth/auth3.js"], },
        }
        get_evg_task_config_mock.side_effect = [{"task_config_key": "task_config_value_1"},
                                                {"task_config_key": "task_config_value_2"}]

        task_configs = under_test._get_task_configs_for_test_mappings({}, tests_by_task,
                                                                      MagicMock())

        self.assertEqual(task_configs["jsCore_auth"]["task_config_key"], "task_config_value_1")
        self.assertEqual(
            task_configs["jsCore_auth"]["selected_tests_to_run"],
            {"jstests/core/currentop_waiting_for_latch.js", "jstests/core/latch_analyzer.js"})
        self.assertEqual(task_configs["auth_gen"]["task_config_key"], "task_config_value_2")
        self.assertEqual(task_configs["auth_gen"]["selected_tests_to_run"],
                         {'jstests/auth/auth3.js'})

    @patch(ns("_get_evg_task_config"))
    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_test_mapping_when_task_should_be_excluded(
            self, find_task_mock, exclude_task_mock, get_evg_task_config_mock):
        find_task_mock.return_value = _evergreen.Task({"name": "jsCore_auth"})
        exclude_task_mock.return_value = True
        tests_by_task = {
            "jsCore_auth": {
                "tests": [
                    "jstests/core/currentop_waiting_for_latch.js",
                    "jstests/core/latch_analyzer.js",
                ],
            },
        }
        get_evg_task_config_mock.return_value = {"task_config_key": "task_config_value_1"}

        task_configs = under_test._get_task_configs_for_test_mappings({}, tests_by_task,
                                                                      MagicMock())

        self.assertEqual(task_configs, {})

    @patch(ns("_get_evg_task_config"))
    @patch(ns("_find_task"))
    def test_get_config_for_test_mapping_when_task_does_not_exist(self, find_task_mock,
                                                                  get_evg_task_config_mock):
        find_task_mock.return_value = None
        tests_by_task = {
            "jsCore_auth": {
                "tests": [
                    "jstests/core/currentop_waiting_for_latch.js",
                    "jstests/core/latch_analyzer.js",
                ],
            },
        }
        get_evg_task_config_mock.return_value = {"task_config_key": "task_config_value_1"}

        task_configs = under_test._get_task_configs_for_test_mappings({}, tests_by_task,
                                                                      MagicMock())

        self.assertEqual(task_configs, {})


class TestGetTaskConfigsForTaskMappings(unittest.TestCase):
    @patch(ns("_get_evg_task_config"))
    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_task_mapping(self, find_task_mock, exclude_task_mock,
                                         get_evg_task_config_mock):
        find_task_mock.side_effect = [
            _evergreen.Task({"name": "task_1"}),
            _evergreen.Task({"name": "task_2"})
        ]
        exclude_task_mock.return_value = False
        tasks = ["task_1", "task_2"]
        get_evg_task_config_mock.side_effect = [{"task_config_key": "task_config_value_1"},
                                                {"task_config_key": "task_config_value_2"}]

        task_configs = under_test._get_task_configs_for_task_mappings({}, tasks, MagicMock())

        self.assertEqual(task_configs["task_1"]["task_config_key"], "task_config_value_1")
        self.assertEqual(task_configs["task_2"]["task_config_key"], "task_config_value_2")

    @patch(ns("_get_evg_task_config"))
    @patch(ns("_exclude_task"))
    @patch(ns("_find_task"))
    def test_get_config_for_task_mapping_when_task_should_be_excluded(
            self, find_task_mock, exclude_task_mock, get_evg_task_config_mock):
        find_task_mock.return_value = _evergreen.Task({"name": "task_1"})
        exclude_task_mock.return_value = True
        tasks = ["task_1"]
        get_evg_task_config_mock.return_value = {"task_config_key": "task_config_value_1"}

        task_configs = under_test._get_task_configs_for_task_mappings({}, tasks, MagicMock())

        self.assertEqual(task_configs, {})

    @patch(ns("_get_evg_task_config"))
    @patch(ns("_find_task"))
    def test_get_config_for_task_mapping_when_task_does_not_exist(self, find_task_mock,
                                                                  get_evg_task_config_mock):
        find_task_mock.return_value = None
        tasks = ["task_1"]
        get_evg_task_config_mock.return_value = {"task_config_key": "task_config_value_1"}

        task_configs = under_test._get_task_configs_for_task_mappings({}, tasks, MagicMock())

        self.assertEqual(task_configs, {})


class TestGetTaskConfigs(unittest.TestCase):
    @patch(ns("_find_selected_test_files"))
    @patch(ns("create_task_list_for_tests"))
    @patch(ns("_get_task_configs_for_test_mappings"))
    @patch(ns("_find_selected_tasks"))
    def test_with_related_tests_but_no_related_tasks(
            self, find_selected_tasks_mock, get_task_configs_for_test_mappings_mock,
            create_task_list_for_tests_mock, find_selected_test_files_mock):
        find_selected_test_files_mock.return_value = {"jstests/file-1.js", "jstests/file-3.js"}
        get_task_configs_for_test_mappings_mock.return_value = {
            "task_config_key": "task_config_value_1"
        }
        find_selected_tasks_mock.return_value = set()
        changed_files = {"src/file1.cpp", "src/file2.js"}

        task_configs = under_test._get_task_configs(MagicMock(), MagicMock(), {}, MagicMock(),
                                                    changed_files)

        self.assertEqual(task_configs["task_config_key"], "task_config_value_1")

    @patch(ns("_find_selected_test_files"))
    @patch(ns("create_task_list_for_tests"))
    @patch(ns("_get_task_configs_for_task_mappings"))
    @patch(ns("_find_selected_tasks"))
    def test_with_no_related_tests_but_related_tasks(
            self, find_selected_tasks_mock, get_task_configs_for_task_mappings_mock,
            create_task_list_for_tests_mock, find_selected_test_files_mock):
        find_selected_test_files_mock.return_value = {}
        find_selected_tasks_mock.return_value = {"jsCore_auth", "auth_gen"}
        get_task_configs_for_task_mappings_mock.return_value = {
            "task_config_key": "task_config_value_2"
        }
        changed_files = {"src/file1.cpp", "src/file2.js"}

        task_configs = under_test._get_task_configs(MagicMock(), MagicMock(), {}, MagicMock(),
                                                    changed_files)

        self.assertEqual(task_configs["task_config_key"], "task_config_value_2")

    @patch(ns("_find_selected_test_files"))
    @patch(ns("create_task_list_for_tests"))
    @patch(ns("_get_task_configs_for_test_mappings"))
    @patch(ns("_get_task_configs_for_task_mappings"))
    @patch(ns("_find_selected_tasks"))
    # pylint: disable=too-many-arguments
    def test_task_mapping_configs_will_overwrite_test_mapping_configs(
            self, find_selected_tasks_mock, get_task_configs_for_task_mappings_mock,
            get_task_configs_for_test_mappings_mock, create_task_list_for_tests_mock,
            find_selected_test_files_mock):
        find_selected_test_files_mock.return_value = {"jstests/file-1.js", "jstests/file-3.js"}
        get_task_configs_for_test_mappings_mock.return_value = {
            "task_config_key": "task_config_value_1"
        }
        find_selected_tasks_mock.return_value = {"jsCore_auth", "auth_gen"}
        get_task_configs_for_task_mappings_mock.return_value = {
            "task_config_key": "task_config_value_2"
        }
        changed_files = {"src/file1.cpp", "src/file2.js"}

        task_configs = under_test._get_task_configs(MagicMock(), MagicMock(), {}, MagicMock(),
                                                    changed_files)

        self.assertEqual(task_configs["task_config_key"], "task_config_value_2")


class RemoveRepoPathPrefix(unittest.TestCase):
    def test_file_is_in_enterprise_modules(self):
        filepath = under_test._remove_repo_path_prefix(
            "src/mongo/db/modules/enterprise/src/file1.cpp")

        self.assertEqual(filepath, "src/file1.cpp")

    def test_file_is_not_in_enterprise_modules(self):
        filepath = under_test._remove_repo_path_prefix("other_directory/src/file1.cpp")

        self.assertEqual(filepath, "other_directory/src/file1.cpp")
