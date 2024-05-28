"""Test hook that periodically transitions the config server in/out of config shard mode."""

import bson
import time
import threading
import random
import re
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.retry import retryable_codes as retryable_network_errs
from buildscripts.resmokelib.testing.retry import (
    retryable_code_names as retryable_network_err_names,
)


class ContinuousConfigShardTransition(interface.Hook):
    DESCRIPTION = (
        "Continuous config shard transition (transitions in/out of config shard mode at regular"
        " intervals)"
    )

    IS_BACKGROUND = True

    STOPS_FIXTURE = False

    def __init__(
        self,
        hook_logger,
        fixture,
        auth_options=None,
        random_balancer_on=True,
        move_primary_comment=None,
    ):
        interface.Hook.__init__(
            self, hook_logger, fixture, ContinuousConfigShardTransition.DESCRIPTION
        )
        self._fixture = fixture
        self._transition_thread = None
        self._auth_options = auth_options
        self._random_balancer_on = random_balancer_on
        self._move_primary_comment = move_primary_comment

    def before_suite(self, test_report):
        """Before suite."""
        lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        if not isinstance(self._fixture, shardedcluster.ShardedClusterFixture):
            msg = "Can only transition config shard mode for sharded cluster fixtures."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        self._transition_thread = _TransitionThread(
            self.logger,
            lifecycle,
            self._fixture,
            self._auth_options,
            self._random_balancer_on,
            self._move_primary_comment,
        )
        self.logger.info("Starting the transition thread.")
        self._transition_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the transition thread.")
        self._transition_thread.stop()
        self.logger.info("Transition thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the transition thread.")
        self._transition_thread.pause()
        self._transition_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the transition thread.")
        self._transition_thread.pause()
        self.logger.info("Paused the transition thread.")


class _TransitionThread(threading.Thread):
    CONFIG_SHARD = "config shard mode"
    DEDICATED = "dedicated config server mode"
    # The possible number of seconds to wait before initiating a transition.
    TRANSITION_INTERVALS = [0, 1, 1, 1, 1, 3, 5, 10]
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

    _UNMOVABLE_NAMESPACE_REGEXES = [
        r"\.system\.",
        r"enxcol_\..*\.esc",
        r"enxcol_\..*\.ecc",
        r"enxcol_\..*\.ecoc",
    ]

    def __init__(
        self,
        logger,
        stepdown_lifecycle,
        fixture,
        auth_options,
        random_balancer_on,
        move_primary_comment,
    ):
        threading.Thread.__init__(self, name="TransitionThread")
        self.logger = logger
        self.__lifecycle = stepdown_lifecycle
        self._fixture = fixture
        self._auth_options = auth_options
        self._random_balancer_on = random_balancer_on
        self._move_primary_comment = move_primary_comment
        self._client = fixture_interface.build_client(self._fixture, self._auth_options)
        self._current_mode = self._current_fixture_mode()
        self._should_wait_for_balancer_round = False

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

    def run(self):
        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                wait_secs = random.choice(self.TRANSITION_INTERVALS)
                self.logger.info(f"Waiting {wait_secs} seconds before transition to dedicated.")
                self.__lifecycle.wait_for_action_interval(wait_secs)

                succeeded = self._transition_to_dedicated()
                if not succeeded:
                    # The transition failed with a retryable error, so loop around and try again.
                    continue

                self._current_mode = self.DEDICATED

                self._run_post_remove_shard_checks(self._fixture.configsvr, "config")

                # Wait a random interval before transitioning back, unless the test already ended.
                if not self.__lifecycle.poll_for_idle_request():
                    wait_secs = random.choice(self.TRANSITION_INTERVALS)
                    self.logger.info(
                        f"Waiting {wait_secs} seconds before transition to config shard."
                    )
                    self.__lifecycle.wait_for_action_interval(wait_secs)

                # Always end in config shard mode so the shard list at test startup is the
                # same at the end.
                self._transition_to_config_shard()
                self._current_mode = self.CONFIG_SHARD

        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Transition Thread threw exception")
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
            msg = "The transition thread is not running."
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
            if "Cannot remove from a capped collection in a multi-document transaction" in str(err):
                # TODO (SERVER-89826): Investigate errors related to moving capped collection that
                # are part of multi-document transactions.
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
            # Tests with interruptions may interrupt the transition thread while running
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

    def _get_tracked_collections_on_config(self):
        return list(
            self._client.config.collections.aggregate(
                [
                    {
                        "$lookup": {
                            "from": "chunks",
                            "localField": "uuid",
                            "foreignField": "uuid",
                            "as": "chunksOnConfig",
                            "pipeline": [
                                {"$match": {"shard": "config"}},
                                # History can be very large because we randomize migrations, so
                                # exclude it to reduce log spam.
                                {"$project": {"history": 0}},
                            ],
                        }
                    },
                    {"$match": {"chunksOnConfig": {"$ne": []}}},
                ]
            )
        )

    def _get_untracked_collections_on_config(self):
        untracked_collections = []
        databases = list(
            self._client.config.databases.aggregate(
                [
                    {
                        "$match": {"primary": "config"},
                    }
                ]
            )
        )
        for database in databases:
            for collection in self._client.get_database(database["_id"]).list_collections():
                namespace = database["_id"] + "." + collection["name"]
                coll_doc = self._client.config.collections.find_one({"_id": namespace})
                if not coll_doc:
                    collection["_id"] = namespace
                    untracked_collections.append(collection)
        return untracked_collections

    def _move_collection_all_from_config(self, collections):
        for collection in collections:
            namespace = collection["_id"]
            destination = self._get_non_config_shard_id()
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

    def _move_primary_all_from_config(self, databases):
        for database in databases:
            destination = self._get_non_config_shard_id()
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

    def _drain_config_for_ongoing_transition(self, num_rounds, transition_result):
        tracked_colls = self._get_tracked_collections_on_config()
        sharded_colls = []
        tracked_unsharded_colls = []
        for coll in tracked_colls:
            if "unsplittable" in coll:
                tracked_unsharded_colls.append(coll)
            else:
                sharded_colls.append(coll)
        untracked_unsharded_colls = self._get_untracked_collections_on_config()

        if num_rounds % 10 == 0:
            self.logger.info("Draining the config shard: " + str({"num_rounds": num_rounds}))
            self.logger.info(
                "Sharded collections on config server: "
                + str({"count": len(sharded_colls), "collections": sharded_colls})
            )
            self.logger.info(
                "Tracked unsharded collections on config server: "
                + str(
                    {"count": len(tracked_unsharded_colls), "collections": tracked_unsharded_colls}
                )
            )
            self.logger.info(
                "Untracked unsharded collections on config server: "
                + str(
                    {
                        "count": len(untracked_unsharded_colls),
                        "collections": untracked_unsharded_colls,
                    }
                )
            )
            self.logger.info(
                "Databases on config server: "
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
            self._move_collection_all_from_config(
                tracked_unsharded_colls + untracked_unsharded_colls
            )
        self._move_primary_all_from_config(transition_result["dbsToMove"])

    def _get_balancer_status_on_shard_not_found(self, prev_round_interrupted):
        try:
            latest_status = self._client.admin.command({"balancerStatus": 1})
        except pymongo.errors.OperationFailure as balancerStatusErr:
            if balancerStatusErr.code in set(retryable_network_errs):
                self.logger.info(
                    "Network error when running balancerStatus after "
                    "receiving ShardNotFound error on transition to dedicated, will "
                    "retry. err: " + str(balancerStatusErr)
                )
                prev_round_interrupted = False
                return None, prev_round_interrupted

            if balancerStatusErr.code not in [self._INTERRUPTED]:
                raise balancerStatusErr

            prev_round_interrupted = True
            self.logger.info(
                "Ignoring 'Interrupted' error when running balancerStatus "
                "after receiving ShardNotFound error on transition to dedicated."
            )
            return None, prev_round_interrupted

        return latest_status, prev_round_interrupted

    def _transition_to_dedicated(self):
        self.logger.info("Starting transition from " + self._current_mode)
        res = None
        start_time = time.time()
        last_balancer_status = None
        prev_round_interrupted = False
        num_draining_rounds = -1

        while True:
            try:
                if last_balancer_status is None:
                    last_balancer_status = self._client.admin.command({"balancerStatus": 1})

                if self._should_wait_for_balancer_round:
                    # TODO SERVER-90291: Remove.
                    #
                    # Wait for one balancer round after starting to drain if the config server owned no
                    # chunks to avoid a race where the migration of the first chunk to the config server
                    # can leave the collection orphaned on it after it's been removed as a shard.
                    latest_status = self._client.admin.command({"balancerStatus": 1})

                    if last_balancer_status["term"] != latest_status["term"]:
                        self.logger.info(
                            "Detected change in repl set term while waiting for a balancer round "
                            "before transitioning to dedicated CSRS. last term: %d, new term: %d",
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
                            "Waiting for a balancer round before transition to dedicated. "
                            "Last round: %d, latest round: %d",
                            last_balancer_status["numBalancerRounds"],
                            latest_status["numBalancerRounds"],
                        )
                        time.sleep(1)
                        continue

                    self.logger.info(
                        "Done waiting for a balancer round before transition to dedicated"
                    )
                    self._should_wait_for_balancer_round = False

                res = self._client.admin.command({"transitionToDedicatedConfigServer": 1})

                if res["state"] == "completed":
                    self.logger.info(
                        "Completed transition to %s in %0d ms",
                        self.DEDICATED,
                        (time.time() - start_time) * 1000,
                    )
                    return True
                elif res["state"] == "started":
                    if self._client.config.chunks.count_documents({"shard": "config"}) == 0:
                        self._should_wait_for_balancer_round = True
                elif res["state"] == "ongoing":
                    num_draining_rounds += 1
                    self._drain_config_for_ongoing_transition(num_draining_rounds, res)

                prev_round_interrupted = False
                time.sleep(1)

                if time.time() - start_time > self.TRANSITION_TIMEOUT_SECS:
                    msg = (
                        "Could not transition to dedicated config server. with last response: "
                        + str(res)
                    )
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
                        "Network error when transitioning to dedicated config server, "
                        "will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    prev_round_interrupted = False
                    continue

                # If there was a failover when finishing the transition to a dedicated CSRS or if
                # the transitionToDedicated request was interrupted when finishing the transition,
                # it's possible that this thread didn't learn that the transition finished. When the
                # the transition to dedicated is retried, it will fail because the shard "config"
                # will no longer exist.
                if err.code in [self._SHARD_NOT_FOUND]:
                    latest_status, prev_round_interrupted = (
                        self._get_balancer_status_on_shard_not_found(prev_round_interrupted)
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
                            "Did not find entry for 'config' in config.shards after detecting a "
                            "change in repl set term or after transition was interrutped. Assuming "
                            "transition to dedicated finished on previous transition request."
                        )
                        return True

                if not self._is_expected_transition_error_code(err.code):
                    raise err

                prev_round_interrupted = True
                self.logger.info("Ignoring error transitioning to dedicated: " + str(err))

    def _transition_to_config_shard(self):
        self.logger.info("Starting transition from " + self._current_mode)
        while True:
            try:
                self._client.admin.command({"transitionFromDedicatedConfigServer": 1})
                return
            except pymongo.errors.OperationFailure as err:
                # Some suites run with forced failovers, if transitioning fails with a retryable
                # network error, we should retry.
                if err.code in set(retryable_network_errs):
                    self.logger.info(
                        "Network error when transitioning from dedicated config "
                        "server, will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    continue

                # If one of the nodes in the config server is killed just before attempting to
                # transition, addShard will fail because it will not be able to connect. The error
                # code return is not retryable (it is OperationFailed), so we check the specific
                # error message as well.
                if err.code in [self._OPERATION_FAILED] and (
                    "Connection refused" in str(err)
                    or any(err_name in str(err) for err_name in retryable_network_err_names)
                ):
                    self.logger.info(
                        "Network error adding shard when transitioning from dedicated config "
                        "server, will retry. err: " + str(err)
                    )
                    time.sleep(1)
                    continue

                # Some workloads kill sessions which may interrupt the transition.
                if not self._is_expected_transition_error_code(err.code):
                    raise err
                self.logger.info("Ignoring error transitioning to config shard: " + str(err))

    def _get_non_config_shard_id(self):
        res = self._client.admin.command({"listShards": 1})

        if len(res["shards"]) < 2:
            msg = "Did not find a non-config shard"
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        possible_choices = [
            shard_info["_id"] for shard_info in res["shards"] if shard_info["_id"] != "config"
        ]
        return random.choice(possible_choices)

    def _run_post_remove_shard_checks(self, removed_shard_fixture, removed_shard_name):
        # Configsvr metadata checks:
        ## Check that the removed shard no longer exists on config.shards.
        assert (
            self._client["config"]["shards"].count_documents({"_id": removed_shard_name}) == 0
        ), f"Removed shard still exists on config.shards: {removed_shard_name}"

        ## Check that no database has the removed shard as its primary shard.
        databasesPointingToRemovedShard = [
            doc for doc in self._client["config"]["databases"].find({"primary": removed_shard_name})
        ]
        assert not databasesPointingToRemovedShard, f"Found databases whose primary shard is a removed shard: {databasesPointingToRemovedShard}"

        ## Check that no chunk has the removed shard as its owner.
        chunksPointingToRemovedShard = [
            doc for doc in self._client["config"]["chunks"].find({"shard": removed_shard_name})
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
        tagsWithoutShardPipelineResult = [doc for doc in tagsWithoutShardPipelineResultCursor]
        assert not tagsWithoutShardPipelineResult, f"Found tags in config.tags that are not owned by any shard: {tagsWithoutShardPipelineResult}"

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
                if "system.resharding" in nss:
                    continue

                assert (
                    metadata["placementVersion"] == bson.Timestamp(0, 0)
                ), f"Expected remove shard's filtering information to reflect that the shard does not own any chunk for collection {nss}, but found {metadata} on node {removed_shard_node.get_driver_connection_url()}"
