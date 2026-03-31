"""Test that fixture environment variables are properly passed to test cases."""

import subprocess
import sys
import unittest


def execute_resmoke(resmoke_args: list[str], subcommand: str = "run"):
    """Execute resmoke with the given arguments."""
    return subprocess.run(
        [sys.executable, "buildscripts/resmoke.py", subcommand] + resmoke_args,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


class TestFixtureEnvironmentVariables(unittest.TestCase):
    """Test that fixture environment variables are properly passed to test cases."""

    def test_fixture_env_vars_basic(self):
        """Test that basic fixture environment variables are set."""
        resmoke_args = [
            "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_test_fixture_env_vars.yml",
        ]

        result = execute_resmoke(resmoke_args)

        # The test should pass, which means the environment variables were set correctly
        self.assertEqual(
            result.returncode,
            0,
            f"Test should pass with fixture env vars. stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def test_fixture_env_vars_with_uds(self):
        """Test that UDS fixture environment variables are set when configured."""
        resmoke_args = [
            "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_test_fixture_env_vars_with_uds.yml",
        ]

        result = execute_resmoke(resmoke_args)

        # The test should pass, which means the UDS environment variables were set correctly
        self.assertEqual(
            result.returncode,
            0,
            f"Test should pass with UDS env vars. stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def test_fixture_env_vars_with_uds_replset(self):
        """Test that UDS fixture environment variables are set for replica sets."""
        resmoke_args = [
            "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_test_fixture_env_vars_with_uds_replset.yml",
        ]

        result = execute_resmoke(resmoke_args)

        # The test should pass, which means the UDS environment variables were set correctly
        self.assertEqual(
            result.returncode,
            0,
            f"Test should pass with UDS env vars for replica set. stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def test_fixture_env_vars_with_uds_sharded(self):
        """Test that UDS fixture environment variables are set for sharded clusters."""
        resmoke_args = [
            "--suite=buildscripts/tests/resmoke_end2end/suites/resmoke_test_fixture_env_vars_with_uds_sharded.yml",
        ]

        result = execute_resmoke(resmoke_args)

        # The test should pass, which means the UDS environment variables were set correctly
        self.assertEqual(
            result.returncode,
            0,
            f"Test should pass with UDS env vars for sharded cluster. stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
