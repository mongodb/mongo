"""Test hook that periodically makes the primary of a replica set step down."""

import copy
import random
import sys
import threading
import time

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.mongo_fuzzer_configs import generate_normal_mongo_parameters
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset, shardedcluster, standalone
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


def validate_runtime_parameter_spec(spec):
    for key, value in spec.items():
        if not (isinstance(value, dict) and value.get("period", 0) >= 1):
            raise ValueError(
                f"Invalid runtime parameter fuzz config entry for key '{key}' : {value}"
            )


class RuntimeParametersState:
    """Encapsulates the runtime-state of a set of parameters we are fuzzing. Tracks the last time we set a parameter value and holds
    the logic for generating new values."""

    def __init__(self, spec, seed):
        # Initialize the runtime state of each parameter in the spec, including the lastSet time at now, so we start setting the parameters
        # at appropriate intervals after the suite begins.
        now = time.time()
        self._params = {
            key: {**copy.deepcopy(value), "lastSet": now} for key, value in spec.items()
        }
        self._rng = random.Random(seed)

    def generate_parameters(self):
        """Returns a dictionary of what parameters should be set now, along with values to set them to, based on the last time the
        parameter was set and the period provided in the spec"""
        ret = {}
        now = time.time()
        for key, value in self._params.items():
            if now - value["lastSet"] >= value["period"]:
                ret[key] = generate_normal_mongo_parameters(self._rng, value)
                value["lastSet"] = now
        return ret

    def get_spec(self):
        """Return a dictionary of all parameters subject to runtime fuzzing suitable for use with getParameter."""
        return {key: 1 for key in self._params.keys()}


class FuzzRuntimeParameters(interface.Hook):
    """Regularly connect to nodes and sends them a setParameter command."""

    DESCRIPTION = "Changes the value of runtime-settable parameters at regular intervals"

    IS_BACKGROUND = True

    def __init__(
        self,
        hook_logger,
        fixture,
        seed=random.randrange(sys.maxsize),
        auth_options=None,
    ):
        """Initialize the FuzzRuntimeParameters.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (standalone, replica set, sharded cluster, or multi-cluster fixture).
            auth_options: dictionary of auth options.

        """
        interface.Hook.__init__(self, hook_logger, fixture, FuzzRuntimeParameters.DESCRIPTION)
        self._mongod_param_state = None
        self._seed = seed

        self._fixture = fixture

        self._standalone_fixtures = []
        self._rs_fixtures = []
        self._mongos_fixtures = []
        self._setParameter_thread = None

        self._auth_options = auth_options

    def before_suite(self, test_report):
        """Before suite."""
        self._add_fixture(self._fixture)

        from buildscripts.resmokelib.config_fuzzer_limits import (
            config_fuzzer_params,
        )

        # Get only the mongod and mongos parameters that have "runtime" in the "fuzz_at" param value.
        runtime_mongod_params = {
            param: val
            for param, val in config_fuzzer_params["mongod"].items()
            if "runtime" in val.get("fuzz_at", [])
        }
        runtime_mongos_params = {
            param: val
            for param, val in config_fuzzer_params["mongos"].items()
            if "runtime" in val.get("fuzz_at", [])
        }

        validate_runtime_parameter_spec(runtime_mongod_params)
        validate_runtime_parameter_spec(runtime_mongos_params)
        # Construct the runtime state before the suite begins.
        # The initial lastSet time of each parameter is the start time of the suite.
        self._mongod_param_state = RuntimeParametersState(runtime_mongod_params, self._seed)

        self._mongos_param_state = RuntimeParametersState(runtime_mongos_params, self._seed)

        self._setParameter_thread = _SetParameterThread(
            self.logger,
            self._mongos_fixtures,
            self._rs_fixtures,
            self._standalone_fixtures,
            self._fixture,
            self._mongod_param_state,
            self._mongos_param_state,
            lifecycle_interface.FlagBasedThreadLifecycle(),
            self._auth_options,
        )
        self.logger.info("Starting the runtime parameter fuzzing thread.")
        self._setParameter_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the runtime parameter fuzzing thread.")
        self._setParameter_thread.stop()
        self.logger.info("Runtime parameter fuzzing thread stopped.")

    def before_test(self, test, test_report):
        """Before test. Log current config of all runtime-fuzzable params."""
        for repl_set in self._rs_fixtures:
            for node in repl_set.nodes:
                self._invoke_get_parameter_and_log(node)

        for standalone in self._standalone_fixtures:
            self._invoke_get_parameter_and_log(standalone)

        for mongos in self._mongos_fixtures:
            self._invoke_get_parameter_and_log(mongos)

        self.logger.info("Resuming the runtime parameter fuzzing thread.")
        self._setParameter_thread.pause()
        self._setParameter_thread.resume()

    def after_test(self, test, test_report):
        """After test. Log current config of all runtime-fuzzable params."""
        self.logger.info("Pausing the runtime parameter fuzzing thread.")
        self._setParameter_thread.pause()
        self.logger.info("Paused the runtime parameter fuzzing thread.")

        for repl_set in self._rs_fixtures:
            for node in repl_set.nodes:
                self._invoke_get_parameter_and_log(node)

        for standalone in self._standalone_fixtures:
            self._invoke_get_parameter_and_log(standalone)

        for mongos in self._mongos_fixtures:
            self._invoke_get_parameter_and_log(mongos)

    def _add_fixture(self, fixture):
        if isinstance(fixture, standalone.MongoDFixture):
            self._standalone_fixtures.append(fixture)
        elif isinstance(fixture, replicaset.ReplicaSetFixture):
            self._rs_fixtures.append(fixture)
        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            for shard_fixture in fixture.shards:
                self._add_fixture(shard_fixture)
            if fixture.config_shard is None:
                self._add_fixture(fixture.configsvr)
            for mongos_fixture in fixture.mongos:
                self._mongos_fixtures.append(mongos_fixture)
        elif isinstance(fixture, fixture_interface.MultiClusterFixture):
            # Recursively call _add_fixture on all the independent clusters.
            for cluster_fixture in fixture.get_independent_clusters():
                self._add_fixture(cluster_fixture)
        else:
            raise ValueError("No fixture to run setParameter on.")

    def _invoke_get_parameter_and_log(self, node):
        """Helper to print the current state of a node's runtime-fuzzable parameters. Only usable once before_suite has initialized the runtime state of the parameters."""
        client = fixture_interface.build_client(node, self._auth_options)
        params_to_get = (
            self._mongos_param_state.get_spec()
            if client.is_mongos
            else self._mongod_param_state.get_spec()
        )
        get_result = client.admin.command("getParameter", 1, **params_to_get)
        self.logger.info(
            "Current state of runtime-fuzzable parameters on node on port %d. Parameters: %s",
            node.port,
            get_result,
        )


class _SetParameterThread(threading.Thread):
    def __init__(
        self,
        logger,
        mongos_fixtures,
        rs_fixtures,
        standalone_fixtures,
        fixture,
        mongod_param_state,
        mongos_param_state,
        lifecycle,
        auth_options=None,
    ):
        """Initialize _SetParameterThread."""
        threading.Thread.__init__(self, name="SetParameterThread")
        self.daemon = True
        self.logger = logger
        self._mongos_fixtures = mongos_fixtures
        self._rs_fixtures = rs_fixtures
        self._standalone_fixtures = standalone_fixtures
        self._fixture = fixture
        self._mongod_param_state = mongod_param_state
        self._mongos_param_state = mongos_param_state
        self.__lifecycle = lifecycle
        self._auth_options = auth_options
        self._setparameter_interval_secs = 1

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
                    break

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
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("SetParameter thread threw exception")
            # The event should be signaled whenever the thread is not performing stepdowns.
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

        # Wait until we are no longer executing stepdowns.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()

        # Check that fixtures are still running
        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    "ReplicaSetFixture with pids {} expected to be running in"
                    " SetParameter, but wasn't.".format(rs_fixture.pids())
                )
        for mongos_fixture in self._mongos_fixtures:
            if not mongos_fixture.is_running():
                raise errors.ServerFailure(
                    "MongoSFixture with pids {} expected to be running in"
                    " SetParameter, but wasn't.".format(mongos_fixture.pids())
                )

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        # Wait until stop or timeout.
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        if not self.is_alive():
            msg = "The setParameter thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _do_set_parameter(self):
        mongod_params_to_set = self._mongod_param_state.generate_parameters()
        mongos_params_to_set = self._mongos_param_state.generate_parameters()

        def invoke_set_parameter(client, params):
            # Do nothing if there are no params to set this iteration.
            if not params:
                return
            client.admin.command("setParameter", 1, **params)

        for repl_set in self._rs_fixtures:
            self.logger.info(
                "Setting parameters on all nodes of replica set %s. Parameters: %s",
                repl_set.replset_name,
                mongod_params_to_set,
            )
            for node in repl_set.nodes:
                invoke_set_parameter(
                    fixture_interface.build_client(node, self._auth_options), mongod_params_to_set
                )

        for standalone in self._standalone_fixtures:
            self.logger.info(
                "Setting parameters on standalone on port %d. Parameters: %s",
                standalone.port,
                mongod_params_to_set,
            )
            invoke_set_parameter(
                fixture_interface.build_client(standalone, self._auth_options), mongod_params_to_set
            )

        for mongos in self._mongos_fixtures:
            self.logger.info(
                "Setting parameters on mongos port %d. Parameters: %s",
                mongos.port,
                mongos_params_to_set,
            )
            invoke_set_parameter(
                fixture_interface.build_client(mongos, self._auth_options), mongos_params_to_set
            )
