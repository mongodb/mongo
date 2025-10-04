"""Test hook for verifying the consistency of indexes across a sharded cluster."""

import os.path

from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import jsfile


class CheckClusterIndexConsistency(jsfile.DataConsistencyHook):
    """Checks that indexes are the same across chunks for the same collections."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckClusterIndexConsistency."""

        if not isinstance(fixture, shardedcluster.ShardedClusterFixture):
            raise ValueError(
                f"'fixture' must be an instance of ShardedClusterFixture, but got"
                f" {fixture.__class__.__name__}"
            )

        description = "Check index consistency across cluster"
        js_filename = os.path.join("jstests", "hooks", "run_cluster_index_consistency.js")
        super().__init__(
            hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
