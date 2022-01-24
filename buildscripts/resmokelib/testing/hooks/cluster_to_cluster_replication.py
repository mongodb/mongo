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
    # By default, we pause / stop the replicator at the end of the suite and then perform data
    # consistency checks.
    DEFAULT_TESTS_PER_CYCLE = math.inf

    def __init__(self, hook_logger, fixture, shell_options, restart_every_cycle=None,
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

        self._replicator = self._fixture.replicator

        self._before_cycle_replicator_action = self._replicator.resume
        self._after_cycle_replicator_action = self._replicator.pause

        # When 'True', stops and starts the replicator every cycle instead of pausing and resuming.
        self._restart_every_cycle = False if restart_every_cycle is None else restart_every_cycle
        if self._restart_every_cycle:
            self._before_cycle_replicator_action = self._replicator.start
            self._after_cycle_replicator_action = self._replicator.stop

    def before_suite(self, test_report):
        """Before suite."""
        if not self._fixture:
            raise ValueError("No ClusterToClusterFixture to run migrations on")

        self.logger.info("Setting up cluster to cluster test data.")

        # Set up the initial replication direction.
        clusters = self._fixture.get_independent_clusters()
        self._source_cluster = clusters[self._fixture.source_cluster_index]
        self._destination_cluster = clusters[1 - self._fixture.source_cluster_index]

        source_url = self._source_cluster.get_driver_connection_url()
        dest_url = self._destination_cluster.get_driver_connection_url()
        self.logger.info("Setting source cluster string: '%s', destination cluster string: '%s'",
                         source_url, dest_url)

        # The TestData needs to be set to allow the data consistency hooks to run correctly.
        self._shell_options["global_vars"]["TestData"]["sourceConnectionString"] = source_url
        self._shell_options["global_vars"]["TestData"]["destinationConnectionString"] = dest_url

        self._replicator.set_cli_options({'sourceURI': source_url, 'destinationURI': dest_url})

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        # Stop the replicator only if it hasn't been stopped already.
        if self._test_num % self._tests_per_cycle != 0 or not self._restart_every_cycle:
            self._run_replicator_action(test_report, self._replicator.stop)

        self._run_data_consistency_check(self._last_test, test_report)
        self._run_check_repl_db_hash(self._last_test, test_report)

    def before_test(self, test, test_report):
        """Before test."""
        if self._test_num == 0:
            self._run_replicator_action(test_report, self._replicator.start)
            return

        if self._test_num % self._tests_per_cycle == 0:
            # The replicator should be told to start running once again.
            self._run_replicator_action(test_report, self._before_cycle_replicator_action)

    def after_test(self, test, test_report):
        """After test."""
        self._test_num += 1
        self._last_test = test

        # Every 'n' tests, the replicator should be pause / stop the replicator and perform data
        # consistency checks.
        if self._test_num % self._tests_per_cycle == 0:
            self._run_replicator_action(test_report, self._after_cycle_replicator_action)

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
        check_db_hash = dbhash.CheckReplDBHash(self.logger, self._fixture, self._shell_options)
        check_db_hash.before_suite(test_report)
        check_db_hash.before_test(test, test_report)
        check_db_hash.after_test(test, test_report)
        check_db_hash.after_suite(test_report)

    def _run_replicator_action(self, test_report, action):
        self.logger.info(f"Running replicator action: {action.__name__}")
        replicator_action_case = _ReplicatorActionTestCase(self.logger, self._last_test, self,
                                                           action)
        replicator_action_case.run_dynamic_test(test_report)
        self.logger.info(f"Ran replicator action: {action.__name__}")


class _ReplicatorActionTestCase(interface.DynamicTestCase):
    """_ReplicatorActionTestCase class, to run a replicator action as a test."""

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, base_test_name, hook, action):
        """Initialize _ReplicatorActionTestCase."""
        interface.DynamicTestCase.__init__(self, logger, f"replicator_action:{action.__name__}",
                                           "Run a replicator action.", base_test_name, hook)
        self._action = action

    def run_test(self):
        try:
            self._action()
        except:
            self.logger.exception("Failed to run replicator action '%s'.", self._action)
            raise
