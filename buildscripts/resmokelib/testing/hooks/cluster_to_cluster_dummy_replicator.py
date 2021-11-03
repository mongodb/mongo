"""Test implementation for running the dummy replicator on two clusters."""
import copy
import os.path

from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.hooks import replicator_interface


class DummyReplicator(replicator_interface.ReplicatorInterface):
    """A dummy implementation of the replicator."""

    def __init__(self, logger, fixture):
        """Initialize the dummy replicator."""
        replicator_interface.ReplicatorInterface.__init__(self, logger, fixture)

    def start(self, start_options=None):
        """Start the dummy replicator."""
        self.logger.info("Starting the dummy replicator (NOOP).")

    def pause(self, pause_options=None):
        """Pause the dummy replicator."""
        self.logger.info("Pausing the dummy replicator (NOOP).")

    def resume(self, resume_options=None):
        """Resume the replicator."""
        self.logger.info("Resuming the dummy replicator (NOOP).")

    def stop(self, stop_options=None):
        """Stop the replicator."""
        # Since the dummy replicator doesn't run while the cluster is live, we run the dummy
        # replicator from start to finish here instead.
        self.logger.info("Stopping and synchronizing the dummy replicator.")
        replicator_runner = DummyReplicator._ClusterToClusterDummyReplicatorHook(
            self.logger, self._fixture, stop_options["shell_options"])
        replicator_runner.before_suite(stop_options["test_report"])
        replicator_runner.before_test(stop_options["test"], stop_options["test_report"])
        replicator_runner.after_test(stop_options["test"], stop_options["test_report"])
        replicator_runner.after_suite(stop_options["test_report"])
        self.logger.info("Stopped and synchronized the dummy replicator.")

    class _ClusterToClusterDummyReplicatorHook(jsfile.JSHook):
        """A hook that the DummyReplicator uses internally to copy documents from cluster to cluster."""

        IS_BACKGROUND = False

        def __init__(  # pylint: disable=super-init-not-called
                self, hook_logger, fixture, shell_options=None):
            """Initialize ClusterToClusterDummyReplicator."""
            description = "Run the dummy cluster to cluster replicator between two clusters."
            js_filename = os.path.join("jstests", "hooks", "dummy_cluster_to_cluster_replicator.js")
            jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
                self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
