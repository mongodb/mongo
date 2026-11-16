"""Unit tests for the sessions collection drop hook."""

import logging
import unittest
from unittest import mock

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import replicaset, shardedcluster
from buildscripts.resmokelib.testing.hooks import drop_sessions_collection
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class TestCheckMetadataConsistency(unittest.TestCase):
    def setUp(self):
        self.fixture = mock.MagicMock(spec=shardedcluster.ShardedClusterFixture)
        self.fixture.mongos = [mock.MagicMock()]
        self.fixture.get_driver_connection_url.return_value = "mongodb://localhost:20000"
        self.fixture.get_shell_connection_url.return_value = "mongodb://localhost:20001"
        self.thread = drop_sessions_collection._DropSessionsCollectionThread(
            logging.getLogger("test"),
            lifecycle_interface.FlagBasedThreadLifecycle(),
            self.fixture,
        )
        self.shell_program = mock.patch.object(
            drop_sessions_collection.core.programs, "mongo_shell_program"
        ).start()
        self.addCleanup(mock.patch.stopall)
        self.shell_program.return_value.wait.return_value = 0

    def test_uses_shell_connection_url(self):
        """The shell must use its gRPC port rather than the driver's MongoDB port."""
        self.thread._check_metadata_consistency(self.fixture, "beforeDrop")

        self.assertEqual(
            self.shell_program.call_args.kwargs["connection_string"],
            "mongodb://localhost:20001",
        )
        self.shell_program.return_value.start.assert_called_once_with()
        self.shell_program.return_value.wait.assert_called_once_with()

    def test_disables_index_checks_in_hook_shell(self):
        """Preserve the previous cluster check without enabling cross-shard index checks."""
        self.thread._check_metadata_consistency(self.fixture, "beforeDrop")

        shell_options = self.shell_program.call_args.kwargs
        self.assertEqual(
            shell_options.get("global_vars"),
            {"TestData": {"skipCheckingIndexesConsistentAcrossCluster": True}},
        )
        self.assertIn("MetadataConsistencyChecker.run(db.getMongo());", shell_options["eval"])

    def test_failed_check_raises_server_failure(self):
        self.shell_program.return_value.wait.return_value = 1

        with self.assertRaisesRegex(errors.ServerFailure, "phase=afterDrop"):
            self.thread._check_metadata_consistency(self.fixture, "afterDrop")

    def test_skips_replica_set(self):
        fixture = mock.MagicMock(spec=replicaset.ReplicaSetFixture)

        self.thread._check_metadata_consistency(fixture, "beforeDrop")

        self.shell_program.assert_not_called()

    def test_skips_cluster_without_mongos(self):
        self.fixture.mongos = []

        self.thread._check_metadata_consistency(self.fixture, "beforeDrop")

        self.shell_program.assert_not_called()
