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
        test_discovery_subcommand = under_test.TestDiscoverySubcommand([suite_name])

        tests = test_discovery_subcommand.gather_tests(mock_suite)

        self.assertEqual(suite_name, tests.suite_name)

        for i in range(6):
            self.assertIn(f"test_{i}.js", tests.tests)

    def test_execute_should_emit_one_document_per_suite(self):
        import io
        from contextlib import redirect_stdout

        import yaml

        def mock_suite(name):
            suite = MagicMock(spec_set=Suite)
            suite.get_display_name.return_value = name
            suite.tests = [f"{name}_test.js"]
            return suite

        test_discovery_subcommand = under_test.TestDiscoverySubcommand(["suite_a", "suite_b"])
        test_discovery_subcommand.suite_config = MagicMock()
        test_discovery_subcommand.suite_config.get_suite.side_effect = lambda name: mock_suite(name)

        output = io.StringIO()
        with redirect_stdout(output):
            test_discovery_subcommand.execute()

        docs = list(yaml.safe_load_all(output.getvalue()))
        self.assertEqual(
            docs,
            [
                {"suite_name": "suite_a", "tests": ["suite_a_test.js"]},
                {"suite_name": "suite_b", "tests": ["suite_b_test.js"]},
            ],
        )

    def test_execute_with_single_suite_emits_one_document(self):
        import io
        from contextlib import redirect_stdout

        import yaml

        suite = MagicMock(spec_set=Suite)
        suite.get_display_name.return_value = "suite_a"
        suite.tests = ["suite_a_test.js"]

        test_discovery_subcommand = under_test.TestDiscoverySubcommand(["suite_a"])
        test_discovery_subcommand.suite_config = MagicMock()
        test_discovery_subcommand.suite_config.get_suite.return_value = suite

        output = io.StringIO()
        with redirect_stdout(output):
            test_discovery_subcommand.execute()

        self.assertEqual(
            list(yaml.safe_load_all(output.getvalue())),
            [{"suite_name": "suite_a", "tests": ["suite_a_test.js"]}],
        )
        # The historical single-suite output is a single YAML document.
        self.assertNotIn("---", output.getvalue())


if __name__ == "__main__":
    unittest.main()
