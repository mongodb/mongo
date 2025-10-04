"""Unit tests for buildscripts.patch_builds.change_data.py."""

from __future__ import absolute_import

import os
import unittest

from mock import MagicMock, mock_open, patch

import buildscripts.patch_builds.change_data as under_test

NS = "buildscripts.patch_builds.change_data"

FILE_CONTENT = "Hello, Universe!\n"


def ns(relative_name):
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def create_mock_repo(working_dir=""):
    return MagicMock(working_dir=working_dir)


class TestFindChangedFilesInRepos(unittest.TestCase):
    @patch(ns("find_changed_files"))
    def test_nothing_found(self, changed_files_mock):
        repos_mock = [MagicMock()]
        changed_files_mock.return_value = set()

        self.assertEqual(0, len(under_test.find_changed_files_in_repos(repos_mock)))

    @patch(ns("find_changed_files"))
    def test_changed_files_in_multiple_repos(self, changed_files_mock):
        repos_mock = [MagicMock(), MagicMock()]
        first_repo_file_changes = [
            os.path.join("jstests", "test1.js"),
            os.path.join("jstests", "test1.cpp"),
        ]
        second_repo_file_changes = [
            os.path.join("jstests", "test2.js"),
        ]
        changed_files_mock.side_effect = [first_repo_file_changes, second_repo_file_changes]

        self.assertEqual(3, len(under_test.find_changed_files_in_repos(repos_mock)))


class TestGenerateRevisionMap(unittest.TestCase):
    def test_mongo_revisions_is_mapped_correctly(self):
        mock_repo_list = [create_mock_repo(os.getcwd()), create_mock_repo("/path/to/enterprise")]
        revision_data = {"mongo": "revision1234", "enterprise": "revision5678"}

        revision_map = under_test.generate_revision_map(mock_repo_list, revision_data)

        self.assertEqual(revision_map[mock_repo_list[0].git_dir], revision_data["mongo"])
        self.assertEqual(revision_map[mock_repo_list[1].git_dir], revision_data["enterprise"])

    def test_missing_revisions_are_not_returned(self):
        mock_repo_list = [create_mock_repo(os.getcwd()), create_mock_repo("/path/to/enterprise")]
        revision_data = {"mongo": "revision1234"}

        revision_map = under_test.generate_revision_map(mock_repo_list, revision_data)

        self.assertEqual(revision_map[mock_repo_list[0].git_dir], revision_data["mongo"])
        self.assertEqual(len(revision_map), 1)

    def test_missing_repos_are_not_returned(self):
        mock_repo_list = [create_mock_repo(os.getcwd())]
        revision_data = {"mongo": "revision1234", "enterprise": "revision56768"}

        revision_map = under_test.generate_revision_map(mock_repo_list, revision_data)

        self.assertEqual(revision_map[mock_repo_list[0].git_dir], revision_data["mongo"])
        self.assertEqual(len(revision_map), 1)


class TestFindChangedFilesAndLinesInRepos(unittest.TestCase):
    @patch("builtins.open", new_callable=mock_open, read_data=FILE_CONTENT)
    def test_detects_file_and_line_modifications_no_change(self, mock_open):
        repo_mock = MagicMock()
        repo_mock.git.diff.return_value = ""

        changed_files = ["src/module/test1.cpp"]
        revision_map = {}

        result = under_test.find_modified_lines_for_files(repo_mock, changed_files, revision_map)

        expected_result = {}

        mock_open.assert_called_once_with("src/module/test1.cpp", "r")

        self.assertEqual(result, expected_result)

    @patch("builtins.open", new_callable=mock_open, read_data=FILE_CONTENT)
    def test_detects_file_and_line_modifications_one_file(self, mock_open):
        repo_mock = MagicMock()
        repo_mock.git.diff.return_value = """\
@@ -1,2 +1,2 @@
-Hello, World!
+Hello, Universe!
 """

        changed_files = ["src/module/test1.cpp"]
        revision_map = {}

        result = under_test.find_modified_lines_for_files(repo_mock, changed_files, revision_map)

        expected_result = {"src/module/test1.cpp": [(1, "Hello, Universe!")]}

        mock_open.assert_called_once_with("src/module/test1.cpp", "r")

        self.assertEqual(result, expected_result)
