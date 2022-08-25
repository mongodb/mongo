"""Unit tests for resmoke_proxy.py"""
import unittest
from unittest.mock import MagicMock

from buildscripts.resmoke_proxy import resmoke_proxy as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


class TestResmokeProxy(unittest.TestCase):
    def test_list_tests_can_handle_strings_and_lists(self):
        mock_suite = MagicMock(
            tests=["test0", "test1", ["test2a", "tests2b", "test2c"], "test3", ["test4a"]])

        resmoke_proxy = under_test.ResmokeProxyService()
        resmoke_proxy._suite_config = MagicMock()
        resmoke_proxy._suite_config.get_suite.return_value = mock_suite

        test_list = resmoke_proxy.list_tests("some suite")

        self.assertEqual(len(test_list), 7)
