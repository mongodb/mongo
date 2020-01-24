"""Unit tests for burn_in_tags_bypass_compile_and_fetch_binaries."""

import unittest
from unittest.mock import MagicMock

import buildscripts.burn_in_tags_bypass_compile_and_fetch_binaries as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


class TestRetrieveUsedBuildId(unittest.TestCase):
    def test_build_with_no_compile_throws_exception(self):
        build_mock = MagicMock()

        with self.assertRaises(ValueError):
            under_test._retrieve_used_build_id(build_mock)

    def test_compile_with_no_binaries_artifact_throws_exception(self):
        build_mock = MagicMock()
        compile_task = MagicMock(display_name="compile")
        build_mock.get_tasks.return_value = [compile_task]

        with self.assertRaises(ValueError):
            under_test._retrieve_used_build_id(build_mock)

    def test_build_id_from_compile_binaries_is_used(self):
        build_id = "this_is_the_build_id"
        url = f"http://s3.amazon.com/mciuploads/mongodb/build_var//binaries/mongo-{build_id}.tgz"
        build_mock = MagicMock()
        compile_task = MagicMock(display_name="compile")
        build_mock.get_tasks.return_value = [MagicMock(), compile_task, MagicMock()]
        artifact_mock = MagicMock(url=url)
        artifact_mock.name = "Binaries"
        compile_task.artifacts = [MagicMock(), artifact_mock, MagicMock()]

        self.assertEqual(build_id, under_test._retrieve_used_build_id(build_mock))
