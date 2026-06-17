"""
Hook that periodically rotates the 'executionControlConcurrencyAdjustmentAlgorithm' and deprioritization
parameters to new random valid values on all mongod processes.
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


class RotateExecutionControlParams(interface.Hook):
    """
    Periodically sets 'executionControlConcurrencyAdjustmentAlgorithm' and deprioritization parameters
    to random valid values.
    """

    DESCRIPTION = "Periodically rotates 'executionControlConcurrencyAdjustmentAlgorithm' and deprioritization parameters to random valid values"
    IS_BACKGROUND = True

    # The list of valid values to choose from.
    _ALGORITHM_OPTIONS = [
        "fixedConcurrentTransactions",
        "throughputProbing",
    ]

    def __init__(
        self,
        hook_logger,
        fixture,
        seed=random.randrange(sys.maxsize),
        auth_options=None,
    ):
        """Initialize the RotateExecutionControlParams hook.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (standalone, replica set, sharded cluster, or multi-cluster fixture).
            seed: the random seed to use.
            auth_options: dictionary of auth options.
        """
        interface.Hook.__init__(
            self, hook_logger, fixture, RotateExecutionControlParams.DESCRIPTION
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

        self._set_param_thread = _RotateExecutionControlParamsThread(
            self.logger,
            self._rs_fixtures,
            self._standalone_fixtures,
            self._rng,
            self._ALGORITHM_OPTIONS,
            lifecycle_interface.FlagBasedThreadLifecycle(),
            self._auth_options,
        )
        self.logger.info("Starting the execution control parameters rotation thread.")
        self._set_param_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the execution control parameters rotation thread.")
        if self._set_param_thread:
            self._set_param_thread.stop()
        self.logger.info("Execution control parameters rotation thread stopped.")

    def before_test(self, test, test_report):
        """Before test. Log current config."""
        self.logger.info("Logging current parameter state before test...")
        for repl_set in self._rs_fixtures:
            for node in repl_set.nodes:
                self._invoke_get_parameter_and_log(node)

        for standalone in self._standalone_fixtures:
            self._invoke_get_parameter_and_log(standalone)

        self.logger.info("Resuming the execution control parameters rotation thread.")
        self._set_param_thread.pause()
        self._set_param_thread.resume()

    def after_test(self, test, test_report):
        """After test. Log current config."""
        self.logger.info("Pausing the execution control parameters rotation thread.")
        self._set_param_thread.pause()
        self.logger.info("Paused the execution control parameters rotation thread.")

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
        Helper to print the current state of the execution control parameters.
        """
        client = fixture_interface.build_hook_client(node, self._auth_options)
        try:
            algorithm_result = client.admin.command(
                "getParameter",
                1,
                executionControlConcurrencyAdjustmentAlgorithm=1,
            )
            heuristic_result = client.admin.command(
                "getParameter",
                1,
                executionControlHeuristicDeprioritization=1,
            )
            background_result = client.admin.command(
                "getParameter",
                1,
                executionControlBackgroundTasksDeprioritization=1,
            )
            deprioritization_result = client.admin.command(
                "getParameter",
                1,
                executionControlDeprioritizationGate=1,
            )
            self.logger.info(
                "Current state on node %d: algorithm=%s, heuristic=%s, background=%s, deprio=%s",
                node.port,
                algorithm_result.get("executionControlConcurrencyAdjustmentAlgorithm", "NOT_FOUND"),
                heuristic_result.get("executionControlHeuristicDeprioritization", "NOT_FOUND"),
                background_result.get(
                    "executionControlBackgroundTasksDeprioritization", "NOT_FOUND"
                ),
                deprioritization_result.get("executionControlDeprioritizationGate", "NOT_FOUND"),
            )
        except Exception as e:
            self.logger.warning(
                "Failed to getParameter from node %d: %s",
                node.port,
                e,
            )


class _RotateExecutionControlParamsThread(threading.Thread):
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
        """Initialize _RotateExecutionControlParamsThread."""
        threading.Thread.__init__(self, name="RotateExecutionControlParamsThread")
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

        self._pause_timeout_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        self._stop_timeout_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        self._thread_state = lifecycle_interface.HookThreadState()

    def run(self):
        """Execute the thread."""
        try:
            while True:
                self._thread_state.mark_idle("waiting_for_action_permitted")

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    self._thread_state.mark_stopped("lifecycle_stop_requested")
                    break  # Thread was stopped

                self._thread_state.mark_running("rotate_execution_control_cycle")

                now = time.time()
                if now - self._last_exec > self._setparameter_interval_secs:
                    self._thread_state.set_phase("do_set_parameter")
                    self._do_set_parameter()
                    self._last_exec = time.time()

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self._thread_state.set_phase("sending_idle_acknowledgement")
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                # The 'wait_secs' is used to wait 'self._setparameter_interval_secs' from the moment
                # the last setParameter command was sent.
                now = time.time()
                wait_secs = max(0, self._setparameter_interval_secs - (now - self._last_exec))
                self._thread_state.mark_idle("waiting_for_action_interval")
                self.__lifecycle.wait_for_action_interval(wait_secs)
        except Exception as err:
            # Proactively log the exception
            self.logger.exception("RotateExecutionControlParamsThread threw exception")
            self._thread_state.mark_failed(err, "run_loop")
        finally:
            state, _phase = self._thread_state.describe()
            if state not in ("failed", "stopped"):
                self._thread_state.mark_stopped("run_loop_exited")

    def stop(self):
        """Stop the thread."""
        self._thread_state.mark_stopping("stop_requested")
        self.__lifecycle.stop()
        # Unpause to allow the thread to finish.
        self.resume()
        self.join(self._stop_timeout_secs)
        if self.is_alive():
            state, phase = self._thread_state.describe()
            raise errors.ServerFailure(
                "Timed out waiting for RotateExecutionControlParamsThread to stop; "
                f"state={state}, phase={phase}, timeout={self._stop_timeout_secs}s."
            )
        self._thread_state.assert_healthy(
            self.is_alive(), "rotateExecutionControlParams", allow_stopped=True
        )

    def pause(self):
        """Pause the thread."""
        self.__lifecycle.mark_test_finished()

        # Wait until we are no longer executing setParameter.
        self._thread_state.wait_until_idle(self._pause_timeout_secs, "rotateExecutionControlParams")
        self._thread_state.assert_healthy(self.is_alive(), "rotateExecutionControlParams")

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

    def _invoke_set_parameter(self, client, param_name, param_value):
        """Helper to invoke setParameter on a given client for a single parameter."""
        client.admin.command("setParameter", 1, **{param_name: param_value})

    def _do_set_parameter(self):
        """
        Picks a new random algorithm and random boolean values for the deprioritization parameters,
        then applies them to all standalone and replica set nodes.
        """
        params_to_set = {
            "executionControlConcurrencyAdjustmentAlgorithm": self._rng.choice(
                self._algorithm_options
            ),
            "executionControlHeuristicDeprioritization": self._rng.choice([True, False]),
            "executionControlBackgroundTasksDeprioritization": self._rng.choice([True, False]),
            "executionControlDeprioritizationGate": self._rng.choice([True, False]),
        }

        for repl_set in self._rs_fixtures:
            self.logger.info(
                "Setting parameters on all nodes of replica set %s. Parameters: %s",
                repl_set.replset_name,
                params_to_set,
            )
            for node in repl_set.nodes:
                client = fixture_interface.build_hook_client(node, self._auth_options)
                for param_name, param_value in params_to_set.items():
                    self._invoke_set_parameter(client, param_name, param_value)

        for standalone in self._standalone_fixtures:
            self.logger.info(
                "Setting parameters on standalone on port %d. Parameters: %s",
                standalone.port,
                params_to_set,
            )
            client = fixture_interface.build_hook_client(standalone, self._auth_options)
            for param_name, param_value in params_to_set.items():
                self._invoke_set_parameter(client, param_name, param_value)
