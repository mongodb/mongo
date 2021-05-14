"""Unit tests for gentask_options.py."""

import unittest

import buildscripts.task_generation.task_types.gentask_options as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


def build_mock_gen_task_options(config_dir="."):
    return under_test.GenTaskOptions(
        create_misc_suite=True,
        is_patch=True,
        generated_config_dir=config_dir,
        use_default_timeouts=False,
    )


class TestSuiteLocation(unittest.TestCase):
    def test_should_return_suite_under_generated_config_dir(self):
        config_dir = "path/to/config"
        suite_name = "my_suite"
        mock_gen_task_options = build_mock_gen_task_options(config_dir=config_dir)

        suite_location = mock_gen_task_options.suite_location(suite_name)

        self.assertEqual(suite_location, f"{config_dir}/{suite_name}")
