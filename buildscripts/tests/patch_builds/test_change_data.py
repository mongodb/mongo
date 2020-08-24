"""Unit tests for buildscripts.patch_builds.change_data.py."""
from __future__ import absolute_import

import os
import unittest

from mock import patch, MagicMock

import buildscripts.patch_builds.change_data as under_test

# pylint: disable=missing-docstring

NS = "buildscripts.patch_builds.change_data"


def ns(relative_name):  # pylint: disable=invalid-name
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


class TestGenerateRevisionMapFromManifest(unittest.TestCase):
    def test_map_can_be_created_from_evergreen_api(self):
        mock_repo_list = [create_mock_repo(os.getcwd()), create_mock_repo("/path/to/enterprise")]
        mongo_revision = "revision1234"
        enterprise_revision = "revision5678"
        mock_manifest = MagicMock(revision=mongo_revision,
                                  modules={"enterprise": MagicMock(revision=enterprise_revision)})
        mock_evg_api = MagicMock()
        mock_evg_api.manifest_for_task.return_value = mock_manifest

        revision_map = under_test.generate_revision_map_from_manifest(mock_repo_list, "task_id",
                                                                      mock_evg_api)

        self.assertEqual(revision_map[mock_repo_list[0].git_dir], mongo_revision)
        self.assertEqual(revision_map[mock_repo_list[1].git_dir], enterprise_revision)
