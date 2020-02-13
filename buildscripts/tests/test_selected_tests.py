"""Unit tests for the selected_tests script."""
import os
import unittest

from mock import MagicMock, patch
from shrub.config import Configuration

# pylint: disable=wrong-import-position
import buildscripts.ciconfig.evergreen as _evergreen
from buildscripts import selected_tests as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,protected-access

NS = "buildscripts.selected_tests"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def tests_by_task_stub():
    return {
        "jsCore_auth": {
            "tests": [
                "jstests/core/currentop_waiting_for_latch.js",
                "jstests/core/latch_analyzer.js",
            ],
            "use_multiversion": None,
            "distro": "rhel62-small",
        },
        "auth_gen": {
            "tests": ["jstests/auth/auth3.js"],
            "use_multiversion": None,
            "distro": "rhel62-small",
        },
    }


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

    @patch(ns("read_config"))
    def test_generate_display_task(self, read_config_mock):
        config_options = under_test.SelectedTestsConfigOptions(
            {"task_name": "my_task", "build_variant": "my_variant"}, {}, {}, {})

        display_task = config_options.generate_display_task(["task_1", "task_2"])

        self.assertEqual("my_task_my_variant", display_task._name)
        self.assertIn("task_1", display_task.to_map()["execution_tasks"])
        self.assertIn("task_2", display_task.to_map()["execution_tasks"])


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
        build_variant_conf = MagicMock()
        build_variant_conf.get_task.return_value = _evergreen.Task({"name": "my_task_1"})

        related_tasks = under_test._find_selected_tasks(selected_tests_service_mock, changed_files,
                                                        build_variant_conf)

        self.assertEqual(related_tasks, {"my_task_1"})

    @patch(ns("SelectedTestsService"))
    def test_returned_tasks_do_not_exist(self, selected_tests_service_mock):
        selected_tests_service_mock.get_task_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "tasks": [{"name": "my_task_1"}],
            },
        ]
        changed_files = {"src/file1.cpp", "src/file2.js"}
        build_variant_conf = MagicMock()
        build_variant_conf.get_task.return_value = None

        related_tasks = under_test._find_selected_tasks(selected_tests_service_mock, changed_files,
                                                        build_variant_conf)

        self.assertEqual(related_tasks, set())

    @patch(ns("SelectedTestsService"))
    def test_returned_tasks_should_be_excluded(self, selected_tests_service_mock):
        excluded_task = under_test.EXCLUDE_TASK_LIST[0]
        selected_tests_service_mock.get_task_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "tasks": [{"name": excluded_task}],
            },
        ]
        changed_files = {"src/file1.cpp", "src/file2.js"}
        build_variant_conf = MagicMock()
        build_variant_conf.get_task.return_value = _evergreen.Task({"name": excluded_task})

        related_tasks = under_test._find_selected_tasks(selected_tests_service_mock, changed_files,
                                                        build_variant_conf)

        self.assertEqual(related_tasks, set())

    @patch(ns("SelectedTestsService"))
    def test_returned_tasks_match_excluded_pattern(self, selected_tests_service_mock):
        task_that_matches_exclude_pattern = "compile_all"
        selected_tests_service_mock.get_task_mappings.return_value = [
            {
                "source_file": "src/file1.cpp",
                "tasks": [{"name": task_that_matches_exclude_pattern}],
            },
        ]
        changed_files = {"src/file1.cpp", "src/file2.js"}
        build_variant_conf = MagicMock()
        build_variant_conf.get_task.return_value = _evergreen.Task(
            {"name": task_that_matches_exclude_pattern})

        related_tasks = under_test._find_selected_tasks(selected_tests_service_mock, changed_files,
                                                        build_variant_conf)

        self.assertEqual(related_tasks, set())


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
        task_name = "auth_gen"
        build_variant_conf = MagicMock()
        build_variant_conf.name = "variant"
        build_variant_conf.get_task.return_value = _evergreen.Task({
            "name":
                task_name,
            "commands": [{
                "func": "generate resmoke tasks",
                "vars": {
                    "fallback_num_sub_suites": "4",
                    "resmoke_args": "--storageEngine=wiredTiger",
                },
            }],
        })

        evg_task_config = under_test._get_evg_task_config({}, task_name, build_variant_conf)

        self.assertEqual(evg_task_config["task_name"], task_name)
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
        task_name = "jsCore_auth"
        build_variant_conf = MagicMock()
        build_variant_conf.name = "variant"
        build_variant_conf.get_task.return_value = _evergreen.Task({
            "name":
                task_name,
            "commands": [{
                "func": "run tests",
                "vars": {"resmoke_args": "--suites=core_auth --storageEngine=wiredTiger"}
            }],
        })

        evg_task_config = under_test._get_evg_task_config({}, task_name, build_variant_conf)

        self.assertEqual(evg_task_config["task_name"], task_name)
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
    def test_suites_and_tasks_are_generated(self, generate_subsuites_mock,
                                            selected_tests_config_options_mock):
        suites_config_mock = {"my_suite_0.yml": "suite file contents"}
        generate_subsuites_mock.return_value.generate_suites_config.return_value = suites_config_mock

        def generate_task_config(shrub_config, suites):
            shrub_config.task("my_fake_task")

        generate_subsuites_mock.return_value.generate_task_config.side_effect = generate_task_config

        shrub_config = Configuration()
        config_dict_of_suites_and_tasks = {}
        under_test._update_config_with_task(
            evg_api=MagicMock(), shrub_config=shrub_config, config_options=MagicMock(),
            config_dict_of_suites_and_tasks=config_dict_of_suites_and_tasks)

        self.assertEqual(config_dict_of_suites_and_tasks, suites_config_mock)
        self.assertIn("my_fake_task", shrub_config.to_json())

    @patch(ns("SelectedTestsConfigOptions"))
    @patch(ns("GenerateSubSuites"))
    def test_no_suites_or_tasks_are_generated(self, generate_subsuites_mock,
                                              selected_tests_config_options_mock):
        generate_subsuites_mock.return_value.generate_suites_config.return_value = {}

        def generate_task_config(shrub_config, suites):
            pass

        generate_subsuites_mock.return_value.generate_task_config.side_effect = generate_task_config

        shrub_config = Configuration()
        config_dict_of_suites_and_tasks = {}
        under_test._update_config_with_task(
            evg_api=MagicMock(), shrub_config=shrub_config, config_options=MagicMock(),
            config_dict_of_suites_and_tasks=config_dict_of_suites_and_tasks)

        self.assertEqual(config_dict_of_suites_and_tasks, {})
        self.assertEqual(shrub_config.to_json(), "{}")


class TestGetTaskConfigsForTestMappings(unittest.TestCase):
    @patch(ns("_get_evg_task_config"))
    def test_get_config_for_test_mapping(self, get_evg_task_config_mock):
        tests_by_task = tests_by_task_stub()
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


class TestGetTaskConfigsForTaskMappings(unittest.TestCase):
    @patch(ns("_get_evg_task_config"))
    def test_get_config_for_task_mapping(self, get_evg_task_config_mock):
        tasks = ["task_1", "task_2"]
        get_evg_task_config_mock.side_effect = [{"task_config_key": "task_config_value_1"},
                                                {"task_config_key": "task_config_value_2"}]
        task_configs = under_test._get_task_configs_for_task_mappings({}, tasks, MagicMock())

        self.assertEqual(task_configs["task_1"]["task_config_key"], "task_config_value_1")
        self.assertEqual(task_configs["task_2"]["task_config_key"], "task_config_value_2")


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


class TestRun(unittest.TestCase):
    @patch(ns("SelectedTestsConfigOptions"))
    @patch(ns("_get_task_configs"))
    @patch(ns("_update_config_with_task"))
    def test_run(self, update_config_with_task_mock, get_task_configs_mock,
                 selected_tests_config_options):
        get_task_configs_mock.return_value = {"task_config_key": "task_config_value_1"}

        def update_config_with_task(evg_api, shrub_config, config_options,
                                    config_dict_of_suites_and_tasks):
            config_dict_of_suites_and_tasks["new_config_key"] = "new_config_values"
            shrub_config.task("my_fake_task")

        update_config_with_task_mock.side_effect = update_config_with_task
        changed_files = {"src/file1.cpp", "src/file2.js"}

        config_dict_of_suites_and_tasks = under_test.run(MagicMock(), MagicMock(), MagicMock(), {},
                                                         changed_files, ["variant_1", "variant_2"])

        self.assertEqual(config_dict_of_suites_and_tasks["new_config_key"], "new_config_values")
        self.assertIn("my_fake_task", config_dict_of_suites_and_tasks["selected_tests_config.json"])
