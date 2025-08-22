"""Test hook that periodically drops and rereates the sessions collection."""

import os.path
import random
import threading
import time

import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import replicaset, shardedcluster
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.retry import with_naive_retry


class DropSessionsCollection(interface.Hook):
    """Regularly connect to replica sets and drops and recreates the sessions collection."""

    DESCRIPTION = (
        "Sessions collection drop (drops and recreates config.system.sessions in the background)."
    )

    IS_BACKGROUND = True

    # The hook does not affect the fixture itself.
    STOPS_FIXTURE = False

    def __init__(
        self,
        hook_logger,
        fixture,
        is_fsm_workload=False,
        auth_options=None,
    ):
        """Initialize the DropSessionsCollection.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (replica sets or a sharded cluster).
            is_fsm_workload: Whether the hook is running as an FSM workload is executing
            auth_options: dictionary of auth options.
        """
        interface.Hook.__init__(self, hook_logger, fixture, DropSessionsCollection.DESCRIPTION)

        self._fixture = fixture

        self._drop_sessions_collection_thread = None

        self._auth_options = auth_options

        # The action file names need to match the same construction as found in
        # jstests/concurrency/fsm_libs/resmoke_runner.js.
        dbpath_prefix = fixture.get_dbpath_prefix()

        # When running an FSM workload, we use the file-based lifecycle protocol
        # in which a file is used as a form of communication between the hook and
        # the FSM workload to decided when the hook is allowed to run.
        if is_fsm_workload:
            # Each hook uses a unique set of action files - the uniqueness is brought
            # about by using the hook's name as a suffix.
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
        if self.__action_files is not None:
            lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.__action_files)
        else:
            lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        self._drop_sessions_collection_thread = _DropSessionsCollectionThread(
            self.logger,
            lifecycle,
            self._fixture,
            self._auth_options,
        )
        self.logger.info("Starting the drop sessions collection thread.")
        self._drop_sessions_collection_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the drop sessions collection thread.")
        self._drop_sessions_collection_thread.stop()
        self.logger.info("drop sessions collection thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the drop sessions collection thread.")
        self._drop_sessions_collection_thread.pause()
        self._drop_sessions_collection_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the drop sessions collection thread.")
        self._drop_sessions_collection_thread.pause()
        self.logger.info("Paused the drop sessions collection thread.")


class _DropSessionsCollectionThread(threading.Thread):
    def __init__(
        self,
        logger,
        drop_lifecycle,
        fixture,
        auth_options=None,
    ):
        """Initialize _DropSessionsCollectionThread."""
        threading.Thread.__init__(self, name="DropSessionsCollectionThread")
        self.daemon = True
        self.logger = logger
        self.__lifecycle = drop_lifecycle
        self._fixture = fixture
        self._auth_options = auth_options

        self._last_exec = time.time()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is .
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

                # Randomize the dropping and recreating so that we also get test coverage of the
                # collection not existing for longer (drop only) and refreshes happening when the
                # collection already exists (recreate only)
                for cluster in self._fixture.get_testable_clusters():
                    if random.choice([True, False]):
                        self._drop_sessions_collection(cluster)
                    if random.choice([True, False]):
                        self._recreate_sessions_collection(cluster)

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                # Choose a random number of seconds to wait, between 0 and 8.
                wait_secs = random.randint(0, 8)
                self.__lifecycle.wait_for_action_interval(wait_secs)
        except Exception:
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("DropSessionsCollection Thread threw exception")
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

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        # Wait until stop or timeout.
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        if not self.is_alive():
            msg = "The drop sessions collection thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _rs_drop_collection(self, rs_fixture):
        primary = rs_fixture.get_primary().mongo_client()
        primary.config.system.sessions.drop()

    def _sc_block_sessions_refresh(self, sc_fixture):
        # First configure the failpoint on all CSRS nodes - this will fail any refresh which tries
        # to create the sessions collection.
        failpoint_cmd = {
            "configureFailPoint": "preventSessionsCollectionSharding",
            "mode": "alwaysOn",
        }
        for node in sc_fixture.configsvr.nodes:
            client = node.mongo_client()
            client.admin.command(failpoint_cmd)
        # Now run a refresh, this will take the mutex protecting collection creation thus ensuring
        # any ongoing periodic refresh is drained.
        try:
            with_naive_retry(lambda: self._refresh_sessions_collection(sc_fixture))
        except pymongo.errors.OperationFailure as err:
            # 117 = ConflictingOperationInProgress - this is the error code the failpoint throws.
            if err.code != 117:
                raise err

    def _sc_unblock_sessions_refresh(self, sc_fixture):
        # Turn off the failpoint on all nodes, thus re-allowing refreshes
        failpoint_cmd = {
            "configureFailPoint": "preventSessionsCollectionSharding",
            "mode": "off",
        }
        for node in sc_fixture.configsvr.nodes:
            client = node.mongo_client()
            client.admin.command(failpoint_cmd)

    def _sc_drop_collection(self, sc_fixture, uuid):
        config_primary = sc_fixture.configsvr.get_primary().mongo_client()
        config_db = config_primary.get_database(
            "config",
            read_concern=pymongo.read_concern.ReadConcern(level="majority"),
            write_concern=pymongo.write_concern.WriteConcern(w="majority"),
        )
        # Drop sharding catalog metadata
        config_db.collections.delete_one({"_id": "config.system.sessions"})
        config_db.chunks.delete_many({"uuid": uuid})
        # Drop collection on all replica sets
        config_db.system.sessions.drop()
        config_primary.admin.command(
            {
                "_flushRoutingTableCacheUpdatesWithWriteConcern": "config.system.sessions",
                "writeConcern": {"w": "majority"},
            }
        )
        for shard in sc_fixture.shards:
            shard_primary = shard.get_primary().mongo_client()
            shard_primary.get_database(
                "config",
                read_concern=pymongo.read_concern.ReadConcern(level="majority"),
                write_concern=pymongo.write_concern.WriteConcern(w="majority"),
            ).system.sessions.drop()
            shard_primary.admin.command(
                {
                    "_flushRoutingTableCacheUpdatesWithWriteConcern": "config.system.sessions",
                    "writeConcern": {"w": "majority"},
                }
            )

    def _drop_sessions_collection(self, fixture):
        self.logger.info("Starting drop of the sessions collection.")
        if isinstance(fixture, replicaset.ReplicaSetFixture):
            with_naive_retry(lambda: self._rs_drop_collection(fixture))
        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            coll_doc = with_naive_retry(
                lambda: fixture.configsvr.get_primary()
                .mongo_client()
                .config.get_collection(
                    "collections",
                    read_concern=pymongo.read_concern.ReadConcern(level="majority"),
                    write_concern=pymongo.write_concern.WriteConcern(w="majority"),
                )
                .find_one({"_id": "config.system.sessions"})
            )
            if not coll_doc or "uuid" not in coll_doc:
                return
            self._sc_block_sessions_refresh(fixture)
            with_naive_retry(lambda: self._sc_drop_collection(fixture, coll_doc["uuid"]))
            self._sc_unblock_sessions_refresh(fixture)
        self.logger.info("Finished drop of the sessions collection.")

    def _refresh_sessions_collection(self, fixture):
        primary_conn = None
        if isinstance(fixture, replicaset.ReplicaSetFixture):
            primary_conn = fixture.get_primary().mongo_client()
        elif isinstance(fixture, shardedcluster.ShardedClusterFixture):
            primary_conn = fixture.configsvr.get_primary().mongo_client()
        if not primary_conn:
            return
        primary_conn.admin.command({"refreshLogicalSessionCacheNow": 1})

    def _recreate_sessions_collection(self, fixture):
        self.logger.info("Starting refresh of the sessions collection.")
        try:
            # We retry also on NamespaceNotFound as this indicates we ran the command on a
            # secondary. Since the function to retry includes the get_primary, this should find the
            # new primary before retrying.
            with_naive_retry(
                lambda: self._refresh_sessions_collection(fixture), extra_retryable_error_codes=[26]
            )
        except pymongo.errors.OperationFailure as err:
            if err.code != 64:
                raise err
            self.logger.info("Ignoring acceptable refreshLogicalSessionCache error: " + str(err))
        self.logger.info("Finished refresh of the sessions collection.")
