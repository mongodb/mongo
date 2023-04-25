"""Test hook that periodically initial-syncs a node and steps it up."""

import collections
import os.path
import random
import threading
import time

from enum import Enum
from random import choice
import bson
import bson.errors
import pymongo.errors

import buildscripts.resmokelib.utils.filesystem as fs
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class ContinuousInitialSync(interface.Hook):
    """Periodically initial sync nodes then step them up."""

    DESCRIPTION = ("Continuous initial sync with failover")

    IS_BACKGROUND = True

    # The hook stops the fixture partially during its execution.
    STOPS_FIXTURE = True

    def __init__(self, hook_logger, fixture, use_action_permitted_file=False, sync_interval_secs=8):
        """Initialize the ContinuousInitialSync.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (replica sets or a sharded cluster).
            use_action_permitted_file: use a file to control if the syncer thread should do a failover or initial sync
            sync_interval_secs: how often to trigger a new cycle
        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousInitialSync.DESCRIPTION)

        self.hook_logger = hook_logger

        self._fixture = fixture
        self._rs_fixtures = []
        self._mongos_fixtures = []

        self._sync_interval_secs = sync_interval_secs

        self._initial_sync_thread = None

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
            lifecycle_interface.FlagBasedThreadLifecycle()

        self._initial_sync_thread = _InitialSyncThread(self.logger, self._rs_fixtures,
                                                       self._mongos_fixtures, self._fixture,
                                                       lifecycle, self._sync_interval_secs)
        self.logger.info("Starting the continuous initial syncer thread.")
        self._initial_sync_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the continuous initial syncer thread.")
        self._initial_sync_thread.stop()
        self.logger.info("Continuous initial syncer thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the continuous initial syncer thread.")
        self._initial_sync_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the continuous initial syncer thread.")
        self._initial_sync_thread.pause()
        self.logger.info("Paused the continuous initial syncer thread.")

    def _add_fixture(self, fixture):
        """Catalogues all replica set and mongos fixtures."""
        if isinstance(fixture, replicaset.ReplicaSetFixture):
            self._rs_fixtures.append(fixture)

        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            for shard_fixture in fixture.shards:
                self._add_fixture(shard_fixture)

            if fixture.config_shard is None:
                self._add_fixture(fixture.configsvr)

            for mongos_fixture in fixture.mongos:
                self._mongos_fixtures.append(mongos_fixture)


class SyncerStage(Enum):
    """An enum to hold the various stages each execution cycle goes through."""

    RUN_INITSYNC = 1
    RUN_AS_SECONDARY = 2
    INITSYNC_PRIMARY = 3
    ORIGINAL_PRIMARY = 4


class _InitialSyncThread(threading.Thread):

    # Error codes, taken from mongo/base/error_codes.yml.
    _NODE_NOT_FOUND = 74
    _NEW_REPLICA_SET_CONFIGURATION_INCOMPATIBLE = 103
    _CONFIGURATION_IN_PROGRESS = 109
    _CURRENT_CONFIG_NOT_COMMITTED_YET = 308
    _INTERRUPTED_DUE_TO_STORAGE_CHANGE = 355
    _INTERRUPTED_DUE_TO_REPL_STATE_CHANGE = 11602

    def __init__(self, logger, rs_fixtures, mongos_fixtures, fixture, lifecycle,
                 sync_interval_secs):
        """Initialize _InitialSyncThread."""
        threading.Thread.__init__(self, name="InitialSyncThread")
        self.daemon = True
        self.logger = logger

        self._fixture = fixture
        self._rs_fixtures = rs_fixtures
        self._mongos_fixtures = mongos_fixtures

        self.__lifecycle = lifecycle

        self._sync_interval_secs = sync_interval_secs

        self._last_exec = time.time()
        self._is_stopped_evt = threading.Event()
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

    def run(self):
        """Execute the thread."""
        if not self._rs_fixtures:
            self.logger.warning("No replica sets on which to run initial sync")
            return

        try:
            stage = SyncerStage.RUN_INITSYNC

            self.logger.info("Adding unique tag to all initial sync nodes.")
            for fixture in self._rs_fixtures:
                self._add_initsync_tag(fixture)

            wait_secs = None

            while True:
                # Check for permission to run and also for shutdown.
                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break
                self._is_idle_evt.clear()

                # Run initial sync on all replica sets.
                if stage == SyncerStage.RUN_INITSYNC:
                    self.logger.info("Starting all initial syncs...")
                    for fixture in self._rs_fixtures:
                        self._start_initial_sync(fixture)
                        self._await_sync_node_ready(fixture)

                    self.logger.info("Waiting for new secondaries...")
                    for fixture in self._rs_fixtures:
                        self._await_initial_sync_done(fixture)

                    stage = SyncerStage.RUN_AS_SECONDARY
                    wait_secs = 0

                # Nothing to be done. Just let the nodes stay as secondaries for the duration.
                elif stage == SyncerStage.RUN_AS_SECONDARY:
                    self.logger.info("Letting new secondaries run...")
                    stage = SyncerStage.INITSYNC_PRIMARY
                    wait_secs = self._sync_interval_secs

                # Elect initial-synced nodes as primaries.
                elif stage == SyncerStage.INITSYNC_PRIMARY:
                    self.logger.info("Stepping up new secondaries...")
                    for fixture in self._rs_fixtures:
                        self._fail_over_to_node(fixture.get_initial_sync_node(), fixture)

                    stage = SyncerStage.ORIGINAL_PRIMARY
                    wait_secs = self._sync_interval_secs

                # Failover back to the original primaries before restarting initial sync.
                elif stage == SyncerStage.ORIGINAL_PRIMARY:
                    self.logger.info("Restoring original primaries...")
                    for fixture in self._rs_fixtures:
                        self._fail_over_to_node(fixture.get_secondaries()[0], fixture)

                    stage = SyncerStage.RUN_INITSYNC
                    wait_secs = 0

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()

                self._is_idle_evt.set()
                self.logger.info(
                    "Syncer sleeping for {} seconds before moving to the next stage.".format(
                        wait_secs))
                self.__lifecycle.wait_for_action_interval(wait_secs)

        except Exception as err:  # pylint: disable=broad-except
            msg = "Syncer Thread threw exception: {}".format(err)
            self.logger.exception(msg)
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

        # Wait until the thread is idle.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()
        # Wait until we all the replica sets have primaries.
        self._await_primaries()
        # Check that fixtures are still running.
        self._check_fixtures()

    def resume(self):
        """Resumes the thread."""
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        """Waits until stop or timeout."""
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        """Checks if the syncer thread is still alive."""
        if not self.is_alive():
            msg = "The syncer thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _await_primaries(self):
        """Waits for all replica sets to have responsive primaries."""
        for fixture in self._rs_fixtures:
            fixture.get_primary()

    def _check_fixtures(self):
        """Check that fixtures are still running."""
        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    "ReplicaSetFixture with pids {} expected to be running in"
                    " ContinuousInitialSync, but wasn't.".format(rs_fixture.pids()))
        for mongos_fixture in self._mongos_fixtures:
            if not mongos_fixture.is_running():
                raise errors.ServerFailure("MongoSFixture with pids {} expected to be running in"
                                           " ContinuousInitialSync, but wasn't.".format(
                                               mongos_fixture.pids()))

    def _add_initsync_tag(self, fixture):
        """Adds the 'INIT_SYNC' unique tag to the initial-sync node of the given fixture."""
        sync_node = fixture.get_initial_sync_node()

        self.logger.info("Adding unique tag to initial sync node on port {} in set {}".format(
            sync_node.port, fixture.replset_name))

        retry_time_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        retry_start_time = time.time()

        while True:
            if time.time() - retry_start_time > retry_time_secs:
                raise errors.ServerFailure(
                    "Could not add unique tag to node on port {} for replica set {} in {} seconds.".
                    format(sync_node.port, fixture.replset_name, retry_time_secs))
            try:
                primary = fixture.get_primary()
                client = primary.mongo_client()

                repl_config = client.admin.command({"replSetGetConfig": 1})["config"]
                repl_config["version"] += 1
                repl_config["members"][-1]["tags"]["uniqueTag"] = "INIT_SYNC_NODE"

                client.admin.command({
                    "replSetReconfig": repl_config,
                    "maxTimeMS": fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60 * 1000
                })
                break

            except pymongo.errors.AutoReconnect:
                time.sleep(0.1)
                continue

            except pymongo.errors.ConnectionFailure:
                time.sleep(0.1)
                continue

            except pymongo.errors.OperationFailure as err:
                # These error codes may be transient, and so we retry the reconfig with a
                # (potentially) higher config version. We should not receive these codes
                # indefinitely.
                # pylint: disable=too-many-boolean-expressions
                if (err.code != self._NEW_REPLICA_SET_CONFIGURATION_INCOMPATIBLE
                        and err.code != self._CURRENT_CONFIG_NOT_COMMITTED_YET
                        and err.code != self._CONFIGURATION_IN_PROGRESS
                        and err.code != self._NODE_NOT_FOUND
                        and err.code != self._INTERRUPTED_DUE_TO_REPL_STATE_CHANGE
                        and err.code != self._INTERRUPTED_DUE_TO_STORAGE_CHANGE):
                    msg = (
                        "Operation failure while adding tag for node on port {} in fixture {}: {}"
                    ).format(sync_node.port, fixture.replset_name, err)
                    self.logger.error(msg)
                    raise self.fixturelib.ServerFailure(msg)

                msg = (
                    "Retrying failed attempt to add new node on port {} to fixture {}: {}").format(
                        sync_node.port, fixture.replset_name, err)
                self.logger.error(msg)
                time.sleep(0.1)
                continue

    def _start_initial_sync(self, fixture):
        """Restarts the node fixture with clean data so it can go into initial sync."""
        sync_node = fixture.get_initial_sync_node()

        method = random.choice(["logical", "fileCopyBased"])

        self.logger.info(
            "Restarting initial sync on node on port {} in set {} with initial sync method {}".
            format(sync_node.port, fixture.replset_name, method))

        sync_node.teardown()
        sync_node.mongod_options["set_parameters"]["initialSyncMethod"] = method
        sync_node.setup()

    def _await_sync_node_ready(self, fixture):
        """Waits until the node is ready to be used for testing."""
        sync_node = fixture.get_initial_sync_node()
        sync_node.await_ready()

    def _await_initial_sync_done(self, fixture):
        """Waits for the initial sync node to complete its transition to secondary."""
        sync_node = fixture.get_initial_sync_node()

        self.logger.info("Waiting for node on port {} in set {} to complete initial sync".format(
            sync_node.port, fixture.replset_name))

        retry_time_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        retry_start_time = time.time()

        while True:
            if self._check_initial_sync_done(fixture):
                return
            time.sleep(2)
            if time.time() - retry_start_time > retry_time_secs:
                raise errors.ServerFailure(
                    "Node on port {} of replica set {} did not finish initial sync in {} seconds.".
                    format(sync_node.port, fixture.replset_name, retry_time_secs))

    def _check_initial_sync_done(self, fixture):
        """A one-time check for whether a node has completed initial sync and transitioned to a secondary state."""
        sync_node = fixture.get_initial_sync_node()
        sync_node_conn = sync_node.mongo_client()

        self.logger.info("Checking initial sync progress for node on port {} in set {}".format(
            sync_node.port, fixture.replset_name))

        try:
            state = sync_node_conn.admin.command("replSetGetStatus").get("myState")
            return state == 2
        except pymongo.errors.OperationFailure:
            # This can fail if the node is in STARTUP state. Just check again next round.
            return False

    def _fail_over_to_node(self, node, fixture):
        """Steps up a node and waits for it to complete its transition to primary."""
        conn = node.mongo_client()
        old_primary = fixture.get_primary()

        self.logger.info("Failing over to node on port {} from node on port {} in set {}".format(
            node.port, old_primary.port, fixture.replset_name))

        retry_time_secs = fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60
        retry_start_time = time.time()

        while True:
            try:
                conn.admin.command("replSetStepUp")
                break
            except pymongo.errors.OperationFailure:
                time.sleep(0.2)
                continue
            except pymongo.errors.AutoReconnect:
                time.sleep(0.2)
                continue
            if time.time() - retry_start_time > retry_time_secs:
                raise errors.ServerFailure(
                    "Node on port {} of replica set {} did not step up in {} seconds.".format(
                        node.port, fixture.replset_name, retry_time_secs))

        cmd = bson.SON([("replSetTest", 1), ("waitForMemberState", 1),
                        ("timeoutMillis",
                         fixture_interface.ReplFixture.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60)])

        retry_start_time = time.time()

        while True:
            try:
                conn.admin.command(cmd)
                break
            except pymongo.errors.OperationFailure as err:
                if (err.code != self.INTERRUPTED_DUE_TO_REPL_STATE_CHANGE
                        and err.code != self.INTERRUPTED_DUE_TO_STORAGE_CHANGE):
                    raise
                msg = (
                    "Interrupted while waiting for node on port {} in set {} to reach primary state, retrying: {}"
                ).format(node.port, fixture.replset_name, err)
                self.logger.error(msg)
            if time.time() - retry_start_time > retry_time_secs:
                raise errors.ServerFailure(
                    "Node on port {} of replica set {} did not finish stepping up in {} seconds.".
                    format(node.port, fixture.replset_name, retry_time_secs))
            time.sleep(0.2)

        self.logger.info(
            "Successfully stepped up node on port {} in set {}. Waiting for old primary on port {} to step down"
            .format(node.port, fixture.replset_name, old_primary.port))

        retry_start_time = time.time()

        # Also wait for the old primary to step down, to avoid races with other test hooks.
        while True:
            try:
                client = old_primary.mongo_client()
                is_secondary = client.admin.command("hello")["secondary"]
                if is_secondary:
                    break
            except pymongo.errors.AutoReconnect:
                pass
            self.logger.info("Waiting for old primary on port {} in set {} to step down.".format(
                old_primary.port, fixture.replset_name))
            if time.time() - retry_start_time > retry_time_secs:
                raise errors.ServerFailure(
                    "Old primary on port {} of replica set {} did not step down in {} seconds.".
                    format(node.port, fixture.replset_name, retry_time_secs))
            time.sleep(0.2)

        self.logger.info("Old primary on port {} in set {} successfully stepped down".format(
            old_primary.port, fixture.replset_name))
