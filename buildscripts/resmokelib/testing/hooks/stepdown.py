"""Test hook that periodically makes the primary of a replica set step down."""
from __future__ import absolute_import

import collections
import random
import time
import threading

import bson
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures import shardedcluster


class ContinuousStepdown(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Regularly connect to replica sets and send a replSetStepDown command."""

    DESCRIPTION = ("Continuous stepdown (steps down the primary of replica sets at regular"
                   " intervals)")

    def __init__(  # pylint: disable=too-many-arguments
            self, hook_logger, fixture, config_stepdown=True, shard_stepdown=True,
            stepdown_duration_secs=10, stepdown_interval_ms=8000, terminate=False, kill=False):
        """Initialize the ContinuousStepdown.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (a replica set or sharded cluster).
            config_stepdown: whether to stepdown the CSRS.
            shard_stepdown: whether to stepdown the shard replica sets in a sharded cluster.
            stepdown_duration_secs: the number of seconds to step down the primary.
            stepdown_interval_ms: the number of milliseconds between stepdowns.
            terminate: shut down the node cleanly as a means of stepping it down.
            kill: With a 50% probability, kill the node instead of shutting it down cleanly.

        Note that the "terminate" and "kill" arguments are named after the "SIGTERM" and
        "SIGKILL" signals that are used to stop the process. On Windows, there are no signals,
        so we use a different means to achieve the same result as sending SIGTERM or SIGKILL.
        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousStepdown.DESCRIPTION)

        self._fixture = fixture
        self._config_stepdown = config_stepdown
        self._shard_stepdown = shard_stepdown
        self._stepdown_duration_secs = stepdown_duration_secs
        self._stepdown_interval_secs = float(stepdown_interval_ms) / 1000

        self._rs_fixtures = []
        self._stepdown_thread = None

        # kill implies terminate.
        self._terminate = terminate or kill
        self._kill = kill

    def before_suite(self, test_report):
        """Before suite."""
        if not self._rs_fixtures:
            self._add_fixture(self._fixture)
        self._stepdown_thread = _StepdownThread(
            self.logger, self._rs_fixtures, self._stepdown_interval_secs,
            self._stepdown_duration_secs, self._terminate, self._kill)
        self.logger.info("Starting the stepdown thread.")
        self._stepdown_thread.start()

    def after_suite(self, test_report):
        """After suite."""
        self.logger.info("Stopping the stepdown thread.")
        self._stepdown_thread.stop()

    def before_test(self, test, test_report):
        """Before test."""
        self._check_thread()
        self.logger.info("Resuming the stepdown thread.")
        self._stepdown_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self._check_thread()
        self.logger.info("Pausing the stepdown thread.")
        self._stepdown_thread.pause()
        self.logger.info("Paused the stepdown thread.")

    def _check_thread(self):
        if not self._stepdown_thread.is_alive():
            msg = "The stepdown thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _add_fixture(self, fixture):
        if isinstance(fixture, replicaset.ReplicaSetFixture):
            if not fixture.all_nodes_electable:
                raise ValueError(
                    "The replica sets that are the target of the ContinuousStepdown hook must have"
                    " the 'all_nodes_electable' option set.")
            self._rs_fixtures.append(fixture)
        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            if self._shard_stepdown:
                for shard_fixture in fixture.shards:
                    self._add_fixture(shard_fixture)
            if self._config_stepdown:
                self._add_fixture(fixture.configsvr)


class _StepdownThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    def __init__(  # pylint: disable=too-many-arguments
            self, logger, rs_fixtures, stepdown_interval_secs, stepdown_duration_secs, terminate,
            kill):
        """Initialize _StepdownThread."""
        threading.Thread.__init__(self, name="StepdownThread")
        self.daemon = True
        self.logger = logger
        self._rs_fixtures = rs_fixtures
        self._stepdown_interval_secs = stepdown_interval_secs
        self._stepdown_duration_secs = stepdown_duration_secs
        self._terminate = terminate
        self._kill = kill

        self._last_exec = time.time()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not paused.
        self._is_resumed_evt = threading.Event()
        self._is_resumed_evt.set()
        # Event set when the thread is not performing stepdowns.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

        self._step_up_stats = collections.Counter()

    def run(self):
        """Execute the thread."""
        if not self._rs_fixtures:
            self.logger.warning("No replica set on which to run stepdowns.")
            return

        while True:
            self._pause_if_needed()
            if self._is_stopped():
                break
            now = time.time()
            if now - self._last_exec > self._stepdown_interval_secs:
                self._step_down_all()
                # Wait until each replica set has a primary, so the test can make progress.
                self._await_primaries()
                self._last_exec = time.time()
            now = time.time()
            # 'wait_secs' is used to wait 'self._stepdown_interval_secs' from the moment the last
            # stepdown command was sent.
            wait_secs = max(0, self._stepdown_interval_secs - (now - self._last_exec))
            self._wait(wait_secs)

    def stop(self):
        """Stop the thread."""
        self._is_stopped_evt.set()
        # Unpause to allow the thread to finish.
        self.resume()
        self.join()

    def _is_stopped(self):
        return self._is_stopped_evt.is_set()

    def pause(self):
        """Pause the thread."""
        self._is_resumed_evt.clear()
        # Wait until we are no longer executing stepdowns.
        self._is_idle_evt.wait()
        # Wait until we all the replica sets have primaries.
        self._await_primaries()

    def resume(self):
        """Resume the thread."""
        self._is_resumed_evt.set()

        self.logger.info(
            "Current statistics about which nodes have been successfully stepped up: %s",
            self._step_up_stats)

    def _pause_if_needed(self):
        # Wait until resume or stop.
        self._is_resumed_evt.wait()

    def _wait(self, timeout):
        # Wait until stop or timeout.
        self._is_stopped_evt.wait(timeout)

    def _await_primaries(self):
        for fixture in self._rs_fixtures:
            fixture.get_primary()

    def _step_down_all(self):
        self._is_idle_evt.clear()
        try:
            for rs_fixture in self._rs_fixtures:
                self._step_down(rs_fixture)
        finally:
            self._is_idle_evt.set()

    def _step_down(self, rs_fixture):
        try:
            primary = rs_fixture.get_primary(timeout_secs=self._stepdown_interval_secs)
        except errors.ServerFailure:
            # We ignore the ServerFailure exception because it means a primary wasn't available.
            # We'll try again after self._stepdown_interval_secs seconds.
            return

        secondaries = rs_fixture.get_secondaries()

        # Check that the fixture is still running before stepping down or killing the primary.
        # This ensures we still detect some cases in which the fixture has already crashed.
        if not rs_fixture.is_running():
            raise errors.ServerFailure("ReplicaSetFixture expected to be running in"
                                       " ContinuousStepdown, but wasn't.")

        if self._terminate:
            should_kill = self._kill and random.choice([True, False])
            action = "Killing" if should_kill else "Terminating"
            self.logger.info("%s the primary on port %d of replica set '%s'.", action, primary.port,
                             rs_fixture.replset_name)

            primary.mongod.stop(kill=should_kill)
            primary.mongod.wait()
        else:
            self.logger.info("Stepping down the primary on port %d of replica set '%s'.",
                             primary.port, rs_fixture.replset_name)
            try:
                client = primary.mongo_client()
                client.admin.command(
                    bson.SON([
                        ("replSetStepDown", self._stepdown_duration_secs),
                        ("force", True),
                    ]))
            except pymongo.errors.AutoReconnect:
                # AutoReconnect exceptions are expected as connections are closed during stepdown.
                pass
            except pymongo.errors.PyMongoError:
                self.logger.exception(
                    "Error while stepping down the primary on port %d of replica set '%s'.",
                    primary.port, rs_fixture.replset_name)
                raise

        # We pick arbitrary secondary to run for election immediately in order to avoid a long
        # period where the replica set doesn't have write availability. If none of the secondaries
        # are eligible, or their election attempt fails, then we'll simply not have write
        # availability until the self._stepdown_duration_secs duration expires and 'primary' steps
        # back up again.
        while secondaries:
            chosen = random.choice(secondaries)

            self.logger.info("Attempting to step up the secondary on port %d of replica set '%s'.",
                             chosen.port, rs_fixture.replset_name)

            try:
                client = chosen.mongo_client()
                client.admin.command("replSetStepUp")
                break
            except pymongo.errors.OperationFailure:
                # OperationFailure exceptions are expected when the election attempt fails due to
                # not receiving enough votes. This can happen when the 'chosen' secondary's opTime
                # is behind that of other secondaries. We handle this by attempting to elect a
                # different secondary.
                self.logger.info("Failed to step up the secondary on port %d of replica set '%s'.",
                                 chosen.port, rs_fixture.replset_name)
                secondaries.remove(chosen)

        if self._terminate:
            self.logger.info("Attempting to restart the old primary on port %d of replica set '%s.",
                             primary.port, rs_fixture.replset_name)

            # Restart the mongod on the old primary and wait until we can contact it again. Keep the
            # original preserve_dbpath to restore after restarting the mongod.
            original_preserve_dbpath = primary.preserve_dbpath
            primary.preserve_dbpath = True
            try:
                primary.setup()
                primary.await_ready()
            finally:
                primary.preserve_dbpath = original_preserve_dbpath

        # Bump the counter for the chosen secondary to indicate that the replSetStepUp command
        # executed successfully.
        key = "{}/{}".format(rs_fixture.replset_name,
                             chosen.get_internal_connection_string() if secondaries else "none")
        self._step_up_stats[key] += 1
