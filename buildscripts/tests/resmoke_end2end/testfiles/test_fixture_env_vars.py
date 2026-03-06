"""Test that fixture environment variables are properly set."""

import os
import unittest


class TestFixtureEnvironmentVariables(unittest.TestCase):
    """Test case to verify fixture environment variables."""

    def test_mongodb_connection_string_is_set(self):
        """Test that MONGODB_CONNECTION_STRING environment variable is set."""
        conn_str = os.environ.get("MONGODB_CONNECTION_STRING")
        self.assertIsNotNone(
            conn_str, "MONGODB_CONNECTION_STRING environment variable should be set"
        )
        self.assertIn(
            "localhost", conn_str, f"Connection string should contain localhost: {conn_str}"
        )

    def test_mongodb_uds_path_when_configured(self):
        """Test that MONGODB_UDS_PATH is set when UDS is configured."""
        # This test will check if UDS path is set when the fixture is configured with UDS
        # If not configured, the env var won't be present
        uds_path = os.environ.get("MONGODB_UDS_PATH")
        if uds_path:
            # If set, verify it looks like a valid path
            self.assertTrue(
                uds_path.startswith("/") or "mongodb-" in uds_path,
                f"UDS path should be a valid path: {uds_path}",
            )

    def test_environment_variables_are_strings(self):
        """Test that all MongoDB environment variables are strings."""
        for key in ["MONGODB_CONNECTION_STRING"]:
            value = os.environ.get(key)
            if value is not None:
                self.assertIsInstance(value, str, f"{key} should be a string, got {type(value)}")


if __name__ == "__main__":
    unittest.main()
