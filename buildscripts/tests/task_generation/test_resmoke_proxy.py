"""Unit tests for resmoke_proxy.py"""
import unittest
from unittest.mock import MagicMock

from buildscripts.task_generation import resmoke_proxy as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


class TestResmokeProxy(unittest.TestCase):
    def test_list_tests_can_handle_strings_and_lists(self):
        mock_suite = MagicMock(
            tests=["test0", "test1", ["test2a", "tests2b", "test2c"], "test3", ["test4a"]])

        resmoke_proxy = under_test.ResmokeProxyService(under_test.ResmokeProxyConfig("suites_dir"))
        resmoke_proxy.suitesconfig = MagicMock()
        resmoke_proxy.suitesconfig.get_suite.return_value = mock_suite

        test_list = resmoke_proxy.list_tests("some suite")

        self.assertEqual(len(test_list), 7)


class UpdateSuiteConfigTest(unittest.TestCase):
    def test_roots_are_updated(self):
        config = {"selector": {}}

        updated_config = under_test.update_suite_config(config, "root value")
        self.assertEqual("root value", updated_config["selector"]["roots"])

    def test_excluded_files_not_included_if_not_specified(self):
        config = {"selector": {"excluded_files": "files to exclude"}}

        updated_config = under_test.update_suite_config(config, excludes=None)
        self.assertNotIn("exclude_files", updated_config["selector"])

    def test_excluded_files_added_to_misc(self):
        config = {"selector": {}}

        updated_config = under_test.update_suite_config(config, excludes="files to exclude")
        self.assertEqual("files to exclude", updated_config["selector"]["exclude_files"])

    def test_excluded_files_extended_in_misc(self):
        config = {"selector": {"exclude_files": ["file 0", "file 1"]}}

        updated_config = under_test.update_suite_config(config, excludes=["file 2", "file 3"])
        self.assertEqual(4, len(updated_config["selector"]["exclude_files"]))
        for exclude in ["file 0", "file 1", "file 2", "file 3"]:
            self.assertIn(exclude, updated_config["selector"]["exclude_files"])
