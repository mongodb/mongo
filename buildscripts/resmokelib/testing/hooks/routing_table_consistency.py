"""Test hook for verifying the consistency of sharded collections metadata stored in the config server of a sharded cluster."""

import os.path

from buildscripts.resmokelib.testing.fixtures import multi_sharded_cluster, shardedcluster
from buildscripts.resmokelib.testing.hooks import jsfile


class CheckRoutingTableConsistency(jsfile.PerClusterDataConsistencyHook):
    """Verifies the absence of corrupted entries in config.chunks and config.collections."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckRoutingTableConsistency."""

        if not isinstance(fixture, shardedcluster.ShardedClusterFixture) and not isinstance(
            fixture, multi_sharded_cluster.MultiShardedClusterFixture
        ):
            raise ValueError(
                f"'fixture' must be an instance of ShardedClusterFixture or MultiShardedClusterFixture, but got"
                f" {fixture.__class__.__name__}"
            )

        description = "Inspect collection and chunk metadata in config server"
        js_filename = os.path.join("jstests", "hooks", "run_check_routing_table_consistency.js")
        super().__init__(
            hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
