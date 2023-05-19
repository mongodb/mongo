"""Test hook that periodically makes the primary of a replica set step down."""

import collections
import os.path
import random
import threading
import time

import pymongo.errors

import buildscripts.resmokelib.utils.filesystem as fs
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface, talk_directly_to_shardsvrs
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.fixtures import tenant_migration
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class ContinuousStepdown(interface.Hook):
    """Regularly connect to replica sets and send a replSetStepDown command."""

    DESCRIPTION = ("Continuous stepdown (steps down the primary of replica sets at regular"
                   " intervals)")

    IS_BACKGROUND = True

    # The hook stops the fixture partially during its execution.
    STOPS_FIXTURE = True

    def __init__(self, hook_logger, fixture, config_stepdown=True, shard_stepdown=True,
                 stepdown_interval_ms=8000, terminate=False, kill=False,
                 use_action_permitted_file=False, background_reconfig=False, auth_options=None,
                 should_downgrade=False):
        """Initialize the ContinuousStepdown.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (replica sets or a sharded cluster).
            config_stepdown: whether to stepdown the CSRS.
            shard_stepdown: whether to stepdown the shard replica sets in a sharded cluster.
            stepdown_interval_ms: the number of milliseconds between stepdowns.
            terminate: shut down the node cleanly as a means of stepping it down.
            kill: With a 50% probability, kill the node instead of shutting it down cleanly.
            use_action_permitted_file: use a file to control if stepdown thread should do a stepdown.
            auth_options: dictionary of auth options.
            background_reconfig: whether to conduct reconfig in the background.
            should_downgrade: whether dowgrades should be performed as part of the stepdown.

        Note that the "terminate" and "kill" arguments are named after the "SIGTERM" and
        "SIGKILL" signals that are used to stop the process. On Windows, there are no signals,
        so we use a different means to achieve the same result as sending SIGTERM or SIGKILL.
        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousStepdown.DESCRIPTION)

        self._fixture = fixture
        if hasattr(fixture, "config_shard") and fixture.config_shard is not None and shard_stepdown:
            # If the config server is a shard, shard_stepdown implies config_stepdown.
            config_stepdown = shard_stepdown

        self._config_stepdown = config_stepdown
        self._shard_stepdown = shard_stepdown
        self._stepdown_interval_secs = float(stepdown_interval_ms) / 1000

        self._rs_fixtures = []
        self._mongos_fixtures = []
        self._stepdown_thread = None

        # kill implies terminate.
        self._terminate = terminate or kill
        self._kill = kill

        self._background_reconfig = background_reconfig
        self._auth_options = auth_options
        self._should_downgrade = should_downgrade

        # The action file names need to match the same construction as found in
        # jstests/concurrency/fsm_libs/resmoke_runner.js.
        dbpath_prefix = fixture.get_dbpath_prefix()

        if use_action_permitted_file:
            self.__action_files = lifecycle_interface.ActionFiles._make([
                os.path.join(dbpath_prefix, field)
                for field in lifecycle_interface.ActionFiles._fields
            ])
        else:
            self.__action_files = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._rs_fixtures:
            self._add_fixture(self._fixture)

        if self.__action_files is not None:
            lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.__action_files)
        else:
            lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        self._stepdown_thread = _StepdownThread(
            self.logger, self._mongos_fixtures, self._rs_fixtures, self._stepdown_interval_secs,
            self._terminate, self._kill, lifecycle, self._background_reconfig, self._fixture,
            self._auth_options, self._should_downgrade)
        self.logger.info("Starting the stepdown thread.")
        self._stepdown_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the stepdown thread.")
        self._stepdown_thread.stop()
        self.logger.info("Stepdown thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the stepdown thread.")
        self._stepdown_thread.pause()
        self._stepdown_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the stepdown thread.")
        self._stepdown_thread.pause()
        self.logger.info("Paused the stepdown thread.")

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
                    if shard_fixture.config_shard is None or self._config_stepdown:
                        self._add_fixture(shard_fixture)
            if self._config_stepdown and fixture.config_shard is None:
                self._add_fixture(fixture.configsvr)
            for mongos_fixture in fixture.mongos:
                self._mongos_fixtures.append(mongos_fixture)
        elif isinstance(fixture, tenant_migration.TenantMigrationFixture):
            if not fixture.all_nodes_electable:
                raise ValueError(
                    "The replica sets that are the target of the ContinuousStepdown hook must have"
                    " the 'all_nodes_electable' option set.")

            for rs_fixture in fixture.get_replsets():
                self._rs_fixtures.append(rs_fixture)
        elif isinstance(fixture, fixture_interface.MultiClusterFixture):
            # Recursively call _add_fixture on all the independent clusters.
            for cluster_fixture in fixture.get_independent_clusters():
                self._add_fixture(cluster_fixture)
        elif isinstance(fixture, talk_directly_to_shardsvrs.TalkDirectlyToShardsvrsFixture):
            if not fixture.all_nodes_electable:
                raise ValueError(
                    "The replica sets that are the target of the ContinuousStepdown hook must have"
                    " the 'all_nodes_electable' option set.")
            for rs_fixture in fixture.get_replsets():
                self._rs_fixtures.append(rs_fixture)


def is_shard_split(fixture):
    """Used to determine if the provided fixture is an instance of the ShardSplitFixture class."""
    return fixture.__class__.__name__ == 'ShardSplitFixture'


class _StepdownThread(threading.Thread):
    def __init__(self, logger, mongos_fixtures, rs_fixtures, stepdown_interval_secs, terminate,
                 kill, stepdown_lifecycle, background_reconfig, fixture, auth_options=None,
                 should_downgrade=False):
        """Initialize _StepdownThread."""
        threading.Thread.__init__(self, name="StepdownThread")
        self.daemon = True
        self.logger = logger
        self._mongos_fixtures = mongos_fixtures
        self._rs_fixtures = rs_fixtures
        self._stepdown_interval_secs = stepdown_interval_secs
        # We set the self._stepdown_duration_secs to a very long time, to ensure that the former
        # primary will not step back up on its own and the stepdown thread will cause it step up via
        # replSetStepUp.
        self._stepdown_duration_secs = 24 * 60 * 60  # 24 hours
        self._terminate = terminate
        self._kill = kill
        self.__lifecycle = stepdown_lifecycle
        self._background_reconfig = background_reconfig
        self._fixture = fixture
        self._auth_options = auth_options
        self._should_downgrade = should_downgrade

        self._last_exec = time.time()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing stepdowns.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

        self._step_up_stats = collections.Counter()

    def run(self):
        """Execute the thread."""
        if not self._rs_fixtures:
            self.logger.warning("No replica set on which to run stepdowns.")
            return

        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                if is_shard_split(self._fixture):
                    self._fixture.enter_step_down()
                    self._rs_fixtures = [self._fixture.get_donor_rs()]

                now = time.time()
                if now - self._last_exec > self._stepdown_interval_secs:
                    self.logger.info("Starting stepdown of all primaries")
                    self._step_down_all()
                    # Wait until each replica set has a primary, so the test can make progress.
                    self._await_primaries()
                    self._last_exec = time.time()
                    self.logger.info("Completed stepdown of all primaries in %0d ms",
                                     (self._last_exec - now) * 1000)

                if is_shard_split(self._fixture):
                    self._rs_fixtures = []
                    self._fixture.exit_step_down()

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                # The 'wait_secs' is used to wait 'self._stepdown_interval_secs' from the moment
                # the last stepdown command was sent.
                now = time.time()
                wait_secs = max(0, self._stepdown_interval_secs - (now - self._last_exec))
                self.__lifecycle.wait_for_action_interval(wait_secs)
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Stepdown Thread threw exception")
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
        # Wait until we all the replica sets have primaries.
        self._await_primaries()

        # Check that fixtures are still running
        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    "ReplicaSetFixture with pids {} expected to be running in"
                    " ContinuousStepdown, but wasn't.".format(rs_fixture.pids()))
        for mongos_fixture in self._mongos_fixtures:
            if not mongos_fixture.is_running():
                raise errors.ServerFailure("MongoSFixture with pids {} expected to be running in"
                                           " ContinuousStepdown, but wasn't.".format(
                                               mongos_fixture.pids()))

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

        self.logger.info(
            "Current statistics about which nodes have been successfully stepped up: %s",
            self._step_up_stats)

    def _wait(self, timeout):
        # Wait until stop or timeout.
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        if not self.is_alive():
            msg = "The stepdown thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _await_primaries(self):
        for fixture in self._rs_fixtures:
            fixture.get_primary()

    def _create_client(self, node):
        return fixture_interface.build_client(node, self._auth_options)

    def _step_down_all(self):
        for rs_fixture in self._rs_fixtures:
            self._step_down(rs_fixture)

    def _step_down(self, rs_fixture):
        try:
            old_primary = rs_fixture.get_primary(timeout_secs=self._stepdown_interval_secs)
        except errors.ServerFailure:
            # We ignore the ServerFailure exception because it means a primary wasn't available.
            # We'll try again after self._stepdown_interval_secs seconds.
            return

        secondaries = rs_fixture.get_secondaries()

        if self._terminate:
            if not rs_fixture.stop_primary(old_primary, self._background_reconfig, self._kill):
                return

        if self._should_downgrade:
            new_primary = rs_fixture.change_version_and_restart_node(old_primary,
                                                                     self._auth_options)
        else:

            def step_up_secondary():
                while secondaries:
                    chosen = random.choice(secondaries)
                    if not rs_fixture.stepup_node(chosen, self._auth_options):
                        secondaries.remove(chosen)
                    else:
                        return chosen

            new_primary = step_up_secondary()

        if self._terminate:
            rs_fixture.restart_node(old_primary)

        if secondaries:
            # We successfully stepped up a secondary, wait for the former primary to step down via
            # heartbeats. We need to wait for the former primary to step down to complete this step
            # down round and to avoid races between the ContinuousStepdown hook and other test hooks
            # that may depend on the health of the replica set.
            self.logger.info(
                "Successfully stepped up the secondary on port %d of replica set '%s'.",
                new_primary.port, rs_fixture.replset_name)
            retry_time_secs = rs_fixture.AWAIT_REPL_TIMEOUT_MINS * 60
            retry_start_time = time.time()
            while True:
                try:
                    client = self._create_client(old_primary)
                    is_secondary = client.admin.command("isMaster")["secondary"]
                    if is_secondary:
                        break
                except pymongo.errors.AutoReconnect:
                    pass
                if time.time() - retry_start_time > retry_time_secs:
                    raise errors.ServerFailure(
                        "The old primary on port {} of replica set {} did not step down in"
                        " {} seconds.".format(client.port, rs_fixture.replset_name,
                                              retry_time_secs))
                self.logger.info("Waiting for primary on port %d of replica set '%s' to step down.",
                                 old_primary.port, rs_fixture.replset_name)
                time.sleep(0.2)  # Wait a little bit before trying again.
            self.logger.info("Primary on port %d of replica set '%s' stepped down.",
                             old_primary.port, rs_fixture.replset_name)

        if not secondaries:
            # If we failed to step up one of the secondaries, then we run the replSetStepUp to try
            # and elect the former primary again. This way we don't need to wait
            # self._stepdown_duration_secs seconds to restore write availability to the cluster.
            # Since the former primary may have been killed, we need to wait until it has been
            # restarted by retrying replSetStepUp.

            retry_time_secs = rs_fixture.AWAIT_REPL_TIMEOUT_MINS * 60
            retry_start_time = time.time()
            while True:
                try:
                    client = self._create_client(old_primary)
                    client.admin.command("replSetStepUp")
                    is_primary = client.admin.command("isMaster")["ismaster"]
                    # There is a chance that the old master is still in catchup stage when we issue replSetStepUp
                    # then it will step down due to term change in the previous election failure. We should ensure the old
                    # primary becomes a writable primary here, or there will have no primary for a day.
                    if is_primary:
                        break
                    else:
                        self._wait(0.2)
                except pymongo.errors.OperationFailure:
                    self._wait(0.2)
                if time.time() - retry_start_time > retry_time_secs:
                    raise errors.ServerFailure(
                        "The old primary on port {} of replica set {} did not step up in"
                        " {} seconds.".format(client.port, rs_fixture.replset_name,
                                              retry_time_secs))

        # Bump the counter for the chosen secondary to indicate that the replSetStepUp command
        # executed successfully.
        key = "{}/{}".format(
            rs_fixture.replset_name,
            new_primary.get_internal_connection_string() if secondaries else "none")
        self._step_up_stats[key] += 1
