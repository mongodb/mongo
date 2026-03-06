"""Test that fixture UDS environment variables are properly set."""

import os
import unittest


class TestFixtureUDSEnvironmentVariables(unittest.TestCase):
    """Test case to verify fixture UDS environment variables."""

    def test_fixture_type_is_set(self):
        """Test that MONGODB_FIXTURE_TYPE environment variable is set."""
        fixture_type = os.environ.get("MONGODB_FIXTURE_TYPE")
        self.assertIsNotNone(fixture_type, "MONGODB_FIXTURE_TYPE should be set")
        self.assertEqual(fixture_type, "MongoDFixture", "Fixture type should be MongoDFixture")

    def test_mongodb_uds_path_is_set(self):
        """Test that MONGODB_UDS_PATH environment variable is set when UDS is configured."""
        uds_path = os.environ.get("MONGODB_UDS_PATH")
        self.assertIsNotNone(
            uds_path, "MONGODB_UDS_PATH environment variable should be set when UDS is configured"
        )

        # Verify it's a path
        self.assertIn("/", uds_path, f"UDS path should be an absolute path: {uds_path}")

        # Verify it contains mongodb in the filename
        self.assertIn(
            "mongodb-", uds_path, f"UDS path should contain mongodb- in filename: {uds_path}"
        )

        # Verify it ends with .sock
        self.assertTrue(uds_path.endswith(".sock"), f"UDS path should end with .sock: {uds_path}")

        # Verify it contains the expected prefix
        self.assertIn(
            "/tmp/mongodb-test-sockets",
            uds_path,
            f"UDS path should contain the configured prefix: {uds_path}",
        )

        # Verify the UDS path actually exists on the filesystem
        self.assertTrue(
            os.path.exists(uds_path), f"UDS path should exist on the filesystem: {uds_path}"
        )

    def test_mongodb_uds_paths_is_set(self):
        """Test that MONGODB_UDS_PATHS (plural) is also set for consistency."""
        uds_paths = os.environ.get("MONGODB_UDS_PATHS")
        self.assertIsNotNone(uds_paths, "MONGODB_UDS_PATHS should be set")
        # For standalone, should be same as singular
        uds_path = os.environ.get("MONGODB_UDS_PATH")
        self.assertEqual(
            uds_paths, uds_path, "MONGODB_UDS_PATHS should equal MONGODB_UDS_PATH for standalone"
        )

    def test_mongodb_connection_string_is_still_set(self):
        """Test that MONGODB_CONNECTION_STRING is still set even with UDS."""
        conn_str = os.environ.get("MONGODB_CONNECTION_STRING")
        self.assertIsNotNone(conn_str, "MONGODB_CONNECTION_STRING should still be set with UDS")


if __name__ == "__main__":
    unittest.main()
