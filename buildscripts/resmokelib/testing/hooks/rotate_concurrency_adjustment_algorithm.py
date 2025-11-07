"""
Hook that periodically rotates the 'storageEngineConcurrencyAdjustmentAlgorithm' server parameter to
a new random valid value on all mongod processes.
"""

import random
import sys
import threading
import time

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset, shardedcluster, standalone
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class RotateConcurrencyAdjustmentAlgorithm(interface.Hook):
    """
    Periodically sets 'storageEngineConcurrencyAdjustmentAlgorithm' to a random valid value from a
    predefined list.
    """

    DESCRIPTION = (
        "Periodically rotates 'storageEngineConcurrencyAdjustmentAlgorithm' to a random valid value"
    )
    IS_BACKGROUND = True

    # The list of valid values to choose from.
    _ALGORITHM_OPTIONS = [
        "fixedConcurrentTransactions",
        "fixedConcurrentTransactionsWithPrioritization",
        "throughputProbing",
    ]

    def __init__(
        self,
        hook_logger,
        fixture,
        seed=random.randrange(sys.maxsize),
        auth_options=None,
    ):
        """Initialize the RotateConcurrencyAdjustmentAlgorithm hook.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (standalone, replica set, sharded cluster, or multi-cluster fixture).
            seed: the random seed to use.
            auth_options: dictionary of auth options.
        """
        interface.Hook.__init__(
            self, hook_logger, fixture, RotateConcurrencyAdjustmentAlgorithm.DESCRIPTION
        )
        self._fixture = fixture
        self._auth_options = auth_options
        self._rng = random.Random(seed)

        self._standalone_fixtures = []
        self._rs_fixtures = []
        self._set_param_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        self.logger.info("Finding all mongod fixtures to target...")
        for cluster in self._fixture.get_testable_clusters():
            self._add_fixture(cluster)

        self.logger.info(
            f"Found {len(self._standalone_fixtures)} standalone and {len(self._rs_fixtures)} replica set fixtures."
        )

        self._set_param_thread = _SetConcurrencyAlgorithmThread(
            self.logger,
            self._rs_fixtures,
            self._standalone_fixtures,
            self._rng,
            self._ALGORITHM_OPTIONS,
            lifecycle_interface.FlagBasedThreadLifecycle(),
            self._auth_options,
        )
        self.logger.info("Starting the concurrency adjustment algorithm rotation thread.")
        self._set_param_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the concurrency adjustment algorithm rotation thread.")
        if self._set_param_thread:
            self._set_param_thread.stop()
        self.logger.info("Concurrency adjustment algorithm rotation thread stopped.")

    def before_test(self, test, test_report):
        """Before test. Log current config."""
        self.logger.info("Logging current parameter state before test...")
        for repl_set in self._rs_fixtures:
            for node in repl_set.nodes:
                self._invoke_get_parameter_and_log(node)

        for standalone in self._standalone_fixtures:
            self._invoke_get_parameter_and_log(standalone)

        self.logger.info("Resuming the concurrency adjustment algorithm rotation thread.")
        self._set_param_thread.pause()
        self._set_param_thread.resume()

    def after_test(self, test, test_report):
        """After test. Log current config."""
        self.logger.info("Pausing the concurrency adjustment algorithm rotation thread.")
        self._set_param_thread.pause()
        self.logger.info("Paused the concurrency adjustment algorithm rotation thread.")

        self.logger.info("Logging current parameter state after test...")
        for repl_set in self._rs_fixtures:
            for node in repl_set.nodes:
                self._invoke_get_parameter_and_log(node)

        for standalone in self._standalone_fixtures:
            self._invoke_get_parameter_and_log(standalone)

    def _add_fixture(self, fixture):
        """
        Recursively find and add all mongod fixtures (standalone or replicaset) to our internal lists.
        """
        if isinstance(fixture, standalone.MongoDFixture):
            self._standalone_fixtures.append(fixture)
        elif isinstance(fixture, replicaset.ReplicaSetFixture):
            self._rs_fixtures.append(fixture)
        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            # Recurse into shards
            for shard_fixture in fixture.shards:
                self._add_fixture(shard_fixture)

            # Recurse into config server
            if fixture.config_shard is None:
                self._add_fixture(fixture.configsvr)

            # We intentionally DO NOT add fixture.mongos, as the parameter is not valid on mongos.
        else:
            # This could be a direct MongoSFixture or other non-mongod fixture.
            self.logger.debug(f"Skipping fixture {fixture} as it is not a mongod.")

    def _invoke_get_parameter_and_log(self, node):
        """
        Helper to print the current state of the 'storageEngineConcurrencyAdjustmentAlgorithm' parameter.
        """
        client = fixture_interface.build_client(node, self._auth_options)
        try:
            get_result = client.admin.command(
                "getParameter", 1, storageEngineConcurrencyAdjustmentAlgorithm=1
            )
            self.logger.info(
                "Current state of 'storageEngineConcurrencyAdjustmentAlgorithm' on node %d: %s",
                node.port,
                get_result.get("storageEngineConcurrencyAdjustmentAlgorithm", "NOT_FOUND"),
            )
        except Exception as e:
            self.logger.warning(
                "Failed to getParameter 'storageEngineConcurrencyAdjustmentAlgorithm' from node %d: %s",
                node.port,
                e,
            )


class _SetConcurrencyAlgorithmThread(threading.Thread):
    def __init__(
        self,
        logger,
        rs_fixtures,
        standalone_fixtures,
        rng,
        algorithm_options,
        lifecycle,
        auth_options=None,
    ):
        """Initialize _SetConcurrencyAlgorithmThread."""
        threading.Thread.__init__(self, name="RotateConcurrencyAlgorithmThread")
        self.daemon = True
        self.logger = logger
        self._rs_fixtures = rs_fixtures
        self._standalone_fixtures = standalone_fixtures
        self._rng = rng
        self._algorithm_options = algorithm_options
        self.__lifecycle = lifecycle
        self._auth_options = auth_options
        self._setparameter_interval_secs = 30  # Set parameter every 30 seconds
        self._last_exec = time.time()

        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing stepdowns.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

    def run(self):
        """Execute the thread."""
        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break  # Thread was stopped

                self._is_idle_evt.clear()

                now = time.time()
                if now - self._last_exec > self._setparameter_interval_secs:
                    self._do_set_parameter()
                    self._last_exec = time.time()

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                # The 'wait_secs' is used to wait 'self._setparameter_interval_secs' from the moment
                # the last setParameter command was sent.
                now = time.time()
                wait_secs = max(0, self._setparameter_interval_secs - (now - self._last_exec))
                self.__lifecycle.wait_for_action_interval(wait_secs)
        except Exception:
            # Proactively log the exception
            self.logger.exception("RotateConcurrencyAlgorithmThread threw exception")
            self._is_idle_evt.set()

    def stop(self):
        """Stop the thread."""
        self.__lifecycle.stop()
        self._is_stopped_evt.set()
        # Unpause to allow the thread to finish.
        self.resume()
        self.join()

    def pause(self):
        """Pause the thread."""
        self.__lifecycle.mark_test_finished()

        # Wait until we are no longer executing setParameter.
        self._is_idle_evt.wait()
        # Check if the thread is alive
        self._check_thread()

        # Check that fixtures are still running
        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    f"ReplicaSetFixture with pids {rs_fixture.pids()} expected to be running in"
                    " SetParameter, but wasn't."
                )

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        # Wait until stop or timeout.
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        if not self.is_alive():
            msg = "The RotateConcurrencyAlgorithmThread thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _invoke_set_parameter(self, client, params):
        """Helper to invoke setParameter on a given client."""
        client.admin.command("setParameter", 1, **params)

    def _do_set_parameter(self):
        """
        Picks a new random algorithm and applies it to all standalone and replica set nodes.
        """
        new_algorithm = self._rng.choice(self._algorithm_options)
        params_to_set = {"storageEngineConcurrencyAdjustmentAlgorithm": new_algorithm}

        for repl_set in self._rs_fixtures:
            self.logger.info(
                "Setting parameters on all nodes of replica set %s. Parameters: %s",
                repl_set.replset_name,
                params_to_set,
            )
            for node in repl_set.nodes:
                client = fixture_interface.build_client(node, self._auth_options)
                self._invoke_set_parameter(client, params_to_set)

        for standalone in self._standalone_fixtures:
            self.logger.info(
                "Setting parameters on standalone on port %d. Parameters: %s",
                standalone.port,
                params_to_set,
            )
            client = fixture_interface.build_client(standalone, self._auth_options)
            self._invoke_set_parameter(client, params_to_set)
