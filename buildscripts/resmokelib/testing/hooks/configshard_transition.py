"""Test hook that periodically transitions the config server in/out of config shard mode."""

import time
import threading
import random

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

    def __init__(self, hook_logger, fixture, auth_options=None):
        interface.Hook.__init__(self, hook_logger, fixture,
                                ContinuousConfigShardTransition.DESCRIPTION)
        self._fixture = fixture
        self._transition_thread = None
        self._auth_options = auth_options

    def before_suite(self, test_report):
        """Before suite."""
        lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        if not isinstance(self._fixture, shardedcluster.ShardedClusterFixture):
            msg = "Can only transition config shard mode for sharded cluster fixtures."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

        self._transition_thread = _TransitionThread(self.logger, lifecycle, self._fixture,
                                                    self._auth_options)
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

    def __init__(self, logger, stepdown_lifecycle, fixture, auth_options):
        threading.Thread.__init__(self, name="TransitionThread")
        self.logger = logger
        self.__lifecycle = stepdown_lifecycle
        self._fixture = fixture
        self._auth_options = auth_options
        self._client = fixture_interface.build_client(self._fixture, self._auth_options)
        self._current_mode = self._current_fixture_mode()

        self._last_exec = time.time()
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
                secs = float(10)
                now = time.time()
                if now - self._last_exec > secs:
                    self.logger.info("Starting transition from " + self._current_mode)
                    if self._current_mode is self.CONFIG_SHARD:
                        self._transition_to_dedicated()
                        self._current_mode = self.DEDICATED
                    else:
                        self._transition_to_config_shard()
                        self._current_mode = self.CONFIG_SHARD
                    self._last_exec = time.time()
                    self.logger.info("Completed transition to %s in %0d ms", self._current_mode,
                                     (self._last_exec - now) * 1000)
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

        # Wait until we are no longer executing stepdowns.
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

    def _transition_to_dedicated(self):
        res = None
        start_time = time.time()
        while True:
            res = self._client.admin.command({"transitionToDedicatedConfigServer": 1})

            if res["state"] == "completed":
                break
            elif res["state"] == "ongoing" and res["dbsToMove"]:
                non_config_shard_id = self._get_non_config_shard_id()
                for db in res["dbsToMove"]:
                    msg = "running movePrimary for: " + str(db)
                    self.logger.info(msg)
                    self._client.admin.command({"movePrimary": db, "to": non_config_shard_id})

            time.sleep(1)

            if time.time() - start_time > float(300):
                msg = "Could not transition to dedicated config server. with last response: " + str(
                    res)
                self.logger.error(msg)
                raise errors.ServerFailure(msg)

    def _transition_to_config_shard(self):
        self._client.admin.command({"transitionFromDedicatedConfigServer": 1})

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
