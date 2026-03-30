"""Test resmoke's external module support."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

# Add repo root to sys.path so we can import buildscripts
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent))

from buildscripts.resmokelib import config


class TestExternalModule(unittest.TestCase):
    """Test external module configuration and suite loading."""

    @classmethod
    def setUpClass(cls):
        """Set up test references to external module test files."""
        # Since we run from resmoke_end2end directory, paths are relative to that
        cls.external_module_dir = "external_module_test"
        cls.external_config_path = "external_module_test/resmoke_config.yml"

        # Suite names that exist in external_module_test/suites/
        cls.external_suite_name = "external_test_suite"
        cls.builtin_suite_name = "external_with_builtin"
        cls.builtin_exclude_suite_name = "external_with_builtin_exclude"
        cls.fixture_suite_name = "external_with_fixture"

    def run_resmoke(self, args, env=None):
        """Run resmoke with the given arguments from the resmoke_end2end directory.

        This simulates running resmoke from an external project directory.
        """
        # Get the MongoDB repo root from resmoke config (always correct)
        repo_root = config.RESMOKE_ROOT

        # Run from resmoke_end2end directory to simulate external project
        end2end_dir = os.path.join(repo_root, "buildscripts", "tests", "resmoke_end2end")

        # Use absolute path to resmoke.py since we're running from a different directory
        resmoke_path = os.path.join(repo_root, "buildscripts", "resmoke.py")
        cmd = [sys.executable, resmoke_path] + args

        result = subprocess.run(
            cmd, cwd=end2end_dir, env=env or os.environ.copy(), capture_output=True, text=True
        )
        return result

    def test_list_suites_without_external_module(self):
        """Test that list-suites works without external module config."""
        result = self.run_resmoke(["list-suites"])
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")
        self.assertIn("aggregation", result.stdout)
        self.assertNotIn(self.external_suite_name, result.stdout)

    def test_list_suites_with_external_module_env_var(self):
        """Test that external module suite appears when using environment variable."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(["list-suites"], env=env)
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # Should include both built-in and external suites
        self.assertIn("aggregation", result.stdout)
        self.assertIn(self.external_suite_name, result.stdout)
        self.assertIn(self.builtin_suite_name, result.stdout)

    def test_suiteconfig_external_suite(self):
        """Test that external suite configuration can be loaded."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(["suiteconfig", "--suite", self.external_suite_name], env=env)
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # Verify suite config contains expected content
        self.assertIn("test_kind", result.stdout)
        self.assertIn("js_test", result.stdout)
        self.assertIn("selector", result.stdout)

    def test_external_module_config_not_found(self):
        """Test that helpful error is shown when config file doesn't exist."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = "/nonexistent/config.yml"

        result = self.run_resmoke(["list-suites"], env=env)
        # Should fail with clear error message
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("External module config file not found", result.stderr)

    def test_external_module_with_empty_suite_dirs(self):
        """Test that external module with empty suite directories works."""
        # Use relative path since resmoke runs from resmoke_end2end directory
        empty_config_path = os.path.join(self.external_module_dir, "empty_config.yml")

        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = empty_config_path

        result = self.run_resmoke(["list-suites"], env=env)
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # Should only show built-in suites
        self.assertIn("aggregation", result.stdout)
        self.assertNotIn(self.external_suite_name, result.stdout)

    def test_external_suite_test_discovery(self):
        """Test that external suite discovers tests correctly."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(["test-discovery", "--suite", self.external_suite_name], env=env)
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # Should discover both external test files
        self.assertIn("simple_test.js", result.stdout)
        self.assertIn("another_test.js", result.stdout)
        # Paths should be relative to external module root
        self.assertIn("external_module_test/tests/", result.stdout)

    def test_builtin_prefix_in_external_suite(self):
        """Test that builtin: prefix allows referencing MongoDB built-in tests."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(["test-discovery", "--suite", self.builtin_suite_name], env=env)
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # Should include both external and built-in tests
        self.assertIn("simple_test.js", result.stdout)
        self.assertIn("another_test.js", result.stdout)
        # Should include tests from builtin: prefix (from resmoke_end2end/testfiles)
        self.assertIn("/testfiles/one.js", result.stdout)
        self.assertIn("/testfiles/two.js", result.stdout)

    def test_builtin_prefix_with_exclude(self):
        """Test that builtin: prefix works with exclude_files."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(
            ["test-discovery", "--suite", self.builtin_exclude_suite_name], env=env
        )
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # Should include tests from builtin: but exclude one.js
        self.assertIn("/testfiles/two.js", result.stdout)
        self.assertNotIn("/testfiles/one.js", result.stdout)

    # skipping on TSAN because of the supressions file being relative to the root repo
    # This would work fine on TSAN in an exteral repo beause they would specify the correct
    # relative path.
    @unittest.skipIf("TSAN_OPTIONS" in os.environ, "Skipping when TSAN_OPTIONS is set")
    def test_external_suite_with_fixture_runs(self):
        """Test that external suite with MongoDFixture can run both external and builtin tests."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(["run", "--suite", self.fixture_suite_name], env=env)

        # Check that the suite ran successfully
        if result.returncode != 0:
            self.fail(f"resmoke failed:\nstdout: {result.stdout}\nstderr: {result.stderr}")

        # Should have run both external and builtin tests
        self.assertIn("mongo_test.js", result.stdout)
        self.assertIn("one.js", result.stdout)

    def test_suite_root_tracking(self):
        """Test that suite root is tracked correctly for external suites."""
        env = os.environ.copy()
        env["EXTERNAL_MODULE_CONFIG"] = self.external_config_path

        result = self.run_resmoke(["test-discovery", "--suite", self.external_suite_name], env=env)
        self.assertEqual(result.returncode, 0, f"resmoke failed: {result.stderr}")

        # External suite paths should be relative since we're running from external module directory
        self.assertIn(
            "external_module_test/tests/simple_test.js",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
