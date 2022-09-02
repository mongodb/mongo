"""Unit tests for discovery subcommand."""

import unittest
from unittest.mock import MagicMock

import buildscripts.resmokelib.discovery as under_test
from buildscripts.resmokelib.testing.suite import Suite


class TestTestDiscoverySubCommand(unittest.TestCase):
    def test_gather_tests_should_return_discovered_tests(self):
        suite_name = "my suite"
        mock_suite = MagicMock(spec_set=Suite)
        mock_suite.get_display_name.return_value = suite_name
        mock_suite.tests = [
            "test_0.js",
            "test_1.js",
            [
                "test_2.js",
                "test_3.js",
                "test_4.js",
            ],
            "test_5.js",
        ]
        test_discovery_subcommand = under_test.TestDiscoverySubcommand(suite_name)

        tests = test_discovery_subcommand.gather_tests(mock_suite)

        self.assertEqual(suite_name, tests.suite_name)

        for i in range(6):
            self.assertIn(f"test_{i}.js", tests.tests)
