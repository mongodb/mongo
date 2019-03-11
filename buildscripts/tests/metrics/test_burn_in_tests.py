"""Unit tests for buildscripts/metrics/burn_in_tests.py."""

from __future__ import absolute_import

import copy
import datetime
import unittest

import requests

from mock import Mock, MagicMock, patch

import buildscripts.metrics.burn_in_tests as burn_in
import buildscripts.client.evergreen as evg_client

# pylint: disable=missing-docstring,protected-access

BURN_IN = "buildscripts.metrics.burn_in_tests"
EVERGREEN = "buildscripts.client.evergreen"

PROJECT_PATCHES = {
    "myproject": [
        {
            "patch_id": "patch1",
            "create_time": "2019-01-01T00:00:00.000Z",
            "status": "failed"
        },
        {
            "patch_id": "patch2",
            "create_time": "2019-03-01T00:00:00.000Z",
            "status": "failed"
        },
        {
            "patch_id": "patch_only_burn_failure",
            "create_time": "2019-04-01T00:00:00.000Z",
            "status": "failed"
        },
        {
            "patch_id": "patch4",
            "create_time": "2019-04-01T00:00:00.000Z",
            "status": "created"
        },
        {
            "patch_id": "patch5",
            "create_time": "2019-04-01T00:00:00.000Z",
            "status": "succeeded"
        }
    ],
    "another_project": [
        {
            "patch_id": "patch1",
            "create_time": "2019-01-01T00:00:00.000Z",
            "status": "failed"
        },
        {
            "patch_id": "patch2",
            "create_time": "2019-03-01T00:00:00.000Z",
            "status": "failed"
        },
        {
            "patch_id": "apatch3",
            "create_time": "2019-04-01T00:00:00.000Z",
            "status": "failed"
        },
        {
            "patch_id": "patch5",
            "create_time": "1900-04-01T00:00:00.000Z",
            "status": "succeeded"
        }
    ]
} # yapf: disable

VERSION_BUILD1 = {
    "_id": "build1",
    "version": "patch1",
    "create_time": "2019-01-01T00:00:00.000Z",
    "status": "failed",
    "status_counts": {"succeeded": 2, "failed": 2},
    "build_variant": "variant1",
    "tasks": ["build1mytask1a", "build1mytask2a", "build1myburn_in_tests_here", "build1mytask3a"]
}  # yapf: disable
VERSION_BUILD2 = {
    "_id": "build2",
    "version": "patch1",
    "create_time": "2019-02-01T00:00:00.000Z",
    "status": "failed",
    "status_counts": {"succeeded": 1, "failed": 2},
    "build_variant": "variant2",
    "tasks": ["build2mytask1b", "build2mytask2b", "build2mytask3b"]
} # yapf: disable
VERSION_BUILD3 = {
    "_id": "build3",
    "version": "patch2",
    "create_time": "2019-03-01T00:00:00.000Z",
    "status": "failed",
    "status_counts": {"succeeded": 3, "failed": 1},
    "build_variant": "variant1",
    "tasks": [
        "build3mytask1c",
        "build3myburn_in_tests_yes",
        "build3myburn_in_tests_yes2",
        "build3mytask3c"
    ]
} # yapf: disable
VERSION_BUILD4 = {
    "_id": "build4",
    "version": "patch2",
    "create_time": "2019-04-01T00:00:00.000Z",
    "status": "failed",
    "status_counts": {"succeeded": 3, "failed": 1},
    "build_variant": "variant3",
    "tasks": ["build4mytask1c", "build4mytask2c", "build4myburn_in_tests_1", "build4mytask3c"],
} # yapf: disable
VERSION_BUILD5 = {
    "_id": "build5",
    "version": "patch_only_burn_failure",
    "create_time": "2019-04-01T00:00:00.000Z",
    "status": "failed",
    "status_counts": {"succeeded": 1, "failed": 0},
    "build_variant": "variant1",
    "tasks": ["build5task"]
} # yapf: disable
VERSION_BUILD6 = {
    "_id": "build6",
    "version": "patch_only_burn_failure",
    "create_time": "2019-04-01T00:00:00.000Z",
    "status": "failed",
    "status_counts": {"succeeded": 2, "failed": 1},
    "build_variant": "variant3",
    "tasks": ["build6task1", "build6task2", "build6anotherburn_in_tests"]
} # yapf: disable
VERSION_BUILD7 = {
    "_id": "build7",
    "version": "patch5",
    "create_time": "2019-04-01T00:00:00.000Z",
    "status": "success",
    "status_counts": {"succeeded": 2, "failed": 0},
    "build_variant": "variant3",
    "tasks": ["build7anotherburn_in_tests", "build7task"]
} # yapf: disable

VERSION_BUILDS = {
    "patch1": [VERSION_BUILD1, VERSION_BUILD2],
    "patch2": [VERSION_BUILD3, VERSION_BUILD4],
    "patch_only_burn_failure": [VERSION_BUILD5, VERSION_BUILD6],
    "patch4": [],
    "patch5": [VERSION_BUILD7]
} # yapf: disable

BUILDS_WITH_BURN_IN = [{"_id": "build1"}, {"_id": "build3"}, {"_id": "build4"}, {"_id": "build5"},
                       {"_id": "build6"}]
BUILDS = BUILDS_WITH_BURN_IN + [{"_id": "build2"}]

BUILD1_TASKS = [
    {"task_id": "build1mytask1a",
     "execution": 0,
     "display_name": "task1",
     "status": "succeeded",
     "time_taken_ms": 100
     },
    {"task_id": "build1mytask2a",
     "execution": 0,
     "display_name": "task2",
     "status": "failed",
     "time_taken_ms": 200
    },
    {"task_id": "build1myburn_in_tests_here",
     "execution": 0,
     "display_name": burn_in.BURN_IN_GENERATED_TASK_PREFIX + "thistask",
     "status": "succeeded",
     "time_taken_ms": 300
    },
    {"task_id": "build1mytask3a",
     "execution": 0,
     "display_name": "task3",
     "status": "failed",
     "time_taken_ms": 100
    }
] # yapf: disable
BUILD2_TASKS = [
    {"task_id": "build2mytask1b",
    "execution": 0,
    "display_name": "task1",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build2mytask2b",
    "execution": 0,
    "display_name": "task2",
     "status": "failed",
     "time_taken_ms": 100
    },
    {"task_id": "build2mytask3b",
    "execution": 0,
    "display_name": "task3",
     "status": "failed",
     "time_taken_ms": 100
    }
] # yapf: disable
BUILD3_TASKS = [
    {"task_id": "build3mytask1c",
     "execution": 0,
     "display_name": "task1",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build3myburn_in_tests_yes",
     "execution": 0,
     "display_name": burn_in.BURN_IN_GENERATED_TASK_PREFIX + "anothertask1",
     "status": "succeeded",
     "time_taken_ms": burn_in.BURN_IN_TIME_MS - 1
    },
    {"task_id": "build3myburn_in_tests_yes2",
     "execution": 0,
     "display_name": burn_in.BURN_IN_GENERATED_TASK_PREFIX + "anothertask2",
     "status": "failed",
     "time_taken_ms": burn_in.BURN_IN_TIME_MS + 1
    },
    {"task_id": "build3mytask3c",
     "execution": 0,
     "display_name": "burn_in_tests_not",
     "status": "succeeded",
     "time_taken_ms": 100
    }
] # yapf: disable
BUILD4_TASKS = [
    {"task_id": "build4mytask1c",
     "execution": 0,
     "display_name": "task1",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build4mytask2c",
     "execution": 0,
     "display_name": "task2",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build4myburn_in_tests_1",
     "execution": 1,
     "display_name": burn_in.BURN_IN_GENERATED_TASK_PREFIX + "anothertask",
     "status": "failed",
     "time_taken_ms": 200
    },
    {"task_id": "build4mytask3c",
     "execution": 0,
     "display_name": "burn_in_tests_not",
     "status": "succeeded",
     "time_taken_ms": 100
    }
] # yapf: disable
BUILD5_TASKS = [
    {"task_id": "build5task",
     "execution": 0,
     "display_name": "task4",
     "status": "succeeded",
     "time_taken_ms": 100
    },
] # yapf: disable
BUILD6_TASKS = [
    {"task_id": "build6task1",
     "execution": 0,
     "display_name": "task5",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build6task2",
     "execution": 0,
     "display_name": "task6",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build6anotherburn_in_tests",
     "execution": 1,
     "display_name": burn_in.BURN_IN_GENERATED_TASK_PREFIX + "anothertask6",
     "status": "failed",
     "time_taken_ms": burn_in.BURN_IN_TIME_MS + 1
    }
] # yapf: disable
BUILD7_TASKS = [
    {"task_id": "build7task",
     "execution": 0,
     "display_name": "task1",
     "status": "succeeded",
     "time_taken_ms": 100
    },
    {"task_id": "build7anotherburn_in_tests",
     "execution": 1,
     "display_name": burn_in.BURN_IN_GENERATED_TASK_PREFIX + "anothertask7",
     "status": "succeeded",
     "time_taken_ms": 200
    }
] # yapf: disable

BUILD_TASKS_WITH_BURN_IN_LIST = BUILD1_TASKS + BUILD3_TASKS + BUILD4_TASKS + BUILD5_TASKS + BUILD6_TASKS + BUILD7_TASKS

BUILD_TASKS_WITH_BURN_IN = [
    {"_id": "build1", "tasks": BUILD1_TASKS},
    {"_id": "build3", "tasks": BUILD3_TASKS},
    {"_id": "build4", "tasks": BUILD4_TASKS},
    {"_id": "build5", "tasks": BUILD5_TASKS},
    {"_id": "build6", "tasks": BUILD6_TASKS},
    {"_id": "build7", "tasks": BUILD7_TASKS},
] # yapf: disable

BUILD_TASKS = BUILD_TASKS_WITH_BURN_IN + [
    {"_id": "build2", "tasks": BUILD2_TASKS}
] # yapf: disable

TASKS_TESTS = [
    {"task_id": "build1myburn_in_tests_here", "execution": 0, "tests": ["test1.js", "test2.js"]},
    {"task_id": "build3myburn_in_tests_yes", "execution": 0, "tests": []},
    {"task_id": "build3myburn_in_tests_yes2", "execution": 0, "tests": ["test1.js"]},
    {"task_id": "build4myburn_in_tests_1", "execution": 1, "tests": ["test1.js", "test2.js"]},
    {"task_id": "build6anotherburn_in_tests", "execution": 3, "tests": ["test1.js"]},
    {"task_id": "build7anotherburn_in_tests", "execution": 7, "tests": ["test1.js"]},
] # yapf: disable


def raise_http_error(code):
    response = requests.Response()
    response.status_code = code
    response.raise_for_status()


class TestParseCommandLine(unittest.TestCase):
    def test_parse_command_line(self):
        options = burn_in.parse_command_line().parse_args([])
        self.assertEqual(options.days, burn_in.DEFAULT_DAYS)
        self.assertEqual(options.project, burn_in.DEFAULT_PROJECT)
        self.assertEqual(options.report_file, burn_in.DEFAULT_REPORT_FILE)
        self.assertIsNone(options.log_level)
        self.assertIsNone(options.evg_client_log_level)

    def test_parse_command_line_partial_args(self):
        days = 5
        project = "myproject"
        arg_list = ["--days", str(days), "--project", project]
        options = burn_in.parse_command_line().parse_args(arg_list)
        self.assertEqual(options.days, days)
        self.assertEqual(options.project, project)
        self.assertEqual(options.report_file, burn_in.DEFAULT_REPORT_FILE)
        self.assertIsNone(options.log_level)
        self.assertIsNone(options.evg_client_log_level)

    def test_parse_command_line_all_args(self):
        days = 5
        project = "myproject"
        report_file = "myreport.json"
        log_level = "INFO"
        evg_client_log_level = "DEBUG"
        arg_list = [
            "--days", str(days),
            "--project", project,
            "--report", report_file,
            "--log", log_level,
            "--evgClientLog", evg_client_log_level
        ] # yapf: disable
        parser_args = burn_in.parse_command_line().parse_args(arg_list)
        self.assertEqual(parser_args.days, days)
        self.assertEqual(parser_args.project, project)
        self.assertEqual(parser_args.report_file, report_file)
        self.assertEqual(parser_args.log_level, log_level)
        self.assertEqual(parser_args.evg_client_log_level, evg_client_log_level)


class TestConfigureLogging(unittest.TestCase):
    def test_configure_logging(self):
        log_level = 15
        evg_client_log_level = 20
        burn_in.configure_logging(log_level, evg_client_log_level)
        self.assertEqual(burn_in.LOGGER.getEffectiveLevel(), log_level)
        self.assertEqual(evg_client.LOGGER.getEffectiveLevel(), evg_client_log_level)

    def test_configure_logging_log_level(self):
        log_level = 15
        evg_client_log_level = evg_client.LOGGER.getEffectiveLevel()
        burn_in.configure_logging(log_level, None)
        self.assertEqual(burn_in.LOGGER.getEffectiveLevel(), log_level)
        self.assertEqual(evg_client.LOGGER.getEffectiveLevel(), evg_client_log_level)

    def test_configure_logging_evg_client_log_level(self):
        log_level = burn_in.LOGGER.getEffectiveLevel()
        evg_client_log_level = 30
        burn_in.configure_logging(None, evg_client_log_level)
        self.assertEqual(burn_in.LOGGER.getEffectiveLevel(), log_level)
        self.assertEqual(evg_client.LOGGER.getEffectiveLevel(), evg_client_log_level)

    def test_configure_logging_default(self):
        log_level = burn_in.LOGGER.getEffectiveLevel()
        evg_client_log_level = evg_client.LOGGER.getEffectiveLevel()
        burn_in.configure_logging(None, None)
        self.assertEqual(burn_in.LOGGER.getEffectiveLevel(), log_level)
        self.assertEqual(evg_client.LOGGER.getEffectiveLevel(), evg_client_log_level)


class TestWriteJsonFile(unittest.TestCase):
    def test_write_json_file(self):
        my_data = {"key1": "val1", "key_list": ["l1", "l2"]}
        path = "myfile"
        with patch("__builtin__.open") as mock_file,\
             patch("json.dump") as mock_json_dump:
            burn_in.write_json_file(my_data, path)
            mock_file.assert_called_once_with("myfile", "w")
            mock_json_dump.assert_called_once()
            self.assertDictEqual(mock_json_dump.call_args_list[0][0][0], my_data)


class TestStrToDatetime(unittest.TestCase):
    def test_str_to_datetime(self):
        date_str = "2019-01-01T10:03:33.190Z"
        self.assertEqual(
            burn_in.str_to_datetime(date_str), datetime.datetime(2019, 1, 1, 10, 3, 33, 190000))
        self.assertNotEqual(burn_in.str_to_datetime(date_str), datetime.datetime(2019, 1, 1, 10))


class TestStrToDatimeDate(unittest.TestCase):
    def test_datetime_date(self):
        date_str = "2019-01-01T10:01:22.857Z"
        self.assertEqual(
            burn_in.str_to_datetime_date(date_str),
            datetime.datetime(2019, 1, 1).date())


class Projects(object):
    def __init__(self, patches):
        self.patches = patches

    def _project_patches_gen(self, project):
        if project not in self.patches:
            raise requests.exceptions.HTTPError
        for project_patch in self.patches[project]:
            yield project_patch


class VersionBuilds(object):
    def __init__(self, version_builds):
        self.version_builds = version_builds

    def _version_builds(self, patch_id):
        if patch_id in self.version_builds:
            return self.version_builds[patch_id]
        return []


class BuildTasks(object):
    def __init__(self, build_tasks):
        self.build_tasks = build_tasks

    def _tasks_by_build_id(self, build_id):
        for build_task in self.build_tasks:
            if build_id == build_task["_id"]:
                return build_task["tasks"]
        return []


class TaskTests(object):
    def __init__(self, tasks_tests, http_error_code=404):
        self.tasks_tests = tasks_tests
        self.http_error_code = http_error_code
        self.http_errors = 0

    def _tests_by_task(self, task_id, execution):
        for task in self.tasks_tests:
            if task["task_id"] == task_id and task["execution"] == execution:
                return task["tests"]
        self.http_errors += 1
        raise_http_error(self.http_error_code)
        return []


class TestGetBurnInBuilds(unittest.TestCase):
    def test_get_burn_in_builds(self):
        projects = Projects(PROJECT_PATCHES)
        version_builds = VersionBuilds(VERSION_BUILDS)
        evg_api = Mock()
        evg_api.project_patches_gen = projects._project_patches_gen
        evg_api.version_builds = version_builds._version_builds
        project = "myproject"
        days = 30000
        burn_in_builds = burn_in.get_burn_in_builds(evg_api, project, days)
        self.assertEqual(5, len(burn_in_builds))

    def test_get_burn_in_builds_partial_patches(self):
        projects = Projects(PROJECT_PATCHES)
        version_builds = VersionBuilds(VERSION_BUILDS)
        evg_api = Mock()
        evg_api.project_patches_gen = projects._project_patches_gen
        evg_api.version_builds = version_builds._version_builds
        project = "another_project"
        days = 30000
        burn_in_builds = burn_in.get_burn_in_builds(evg_api, project, days)
        self.assertEqual(3, len(burn_in_builds))
        days = 300000  # Go further back in time to pull in more patch builds.
        burn_in_builds = burn_in.get_burn_in_builds(evg_api, project, days)
        self.assertEqual(4, len(burn_in_builds))

    def test_get_burn_in_builds_no_patches(self):
        version_builds = VersionBuilds(VERSION_BUILDS)
        evg_api = Mock()
        evg_api.project_patches_gen = lambda _: []
        evg_api.version_builds = version_builds._version_builds
        project = "myproject"
        days = 30000
        burn_in_builds = burn_in.get_burn_in_builds(evg_api, project, days)
        self.assertEqual(0, len(burn_in_builds))

    def test_get_burn_in_builds_no_patches_days(self):
        projects = Projects(PROJECT_PATCHES)
        version_builds = VersionBuilds(VERSION_BUILDS)
        evg_api = Mock()
        evg_api.project_patches_gen = projects._project_patches_gen
        evg_api.version_builds = version_builds._version_builds
        project = "myproject"
        days = 0
        burn_in_builds = burn_in.get_burn_in_builds(evg_api, project, days)
        self.assertEqual(0, len(burn_in_builds))

    def test_get_burn_in_builds_missing_patch(self):
        projects = Projects(PROJECT_PATCHES)
        version_builds = VersionBuilds(VERSION_BUILDS)
        evg_api = Mock()
        evg_api.project_patches_gen = projects._project_patches_gen
        evg_api.version_builds = version_builds._version_builds
        project = "another_project"
        days = 30000
        burn_in_builds = burn_in.get_burn_in_builds(evg_api, project, days)
        self.assertEqual(3, len(burn_in_builds))


class TestIsBurnInDisplayTask(unittest.TestCase):
    def test_is_burn_in_display_task(self):
        self.assertTrue(burn_in.is_burn_in_display_task("burn_in_tests"))
        self.assertTrue(burn_in.is_burn_in_display_task("burn_in:mytask"))
        self.assertFalse(burn_in.is_burn_in_display_task("burn_in_test"))


class TestGetBurnInTasks(unittest.TestCase):
    def test_get_burn_in_tasks(self):
        builds = BUILDS_WITH_BURN_IN
        build_tasks = BuildTasks(BUILD_TASKS_WITH_BURN_IN)
        evg_api = Mock()
        evg_api.tasks_by_build_id = build_tasks._tasks_by_build_id
        burn_in_tasks = burn_in.get_burn_in_tasks(evg_api, builds)
        self.assertEqual(5, len(burn_in_tasks))
        for task in burn_in_tasks:
            self.assertTrue(task["display_name"].startswith(burn_in.BURN_IN_GENERATED_TASK_PREFIX))

    def test_get_burn_in_tasks_no_builds(self):
        build_tasks = BuildTasks(BUILD_TASKS_WITH_BURN_IN)
        evg_api = Mock()
        evg_api.tasks_by_build_id = build_tasks._tasks_by_build_id
        burn_in_tasks = burn_in.get_burn_in_tasks(evg_api, [])
        self.assertEqual(0, len(burn_in_tasks))

    def test_get_burn_in_tasks_missing_build(self):
        builds = BUILDS
        build_tasks = BuildTasks(BUILD_TASKS_WITH_BURN_IN)
        evg_api = Mock()
        evg_api.tasks_by_build_id = build_tasks._tasks_by_build_id
        burn_in_tasks = burn_in.get_burn_in_tasks(evg_api, builds)
        self.assertEqual(5, len(burn_in_tasks))


class TestGetTestsFromTasks(unittest.TestCase):
    def test_get_tests(self):
        test_tasks = TaskTests(TASKS_TESTS)
        evg_api = Mock()
        evg_api.tests_by_task = test_tasks._tests_by_task
        tests = burn_in.get_tests_from_tasks(evg_api, BUILD_TASKS_WITH_BURN_IN_LIST)
        self.assertEqual(5, len(tests))
        for test in tests:
            self.assertIn(test, ["test1.js", "test2.js"])

    def test_get_tests_no_tasks(self):
        test_tasks = TaskTests(TASKS_TESTS)
        evg_api = Mock()
        evg_api.tests_by_task = test_tasks._tests_by_task
        tests = burn_in.get_tests_from_tasks(evg_api, [])
        self.assertEqual(0, len(tests))

    def test_get_tests_missing_task(self):
        test_tasks = TaskTests(TASKS_TESTS)
        evg_api = Mock()
        evg_api.tests_by_task = test_tasks._tests_by_task
        tests = burn_in.get_tests_from_tasks(evg_api, BUILD2_TASKS)
        self.assertEqual(0, len(tests))

    def test_get_tests_uncaught_http_error(self):
        http_error_code = 401
        test_tasks = TaskTests(TASKS_TESTS, http_error_code=http_error_code)
        evg_api = Mock()
        evg_api.tests_by_task = test_tasks._tests_by_task
        with self.assertRaises(requests.exceptions.HTTPError) as err:
            burn_in.get_tests_from_tasks(evg_api, BUILD4_TASKS)
            self.assertEqual(1, test_tasks.http_errors)
            self.assertEqual(http_error_code, err.response.status_code)


class TestReport(unittest.TestCase):
    @staticmethod
    def _get_burn_in_builds():
        return [
            VERSION_BUILD1, VERSION_BUILD3, VERSION_BUILD4, VERSION_BUILD5, VERSION_BUILD6,
            VERSION_BUILD7
        ]

    @staticmethod
    def _get_burn_in_tasks():
        return [
            task for task in BUILD_TASKS_WITH_BURN_IN_LIST
            if task["display_name"].startswith(burn_in.BURN_IN_GENERATED_TASK_PREFIX)
        ]

    def test__init_burn_in_patch_builds(self):
        burn_in_patch_builds = burn_in.Report._init_burn_in_patch_builds(
            [VERSION_BUILD1, VERSION_BUILD3])
        self.assertEqual(2, len(burn_in_patch_builds))
        self.assertIn("patch1", burn_in_patch_builds)
        self.assertEqual(1, len(burn_in_patch_builds["patch1"]["builds"]))
        self.assertDictEqual(VERSION_BUILD1, burn_in_patch_builds["patch1"]["builds"][0])
        self.assertEqual(1, len(burn_in_patch_builds["patch2"]["builds"]))
        self.assertDictEqual(VERSION_BUILD3, burn_in_patch_builds["patch2"]["builds"][0])

    def test__init_burn_in_patch_multiple_builds(self):
        burn_in_patch_builds = burn_in.Report._init_burn_in_patch_builds(
            [VERSION_BUILD3, VERSION_BUILD4])
        self.assertEqual(1, len(burn_in_patch_builds))
        self.assertIn("patch2", burn_in_patch_builds)
        self.assertEqual(2, len(burn_in_patch_builds["patch2"]["builds"]))
        self.assertDictEqual(VERSION_BUILD3, burn_in_patch_builds["patch2"]["builds"][0])
        self.assertDictEqual(VERSION_BUILD4, burn_in_patch_builds["patch2"]["builds"][1])

    def test__init_burn_in_patch_no_builds(self):
        burn_in_patch_builds = burn_in.Report._init_burn_in_patch_builds([])
        self.assertDictEqual({}, burn_in_patch_builds)

    def test__init_burn_in_tasks(self):
        tasks = [
            {"task_id": "task1", "tests": ["a", "b"]},
            {"task_id": "task2", "tests": []},
            {"task_id": "task3", "tests": ["x", "y"]},
        ]
        burn_in_tasks = burn_in.Report._init_burn_in_tasks(tasks)
        self.assertEqual(3, len(burn_in_tasks))
        for task in tasks:
            self.assertIn(task["task_id"], burn_in_tasks)
            self.assertDictEqual(task, burn_in_tasks[task["task_id"]])

    def test__init_burn_in_tasks_no_tasks(self):
        tasks = []
        burn_in_tasks = burn_in.Report._init_burn_in_tasks(tasks)
        self.assertDictEqual({}, burn_in_tasks)

    def test__init_report_fields(self):
        num_patch_builds = 3
        num_burn_in_tasks = 1
        num_tests = 4
        comment = "my comment"
        report = burn_in.Report._init_report_fields(num_patch_builds, num_burn_in_tasks, num_tests,
                                                    comment)
        self.assertEqual(len(burn_in.REPORT_FIELDS) + 1, len(report))
        for field in burn_in.REPORT_FIELDS:
            self.assertIn(field, report)
        for field in burn_in.REPORT_TIME_FIELDS:
            self.assertIsNone(report[field])
        self.assertEqual(0, report["tasks"])
        self.assertEqual(0, report["tasks_succeeded"])
        self.assertEqual(0, report["tasks_failed"])
        self.assertEqual(0, report["tasks_failed_burn_in"])
        self.assertEqual(0, report["tasks_failed_only_burn_in"])
        self.assertEqual(0, report[burn_in.BURN_IN_TASKS_EXCEED])
        self.assertEqual(num_burn_in_tasks, report["burn_in_generated_tasks"])
        self.assertEqual(num_patch_builds, report["patch_builds_with_burn_in_task"])
        self.assertEqual(num_tests, report["burn_in_tests"])
        self.assertIn(burn_in.REPORT_COMMENT_FIELD, report)
        self.assertEqual(comment, report[burn_in.REPORT_COMMENT_FIELD])

    def test__init_report_fields_no_comment(self):
        num_patch_builds = 13
        num_burn_in_tasks = 11
        num_tests = 14
        report = burn_in.Report._init_report_fields(num_patch_builds, num_burn_in_tasks, num_tests)
        self.assertEqual(len(burn_in.REPORT_FIELDS), len(report))
        for field in burn_in.REPORT_FIELDS:
            self.assertIn(field, report)
        for field in burn_in.REPORT_TIME_FIELDS:
            self.assertIsNone(report[field])
        self.assertEqual(0, report["tasks"])
        self.assertEqual(0, report["tasks_succeeded"])
        self.assertEqual(0, report["tasks_failed"])
        self.assertEqual(0, report["tasks_failed_burn_in"])
        self.assertEqual(0, report["tasks_failed_only_burn_in"])
        self.assertEqual(0, report[burn_in.BURN_IN_TASKS_EXCEED])
        self.assertEqual(num_burn_in_tasks, report["burn_in_generated_tasks"])
        self.assertEqual(num_patch_builds, report["patch_builds_with_burn_in_task"])
        self.assertEqual(num_tests, report["burn_in_tests"])
        self.assertNotIn(burn_in.REPORT_COMMENT_FIELD, report)

    def test_generate_report(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        report = burn_in_report.generate_report()
        self.assertEqual(len(burn_in.REPORT_FIELDS), len(report))
        for field in burn_in.REPORT_FIELDS:
            self.assertIn(field, report)
        self.assertEqual(18, report["tasks"])
        self.assertEqual(13, report["tasks_succeeded"])
        self.assertEqual(5, report["tasks_failed"])
        self.assertEqual(3, report["tasks_failed_burn_in"])
        self.assertEqual(2, report["tasks_failed_only_burn_in"])
        self.assertEqual(6, report["burn_in_generated_tasks"])
        self.assertEqual(4, report["patch_builds_with_burn_in_task"])
        self.assertEqual(len(tests), report["burn_in_tests"])
        self.assertEqual(2, report[burn_in.BURN_IN_TASKS_EXCEED])
        self.assertEqual("2019-01-01T00:00:00.000Z", report["report_start_time"])
        self.assertEqual("2019-04-01T00:00:00.000Z", report["report_end_time"])

    def test_generate_report_with_comment(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        comment = "my_comment"
        burn_in_report = burn_in.Report(builds, tasks, tests, comment=comment)
        report = burn_in_report.generate_report()
        self.assertEqual(len(burn_in.REPORT_FIELDS) + 1, len(report))
        for field in burn_in.REPORT_FIELDS:
            self.assertIn(field, report)
        self.assertIn(burn_in.REPORT_COMMENT_FIELD, report)
        self.assertEqual(comment, report[burn_in.REPORT_COMMENT_FIELD])

    def test___update_report_time_no_time(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        burn_in_report.report["report_start_time"] = None
        create_time = "2019-01-01T00:00:00.000Z"
        burn_in_report._update_report_time(create_time)
        self.assertEqual(burn_in_report.report["report_start_time"], create_time)
        self.assertEqual(burn_in_report.report["report_end_time"], create_time)

    def test___update_report_time_no_endtime(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        start_time = "2019-01-01T00:00:00.000Z"
        burn_in_report.report["report_start_time"] = start_time
        create_time = "2019-02-01T00:00:00.000Z"
        burn_in_report._update_report_time(create_time)
        self.assertEqual(burn_in_report.report["report_start_time"], start_time)
        self.assertEqual(burn_in_report.report["report_end_time"], create_time)

    def test___update_report_time_no_update(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        start_time = "2019-01-01T00:00:00.000Z"
        end_time = "2019-03-01T00:00:00.000Z"
        burn_in_report.report["report_start_time"] = start_time
        burn_in_report.report["report_end_time"] = end_time
        create_time = "2019-02-01T00:00:00.000Z"
        burn_in_report._update_report_time(create_time)
        self.assertEqual(burn_in_report.report["report_start_time"], start_time)
        self.assertEqual(burn_in_report.report["report_end_time"], end_time)

    def test___update_report_burn_in(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        burn_in_report._update_report_burn_in(
            burn_in_report.burn_in_patch_builds["patch_only_burn_failure"]["builds"], 1)
        self.assertEqual(burn_in_report.report["tasks_failed_burn_in"], 1)
        self.assertEqual(burn_in_report.report["tasks_failed_only_burn_in"], 1)
        self.assertEqual(burn_in_report.report[burn_in.BURN_IN_TASKS_EXCEED], 1)

    def test___update_report_burn_in_no_task(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        burn_in_report._update_report_burn_in([], 0)
        self.assertEqual(burn_in_report.report["tasks_failed_burn_in"], 0)
        self.assertEqual(burn_in_report.report["tasks_failed_only_burn_in"], 0)
        self.assertEqual(burn_in_report.report[burn_in.BURN_IN_TASKS_EXCEED], 0)

    def test___update_report_status(self):
        builds = self._get_burn_in_builds()
        tasks = self._get_burn_in_tasks()
        tests = ["test1.js", "test2.js", "test3.js", "test4.js", "test5.js"]
        burn_in_report = burn_in.Report(builds, tasks, tests)
        build = {
            "status_counts": {"succeeded": 2, "failed": 2},
            "tasks": ["t1", "t2", "t3", "t4"],
        }
        burn_in_report._update_report_status(build)
        self.assertEqual(burn_in_report.report["tasks"], 4)
        self.assertEqual(burn_in_report.report["tasks_succeeded"], 2)
        self.assertEqual(burn_in_report.report["tasks_failed"], 2)

    def test__is_patch_build_completed(self):
        builds = [{"status": "failed"}, {"status": "success"}]
        self.assertTrue(burn_in.Report._is_patch_build_completed(builds))

    def test__is_patch_build_completed_incomplete(self):
        builds = [{"status": "failed"}, {"status": "started"}]
        self.assertFalse(burn_in.Report._is_patch_build_completed(builds))

    def test__is_patch_build_completed_no_builds(self):
        self.assertTrue(burn_in.Report._is_patch_build_completed([]))


class TestMain(unittest.TestCase):
    def test_main(self):
        options = MagicMock()
        options.log_level = "NOTSET"
        options.evg_client_log_level = "NOTSET"
        options.days = 30000
        options.project = "myproject"
        projects = Projects(PROJECT_PATCHES)
        version_builds = VersionBuilds(VERSION_BUILDS)
        build_tasks = BuildTasks(BUILD_TASKS_WITH_BURN_IN)
        task_tests = TaskTests(TASKS_TESTS)
        with patch("argparse.ArgumentParser.parse_args", return_value=options),\
             patch(EVERGREEN + ".EvergreenApiV2.project_patches_gen", projects._project_patches_gen),\
             patch(EVERGREEN + ".EvergreenApiV2.version_builds", version_builds._version_builds),\
             patch(EVERGREEN + ".EvergreenApiV2.tasks_by_build_id", build_tasks._tasks_by_build_id),\
             patch(EVERGREEN + ".EvergreenApiV2.tests_by_task", task_tests._tests_by_task),\
             patch(BURN_IN + ".write_json_file") as mock_write_json:
            burn_in.main()
            report = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(burn_in.REPORT_FIELDS) + 1, len(report))
            for field in burn_in.REPORT_FIELDS:
                self.assertIn(field, report)
            self.assertEqual(17, report["tasks"])
            self.assertEqual(12, report["tasks_succeeded"])
            self.assertEqual(5, report["tasks_failed"])
            self.assertEqual(3, report["tasks_failed_burn_in"])
            self.assertEqual(2, report["tasks_failed_only_burn_in"])
            self.assertEqual(6, report["burn_in_generated_tasks"])
            self.assertEqual(4, report["patch_builds_with_burn_in_task"])
            self.assertEqual(5, report["burn_in_tests"])
            self.assertEqual(2, report[burn_in.BURN_IN_TASKS_EXCEED])
            self.assertEqual("2019-01-01T00:00:00.000Z", report["report_start_time"])
            self.assertEqual("2019-04-01T00:00:00.000Z", report["report_end_time"])

    def test_main_nodata(self):
        options = MagicMock()
        options.log_level = "NOTSET"
        options.evg_client_log_level = "NOTSET"
        options.days = 30000
        options.project = "myproject"
        projects = Projects(PROJECT_PATCHES)
        version_builds = VersionBuilds([])
        build_tasks = BuildTasks([])
        task_tests = TaskTests([])
        with patch("argparse.ArgumentParser.parse_args", return_value=options),\
             patch(EVERGREEN + ".EvergreenApiV2.project_patches_gen", projects._project_patches_gen),\
             patch(EVERGREEN + ".EvergreenApiV2.version_builds", version_builds._version_builds),\
             patch(EVERGREEN + ".EvergreenApiV2.tasks_by_build_id", build_tasks._tasks_by_build_id),\
             patch(EVERGREEN + ".EvergreenApiV2.tests_by_task", task_tests._tests_by_task),\
             patch(BURN_IN + ".write_json_file") as mock_write_json:
            burn_in.main()
            report = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(burn_in.REPORT_FIELDS) + 1, len(report))
            for field in burn_in.REPORT_FIELDS:
                self.assertIn(field, report)
            self.assertEqual(0, report["tasks"])
            self.assertEqual(0, report["tasks_succeeded"])
            self.assertEqual(0, report["tasks_failed"])
            self.assertEqual(0, report["tasks_failed_burn_in"])
            self.assertEqual(0, report["tasks_failed_only_burn_in"])
            self.assertEqual(0, report["burn_in_generated_tasks"])
            self.assertEqual(0, report["patch_builds_with_burn_in_task"])
            self.assertEqual(0, report["burn_in_tests"])
            self.assertEqual(0, report[burn_in.BURN_IN_TASKS_EXCEED])
            self.assertIsNone(report["report_start_time"])
            self.assertIsNone(report["report_end_time"])
