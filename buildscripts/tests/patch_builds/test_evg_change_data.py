"""Unit tests for buildscripts.patch_builds.evg_change_data.py."""
from __future__ import absolute_import

import os
import unittest

from mock import MagicMock

import buildscripts.patch_builds.evg_change_data as under_test

# pylint: disable=missing-docstring


def create_mock_repo(working_dir=""):
    return MagicMock(working_dir=working_dir)


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
