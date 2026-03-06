"""Test that fixture UDS environment variables are properly set for sharded clusters."""

import os
import unittest


class TestShardedClusterFixtureUDSEnvironmentVariables(unittest.TestCase):
    """Test case to verify fixture UDS environment variables in sharded clusters."""

    def test_fixture_type_is_set(self):
        """Test that MONGODB_FIXTURE_TYPE environment variable is set."""
        fixture_type = os.environ.get("MONGODB_FIXTURE_TYPE")
        self.assertIsNotNone(fixture_type, "MONGODB_FIXTURE_TYPE should be set")
        self.assertEqual(
            fixture_type, "ShardedClusterFixture", "Fixture type should be ShardedClusterFixture"
        )

    def test_mongodb_uds_path_is_set(self):
        """Test that MONGODB_UDS_PATH (singular) is set for the primary mongos."""
        uds_path = os.environ.get("MONGODB_UDS_PATH")
        self.assertIsNotNone(uds_path, "MONGODB_UDS_PATH should be set")
        self.assertTrue(
            os.path.exists(uds_path), f"Primary mongos UDS path should exist: {uds_path}"
        )

    def test_mongodb_uds_paths_are_set(self):
        """Test that MONGODB_UDS_PATHS (standardized) is set."""
        uds_paths = os.environ.get("MONGODB_UDS_PATHS")
        self.assertIsNotNone(uds_paths, "MONGODB_UDS_PATHS should be set")

        # Should be comma-separated list
        paths_list = uds_paths.split(",")
        self.assertGreater(len(paths_list), 0, f"Should have at least one UDS path: {uds_paths}")

        # Verify each path
        for uds_path in paths_list:
            self.assertIn("/", uds_path, f"UDS path should be an absolute path: {uds_path}")
            self.assertIn(
                "mongodb-", uds_path, f"UDS path should contain mongodb- in filename: {uds_path}"
            )
            self.assertTrue(
                uds_path.endswith(".sock"), f"UDS path should end with .sock: {uds_path}"
            )
            self.assertIn(
                "/tmp/mongodb-test-sockets",
                uds_path,
                f"UDS path should contain the configured prefix: {uds_path}",
            )
            self.assertTrue(
                os.path.exists(uds_path), f"UDS path should exist on the filesystem: {uds_path}"
            )

    def test_mongodb_mongos_uds_paths_are_set(self):
        """Test that MONGODB_MONGOS_UDS_PATHS environment variable is set when UDS is configured."""
        uds_paths = os.environ.get("MONGODB_MONGOS_UDS_PATHS")
        self.assertIsNotNone(
            uds_paths,
            "MONGODB_MONGOS_UDS_PATHS environment variable should be set when UDS is configured",
        )

        # Should be comma-separated list
        paths_list = uds_paths.split(",")
        self.assertGreater(
            len(paths_list), 0, f"Should have at least one mongos UDS path: {uds_paths}"
        )

        # Verify each path
        for uds_path in paths_list:
            # Verify it's a path
            self.assertIn("/", uds_path, f"UDS path should be an absolute path: {uds_path}")

            # Verify it contains mongodb in the filename
            self.assertIn(
                "mongodb-", uds_path, f"UDS path should contain mongodb- in filename: {uds_path}"
            )

            # Verify it ends with .sock
            self.assertTrue(
                uds_path.endswith(".sock"), f"UDS path should end with .sock: {uds_path}"
            )

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

    def test_mongodb_connection_string_is_still_set(self):
        """Test that MONGODB_CONNECTION_STRING is still set even with UDS."""
        conn_str = os.environ.get("MONGODB_CONNECTION_STRING")
        self.assertIsNotNone(conn_str, "MONGODB_CONNECTION_STRING should still be set with UDS")
        # For sharded clusters, connection string points to mongos
        self.assertIn(
            "localhost", conn_str, f"Connection string should contain localhost: {conn_str}"
        )


if __name__ == "__main__":
    unittest.main()
