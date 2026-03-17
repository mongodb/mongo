"""Test external module fixtures and hooks loading."""

import logging
import os
import sys
import unittest
import uuid

import yaml

# Add the repo root to the path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../.."))

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import configure_resmoke, suitesconfig
from buildscripts.resmokelib.testing import fixtures, hooks
from buildscripts.resmokelib.testing.testcases import jstest, pytest


class TestExternalFixturesHooks(unittest.TestCase):
    """Test that external fixtures and hooks are properly loaded."""

    @classmethod
    def setUpClass(cls):
        """Set up test environment once for all tests."""
        # Set EXTERNAL_MODULE_ROOT to the test module directory
        test_module_dir = os.path.join(
            os.path.dirname(__file__), "test_external_module_fixtures_hooks"
        )
        _config.EXTERNAL_MODULE_ROOT = os.path.abspath(test_module_dir)

        # Recalculate IN_EXTERNAL_MODULE after changing EXTERNAL_MODULE_ROOT
        _config.IN_EXTERNAL_MODULE = os.path.normpath(
            _config.EXTERNAL_MODULE_ROOT
        ) != os.path.normpath(_config.RESMOKE_ROOT)

        # Set CONFIG_DIR to avoid errors
        _config.CONFIG_DIR = os.path.join(_config.RESMOKE_ROOT, "buildscripts", "resmokeconfig")

        # Load the external module config once for all tests
        config_path = os.path.join(_config.EXTERNAL_MODULE_ROOT, "external_module.yml")
        configure_resmoke._load_external_module_config(config_path)

    def test_load_external_fixtures(self):
        """Test that external fixtures are loaded and registered."""
        # Try to instantiate the external fixture by name
        # The fixture should be registered in the fixtures registry
        logger = logging.getLogger("test")
        fixture = fixtures.make_fixture("TestExternalFixture", logger, job_num=0)

        self.assertIsNotNone(fixture)
        self.assertEqual(fixture.REGISTERED_NAME, "TestExternalFixture")

    def test_load_external_hooks(self):
        """Test that external hooks are loaded and registered."""
        # Try to instantiate the external hook by name
        # The hook should be registered in the hooks registry
        logger = logging.getLogger("test")

        hook = hooks.make_hook("TestExternalHook", logger, None, "Test hook")

        self.assertIsNotNone(hook)
        self.assertEqual(hook.REGISTERED_NAME, "TestExternalHook")

    def test_test_env_vars_jstest(self):
        """Test that test environment variables are properly passed to JS test cases."""
        # Create a _SingleJSTestCase with env_vars in config (not shell_options)
        logger = logging.getLogger("test")
        test_env_vars = {"TEST_VAR_1": "value1", "TEST_VAR_2": "value2"}
        shell_options = {
            "global_vars": {"TestData": {}},
        }

        test_case = jstest._SingleJSTestCase(
            logger,
            ["jstests/core/query/find/find1.js"],
            "test.js",
            uuid.uuid4(),
            shell_options=shell_options,
            env_vars=test_env_vars,
        )

        # Verify that env_vars were extracted and stored
        self.assertIsNotNone(test_case._test_env_vars)
        self.assertEqual(test_case._test_env_vars, test_env_vars)

    def test_test_env_vars_pytest(self):
        """Test that test environment variables are properly passed to Python test cases."""
        # Create a PyTestCase with env_vars in config
        logger = logging.getLogger("test")
        test_env_vars = {"TEST_VAR_1": "value1", "TEST_VAR_2": "value2"}

        test_case = pytest.PyTestCase(
            logger,
            ["buildscripts/tests/test_example.py"],
            env_vars=test_env_vars,
        )

        # Verify that env_vars were extracted and stored
        self.assertIsNotNone(test_case._test_env_vars)
        self.assertEqual(test_case._test_env_vars, test_env_vars)

    def test_generate_matrix_suites_with_external_module(self):
        """Test that matrix suite generation works with external modules."""
        # Create a MatrixSuiteConfig
        matrix_suite_config = suitesconfig.MatrixSuiteConfig()

        # The suite name is the filename without extension
        suite_name = "test_external_matrix"

        # Generate the matrix suite file
        matrix_suite_config.generate_matrix_suite_file(suite_name)

        # Get the generated suite path
        generated_suite_path = matrix_suite_config.get_generated_suite_path(suite_name)

        # Verify the file exists
        self.assertTrue(
            os.path.exists(generated_suite_path),
            msg=f"Generated suite file not found at {generated_suite_path}",
        )

        # Load the generated suite and verify it contains builtin: prefix
        with open(generated_suite_path, "r", encoding="utf8") as file:
            generated_suite = yaml.safe_load(file)

        # The generated suite should have selector paths with builtin: prefix
        # since the base suite (core) is from RESMOKE_ROOT but the matrix suite
        # is from EXTERNAL_MODULE_ROOT
        self.assertIn("selector", generated_suite)
        selector = generated_suite["selector"]

        # Check roots have builtin: prefix
        self.assertIn("roots", selector)
        roots = selector["roots"]
        self.assertIsInstance(roots, list)
        self.assertGreater(len(roots), 0, "Generated suite should have selector roots")

        for root in roots:
            self.assertTrue(
                root.startswith("builtin:"),
                f"Root '{root}' should have 'builtin:' prefix since it comes from a built-in base suite",
            )

        # Check exclude_files have builtin: prefix (if present)
        if "exclude_files" in selector:
            exclude_files = selector["exclude_files"]
            self.assertIsInstance(exclude_files, list)
            for exclude_file in exclude_files:
                self.assertTrue(
                    exclude_file.startswith("builtin:"),
                    f"Exclude file '{exclude_file}' should have 'builtin:' prefix since it comes from a built-in base suite",
                )

        # Check include_files have builtin: prefix (if present)
        if "include_files" in selector:
            include_files = selector["include_files"]
            self.assertIsInstance(include_files, list)
            for include_file in include_files:
                self.assertTrue(
                    include_file.startswith("builtin:"),
                    f"Include file '{include_file}' should have 'builtin:' prefix since it comes from a built-in base suite",
                )

        # Verify the suite can be loaded and verified
        try:
            suite = matrix_suite_config.get_config_obj_and_verify(suite_name)
            self.assertIsNotNone(suite, msg=f"Suite {suite_name} could not be loaded")
        except Exception as ex:
            self.fail(f"Failed to load generated suite: {repr(ex)}")

        # Clean up: remove the generated file
        if os.path.exists(generated_suite_path):
            os.remove(generated_suite_path)


if __name__ == "__main__":
    unittest.main()
