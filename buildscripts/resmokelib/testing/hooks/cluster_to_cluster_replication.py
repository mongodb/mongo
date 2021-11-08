"""Test hook that runs cluster to cluster replications continuously."""

import copy
import math

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import cluster_to_cluster
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import cluster_to_cluster_data_consistency
from buildscripts.resmokelib.testing.hooks import cluster_to_cluster_dummy_replicator
from buildscripts.resmokelib.testing.hooks import dbhash


class ClusterToClusterReplication(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Starts a cluster to cluster replication thread at the beginning of each test."""

    DESCRIPTION = ("Continuous cluster to cluster replications")

    IS_BACKGROUND = True
    # By default, we check the consistency only at the end of the suite to accommodate for the dummy
    # replicator which has no pause / resume functionality.
    DEFAULT_TESTS_PER_CYCLE = math.inf

    def __init__(self, hook_logger, fixture, shell_options,
                 tests_per_cycle=DEFAULT_TESTS_PER_CYCLE):
        """Initialize the ClusterToClusterReplication.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target ClusterToCluster fixture containing two clusters.
            shell_options: contains the global_vars.
        """
        interface.Hook.__init__(self, hook_logger, fixture, ClusterToClusterReplication.DESCRIPTION)

        if not isinstance(fixture, cluster_to_cluster.ClusterToClusterFixture):
            raise ValueError(
                "The ClusterToClusterReplication hook requires a ClusterToClusterFixture")
        self._fixture = fixture
        self._shell_options = copy.deepcopy(shell_options)

        # The number of tests executing so far.
        self._test_num = 0
        # The number of tests we execute before running a data consistency check and restarting the
        # replicator.
        self._tests_per_cycle = tests_per_cycle

        self._source_cluster = None
        self._destination_cluster = None

        # The last test executed so far.
        self._last_test = None

        self._replicator = cluster_to_cluster_dummy_replicator.DummyReplicator(hook_logger, fixture)

    def before_suite(self, test_report):
        """Before suite."""
        if not self._fixture:
            raise ValueError("No ClusterToClusterFixture to run migrations on")

        self.logger.info("Setting up cluster to cluster test data.")

        # Set up the initial replication direction.
        clusters = self._fixture.get_independent_clusters()
        self._source_cluster = clusters[0]
        self._destination_cluster = clusters[1]

        self._shell_options["global_vars"]["TestData"][
            "sourceConnectionString"] = self._source_cluster.get_driver_connection_url()
        self._shell_options["global_vars"]["TestData"][
            "destinationConnectionString"] = self._destination_cluster.get_driver_connection_url()

        self.logger.info(
            "Setting source cluster string: '%s', destination cluster string: '%s'",
            self._shell_options["global_vars"]["TestData"]["sourceConnectionString"],
            self._shell_options["global_vars"]["TestData"]["destinationConnectionString"])

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        # If the total number of tests was not an exact multiple of the number run per cycle, then
        # pause the replicator first.
        if self._test_num % self._tests_per_cycle != 0:
            # Pause the dummy replicator first.
            self.logger.info("Pausing replicator before stopping.")
            self._run_replicator_action(test_report, self._replicator.pause)
            self.logger.info("Finished pausing the replicator.")

        self.logger.info("Stopping the cluster to cluster replicator.")
        stop_options = {
            "test": self._last_test, "test_report": test_report,
            "shell_options": self._shell_options
        }
        self._run_replicator_action(test_report, self._replicator.stop, stop_options)
        self.logger.info("Stopped the cluster to cluster replicator.")

        self._run_data_consistency_check(self._last_test, test_report)
        self._run_check_repl_db_hash(self._last_test, test_report)

    def before_test(self, test, test_report):
        """Before test."""
        if self._test_num == 0:
            self.logger.info("Starting the replicator.")
            self._run_replicator_action(test_report, self._replicator.start)
            self.logger.info("Started the replicator.")
            return

        if self._test_num % self._tests_per_cycle == 0:
            # The replicator should be told to start running once again.
            self.logger.info("Resuming the cluster to cluster replicator.")
            self._run_replicator_action(test_report, self._replicator.resume)
            self.logger.info("Resumed the cluster to cluster replicator.")

    def after_test(self, test, test_report):
        """After test."""
        self._test_num += 1
        self._last_test = test

        # Every 'n' tests, the replicator should be paused after allowing it to finish synchronizing
        # data across the clusters, so that a data consistency check can be performed.
        if self._test_num % self._tests_per_cycle == 0:
            self.logger.info("Pausing the cluster to cluster replicator.")
            self._run_replicator_action(test_report, self._replicator.pause)
            self.logger.info("Paused the cluster to cluster replicator.")

            self._run_data_consistency_check(test, test_report)
            self._run_check_repl_db_hash(test, test_report)

    def _run_data_consistency_check(self, test, test_report):
        """Run the data consistency check across both clusters."""
        data_consistency = cluster_to_cluster_data_consistency.CheckClusterToClusterDataConsistency(
            self.logger, self._fixture, self._shell_options)
        data_consistency.before_suite(test_report)
        data_consistency.before_test(test, test_report)
        data_consistency.after_test(test, test_report)
        data_consistency.after_suite(test_report)

    def _run_check_repl_db_hash(self, test, test_report):
        """Check the repl DB hash on each cluster."""
        check_db_hash = dbhash.CheckReplDBHash(self.logger, self._fixture)
        check_db_hash.before_suite(test_report)
        check_db_hash.before_test(test, test_report)
        check_db_hash.after_test(test, test_report)
        check_db_hash.after_suite(test_report)

    def _run_replicator_action(self, test_report, action, action_options=None):
        replicator_action_case = _ReplicatorActionTestCase(self.logger, self._last_test, self,
                                                           action, action_options)
        replicator_action_case.run_dynamic_test(test_report)


class _ReplicatorActionTestCase(interface.DynamicTestCase):
    """_ReplicatorActionTestCase class, to run a replicator action as a test."""

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, base_test_name, hook, action, action_options):
        """Initialize _ReplicatorActionTestCase."""
        interface.DynamicTestCase.__init__(self, logger, f"replicator_action:{action.__name__}",
                                           "Run a replicator action.", base_test_name, hook)
        self._action = action
        self._action_options = action_options

    def run_test(self):
        try:
            self._action(self._action_options)
        except:
            self.logger.exception("Failed to run replicator action '%s' with options '%s'",
                                  self._action, self._action_options)
            raise
