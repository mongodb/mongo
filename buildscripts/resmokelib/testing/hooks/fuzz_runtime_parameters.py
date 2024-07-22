"""Test hook that periodically makes the primary of a replica set step down."""

import collections
import os.path
import random
import threading
import time

import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.fixtures import standalone
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class FuzzRuntimeParameters(interface.Hook):
    """Regularly connect to nodes and sends them a setParameter command."""

    DESCRIPTION = "Changes the value of runtime-settable parameters at regular intervals"

    IS_BACKGROUND = True

    def __init__(
        self,
        hook_logger,
        fixture,
        auth_options=None,
    ):
        """Initialize the FuzzRuntimeParameters.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (standalone, replica set, sharded cluster, or multi-cluster fixture).
            auth_options: dictionary of auth options.

        """
        interface.Hook.__init__(self, hook_logger, fixture, FuzzRuntimeParameters.DESCRIPTION)

        self._fixture = fixture

        self._standalone_fixtures = []
        self._rs_fixtures = []
        self._mongos_fixtures = []
        self._setParameter_thread = None

        self._auth_options = auth_options

    def before_suite(self, test_report):
        """Before suite."""
        self._add_fixture(self._fixture)

        self._setParameter_thread = _SetParameterThread(
            self.logger,
            self._mongos_fixtures,
            self._rs_fixtures,
            self._standalone_fixtures,
            self._fixture,
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
        """Before test."""
        self.logger.info("Resuming the runtime parameter fuzzing thread.")
        self._setParameter_thread.pause()
        self._setParameter_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the runtime parameter fuzzing thread.")
        self._setParameter_thread.pause()
        self.logger.info("Paused the runtime parameter fuzzing thread.")

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


class _SetParameterThread(threading.Thread):
    def __init__(
        self,
        logger,
        mongos_fixtures,
        rs_fixtures,
        standalone_fixtures,
        fixture,
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
                    # TODO SERVER-91123 Choose an appropriate log verbosity to ensure we don't overwhelm the log.
                    self.logger.info("Changing a setParameter value at runtime")
                    self._do_set_parameter()
                    self._last_exec = time.time()
                    self.logger.info(
                        "Completed setParameter in %0d ms",
                        (self._last_exec - now) * 1000,
                    )

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
        # TODO SERVER-92714 Intelligently choose paramters to set based on some static configurations.
        # TODO SERVER-91123 Accept failure of the command in certain cases. This command can run
        # regardless of replication state but may fail on terminate primary suites.
        def invoke_set_parameter(client):
            client.admin.command("setParameter", 1, ShardingTaskExecutorPoolMinSize=1)

        for repl_set in self._rs_fixtures:
            self.logger.info(
                "Setting the parameter ShardingTaskExecutorPoolMinSize to value 1 on all nodes of fixture %s",
                repl_set.replset_name,
            )
            for node in repl_set.nodes:
                invoke_set_parameter(fixture_interface.build_client(node, self._auth_options))

        for standalone in self._standalone_fixtures:
            self.logger.info(
                "Setting the parameter ShardingTaskExecutorPoolMinSize to value 1 on standalone node on port %d",
                standalone.port,
            )
            invoke_set_parameter(fixture_interface.build_client(standalone, self._auth_options))
        # TODO SERVER-92715 Handle mongos processes.
