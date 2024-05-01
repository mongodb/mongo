"""Test hook that periodically transitions the config server in/out of config shard mode."""

import time
import threading
import random
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface


class ContinuousConfigShardTransition(interface.Hook):
    DESCRIPTION = (
        "Continuous config shard transition (transitions in/out of config shard mode at regular"
        " intervals)")

    IS_BACKGROUND = True

    STOPS_FIXTURE = False

    def __init__(self, hook_logger, fixture, auth_options=None, random_balancer_on=True):
        interface.Hook.__init__(self, hook_logger, fixture,
                                ContinuousConfigShardTransition.DESCRIPTION)
        self._fixture = fixture
        self._transition_thread = None
        self._auth_options = auth_options
        self._random_balancer_on = random_balancer_on

    def before_suite(self, test_report):
        """Before suite."""
        lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        if not isinstance(self._fixture, shardedcluster.ShardedClusterFixture):
            msg = "Can only transition config shard mode for sharded cluster fixtures."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        self._transition_thread = _TransitionThread(self.logger, lifecycle, self._fixture,
                                                    self._auth_options, self._random_balancer_on)
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
    _ILLEGAL_OPERATION = 20

    def __init__(self, logger, stepdown_lifecycle, fixture, auth_options, random_balancer_on):
        threading.Thread.__init__(self, name="TransitionThread")
        self.logger = logger
        self.__lifecycle = stepdown_lifecycle
        self._fixture = fixture
        self._auth_options = auth_options
        self._random_balancer_on = random_balancer_on
        self._client = fixture_interface.build_client(self._fixture, self._auth_options)
        self._current_mode = self._current_fixture_mode()

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

                # Wait a random interval before transitioning back, unless the test already ended.
                if not self.__lifecycle.poll_for_idle_request():
                    wait_secs = random.choice(self.TRANSITION_INTERVALS)
                    self.logger.info(
                        f"Waiting {wait_secs} seconds before transition to config shard.")
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

    # Moves databases and any unsplittable collections off the config shard. Note this only moves
    # unsplittable collections if at least one database is owned by the config shard so it will not
    # work if random balancing may put unsplittable collections on shards other than the db primary.
    def _move_from_config_no_random_balancer(self, dbs_to_move):
        non_config_shard_id = self._get_non_config_shard_id()
        for db in dbs_to_move:
            try:
                colls_in_db = list(self._client.config.collections.find())
                for coll in colls_in_db:
                    if "unsplittable" in coll:
                        coll_ns = coll["_id"]
                        msg = "running moveCollection for: " + str(coll_ns)
                        self.logger.info(msg)
                        self._client.admin.command(
                            {"moveCollection": coll_ns, "toShard": non_config_shard_id})
                self._client.admin.command({"movePrimary": db, "to": non_config_shard_id})
            except pymongo.errors.OperationFailure as err:
                # A concurrent dropDatabase could have removed the database before we run movePrimary/moveCollection.
                if err.code not in [self._NAMESPACE_NOT_FOUND]:
                    raise err

    # Moves databases off the config shard. Assumes the balancer will move any chunks or
    # unsplittable collections off it, the latter requires the random balancing fail point to be on.
    def _move_from_config(self, res):
        non_config_shard_id = self._get_non_config_shard_id()

        # Find and log all chunks/collections owned by the config server to help debugging.
        colls_with_chunks_on_config = list(
            self._client.config.collections.aggregate([
                {
                    "$lookup": {
                        "from":
                            "chunks",
                        "localField":
                            "uuid",
                        "foreignField":
                            "uuid",
                        "as":
                            "chunksOnConfig",
                        "pipeline": [
                            {"$match": {"shard": "config"}},
                            # History can be very large because we randomize migrations, so
                            # exclude it to reduce log spam.
                            {"$project": {"history": 0}}
                        ]
                    }
                },
                {"$match": {"chunksOnConfig": {"$ne": []}}}
            ]))
        msg = "collections with chunks on config server: " + str(colls_with_chunks_on_config)
        self.logger.info(msg)

        if res["dbsToMove"]:
            for db in res["dbsToMove"]:
                try:
                    self._client.admin.command({"movePrimary": db, "to": non_config_shard_id})
                except pymongo.errors.OperationFailure as err:
                    # A concurrent dropDatabase could have removed the database before we run
                    # movePrimary/moveCollection.
                    # Tests with interruptions may interrupt the transition thread while running
                    # movePrimary, leading the thread to retry and hit
                    # ConflictingOperationInProgress.
                    if err.code not in [
                            self._NAMESPACE_NOT_FOUND, self._CONFLICTING_OPERATION_IN_PROGRESS
                    ]:
                        raise err

    def _transition_to_dedicated(self):
        self.logger.info("Starting transition from " + self._current_mode)
        res = None
        start_time = time.time()

        while True:
            try:
                res = self._client.admin.command({"transitionToDedicatedConfigServer": 1})

                if res["state"] == "completed":
                    self.logger.info("Completed transition to %s in %0d ms", self.DEDICATED,
                                     (time.time() - start_time) * 1000)
                    return True
                elif res["state"] == "ongoing":
                    if self._random_balancer_on:
                        # With random balancing, the balancer will move unsplittable collections
                        # from draining shards.
                        self._move_from_config(res)
                    elif res["dbsToMove"]:
                        # Without random balancing, the hook must move them manually, which
                        # currently requires dbsToMove being set.
                        self._move_from_config_no_random_balancer(res["dbsToMove"])

                time.sleep(1)

                if time.time() - start_time > self.TRANSITION_TIMEOUT_SECS:
                    msg = "Could not transition to dedicated config server. with last response: " + str(
                        res)
                    self.logger.error(msg)
                    raise errors.ServerFailure(msg)
            except pymongo.errors.OperationFailure as err:
                # Some workloads add and remove shards so removing the config shard may fail transiently.
                if err.code in [self._ILLEGAL_OPERATION
                                ] and err.errmsg and "would remove the last shard" in err.errmsg:
                    # Abort the transition attempt and make the hook try again later.
                    return False

                # Some workloads kill sessions which may interrupt the transition.
                if err.code not in [self._INTERRUPTED]:
                    raise err

                self.logger.info("Ignoring error transitioning to dedicated: " + str(err))

    def _transition_to_config_shard(self):
        self.logger.info("Starting transition from " + self._current_mode)
        try:
            self._client.admin.command({"transitionFromDedicatedConfigServer": 1})
        except pymongo.errors.OperationFailure as err:
            # Some workloads kill sessions which may interrupt the transition.
            if err.code not in [self._INTERRUPTED]:
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
