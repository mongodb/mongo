"""Unit tests for buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import collections
import os
import subprocess
import unittest

from mock import Mock, mock_open, patch

import buildscripts.burn_in_tests as burn_in

# pylint: disable=missing-docstring,protected-access

BURN_IN = "buildscripts.burn_in_tests"
EVG_CI = "buildscripts.ciconfig.evergreen"
EVG_CLIENT = "buildscripts.client.evergreen"
GIT = "buildscripts.git"
RESMOKELIB = "buildscripts.resmokelib"


class ValidateOptions(unittest.TestCase):
    class ParserError(Exception):
        pass

    @staticmethod
    def _raise_parser_error(err):
        raise ValidateOptions.ParserError(err)

    @staticmethod
    def _mock_parser():
        parser = Mock()
        parser.error = ValidateOptions._raise_parser_error
        return parser

    @staticmethod
    def _mock_values():
        values = Mock()
        values.repeat_tests_num = None
        values.repeat_tests_max = None
        values.repeat_tests_min = None
        values.repeat_tests_secs = None
        values.buildvariant = None
        values.test_list_file = None
        return values

    def test_validate_options_listfile_buildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.test_list_file = "list_file.json"
        values.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            burn_in.validate_options(parser, values)

    def test_validate_options_nolistfile_buildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            burn_in.validate_options(parser, values)

    def test_validate_options_listfile_nobuildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.test_list_file = "list_file.json"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            burn_in.validate_options(parser, values)

    def test_validate_options_no_listfile_no_buildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)

    def test_validate_options_buildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            burn_in.validate_options(parser, values)

    def test_validate_options_bad_buildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.buildvariant = "badvariant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)

    def test_validate_options_tests_max_no_tests_secs(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.repeat_tests_max = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)

    def test_validate_options_tests_min_no_tests_secs(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.repeat_tests_min = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)

    def test_validate_options_tests_min_gt_tests_max(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.repeat_tests_min = 3
        values.repeat_tests_max = 2
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)

    def test_validate_options_tests_secs(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.buildvariant = "variant1"
        values.repeat_tests_min = 1
        values.repeat_tests_max = 3
        values.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            burn_in.validate_options(parser, values)

    def test_validate_options_tests_secs_and_tests_num(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.buildvariant = "variant1"
        values.repeat_tests_num = 1
        values.repeat_tests_min = 1
        values.repeat_tests_max = 3
        values.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)

    def test_validate_options_tests_secs_no_buildvariant(self):
        parser = self._mock_parser()
        values = self._mock_values()
        values.repeat_tests_min = 1
        values.repeat_tests_max = 3
        values.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=CreateTaskList.evergreen_conf):
            with self.assertRaises(self.ParserError):
                burn_in.validate_options(parser, values)


class UpdateReportDataTests(unittest.TestCase):
    def test_update_report_data_nofile(self):
        data = {}
        task = ""
        pathname = "file_exists"
        with patch("os.path.isfile", return_value=False) as mock_isfile,\
             patch("json.load", return_value=data) as mock_json:
            burn_in._update_report_data(data, pathname, task)
            self.assertEqual(mock_isfile.call_count, 1)
            self.assertEqual(mock_json.call_count, 0)

    def test_update_report_data(self):
        task1 = "task1"
        task2 = "task2"
        data = {
            "failures": 1,
            "results": [
                {"test_file": "test1:" + task1},
                {"test_file": "test2:" + task1}]
        } # yapf: disable
        new_data = {
            "failures": 1,
            "results": [
                {"test_file": "test3"},
                {"test_file": "test4"}]
        } # yapf: disable

        pathname = "file_exists"
        with patch("os.path.isfile", return_value=True),\
             patch("__builtin__.open", mock_open()),\
             patch("json.load", return_value=new_data):
            burn_in._update_report_data(data, pathname, task2)
            self.assertEqual(len(data["results"]), 4)
            self.assertEqual(data["failures"], 2)
            self.assertIn({"test_file": "test1:" + task1}, data["results"])
            self.assertIn({"test_file": "test3:" + task2}, data["results"])


class RunTests(unittest.TestCase):
    class SysExit(Exception):
        pass

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
            "resmoke_args": "--suites=suite4", "tests": []}
    } # yapf: disable

    def _test_run_tests(self, no_exec, tests_by_task, resmoke_cmd):
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch(BURN_IN + "._update_report_data", return_value=None),\
             patch(BURN_IN + "._write_json_file", return_value=None):
            burn_in.run_tests(no_exec, tests_by_task, resmoke_cmd, None)
            self.assertEqual(mock_subproc.call_count, len(tests_by_task.keys()))
            for idx, task in enumerate(sorted(tests_by_task)):
                for task_test in tests_by_task[task].get("tests", []):
                    self.assertIn(task_test, mock_subproc.call_args_list[idx][0][0])

    def test_run_tests_noexec(self):
        no_exec = True
        resmoke_cmd = None
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch(BURN_IN + "._write_json_file", return_value=None) as mock_write_json:
            burn_in.run_tests(no_exec, self.TESTS_BY_TASK, resmoke_cmd, None)
            self.assertEqual(mock_subproc.call_count, 0)
            self.assertEqual(mock_write_json.call_count, 0)

    def test_run_tests_notests(self):
        no_exec = False
        tests_by_task = {}
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        self._test_run_tests(no_exec, tests_by_task, resmoke_cmd)

    def test_run_tests_tests(self):
        no_exec = False
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        self._test_run_tests(no_exec, self.TESTS_BY_TASK, resmoke_cmd)

    def test_run_tests_tests_resmoke_failure(self):
        no_exec = False
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        error_code = -1
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch("sys.exit", return_value=error_code) as mock_exit,\
             patch(BURN_IN + "._update_report_data", return_value=None),\
             patch(BURN_IN + "._write_json_file", return_value=None):
            mock_subproc.side_effect = subprocess.CalledProcessError(error_code, "err1")
            mock_exit.side_effect = self.SysExit(error_code)
            with self.assertRaises(self.SysExit):
                burn_in.run_tests(no_exec, self.TESTS_BY_TASK, resmoke_cmd, None)


class FindLastActivated(unittest.TestCase):

    REVISION_BUILDS = {
        "rev1": {
            "not_mongodb_mongo_master_variant1_build1": {"activated": False},
            "mongodb_mongo_unmaster_variant_build1": {"activated": True},
            "mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_master_variant2_build1": {"activated": False},
            "mongodb_mongo_master_variant3_build1": {"activated": False}
        },
        "rev2": {
            "not_mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_unmaster_variant_build1": {"activated": True},
            "mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_master_variant2_build1": {"activated": False}
        },
        "rev3": {
            "not_mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_unmaster_variant_build1": {"activated": True},
            "mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_master_variant2_build1": {"activated": False},
        },
        "rev4": {
            "not_mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_unmaster_variant_build1": {"activated": True},
            "mongodb_mongo_master_variant1_build1": {"activated": True},
            "mongodb_mongo_master_variant2_build1": {"activated": False},
            "mongodb_mongo_master_variant3_build1": {"activated": True}
        },
    }

    @staticmethod
    def builds_url(build):
        """Return build URL."""
        return "{}{}builds/{}".format(burn_in.API_SERVER_DEFAULT, burn_in.API_REST_PREFIX, build)

    @staticmethod
    def revisions_url(project, revision):
        """Return revisions URL."""
        return "{}{}projects/{}/revisions/{}".format(burn_in.API_SERVER_DEFAULT,
                                                     burn_in.API_REST_PREFIX, project, revision)

    @staticmethod
    def load_urls(request, project, revision_builds):
        """Store request in URLs to support REST APIs."""

        for revision in revision_builds:
            builds = revision_builds[revision]
            # The 'revisions' endpoint contains the list of builds.
            url = FindLastActivated.revisions_url(project, revision)
            build_list = []
            for build in builds:
                build_list.append("{}_{}".format(build, revision))
            build_data = {"builds": build_list}
            request.put(url, None, build_data)

            for build in builds:
                # The 'builds' endpoint contains the activated & revision field.
                url = FindLastActivated.builds_url("{}_{}".format(build, revision))
                build_data = builds[build]
                build_data["revision"] = revision
                request.put(url, None, build_data)

    def _test_find_last_activated_task(self, branch, variant, revision,
                                       revisions=REVISION_BUILDS.keys()):
        with patch(BURN_IN + ".requests", MockRequests()),\
             patch(EVG_CLIENT + ".read_evg_config", return_value=None):
            self.load_urls(burn_in.requests, "mongodb-mongo-master", self.REVISION_BUILDS)
            last_revision = burn_in.find_last_activated_task(revisions, variant, branch)
        self.assertEqual(last_revision, revision)

    def test_find_last_activated_task_first_rev(self):
        self._test_find_last_activated_task("master", "variant1", "rev1")

    def test_find_last_activated_task_last_rev(self):
        self._test_find_last_activated_task("master", "variant3", "rev4")

    def test_find_last_activated_task_no_rev(self):
        self._test_find_last_activated_task("master", "variant2", None)

    def test_find_last_activated_task_no_variant(self):
        self._test_find_last_activated_task("master", "novariant", None)

    def test_find_last_activated_task_no_branch(self):
        with self.assertRaises(AttributeError):
            self._test_find_last_activated_task("nobranch", "variant2", None)

    def test_find_last_activated_norevisions(self):
        self._test_find_last_activated_task("master", "novariant", None, [])


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
    with patch(RESMOKELIB + ".suitesconfig.create_test_membership_map", return_value=MEMBERS_MAP):
        return burn_in.create_executor_list(suites, exclude_suites)


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


def tasks_mock(tasks):
    task_list = Mock()
    task_list.tasks = []
    for idx, task in enumerate(tasks):
        task_list.tasks.append(Mock())
        task_list.tasks[idx].name = task["name"]
        if "combined_resmoke_args" in task:
            resmoke_args = task["combined_resmoke_args"]
        else:
            resmoke_args = None
        task_list.tasks[idx].combined_resmoke_args = resmoke_args
    return task_list


class CreateTaskList(unittest.TestCase):

    VARIANTS = {
        "variantall":
            tasks_mock([{"name": "task1", "combined_resmoke_args": "--suites=suite1 var1arg1"},
                        {"name": "task2", "combined_resmoke_args": "--suites=suite1 var1arg2"},
                        {"name": "task3", "combined_resmoke_args": "--suites=suite1 var1arg3"}]),
        "variant1":
            tasks_mock([{"name": "task1", "combined_resmoke_args": "--suites=suite1 var1arg1"},
                        {"name": "task2"}]),
        "variant2":
            tasks_mock([{"name": "task2", "combined_resmoke_args": "var2arg1"},
                        {"name": "task3", "combined_resmoke_args": "--suites=suite3 var2arg3"}]),
        "variant3":
            tasks_mock([{"name": "task2", "combined_resmoke_args": "var3arg1"}]),
        "variant4":
            tasks_mock([]),
    }

    evergreen_conf = Mock()
    evergreen_conf.get_variant = VARIANTS.get
    evergreen_conf.variant_names = VARIANTS.keys()

    def test_create_task_list(self):
        variant = "variantall"
        suites = [SUITE1, SUITE2, SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(self.evergreen_conf, variant, suite_list,
                                             exclude_suites)
        self.assertIn("task1", task_list)
        self.assertEqual(task_list["task1"]["resmoke_args"], "--suites=suite1 var1arg1")
        self.assertEqual(task_list["task1"]["tests"], SUITE1.tests)
        self.assertIn("task2", task_list)
        self.assertEqual(task_list["task2"]["resmoke_args"], "--suites=suite1 var1arg2")
        self.assertEqual(task_list["task2"]["tests"], SUITE1.tests)
        self.assertIn("task3", task_list)
        self.assertEqual(task_list["task3"]["resmoke_args"], "--suites=suite1 var1arg3")
        self.assertEqual(task_list["task3"]["tests"], SUITE1.tests)

    def test_create_task_list_no_excludes(self):
        variant = "variant1"
        suites = [SUITE1, SUITE2]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(self.evergreen_conf, variant, suite_list,
                                             exclude_suites)
        self.assertIn("task1", task_list)
        self.assertEqual(task_list["task1"]["resmoke_args"], "--suites=suite1 var1arg1")
        self.assertEqual(task_list["task1"]["tests"], SUITE1.tests)
        self.assertNotIn("task2", task_list)
        self.assertNotIn("task3", task_list)

    def test_create_task_list_with_excludes(self):
        variant = "variant2"
        suites = [SUITE1, SUITE2, SUITE3]
        suite_list = _create_executor_list(suites, [])
        exclude_suites = ["suite2"]
        task_list = burn_in.create_task_list(self.evergreen_conf, variant, suite_list,
                                             exclude_suites)
        self.assertIn("task3", task_list)
        self.assertEqual(task_list["task3"]["resmoke_args"], "--suites=suite3 var2arg3")
        self.assertEqual(task_list["task3"]["tests"], SUITE3.tests)
        self.assertNotIn("task1", task_list)
        self.assertNotIn("task2", task_list)

    def test_create_task_list_no_suites(self):
        variant = "variant2"
        suite_list = {}
        exclude_suites = ["suite2"]
        task_list = burn_in.create_task_list(self.evergreen_conf, variant, suite_list,
                                             exclude_suites)
        self.assertEqual(task_list, {})

    def test_create_task_list_novariant(self):
        class BadVariant(Exception):
            pass

        def _raise_bad_variant(code=0):
            raise BadVariant("Bad variant {}".format(code))

        variant = "novariant"
        suites = [SUITE1, SUITE2, SUITE3]
        suite_list = _create_executor_list(suites, [])
        with patch("sys.exit", _raise_bad_variant):
            with self.assertRaises(BadVariant):
                burn_in.create_task_list(self.evergreen_conf, variant, suite_list, [])


class FindChangedTests(unittest.TestCase):

    NUM_COMMITS = 10
    MOD_FILES = [os.path.normpath("jstests/test1.js"), os.path.normpath("jstests/test2.js")]
    REV_DIFF = dict(
        zip([str(x) for x in range(NUM_COMMITS)],
            [MOD_FILES for _ in range(NUM_COMMITS)]))  #type: ignore
    NO_REV_DIFF = dict(
        zip([str(x) for x in range(NUM_COMMITS)], [None for _ in range(NUM_COMMITS)]))

    UNTRACKED_FILES = [
        os.path.normpath("jstests/untracked1.js"),
        os.path.normpath("jstests/untracked2.js")
    ]

    @staticmethod
    def _copy_rev_diff(rev_diff):
        """Use this method instead of copy.deepcopy().

        Note - it was discovered during testing that after using copy.deepcopy() that
        updating one key would update all of them, i.e.,
            rev_diff = {"1": ["abc"], 2": ["abc"]}
            copy_rev_diff = copy.deepcopy(rev_diff)
            copy_rev_diff["2"] += "xyz"
            print(rev_diff)
                Result: {"1": ["abc"], 2": ["abc"]}
            print(copy_rev_diff)
                Result: {"1": ["abc", "xyz"], 2": ["abc", "xyz"]}
        At this point no identifiable issue could be found related to this problem.
        """
        copy_rev_diff = {}
        for key in rev_diff:
            copy_rev_diff[key] = []
            for file_name in rev_diff[key]:
                copy_rev_diff[key].append(file_name)
        return copy_rev_diff

    @staticmethod
    def _get_rev_list(range1, range2):
        return [str(num) for num in range(range1, range2 + 1)]

    def _mock_git_repository(self, directory):
        return MockGitRepository(directory, FindChangedTests._get_rev_list(self.rev1, self.rev2),
                                 self.rev_diff, self.untracked_files)

    def _test_find_changed_tests(  #pylint: disable=too-many-arguments
            self, commit, max_revisions, variant, check_evg, rev1, rev2, rev_diff, untracked_files,
            last_activated_task=None):
        branch = "master"
        # pylint: disable=attribute-defined-outside-init
        self.rev1 = rev1
        self.rev2 = rev2
        self.rev_diff = rev_diff
        self.untracked_files = untracked_files
        self.expected_changed_tests = []
        if commit is None and rev_diff:
            self.expected_changed_tests += rev_diff[str(self.NUM_COMMITS - 1)]
        elif rev_diff.get(commit, []):
            self.expected_changed_tests += rev_diff.get(commit, [])
        self.expected_changed_tests += untracked_files
        # pylint: enable=attribute-defined-outside-init
        with patch(EVG_CLIENT + ".read_evg_config", return_value=None),\
             patch(GIT + ".Repository", self._mock_git_repository),\
             patch("os.path.isfile", return_value=True),\
             patch(BURN_IN + ".find_last_activated_task", return_value=last_activated_task):
            return burn_in.find_changed_tests(branch, commit, max_revisions, variant, check_evg)

    def test_find_changed_tests(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)

    def test_find_changed_tests_no_changes(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.NO_REV_DIFF, [])
        self.assertEqual(changed_tests, [])
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.NO_REV_DIFF, [], "1")
        self.assertEqual(changed_tests, [])

    def test_find_changed_tests_check_evergreen(self):
        commit = "1"
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff["2"] += [os.path.normpath("jstests/test.js")]
        expected_changed_tests = self.REV_DIFF[commit] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3, rev_diff,
                                                      self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, expected_changed_tests)
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff["3"] += [os.path.normpath("jstests/test.js")]
        expected_changed_tests = rev_diff["3"] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3, rev_diff,
                                                      self.UNTRACKED_FILES, "1")
        self.assertEqual(changed_tests, expected_changed_tests)

    def test_find_changed_tests_no_diff(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.NO_REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.UNTRACKED_FILES)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.NO_REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.UNTRACKED_FILES)

    def test_find_changed_tests_no_untracked(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.REV_DIFF, [])
        self.assertEqual(changed_tests, self.REV_DIFF[commit])
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.REV_DIFF, [])
        self.assertEqual(changed_tests, self.REV_DIFF[commit])

    def test_find_changed_tests_no_base_commit(self):
        changed_tests = self._test_find_changed_tests(None, 5, "myvariant", False, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)
        changed_tests = self._test_find_changed_tests(None, 5, "myvariant", True, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)

    def test_find_changed_tests_non_js(self):
        commit = "3"
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff[commit] += [os.path.normpath("jstests/test.yml")]
        untracked_files = self.UNTRACKED_FILES + [os.path.normpath("jstests/untracked.yml")]
        expected_changed_tests = self.REV_DIFF[commit] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)

    def test_find_changed_tests_not_in_jstests(self):
        commit = "3"
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff[commit] += [os.path.normpath("other/test.js")]
        untracked_files = self.UNTRACKED_FILES + [os.path.normpath("other/untracked.js")]
        expected_changed_tests = self.REV_DIFF[commit] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)

    def test_find_changed_tests_no_revisions(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 0,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 0,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)

    def test_find_changed_tests_too_many_revisions(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 9,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, [])
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 9,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, [])


class MockGitRepository(object):
    def __init__(self, _, rev_list, rev_diff, untracked_files):
        self.rev_list = rev_list
        self.rev_diff = rev_diff
        self.untracked_files = untracked_files

    def _get_revs(self, rev_range):
        revs = rev_range.split("...")
        if not revs:
            return revs
        elif len(revs) == 1:
            revs.append("HEAD")
        if revs[1] == "HEAD" and self.rev_list:
            revs[1] = self.rev_list[-1]
        return revs

    def __get_rev_range(self, rev_range):
        commits = []
        if len(self.rev_list) < 2:
            return commits
        revs = self._get_revs(rev_range)
        latest_commit_found = False
        for commit in self.rev_list:
            latest_commit_found = latest_commit_found or revs[0] == commit
            if revs[1] == commit:
                break
            if latest_commit_found:
                commits.append(commit)
        return commits

    def get_merge_base(self, _):
        return self.rev_list[-1]

    def git_rev_list(self, args):
        return "\n".join(self.__get_rev_range(args[0])[::-1])

    def git_diff(self, args):
        revs = self._get_revs(args[1])
        if revs:
            diff_list = self.rev_diff.get(revs[-1], [])
            if diff_list:
                return "\n".join(diff_list)
        return ""

    def git_status(self, args):
        revs = self._get_revs(args[0])
        modified_files = [""]
        if revs:
            diff_list = self.rev_diff.get(revs[-1], [])
            if diff_list:
                modified_files = [" M {}".format(untracked) for untracked in diff_list]
        untracked_files = ["?? {}".format(untracked) for untracked in self.untracked_files]
        return "\n".join(modified_files + untracked_files)


class MockResponse(object):
    def __init__(self, response, json_data):
        self.response = response
        self.json_data = json_data

    def json(self):
        return self.json_data


class MockRequests(object):
    def __init__(self):
        self.responses = {}

    def put(self, url, response, json_data):
        self.responses[url] = MockResponse(response, json_data)

    def get(self, url):
        if url in self.responses:
            return self.responses[url]
        return None
