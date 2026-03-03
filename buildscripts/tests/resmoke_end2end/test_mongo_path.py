"""Test resmoke's MONGO_PATH functionality for load() and import()."""

import os
import subprocess
import sys
import unittest


class TestMongoPath(unittest.TestCase):
    """Test that MONGO_PATH works for both load() and import()."""

    @classmethod
    def setUpClass(cls):
        """Set up paths for the test."""
        cls.end2end_root = "buildscripts/tests/resmoke_end2end"
        cls.mongo_shell_root = os.path.join(cls.end2end_root, "mongo_shell")
        cls.mongo_path_dir = os.path.join(cls.mongo_shell_root, "modules")
        cls.tests_dir = os.path.join(cls.mongo_shell_root, "tests")

    def execute_resmoke(self, resmoke_args):
        """Helper to execute resmoke with the given arguments."""
        return subprocess.run(
            [sys.executable, "buildscripts/resmoke.py", "run"] + resmoke_args,
            capture_output=True,
            text=True,
        )

    def test_mongo_path_load_and_import(self):
        """Test that load() and import() can find modules in MONGO_PATH."""
        # Set up the MONGO_PATH to point to our test directory
        mongo_path = os.path.abspath(self.mongo_path_dir)

        # Run resmoke with --appendMongoPath pointing to the test directory
        result = self.execute_resmoke(
            [
                f"--appendMongoPath={mongo_path}",
                "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_mongo_path.yml",
            ]
        )

        self.assertEqual(
            result.returncode,
            0,
            f"Resmoke failed to run test with MONGO_PATH. "
            f"The test should have found modules in {mongo_path}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}",
        )

    def test_mongo_path_with_multiple_directories(self):
        """Test that MONGO_PATH works with multiple directories specified separately."""
        # Add a non-existent directory first to test that it searches through multiple paths
        mongo_path_dir = os.path.abspath(self.mongo_path_dir)

        # Run resmoke with multiple --appendMongoPath arguments
        result = self.execute_resmoke(
            [
                "--appendMongoPath=/nonexistent/path",
                f"--appendMongoPath={mongo_path_dir}",
                "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_mongo_path.yml",
            ]
        )

        self.assertEqual(
            result.returncode,
            0,
            f"Resmoke failed to run test with multiple MONGO_PATH directories. "
            f"The test should have found modules in the second directory: {mongo_path_dir}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}",
        )

    def test_mongo_path_relative_paths(self):
        """Test that modules can be imported using relative paths without MONGO_PATH."""
        # Run without --appendMongoPath, using only relative paths
        result = self.execute_resmoke(
            [
                "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_mongo_path.yml",
                "buildscripts/tests/resmoke_end2end/mongo_shell/tests/mongo_path_relative_test.js",
            ]
        )

        self.assertEqual(
            result.returncode,
            0,
            "Resmoke failed to run test with relative paths. "
            f"Modules should be loadable via relative paths without MONGO_PATH.\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}",
        )

    def test_mongo_path_mixed_paths(self):
        """Test mixing MONGO_PATH and relative paths in the same test."""
        mongo_path_dir = os.path.abspath(self.mongo_path_dir)

        result = self.execute_resmoke(
            [
                f"--appendMongoPath={mongo_path_dir}",
                "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_mongo_path.yml",
                "buildscripts/tests/resmoke_end2end/mongo_shell/tests/mongo_path_mixed_test.js",
            ]
        )

        self.assertEqual(
            result.returncode,
            0,
            f"Resmoke failed to run test mixing MONGO_PATH and relative paths.\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}",
        )

    def test_mongo_path_negative(self):
        """Test that imports fail without MONGO_PATH when expected."""
        # Run the normal test without --appendMongoPath, expecting failure
        result = self.execute_resmoke(
            [
                "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_mongo_path.yml",
            ]
        )

        self.assertNotEqual(
            result.returncode,
            0,
            "Test should have failed without MONGO_PATH. "
            "The import of 'top_level_module.js' should fail when MONGO_PATH is not set.",
        )
