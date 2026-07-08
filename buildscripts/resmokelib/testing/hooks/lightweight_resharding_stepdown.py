"""Test hook that periodically sends _shardsvrReshardingStepDown to shard primaries."""

import os.path
import threading
import time

import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset, shardedcluster
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class ContinuousLightweightReshardingStepdown(interface.Hook):
    """Regularly connect to shard primaries and send a _shardsvrReshardingStepDown command."""

    DESCRIPTION = (
        "Continuous lightweight resharding stepdown (steps down resharding PrimaryOnlyServices"
        " on shard primaries at regular intervals)"
    )

    IS_BACKGROUND = True
    STOPS_FIXTURE = False

    def __init__(
        self,
        hook_logger,
        fixture,
        config_stepdown=True,
        shard_stepdown=True,
        stepdown_interval_ms=800,
        is_fsm_workload=False,
        auth_options=None,
    ):
        """Initialize the ContinuousLightweightReshardingStepdown.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (a sharded cluster).
            config_stepdown: whether to stepdown resharding services on the config shard.
            shard_stepdown: whether to stepdown resharding services on shard replica sets.
            stepdown_interval_ms: the number of milliseconds between stepdowns.
            is_fsm_workload: whether the hook is running as an FSM workload is executing.
            auth_options: dictionary of auth options.
        """
        interface.Hook.__init__(
            self, hook_logger, fixture, ContinuousLightweightReshardingStepdown.DESCRIPTION
        )

        self._fixture = fixture
        if hasattr(fixture, "config_shard") and fixture.config_shard is not None and shard_stepdown:
            # If the config server is a shard, shard_stepdown implies config_stepdown.
            config_stepdown = shard_stepdown

        self._config_stepdown = config_stepdown
        self._shard_stepdown = shard_stepdown
        self._stepdown_interval_secs = float(stepdown_interval_ms) / 1000

        self._rs_fixtures = []
        self._stepdown_thread = None

        self._auth_options = auth_options

        # The action file names need to match the same construction as found in
        # jstests/concurrency/fsm_libs/resmoke_runner.js.
        dbpath_prefix = fixture.get_dbpath_prefix()

        if is_fsm_workload:
            self.__action_files = lifecycle_interface.ActionFiles._make(
                [
                    os.path.join(dbpath_prefix, field + "_" + self.__class__.__name__)
                    for field in lifecycle_interface.ActionFiles._fields
                ]
            )
        else:
            self.__action_files = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._rs_fixtures:
            for cluster in self._fixture.get_testable_clusters():
                self._add_fixture(cluster)

        if self.__action_files is not None:
            lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.__action_files)
        else:
            lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        self._stepdown_thread = _LightweightReshardingStepdownThread(
            self.logger,
            self._rs_fixtures,
            self._stepdown_interval_secs,
            lifecycle,
            self._auth_options,
        )
        self.logger.info("Starting the lightweight resharding stepdown thread.")
        self._stepdown_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the lightweight resharding stepdown thread.")
        self._stepdown_thread.stop()
        self.logger.info("Lightweight resharding stepdown thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the lightweight resharding stepdown thread.")
        self._stepdown_thread.pause()
        self._stepdown_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the lightweight resharding stepdown thread.")
        self._stepdown_thread.pause()
        self.logger.info("Paused the lightweight resharding stepdown thread.")

    def _add_fixture(self, fixture):
        if isinstance(fixture, replicaset.ReplicaSetFixture):
            self._rs_fixtures.append(fixture)
        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            if self._shard_stepdown:
                for shard_fixture in fixture.shards:
                    if fixture.config_shard is None or self._config_stepdown:
                        self._add_fixture(shard_fixture)
            if self._config_stepdown and fixture.config_shard is None:
                self._add_fixture(fixture.configsvr)


class _LightweightReshardingStepdownThread(threading.Thread):
    def __init__(
        self,
        logger,
        rs_fixtures,
        stepdown_interval_secs,
        stepdown_lifecycle,
        auth_options=None,
    ):
        """Initialize _LightweightReshardingStepdownThread."""
        threading.Thread.__init__(self, name="LightweightReshardingStepdownThread")
        self.daemon = True
        self.logger = logger
        self._rs_fixtures = rs_fixtures
        self._stepdown_interval_secs = stepdown_interval_secs
        self.__lifecycle = stepdown_lifecycle
        self._auth_options = auth_options

        self._last_exec = time.time()
        self._pause_timeout_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        self._stop_timeout_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        self._thread_state = lifecycle_interface.HookThreadState()

    def run(self):
        """Execute the thread."""
        if not self._rs_fixtures:
            self.logger.warning("No replica set on which to run lightweight resharding stepdowns.")
            return

        try:
            while True:
                self._thread_state.mark_idle("waiting_for_action_permitted")

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    self._thread_state.mark_stopped("lifecycle_stop_requested")
                    break

                self._thread_state.mark_running("stepdown_cycle")

                now = time.time()
                if now - self._last_exec > self._stepdown_interval_secs:
                    self._thread_state.set_phase("step_down_all")
                    self.logger.info(
                        "Starting lightweight resharding stepdown of all shard primaries"
                    )
                    self._step_down_all()
                    self._last_exec = time.time()
                    self.logger.info(
                        "Completed lightweight resharding stepdown of all shard primaries in %0d ms",
                        (self._last_exec - now) * 1000,
                    )

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self._thread_state.set_phase("sending_idle_acknowledgement")
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                # The 'wait_secs' is used to wait 'self._stepdown_interval_secs' from the moment
                # the last stepdown command was sent.
                now = time.time()
                wait_secs = max(0, self._stepdown_interval_secs - (now - self._last_exec))
                self._thread_state.mark_idle("waiting_for_action_interval")
                self.__lifecycle.wait_for_action_interval(wait_secs)
        except Exception as err:
            self.logger.exception("Lightweight Resharding Stepdown Thread threw exception")
            self._thread_state.mark_failed(err, "run_loop")
        finally:
            state, _phase = self._thread_state.describe()
            if state not in ("failed", "stopped"):
                self._thread_state.mark_stopped("run_loop_exited")

    def stop(self):
        """Stop the thread."""
        self._thread_state.mark_stopping("stop_requested")
        self.__lifecycle.stop()
        self.resume()
        self.join(self._stop_timeout_secs)
        if self.is_alive():
            state, phase = self._thread_state.describe()
            raise errors.ServerFailure(
                "Timed out waiting for lightweight resharding stepdown thread to stop; "
                f"state={state}, phase={phase}, timeout={self._stop_timeout_secs}s."
            )
        self._thread_state.assert_healthy(
            self.is_alive(), "lightweight resharding stepdown", allow_stopped=True
        )

    def pause(self):
        """Pause the thread."""
        self.__lifecycle.mark_test_finished()

        self._thread_state.wait_until_idle(
            self._pause_timeout_secs, "lightweight resharding stepdown"
        )
        self._thread_state.assert_healthy(self.is_alive(), "lightweight resharding stepdown")

        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    "ReplicaSetFixture with pids {} expected to be running in"
                    " ContinuousLightweightReshardingStepdown, but wasn't.".format(
                        rs_fixture.pids()
                    )
                )

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _create_client(self, node):
        return fixture_interface.build_hook_client(node, self._auth_options)

    def _step_down_all(self):
        for rs_fixture in self._rs_fixtures:
            self._step_down(rs_fixture)

    def _step_down(self, rs_fixture):
        with rs_fixture.removeshard_teardown_mutex:
            if rs_fixture.removeshard_teardown_marker:
                return

            try:
                primary = rs_fixture.get_primary(timeout_secs=self._stepdown_interval_secs)
            except errors.ServerFailure:
                # No primary available; we'll try again after self._stepdown_interval_secs seconds.
                return

            self.logger.info(
                "Sending _shardsvrReshardingStepDown to primary on port %d of replica set '%s'",
                primary.port,
                rs_fixture.replset_name,
            )

            try:
                client = self._create_client(primary)
                client.admin.command({"_shardsvrReshardingStepDown": 1})
                self.logger.info(
                    "Successfully sent _shardsvrReshardingStepDown to primary on port %d of"
                    " replica set '%s'",
                    primary.port,
                    rs_fixture.replset_name,
                )
            except pymongo.errors.AutoReconnect:
                self.logger.warning(
                    "AutoReconnect when sending _shardsvrReshardingStepDown to primary on port %d"
                    " of replica set '%s'",
                    primary.port,
                    rs_fixture.replset_name,
                )
            except pymongo.errors.OperationFailure as err:
                self.logger.warning(
                    "_shardsvrReshardingStepDown failed on primary on port %d of replica set"
                    " '%s': %s",
                    primary.port,
                    rs_fixture.replset_name,
                    err,
                )
