"""Test hook for verifying orphan documents are eventually deleted in a sharded cluster."""

import os.path

from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import jsfile


class CheckOrphansDeleted(jsfile.DataConsistencyHook):
    """Check if the range deleter failed to delete any orphan documents."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckOrphansDeleted."""

        if not isinstance(fixture, shardedcluster.ShardedClusterFixture):
            raise ValueError(f"'fixture' must be an instance of ShardedClusterFixture, but got"
                             f" {fixture.__class__.__name__}")

        description = "Check orphan documents are eventually deleted"
        js_filename = os.path.join("jstests", "hooks", "run_check_orphans_are_deleted.js")
        super().__init__(hook_logger, fixture, js_filename, description,
                         shell_options=shell_options)
