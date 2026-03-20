"""Background hook that runs _internalClearCollectionShardingMetadata on all sharded collections."""

import random

import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import bghook


class ClearCollectionShardingMetadataInBackground(bghook.BGHook):
    """Periodically runs _internalClearCollectionShardingMetadata on all sharded collections."""

    DESCRIPTION = (
        "Continuously runs _internalClearCollectionShardingMetadata on all sharded "
        "collections every X seconds. Defaults to 30 seconds if unspecified."
    )
    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, auth_options=None, frequency_seconds=30):
        bghook.BGHook.__init__(
            self, hook_logger, fixture, self.DESCRIPTION, loop_delay_ms=frequency_seconds * 1_000
        )
        self._auth_options = auth_options
        self._client = None

    def before_suite(self, test_report):
        if not isinstance(self.fixture, shardedcluster.ShardedClusterFixture):
            msg = "ClearCollectionShardingMetadataInBackground requires a ShardedClusterFixture."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        self._client = fixture_interface.build_client(self.fixture, self._auth_options)
        bghook.BGHook.before_suite(self, test_report)

    def run_action(self):
        sharded_colls = list(self._client.config.collections.find({}, {"_id": 1}))
        for entry in sharded_colls:
            ns = entry["_id"]  # e.g. "mydb.mycoll"
            for shard in self.fixture.shards:
                num_nodes_to_target = random.randint(0, len(shard.nodes))
                targeted_nodes = random.sample(shard.nodes, num_nodes_to_target)
                for node in targeted_nodes:
                    client = fixture_interface.build_client(node, self._auth_options)
                    try:
                        client.admin.command({"_internalClearCollectionShardingMetadata": ns})
                        self.logger.debug(
                            "Ran _internalClearCollectionShardingMetadata on %s on shard %s.",
                            ns,
                            client.address,
                        )
                    except pymongo.errors.OperationFailure as err:
                        self.logger.warning(
                            "_internalClearCollectionShardingMetadata on %s on shard %s failed: %s.",
                            ns,
                            client.address,
                            err,
                        )
