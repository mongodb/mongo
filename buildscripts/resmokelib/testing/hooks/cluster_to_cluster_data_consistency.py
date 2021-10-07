"""Test hook for verifying data consistency between two clusters in a cluster to cluster replication."""
import copy
import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class CheckClusterToClusterDataConsistency(jsfile.DataConsistencyHook):
    """Check if the two clusters have the same data.

    This includes metadata such as the shard key (where applicable), indexes, collection options,
    etc.
    """

    IS_BACKGROUND = False

    def __init__(  # pylint: disable=super-init-not-called
            self, hook_logger, fixture, shell_options=None):
        """Initialize CheckClusterToClusterDataConsistency."""
        description = "Ensure that both clusters are data consistent by querying both clusters."
        js_filename = os.path.join("jstests", "hooks", "run_cluster_to_cluster_data_consistency.js")
        jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
