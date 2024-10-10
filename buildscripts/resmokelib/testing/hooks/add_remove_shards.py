"""Test hook that periodically adds and removes a shard. That shard may be the config server, in
which case it is transitioned in/out of config shard mode.
"""

import os.path
import random
import re
import threading
import time

import bson
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.retry import (
    retryable_code_names as retryable_network_err_names,
)
from buildscripts.resmokelib.testing.retry import retryable_codes as retryable_network_errs

# The possible number of seconds to wait before initiating a transition.
TRANSITION_INTERVALS = [10]


class ContinuousAddRemoveShard(interface.Hook):
    DESCRIPTION = (
        "Continuously adds and removes shards at regular intervals. If running with configsvr "
        + "transitions, will transition in/out of config shard mode."
    )

    IS_BACKGROUND = True

    STOPS_FIXTURE = False

    def __init__(
        self,
        hook_logger,
        fixture,
        is_fsm_workload=False,
        auth_options=None,
        random_balancer_on=True,
        transition_configsvr=False,
        add_remove_random_shards=False,
        move_primary_comment=None,
        transition_intervals=TRANSITION_INTERVALS,
    ):
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousAddRemoveShard.DESCRIPTION)
        self._fixture = fixture
        self._add_remove_thread = None
        self._auth_options = auth_options
        self._random_balancer_on = random_balancer_on
        self._transition_configsvr = transition_configsvr
        self._add_remove_random_shards = add_remove_random_shards
        self._move_primary_comment = move_primary_comment
        self._transition_intervals = transition_intervals

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

        if not isinstance(self._fixture, shardedcluster.ShardedClusterFixture):
            msg = "Can only add and remove shards for sharded cluster fixtures."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        if not self._transition_configsvr and not self._add_remove_random_shards:
            msg = "Continuous add and remove shard hook must run with either or both of "
            "transition_configsvr: true or add_remove_random_shards: true."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        self._add_remove_thread = _AddRemoveShardThread(
            self.logger,
            lifecycle,
            self._fixture,
            self._auth_options,
            self._random_balancer_on,
            self._transition_configsvr,
            self._add_remove_random_shards,
            self._move_primary_comment,
            self._transition_intervals,
        )
        self.logger.info("Starting the add/remove shard thread.")
        self._add_remove_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the add/remove shard thread.")
        self._add_remove_thread.stop()
        self.logger.info("Add/remove shard thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the add/remove shard thread.")
        self._add_remove_thread.pause()
        self._add_remove_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the add/remove shard thread.")
        self._add_remove_thread.pause()
        self.logger.info("Paused the add/remove shard thread.")


class _AddRemoveShardThread(threading.Thread):
    CONFIG_SHARD = "config shard mode"
    DEDICATED = "dedicated config server mode"
    TRANSITION_TIMEOUT_SECS = float(900)  # 15 minutes
    # Error codes, taken from mongo/base/error_codes.yml.
    _NAMESPACE_NOT_FOUND = 26
    _INTERRUPTED = 11601
    _CONFLICTING_OPERATION_IN_PROGRESS = 117
    _BACKGROUND_OPERATION_IN_PROGRESS_FOR_NAMESPACE = 12587
    _ILLEGAL_OPERATION = 20
    _SHARD_NOT_FOUND = 70
    _OPERATION_FAILED = 96
    _RESHARD_COLLECTION_ABORTED = 341
    _RESHARD_COLLECTION_IN_PROGRESS = 338
    _LOCK_BUSY = 46
    _FAILED_TO_SATISFY_READ_PREFERENCE = 133

    _UNMOVABLE_NAMESPACE_REGEXES = [
        r"\.system\.",
        r"enxcol_\..*\.esc",
        r"enxcol_\..*\.ecc",
        r"enxcol_\..*\.ecoc",
    ]

    def __init__(
        self,
        logger,
        life_cycle,
        fixture,
        auth_options,
        random_balancer_on,
        transition_configsvr,
        add_remove_random_shards,
        move_primary_comment,
        transition_intervals,
    ):
        threading.Thread.__init__(self, name="AddRemoveShardThread")
        self.logger = logger
        self.__lifecycle = life_cycle
        self._fixture = fixture
        self._auth_options = auth_options
        self._random_balancer_on = random_balancer_on
        self._transition_configsvr = transition_configsvr
        self._add_remove_random_shards = add_remove_random_shards
        self._move_primary_comment = move_primary_comment
        self._transition_intervals = transition_intervals
        self._client = fixture_interface.build_client(self._fixture, self._auth_options)
        self._current_config_mode = self._current_fixture_mode()
        self._should_wait_for_balancer_round = False
        self._shard_name_suffix = 0

        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing stepdowns.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

    def _current_fixture_mode(self):
        res = self._client.admin.command({"listShards": 1})

        for shard_info in res["shards"]:
            if shard_info["_id"] == "config":
                return self.CONFIG_SHARD

        return self.DEDICATED

    def _pick_shard_to_add_remove(self):
        if not self._add_remove_random_shards:
            return "config", None

        # If running with both config transitions and random shard add/removals, pick any shard
        # including the config shard. Otherwise, pick any shard that is not the config shard.
        shard_to_remove_and_add = (
            self._get_other_shard_info(None)
            if self._transition_configsvr and self._current_config_mode is self.CONFIG_SHARD
            else self._get_other_shard_info("config")
        )
        return shard_to_remove_and_add["_id"], shard_to_remove_and_add["host"]

    def run(self):
        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                # Pick the shard to add/remove this round
                shard_id, shard_host = self._pick_shard_to_add_remove()

                wait_secs = random.choice(self._transition_intervals)
                msg = (
                    "transition to dedicated."
                    if shard_id == "config"
                    else "removing shard " + shard_id + "."
                )
                self.logger.info(f"Waiting {wait_secs} seconds before " + msg)
                self.__lifecycle.wait_for_action_interval(wait_secs)

                succeeded = self._transition_to_dedicated_or_remove_shard(shard_id)
                if not succeeded:
                    # The transition failed with a retryable error, so loop around and try again.
                    continue

                shard_obj = None
                removed_shard_fixture = None
                if shard_id == "config":
                    self._current_config_mode = self.DEDICATED
                    removed_shard_fixture = self._fixture.configsvr
                else:
                    self.logger.info("Decomissioning removed shard " + shard_id + ".")
                    shard_obj = self._fixture.get_shard_object(shard_host)
                    removed_shard_fixture = shard_obj
                    self._decomission_removed_shard(shard_obj)

                self._run_post_remove_shard_checks(removed_shard_fixture, shard_id)

                wait_secs = random.choice(self._transition_intervals)
                msg = (
                    "transition to config shard."
                    if shard_id == "config"
                    else "adding shard " + shard_id + "."
                )
                self.logger.info(f"Waiting {wait_secs} seconds before " + msg)
                self.__lifecycle.wait_for_action_interval(wait_secs)

                # Always end with with same shard list at the test end as at startup.

                # If we decomissioned the shard, restart it before adding it back.
                if shard_id != "config":
                    self.logger.info("Restarting decomissioned shard " + shard_id + ".")
                    shard_obj.setup()

                self._transition_to_config_shard_or_add_shard(shard_id, shard_host)
                if shard_id == "config":
                    self._current_config_mode = self.CONFIG_SHARD

                if self.__lifecycle.poll_for_idle_request():
                    self.__lifecycle.send_idle_acknowledgement()

        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Add/Remove Shard Thread threw exception")
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

        # Wait until we are no longer executing transitions.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _check_thread(self):
        if not self.is_alive():
            msg = "The add/remove shard thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _is_expected_move_collection_error(self, err, namespace):
        if err.code == self._NAMESPACE_NOT_FOUND:
            # A concurrent dropDatabase or dropCollection could have removed the database before we
            # run moveCollection.
            return True

        if err.code == self._BACKGROUND_OPERATION_IN_PROGRESS_FOR_NAMESPACE:
            # Ongoing background operations (e.g. index builds) will prevent moveCollection until
            # they complete.
            return True

        if err.code == self._RESHARD_COLLECTION_ABORTED:
            # Tests with interruptions may interrupt moveCollection operation, causing it to get
            # aborted.
            return True

        if err.code == self._RESHARD_COLLECTION_IN_PROGRESS:
            # Tests with interruptions may interrupt the transition thread while running
            # moveCollection, leading the thread to retry and hit ReshardCollectionInProgress.
            # Also, if random balancing is on, the balancer will also move unsharded collections
            # (both tracked and untracked). So the moveCollection operation initiated by the
            # balancer may conflict with the moveCollection operation initiated by this hook.
            return True

        if err.code == self._ILLEGAL_OPERATION:
            if "Can't move an internal resharding collection" in str(err):
                return True
            if "Can't reshard a timeseries collection" in str(err):
                return True
            for regex in self._UNMOVABLE_NAMESPACE_REGEXES:
                if re.search(regex, namespace):
                    return True

        return False

    def _is_expected_move_primary_error_code(self, code):
        if code == self._NAMESPACE_NOT_FOUND:
            # A concurrent dropDatabase could have removed the database before we run movePrimary.
            return True

        if code == self._CONFLICTING_OPERATION_IN_PROGRESS:
            # Tests with interruptions may interrupt the add/remove shard thread while running
            # movePrimary, leading the thread to retry and hit ConflictingOperationInProgress.
            return True

        if code == self._LOCK_BUSY:
            # If there is an in-progress moveCollection operation, movePrimary would fail to acquire
            # the DDL lock.
            return True

        if code == 7120202:
            # Tests with stepdowns might interrupt the movePrimary during the cloning phase,
            # but the _shardsvrClongCatalogData command is not idempotent so the coordinator
            # will fail the request if cloning has started.
            return True

        if code == 9046501:
            # This is an error thrown by a failpoint inside movePrimary when there are still user
            # collections to clone.
            return True

        return False

    def _is_expected_transition_error_code(self, code):
        if code == self._INTERRUPTED:
            # Some workloads kill sessions which may interrupt the transition.
            return True

        if code == self._CONFLICTING_OPERATION_IN_PROGRESS:
            # Trying to update the cluster cardinality parameter in addShard or
            # removeShard will fail with this error if there is another
            # setClusterParameter command already running.
            return True

        if code == 8955101:
            # If there is a failover during _shardsvrJoinMigrations, removeShard will fail with
            # anonymous error 8955101.
            # TODO SERVER-90212 remove this exception for 8955101.
            return True

        return False

    def _decomission_removed_shard(self, shard_obj):
        start_time = time.time()

        while True:
            if time.time() - start_time > self.TRANSITION_TIMEOUT_SECS:
                msg = "Timed out waiting for removed shard to finish data clean up"
                self.logger.error(msg)
                raise errors.ServerFailure(msg)

            direct_shard_conn = pymongo.MongoClient(shard_obj.get_driver_connection_url())

            # Wait until any DDL, resharding, transactions, and migration ops are cleaned up.
            # TODO SERVER-90782 Change these to be assertions, rather than waiting for the collections
            # to be empty
            if len(list(direct_shard_conn.config.system.sharding_ddl_coordinators.find())) != 0:
                self.logger.info(
                    "Waiting for config.system.sharding_ddl_coordinators to be empty before decomissioning."
                )
                time.sleep(1)
                continue

            if len(list(direct_shard_conn.config.localReshardingOperations.recipient.find())) != 0:
                self.logger.info(
                    "Waiting for config.localReshardingOperations.recipient to be empty before decomissioning."
                )
                time.sleep(1)
                continue

            if len(list(direct_shard_conn.config.transaction_coordinators.find())) != 0:
                self.logger.info(
                    "Waiting for config.transaction_coordinators to be empty before decomissioning."
                )
                time.sleep(1)
                continue

            # TODO SERVER-91474 Wait for ongoing transactions to finish on participants
            if self._get_number_of_ongoing_transactions(direct_shard_conn) != 0:
                self.logger.info(
                    "Waiting for ongoing transactions to commit or abort before decomissioning."
                )
                time.sleep(1)
                continue

            # TODO SERVER-50144 Wait for config.rangeDeletions to be empty before decomissioning

            all_dbs = direct_shard_conn.admin.command({"listDatabases": 1})
            for db in all_dbs["databases"]:
                if db["name"] not in ["admin", "config", "local"] and db["empty"] is False:
                    all_collections = direct_shard_conn.db_name.command({"listCollections": 1})
                    for coll in all_collections:
                        if len(list(direct_shard_conn.db_name.coll.find())) != 0:
                            msg = "Found non-empty collection after removing shard: " + coll
                            self.logger.error(msg)
                            raise errors.ServerFailure(msg)

            break

        teardown_handler = fixture_interface.FixtureTeardownHandler(self.logger)
        shard_obj.removeshard_teardown_marker = True
        teardown_handler.teardown(shard_obj, "shard")
        if not teardown_handler.was_successful():
            msg = "Error when decomissioning shard."
            self.logger.error(msg)
            raise errors.ServerFailure(teardown_handler.get_error_message())

    def _get_tracked_collections_on_shard(self, shard_id):
        return list(
            self._client.config.collections.aggregate(
                [
                    {
                        "$lookup": {
                            "from": "chunks",
                            "localField": "uuid",
                            "foreignField": "uuid",
                            "as": "chunksOnRemovedShard",
                            "pipeline": [
                                {"$match": {"shard": shard_id}},
                                # History can be very large because we randomize migrations, so
                                # exclude it to reduce log spam.
                                {"$project": {"history": 0}},
                            ],
                        }
                    },
                    {"$match": {"chunksOnRemovedShard": {"$ne": []}}},
                ]
            )
        )

    def _get_untracked_collections_on_shard(self, source):
        untracked_collections = []
        databases = list(
            self._client.config.databases.aggregate(
                [
                    {
                        "$match": {"primary": source},
                    }
                ]
            )
        )
        for database in databases:
            # listCollections will return the bucket collection and the timeseries views and
            # adding them both to the list of untracked collections to move will trigger two
            # moveCollections for the same bucket collection. We can exclude the bucket collections
            # from the list of collections to move since it doesn't give us any extra test coverage.
            for collection in self._client.get_database(database["_id"]).list_collections(
                filter={"name": {"$not": {"$regex": ".*system\.buckets.*"}}}
            ):
                namespace = database["_id"] + "." + collection["name"]
                coll_doc = self._client.config.collections.find_one({"_id": namespace})
                if not coll_doc:
                    collection["_id"] = namespace
                    untracked_collections.append(collection)
        return untracked_collections

    def _move_all_collections_from_shard(self, collections, source):
        for collection in collections:
            namespace = collection["_id"]
            destination = self._get_other_shard_id(source)
            self.logger.info("Running moveCollection for " + namespace + " to " + destination)
            try:
                self._client.admin.command({"moveCollection": namespace, "toShard": destination})
            except pymongo.errors.OperationFailure as err:
                if not self._is_expected_move_collection_error(err, namespace):
                    raise err
                self.logger.info(
                    "Ignoring error when moving the collection '" + namespace + "': " + str(err)
                )
                if err.code == self._RESHARD_COLLECTION_IN_PROGRESS:
                    self.logger.info(
                        "Skip moving the other collections since there is already a resharding "
                        + "operation in progress"
                    )
                    return

    def _move_all_primaries_from_shard(self, databases, source):
        for database in databases:
            destination = self._get_other_shard_id(source)
            try:
                self.logger.info("Running movePrimary for " + database + " to " + destination)
                cmd_obj = {"movePrimary": database, "to": destination}
                if self._move_primary_comment:
                    cmd_obj["comment"] = self._move_primary_comment
                self._client.admin.command(cmd_obj)
            except pymongo.errors.OperationFailure as err:
                if not self._is_expected_move_primary_error_code(err.code):
                    raise err
                self.logger.info(
                    "Ignoring error when moving the database '" + database + "': " + str(err)
                )

    def _drain_shard_for_ongoing_transition(self, num_rounds, transition_result, source):
        tracked_colls = self._get_tracked_collections_on_shard(source)
        sharded_colls = []
        tracked_unsharded_colls = []
        for coll in tracked_colls:
            if "unsplittable" in coll:
                tracked_unsharded_colls.append(coll)
            else:
                sharded_colls.append(coll)
        untracked_unsharded_colls = self._get_untracked_collections_on_shard(source)

        if num_rounds % 10 == 0:
            self.logger.info("Draining shard " + source + ": " + str({"num_rounds": num_rounds}))
            self.logger.info(
                "Sharded collections on "
                + source
                + ": "
                + str({"count": len(sharded_colls), "collections": sharded_colls})
            )
            self.logger.info(
                "Tracked unsharded collections on "
                + source
                + ": "
                + str(
                    {"count": len(tracked_unsharded_colls), "collections": tracked_unsharded_colls}
                )
            )
            self.logger.info(
                "Untracked unsharded collections on "
                + source
                + ": "
                + str(
                    {
                        "count": len(untracked_unsharded_colls),
                        "collections": untracked_unsharded_colls,
                    }
                )
            )
            self.logger.info(
                "Databases on "
                + source
                + ": "
                + str(
                    {
                        "count": len(transition_result["dbsToMove"]),
                        "collections": transition_result["dbsToMove"],
                    }
                )
            )

        # If random balancing is on, the balancer will also move unsharded collections (both tracked
        # and untracked). However, random balancing is a test-only setting. In production, users are
        # expected to move unsharded collections manually. So even when random balancing is on,
        # still move collections half of the time.
        should_move = not self._random_balancer_on or random.random() < 0.5
        if should_move:
            self._move_all_collections_from_shard(
                tracked_unsharded_colls + untracked_unsharded_colls, source
            )
        self._move_all_primaries_from_shard(transition_result["dbsToMove"], source)

    def _get_balancer_status_on_shard_not_found(self, prev_round_interrupted, msg):
        try:
            latest_status = self._client.admin.command({"balancerStatus": 1})
        except pymongo.errors.OperationFailure as balancerStatusErr:
            if balancerStatusErr.code in set(retryable_network_errs):
                self.logger.info(
                    "Network error when running balancerStatus after "
                    "receiving ShardNotFound error on " + msg + ", will "
                    "retry. err: " + str(balancerStatusErr)
                )
                prev_round_interrupted = False
                return None, prev_round_interrupted

            if balancerStatusErr.code not in [self._INTERRUPTED]:
                raise balancerStatusErr

            prev_round_interrupted = True
            self.logger.info(
                "Ignoring 'Interrupted' error when running balancerStatus "
                "after receiving ShardNotFound error on " + msg
            )
            return None, prev_round_interrupted

        return latest_status, prev_round_interrupted

    def _transition_to_dedicated_or_remove_shard(self, shard_id):
        if shard_id == "config":
            self.logger.info("Starting transition from " + self._current_config_mode)
        else:
            self.logger.info("Starting removal of " + shard_id)

        res = None
        start_time = time.time()
        last_balancer_status = None
        prev_round_interrupted = False
        num_draining_rounds = -1

        msg = "transition to dedicated" if shard_id == "config" else "removing shard"

        while True:
            try:
                if last_balancer_status is None:
                    last_balancer_status = self._client.admin.command({"balancerStatus": 1})

                if self._should_wait_for_balancer_round:
                    # TODO SERVER-90291: Remove.
                    #
                    # Wait for one balancer round after starting to drain if the shard owned no
                    # chunks to avoid a race where the migration of the first chunk to the shard
                    # can leave the collection orphaned on it after it's been removed as a shard.
                    latest_status = self._client.admin.command({"balancerStatus": 1})

                    if last_balancer_status["term"] != latest_status["term"]:
                        self.logger.info(
                            "Detected change in repl set term while waiting for a balancer round "
                            "before " + msg + ". last term: %d, new term: %d",
                            last_balancer_status["term"],
                            latest_status["term"],
                        )
                        last_balancer_status = latest_status
                        time.sleep(1)
                        continue

                    if (
                        last_balancer_status["numBalancerRounds"]
                        >= latest_status["numBalancerRounds"]
                    ):
                        self.logger.info(
                            "Waiting for a balancer round before "
                            + msg
                            + ". Last round: %d, latest round: %d",
                            last_balancer_status["numBalancerRounds"],
                            latest_status["numBalancerRounds"],
                        )
                        time.sleep(1)
                        continue

                    self.logger.info("Done waiting for a balancer round before " + msg)
                    self._should_wait_for_balancer_round = False

                if shard_id == "config":
                    res = self._client.admin.command({"transitionToDedicatedConfigServer": 1})
                else:
                    res = self._client.admin.command({"removeShard": shard_id})

                if res["state"] == "completed":
                    self.logger.info(
                        "Completed " + msg + " in %0d ms", (time.time() - start_time) * 1000
                    )
                    return True
                elif res["state"] == "started":
                    if self._client.config.chunks.count_documents({"shard": shard_id}) == 0:
                        self._should_wait_for_balancer_round = True
                elif res["state"] == "ongoing":
                    num_draining_rounds += 1
                    self._drain_shard_for_ongoing_transition(num_draining_rounds, res, shard_id)

                prev_round_interrupted = False
                time.sleep(1)

                if time.time() - start_time > self.TRANSITION_TIMEOUT_SECS:
                    msg = "Could not " + msg + " with last response: " + str(res)
                    self.logger.error(msg)
                    raise errors.ServerFailure(msg)
            except pymongo.errors.OperationFailure as err:
                # Some workloads add and remove shards so removing the config shard may fail transiently.
                if err.code in [self._ILLEGAL_OPERATION] and "would remove the last shard" in str(
                    err
                ):
                    # Abort the transition attempt and make the hook try again later.
                    return False

                # Some suites run with forced failovers, if transitioning fails with a retryable
                # network error, we should retry.
                if err.code in set(retryable_network_errs):
                    self.logger.info(
                        "Network error during " + msg + ", will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    prev_round_interrupted = False
                    continue

                # Some suites kill the primary causing the request to fail with
                # FailedToSatisfyReadPreference
                if err.code in [self._FAILED_TO_SATISFY_READ_PREFERENCE]:
                    self.logger.info(
                        "Primary not found when " + msg + ", will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    continue

                # If there was a failover when finishing the transition to a dedicated CSRS/shard removal or if
                # the transitionToDedicated/removeShard request was interrupted when finishing the transition,
                # it's possible that this thread didn't learn that the removal finished. When the
                # the transition to dedicated is retried, it will fail because the shard will no longer exist.
                if err.code in [self._SHARD_NOT_FOUND]:
                    latest_status, prev_round_interrupted = (
                        self._get_balancer_status_on_shard_not_found(prev_round_interrupted, msg)
                    )
                    if latest_status is None:
                        # The balancerStatus request was interrupted, so we retry the transition
                        # request. We will fail with ShardNotFound again, and will retry this check
                        # again.
                        time.sleep(1)
                        continue

                    if last_balancer_status is None:
                        last_balancer_status = latest_status

                    if (
                        last_balancer_status["term"] != latest_status["term"]
                        or prev_round_interrupted
                    ):
                        self.logger.info(
                            "Did not find entry for "
                            + shard_id
                            + " in config.shards after detecting a "
                            "change in repl set term or after transition was interrutped. Assuming "
                            + msg
                            + " finished on previous transition request."
                        )
                        return True

                if not self._is_expected_transition_error_code(err.code):
                    raise err

                prev_round_interrupted = True
                self.logger.info("Ignoring error when " + msg + " : " + str(err))

    def _transition_to_config_shard_or_add_shard(self, shard_id, shard_host):
        if shard_id == "config":
            self.logger.info("Starting transition from " + self._current_config_mode)
        else:
            self.logger.info("Starting to add shard " + shard_id)

        msg = "transitioning from dedicated" if shard_id == "config" else "adding shard"

        while True:
            try:
                if shard_id == "config":
                    self._client.admin.command({"transitionFromDedicatedConfigServer": 1})
                else:
                    original_shard_id = (
                        shard_id if self._shard_name_suffix == 0 else shard_id.split("_")[0]
                    )
                    shard_name = original_shard_id + "_" + str(self._shard_name_suffix)
                    self.logger.info("Adding shard with new shardId: " + shard_name)
                    self._client.admin.command({"addShard": shard_host, "name": shard_name})
                    self._shard_name_suffix = self._shard_name_suffix + 1
                return
            except pymongo.errors.OperationFailure as err:
                # Some suites run with forced failovers, if transitioning fails with a retryable
                # network error, we should retry.
                if err.code in set(retryable_network_errs):
                    self.logger.info(
                        "Network error when " + msg + " server, will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    continue

                # If one of the nodes in the shard is killed just before the attempt to
                # transition/addShard, addShard will fail because it will not be able to connect. The
                # error code returned is not retryable (it is OperationFailed), so we check the specific
                # error message as well.
                if err.code in [self._OPERATION_FAILED] and (
                    "Connection refused" in str(err)
                    or any(err_name in str(err) for err_name in retryable_network_err_names)
                ):
                    self.logger.info(
                        "Connection refused when " + msg + ", will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    continue

                # Some suites kill the primary causing the request to fail with
                # FailedToSatisfyReadPreference
                if err.code in [self._FAILED_TO_SATISFY_READ_PREFERENCE]:
                    self.logger.info(
                        "Primary not found when " + msg + ", will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    continue

                # Some workloads kill sessions which may interrupt the transition.
                if not self._is_expected_transition_error_code(err.code):
                    raise err
                self.logger.info("Ignoring error " + msg + " : " + str(err))

    def _get_other_shard_info(self, shard_id):
        res = self._client.admin.command({"listShards": 1})

        if len(res["shards"]) < 2:
            msg = "Did not find a shard different from " + shard_id
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        possible_choices = []
        if shard_id is not None:
            possible_choices = [
                shard_info for shard_info in res["shards"] if shard_info["_id"] != shard_id
            ]
        else:
            possible_choices = [shard_info for shard_info in res["shards"]]

        return random.choice(possible_choices)

    def _get_other_shard_id(self, shard_id):
        return self._get_other_shard_info(shard_id)["_id"]

    def _get_number_of_ongoing_transactions(self, shard_conn):
        res = list(
            shard_conn.admin.aggregate(
                [
                    {
                        "$currentOp": {
                            "allUsers": True,
                            "idleConnections": True,
                            "idleSessions": True,
                        }
                    },
                    {"$match": {"transaction": {"$exists": True}}},
                    {"$count": "num_ongoing_txns"},
                ]
            )
        )
        return res[0]["num_ongoing_txns"] if res else 0

    def _run_post_remove_shard_checks(self, removed_shard_fixture, removed_shard_name):
        while True:
            try:
                # Configsvr metadata checks:
                ## Check that the removed shard no longer exists on config.shards.
                assert (
                    self._client["config"]["shards"].count_documents({"_id": removed_shard_name})
                    == 0
                ), f"Removed shard still exists on config.shards: {removed_shard_name}"

                ## Check that no database has the removed shard as its primary shard.
                databasesPointingToRemovedShard = [
                    doc
                    for doc in self._client["config"]["databases"].find(
                        {"primary": removed_shard_name}
                    )
                ]
                assert not databasesPointingToRemovedShard, f"Found databases whose primary shard is a removed shard: {databasesPointingToRemovedShard}"

                ## Check that no chunk has the removed shard as its owner.
                chunksPointingToRemovedShard = [
                    doc
                    for doc in self._client["config"]["chunks"].find({"shard": removed_shard_name})
                ]
                assert (
                    not chunksPointingToRemovedShard
                ), f"Found chunks whose owner is a removed shard: {chunksPointingToRemovedShard}"

                ## Check that all tag in config.tags refer to at least one existing shard.
                tagsWithoutShardPipeline = [
                    {
                        "$lookup": {
                            "from": "shards",
                            "localField": "tag",
                            "foreignField": "tags",
                            "as": "shards",
                        }
                    },
                    {"$match": {"shards": []}},
                ]
                tagsWithoutShardPipelineResultCursor = self._client["config"]["tags"].aggregate(
                    tagsWithoutShardPipeline
                )
                tagsWithoutShardPipelineResult = [
                    doc for doc in tagsWithoutShardPipelineResultCursor
                ]
                assert not tagsWithoutShardPipelineResult, f"Found tags in config.tags that are not owned by any shard: {tagsWithoutShardPipelineResult}"

                if removed_shard_name != "config":
                    return

                # Check that there is no user data left on the removed shard. (Note: This can only be
                # checked on transitionToDedicatedConfigServer)
                removed_shard_primary_client = removed_shard_fixture.get_primary().mongo_client()
                dbs = removed_shard_primary_client.list_database_names()
                assert all(
                    databaseName in {"local", "admin", "config"} for databaseName in dbs
                ), f"Expected to not have any user database on removed shard: {dbs}"

                # Check the filtering metadata on removed shard. Expect that the shard knows that it does
                # not own any chunk anymore. Check on all replica set nodes.
                # First, await secondaries to replicate the last optime
                removed_shard_fixture.await_last_op_committed(
                    removed_shard_fixture.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60
                )
                for removed_shard_node in [
                    removed_shard_fixture.get_primary()
                ] + removed_shard_fixture.get_secondaries():
                    sharding_state_response = removed_shard_node.mongo_client().admin.command(
                        {"shardingState": 1}
                    )
                    for nss, metadata in sharding_state_response["versions"].items():
                        # placementVersion == Timestamp(0, 0) means that this shard owns no chunk for the
                        # collection.

                        # TODO (SERVER-90810): Re-enable this check for resharding temporary collections.
                        if "system.resharding" in nss or "system.buckets.resharding" in nss:
                            continue

                        assert (
                            metadata["placementVersion"] == bson.Timestamp(0, 0)
                        ), f"Expected remove shard's filtering information to reflect that the shard does not own any chunk for collection {nss}, but found {metadata} on node {removed_shard_node.get_driver_connection_url()}"

                return
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError) as err:
                # The above operations run directly on a shard, so they may fail getting a
                # connection if the shard node is killed.
                self.logger.info(
                    "Connection error when running post removal checks, will retry. err: "
                    + str(err)
                )
                continue
            except pymongo.errors.OperationFailure as err:
                # Retry on retryable errors that might be thrown in suites with forced failovers.
                if err.code in set(retryable_network_errs):
                    self.logger.info(
                        "Retryable error when running post removal checks, will retry. err: "
                        + str(err)
                    )
                    continue
                if err.code in set([self._INTERRUPTED]):
                    # Some workloads kill sessions which may interrupt the transition.
                    self.logger.info(
                        "Received 'Interrupted' error when running post removal checks, will retry. err: "
                        + str(err)
                    )
                    continue

                raise err
