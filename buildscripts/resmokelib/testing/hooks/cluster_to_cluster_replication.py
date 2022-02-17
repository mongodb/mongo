"""Test hook that runs cluster to cluster replications continuously."""

import copy
import math
import random

from buildscripts.resmokelib import config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import cluster_to_cluster
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import cluster_to_cluster_data_consistency
from buildscripts.resmokelib.testing.hooks import dbhash


class ClusterToClusterReplication(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Starts a cluster to cluster replication thread at the beginning of each test."""

    DESCRIPTION = ("Continuous cluster to cluster replications")

    IS_BACKGROUND = True
    # By default, we pause / stop the replicator at the end of the suite and then perform data
    # consistency checks.
    DEFAULT_TESTS_PER_CYCLE = math.inf

    def __init__(self, hook_logger, fixture, shell_options, tests_per_cycle=None,
                 replicator_start_delay=None):
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
        self._tests_per_cycle = self.DEFAULT_TESTS_PER_CYCLE if tests_per_cycle is None else tests_per_cycle
        # The replicator is not started until some number of tests are run first.
        self._replicator_start_delay = replicator_start_delay
        random.seed(config.RANDOM_SEED)

        # The last test executed so far.
        self._last_test = None

        self._replicator = self._fixture.replicator

    def before_suite(self, test_report):
        """Before suite."""
        if not self._fixture:
            raise ValueError("No ClusterToClusterFixture to run migrations on")

        if self._replicator_start_delay is None:
            if math.isinf(self._tests_per_cycle):
                self._replicator_start_delay = random.randint(0, 10)
            else:
                self._replicator_start_delay = random.randint(0, self._tests_per_cycle)
        self.logger.info("Starting the replicator after %d tests are run.",
                         self._replicator_start_delay)

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Ran %d tests in total.", self._test_num)
        # Perform the following actions only if some tests have been run.
        if self._test_num % self._tests_per_cycle > self._replicator_start_delay:
            self._run_replicator_action(test_report, self._replicator.commit)

            self._run_data_consistency_check(self._last_test, test_report)
            self._run_check_repl_db_hash(self._last_test, test_report)

    def before_test(self, test, test_report):
        """Before test."""
        if self._test_num % self._tests_per_cycle == self._replicator_start_delay:
            self._run_replicator_action(test_report, self._replicator.start)

    def after_test(self, test, test_report):
        """After test."""
        self._test_num += 1
        self._last_test = test

        # Every 'n' tests, the replicator should be pause / stop the replicator and perform data
        # consistency checks.
        if self._test_num % self._tests_per_cycle == 0:
            if self._tests_per_cycle == self._replicator_start_delay:
                self._run_replicator_action(test_report, self._replicator.start)

            self._run_replicator_action(test_report, self._replicator.commit)

            self._run_data_consistency_check(test, test_report)
            self._run_check_repl_db_hash(test, test_report)

    def _run_data_consistency_check(self, test, test_report):
        """Run the data consistency check across both clusters."""
        # The TestData needs to be set to allow the data consistency hooks to run correctly.
        clusters = self._fixture.get_independent_clusters()
        source_url = clusters[self._fixture.source_cluster_index].get_driver_connection_url()
        dest_url = clusters[1 - self._fixture.source_cluster_index].get_driver_connection_url()

        shell_options = copy.deepcopy(self._shell_options)
        shell_options["global_vars"]["TestData"]["sourceConnectionString"] = source_url
        shell_options["global_vars"]["TestData"]["destinationConnectionString"] = dest_url

        data_consistency = cluster_to_cluster_data_consistency.CheckClusterToClusterDataConsistency(
            self.logger, self._fixture, shell_options)
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
