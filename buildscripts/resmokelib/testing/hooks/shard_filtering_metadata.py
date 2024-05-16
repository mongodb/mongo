"""Test hook for verifying the correctness of shard filtering metadata on shards of a sharded cluster."""

import os.path

from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import jsfile


class CheckShardFilteringMetadata(jsfile.DataConsistencyHook):
    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckShardFilteringMetadata."""

        if not isinstance(fixture, shardedcluster.ShardedClusterFixture):
            raise ValueError(
                f"'fixture' must be an instance of ShardedClusterFixture, but got"
                f" {fixture.__class__.__name__}"
            )

        description = "Inspect filtering metadata on shards"
        js_filename = os.path.join("jstests", "hooks", "run_check_shard_filtering_metadata.js")
        super().__init__(
            hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
