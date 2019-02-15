"""Unit tests for the buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import collections
import os
import unittest

from mock import patch, Mock

import buildscripts.burn_in_tests as burn_in

# pylint: disable=missing-docstring,protected-access


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
        return "{}{}builds/{}".format(burn_in.API_SERVER_DEFAULT, burn_in.REST_PREFIX, build)

    @staticmethod
    def revisions_url(project, revision):
        """Return revisions URL."""
        return "{}{}projects/{}/revisions/{}".format(burn_in.API_SERVER_DEFAULT,
                                                     burn_in.REST_PREFIX, project, revision)

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
        # revisions = self.REVISION_BUILDS.keys()
        with patch("buildscripts.burn_in_tests.requests", MockRequests()), patch(
                "buildscripts.client.evergreen.read_evg_config") as mock_read_evg:
            self.load_urls(burn_in.requests, "mongodb-mongo-master", self.REVISION_BUILDS)
            mock_read_evg.return_value = None
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
    with patch(
            "buildscripts.resmokelib.suitesconfig.create_test_membership_map") as mock_member_map:
        mock_member_map.return_value = MEMBERS_MAP
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
        with patch("buildscripts.client.evergreen.read_evg_config") as mock_read_evg, patch(
                "buildscripts.git.Repository",
                self._mock_git_repository), patch("os.path.isfile") as mock_isfile, patch(
                    "buildscripts.burn_in_tests.find_last_activated_task"
                ) as mock_find_last_activated_task:
            mock_read_evg.return_value = None
            mock_isfile.return_value = True
            mock_find_last_activated_task.return_value = last_activated_task
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
