"""Unit tests for the selected_tests script."""
import os
import unittest

from mock import MagicMock, patch

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
    @patch(ns("read_config"))
    def test_overwrites_overwrite_filepath_config(self, read_config_mock):
        filepath = MagicMock()
        read_config_mock.read_config_file.return_value = {"key1": 1}
        overwrites = {"key1": 2}
        required_keys = {"key1"}
        defaults = {}
        formats = {"key1": int}

        config_options = under_test.SelectedTestsConfigOptions.from_file(
            filepath, overwrites, required_keys, defaults, formats)

        self.assertEqual(overwrites["key1"], config_options.key1)

    @patch(ns("read_config"))
    def test_overwrites_overwrite_defaults(self, read_config_mock):
        filepath = MagicMock()
        read_config_mock.read_config_file.return_value = {"key1": 1}
        overwrites = {"key1": 2}
        required_keys = {"key1"}
        defaults = {"key1": 3}
        formats = {"key1": int}

        config_options = under_test.SelectedTestsConfigOptions.from_file(
            filepath, overwrites, required_keys, defaults, formats)

        self.assertEqual(overwrites["key1"], config_options.key1)

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

    @patch(ns("read_config"))
    def test_generate_display_task(self, read_config_mock):
        config_options = under_test.SelectedTestsConfigOptions(
            {"task_name": "my_task", "build_variant": "my_variant"}, {}, {}, {})

        display_task = config_options.generate_display_task(["task_1", "task_2"])

        self.assertEqual("my_task_my_variant", display_task._name)
        self.assertIn("task_1", display_task.to_map()["execution_tasks"])
        self.assertIn("task_2", display_task.to_map()["execution_tasks"])


class TestFindRelatedTestFiles(unittest.TestCase):
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

        related_test_files = under_test._find_related_test_files(selected_tests_service_mock,
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

        related_test_files = under_test._find_related_test_files(selected_tests_service_mock,
                                                                 changed_files)

        self.assertEqual(related_test_files, set())

    @patch(ns("SelectedTestsService"))
    def test_no_related_files_returned(self, selected_tests_service_mock):
        selected_tests_service_mock.get_test_mappings.return_value = set()
        changed_files = {"src/file1.cpp", "src/file2.js"}

        related_test_files = under_test._find_related_test_files(selected_tests_service_mock,
                                                                 changed_files)

        self.assertEqual(related_test_files, set())


class TestGetSelectedTestsTaskConfiguration(unittest.TestCase):
    @patch(ns("read_config"))
    def test_gets_values(self, read_config_mock):
        filepath = MagicMock()
        read_config_mock.read_config_file.return_value = {
            "task_name": "my_task", "build_variant": "my-build-variant", "build_id": "my_build_id"
        }

        selected_tests_task_config = under_test._get_selected_tests_task_configuration(filepath)

        self.assertEqual(selected_tests_task_config["name_of_generating_task"], "my_task")
        self.assertEqual(selected_tests_task_config["name_of_generating_build_variant"],
                         "my-build-variant")
        self.assertEqual(selected_tests_task_config["name_of_generating_build_id"], "my_build_id")


class TestGetEvgTaskConfiguration(unittest.TestCase):
    def test_task_is_a_generate_resmoke_task(self):
        task_name = "auth_gen"
        task = _evergreen.Task({
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
        burn_in_task_config = tests_by_task_stub()[task_name]
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value.get_task.return_value = task

        evg_task_config = under_test._get_evg_task_configuration(evg_conf_mock, "variant",
                                                                 task_name, burn_in_task_config)

        self.assertEqual(evg_task_config["task_name"], task_name)
        self.assertEqual(evg_task_config["build_variant"], "variant")
        self.assertEqual(evg_task_config["selected_tests_to_run"], {"jstests/auth/auth3.js"})
        self.assertIsNone(evg_task_config.get("suite"))
        self.assertEqual(
            evg_task_config["resmoke_args"],
            "--storageEngine=wiredTiger",
        )
        self.assertEqual(evg_task_config["fallback_num_sub_suites"], "4")

    def test_task_is_not_a_generate_resmoke_task(self):
        task_name = "jsCore_auth"
        task = _evergreen.Task({
            "name":
                task_name,
            "commands": [{
                "func": "run tests",
                "vars": {"resmoke_args": "--suites=core_auth --storageEngine=wiredTiger"}
            }],
        })
        burn_in_task_config = tests_by_task_stub()[task_name]
        evg_conf_mock = MagicMock()
        evg_conf_mock.get_variant.return_value.get_task.return_value = task

        evg_task_config = under_test._get_evg_task_configuration(evg_conf_mock, "variant",
                                                                 task_name, burn_in_task_config)

        self.assertEqual(evg_task_config["task_name"], task_name)
        self.assertEqual(evg_task_config["build_variant"], "variant")
        self.assertEqual(
            evg_task_config["selected_tests_to_run"],
            {"jstests/core/currentop_waiting_for_latch.js", "jstests/core/latch_analyzer.js"})
        self.assertEqual(evg_task_config["suite"], "core_auth")
        self.assertEqual(
            evg_task_config["resmoke_args"],
            "--storageEngine=wiredTiger",
        )
        self.assertEqual(evg_task_config["fallback_num_sub_suites"], "1")


class TestGenerateShrubConfig(unittest.TestCase):
    @patch(ns("_get_selected_tests_task_configuration"))
    @patch(ns("_get_evg_task_configuration"))
    @patch(ns("SelectedTestsConfigOptions"))
    @patch(ns("GenerateSubSuites"))
    def test_when_test_by_task_returned(
            self, generate_subsuites_mock, selected_tests_config_options_mock,
            get_evg_task_configuration_mock, get_selected_tests_task_configuration_mock):
        evg_api = MagicMock()
        evg_conf = MagicMock()
        expansion_file = MagicMock()
        tests_by_task = tests_by_task_stub()
        yml_suite_file_contents = MagicMock()
        shrub_json_file_contents = MagicMock()
        suite_file_dict_mock = {"auth_0.yml": yml_suite_file_contents}
        generate_subsuites_mock.return_value.generate_task_config_and_suites.return_value = (
            suite_file_dict_mock,
            shrub_json_file_contents,
        )

        config_file_dict = under_test._generate_shrub_config(evg_api, evg_conf, expansion_file,
                                                             tests_by_task, "variant")
        self.assertEqual(
            config_file_dict,
            {
                "auth_0.yml": yml_suite_file_contents,
                "selected_tests_config.json": shrub_json_file_contents,
            },
        )

    @patch(ns("_get_selected_tests_task_configuration"))
    @patch(ns("_get_evg_task_configuration"))
    @patch(ns("SelectedTestsConfigOptions"))
    @patch(ns("GenerateSubSuites"))
    def test_when_no_test_by_task_returned(
            self, generate_subsuites_mock, selected_tests_config_options_mock,
            get_evg_task_configuration_mock, get_selected_tests_task_configuration_mock):
        evg_api = MagicMock()
        evg_conf = MagicMock()
        expansion_file = MagicMock()
        tests_by_task = {}
        yml_suite_file_contents = MagicMock()
        shrub_json_file_contents = MagicMock()
        suite_file_dict_mock = {"auth_0.yml": yml_suite_file_contents}
        generate_subsuites_mock.return_value.generate_task_config_and_suites.return_value = (
            suite_file_dict_mock,
            shrub_json_file_contents,
        )

        config_file_dict = under_test._generate_shrub_config(evg_api, evg_conf, expansion_file,
                                                             tests_by_task, "variant")
        self.assertEqual(config_file_dict, {})
