import logging
import os
import subprocess
import unittest

import yaml

from buildscripts.resmokelib import config, suitesconfig
from buildscripts.resmokelib.errors import InvalidMatrixSuiteError
from buildscripts.resmokelib.logging import loggers
from buildscripts.resmokelib.utils.dictionary import remove_from_lists


class TestSuiteGeneration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        config.CONFIG_DIR = "buildscripts/tests/resmoke_validation"
        cls.matrix_suite_config = suitesconfig.MatrixSuiteConfig()
        loggers.ROOT_EXECUTOR_LOGGER = logging

    def verify_suite_generation(self):
        tested_suite = "test_matrix_suite"

        generated_suite_path = self.matrix_suite_config.get_generated_suite_path(tested_suite)
        if os.path.exists(generated_suite_path):
            os.remove(generated_suite_path)

        with self.assertRaises(
            InvalidMatrixSuiteError,
            msg=f"{tested_suite} suite should have failed because the generated suite does not exist.",
        ):
            self.matrix_suite_config.get_config_obj_and_verify(tested_suite)

        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)

        try:
            suite = self.matrix_suite_config.get_config_obj_and_verify(tested_suite)
            self.assertIsNotNone(suite, msg=f"{suite} was not found.")
        except Exception as ex:
            self.fail(repr(ex))

    def verify_altered_generated_suite(self):
        tested_suite = "test_matrix_suite"
        generated_suite_path = self.matrix_suite_config.get_generated_suite_path(tested_suite)
        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)
        with open(generated_suite_path, "r+", encoding="utf8") as file:
            gen_yaml = yaml.safe_load(file)
            gen_yaml["abc"] = "def"
            file.seek(0)
            yaml.dump(gen_yaml, file)

        with self.assertRaises(
            InvalidMatrixSuiteError,
            msg=f"{tested_suite} suite should have failed because the generated suite was edited.",
        ):
            self.matrix_suite_config.get_config_obj_and_verify(tested_suite)

        # restore original file back
        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)

        try:
            suite = self.matrix_suite_config.get_config_obj_and_verify(tested_suite)
            self.assertIsNotNone(suite, msg=f"{suite} was not found.")
        except Exception as ex:
            self.fail(repr(ex))

    def run_generated_suite(self):
        tested_suite = "test_matrix_suite"
        generated_suite_path = self.matrix_suite_config.get_generated_suite_path(tested_suite)

        self.matrix_suite_config.generate_matrix_suite_file(tested_suite)

        resmoke_process = subprocess.run(
            ["python3", "buildscripts/resmoke.py", "run", "--suites", generated_suite_path]
        )

        self.assertEqual(
            0,
            resmoke_process.returncode,
            msg="Generated resmoke suite did not execute successfully.",
        )

    def test_everything_sequentially(self):
        self.verify_suite_generation()
        self.verify_altered_generated_suite()
        self.run_generated_suite()


class TestRemovesIntegration(unittest.TestCase):
    """Integration test that the 'removes' mapping key removes hooks via process_overrides."""

    @classmethod
    def setUpClass(cls):
        config.CONFIG_DIR = "buildscripts/tests/resmoke_validation"
        cls.matrix_suite_config = suitesconfig.MatrixSuiteConfig()
        loggers.ROOT_EXECUTOR_LOGGER = logging

    def test_removes_hook_from_suite(self):
        """Verify that a mapping with 'removes' removes the specified hook from the generated suite."""
        suite_name = "test_matrix_suite_removes"
        config_obj = self.matrix_suite_config.get_config_obj_no_verify(suite_name)
        self.assertIsNotNone(config_obj, msg=f"{suite_name} was not found.")
        hooks = config_obj["executor"]["hooks"]
        hook_classes = [h["class"] for h in hooks]
        self.assertIn("ValidateCollections", hook_classes)
        self.assertIn("CheckReplOplogs", hook_classes)
        self.assertNotIn(
            "CheckReplDBHash",
            hook_classes,
            msg="CheckReplDBHash should have been removed by the 'removes' key.",
        )


class TestRemoveFromLists(unittest.TestCase):
    """Unit tests for the remove_from_lists utility function."""

    def test_remove_string_from_list(self):
        """Test removing a string from a list of strings."""
        target = {"executor": {"archive": {"hooks": ["HookA", "HookB", "HookC"]}}}
        remove_spec = {"executor": {"archive": {"hooks": ["HookB"]}}}
        remove_from_lists(target, remove_spec)
        self.assertEqual(target["executor"]["archive"]["hooks"], ["HookA", "HookC"])

    def test_remove_dict_from_list_by_class(self):
        """Test removing a dict from a list of dicts by matching a subset of keys."""
        target = {
            "executor": {
                "hooks": [
                    {"class": "HookA"},
                    {"class": "HookB", "shell_options": {"global_vars": {"TestData": {"x": 1}}}},
                    {"class": "HookC"},
                ]
            }
        }
        remove_spec = {"executor": {"hooks": [{"class": "HookB"}]}}
        remove_from_lists(target, remove_spec)
        self.assertEqual(target["executor"]["hooks"], [{"class": "HookA"}, {"class": "HookC"}])

    def test_remove_nonexistent_item(self):
        """Test removing an item that doesn't exist in the list (no-op)."""
        target = {"executor": {"hooks": [{"class": "HookA"}, {"class": "HookB"}]}}
        remove_spec = {"executor": {"hooks": [{"class": "HookZ"}]}}
        remove_from_lists(target, remove_spec)
        self.assertEqual(target["executor"]["hooks"], [{"class": "HookA"}, {"class": "HookB"}])

    def test_remove_multiple_items(self):
        """Test removing multiple items in one call."""
        target = {"executor": {"archive": {"hooks": ["HookA", "HookB", "HookC", "HookD"]}}}
        remove_spec = {"executor": {"archive": {"hooks": ["HookB", "HookD"]}}}
        remove_from_lists(target, remove_spec)
        self.assertEqual(target["executor"]["archive"]["hooks"], ["HookA", "HookC"])

    def test_remove_from_missing_key_raises(self):
        """Test that removing from a key that doesn't exist raises ValueError."""
        target = {"executor": {"hooks": [{"class": "HookA"}]}}
        remove_spec = {"executor": {"archive": {"hooks": ["HookA"]}}}
        with self.assertRaises(ValueError):
            remove_from_lists(target, remove_spec)

    def test_remove_all_items_leaves_empty_list(self):
        """Test removing all items from a list leaves an empty list."""
        target = {"executor": {"hooks": [{"class": "HookA"}, {"class": "HookB"}]}}
        remove_spec = {"executor": {"hooks": [{"class": "HookA"}, {"class": "HookB"}]}}
        remove_from_lists(target, remove_spec)
        self.assertEqual(target["executor"]["hooks"], [])
