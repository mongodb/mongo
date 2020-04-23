"""Test hook that periodically makes the primary of a replica set step down."""

import collections
import os.path
import random
import threading
import time

import bson
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface


class ContinuousStepdown(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Regularly connect to replica sets and send a replSetStepDown command."""

    DESCRIPTION = ("Continuous stepdown (steps down the primary of replica sets at regular"
                   " intervals)")

    def __init__(  # pylint: disable=too-many-arguments
            self, hook_logger, fixture, config_stepdown=True, shard_stepdown=True,
            stepdown_interval_ms=8000, terminate=False, kill=False,
            use_stepdown_permitted_file=False, wait_for_mongos_retarget=False,
            stepdown_via_heartbeats=True, background_reconfig=False):
        """Initialize the ContinuousStepdown.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (a replica set or sharded cluster).
            config_stepdown: whether to stepdown the CSRS.
            shard_stepdown: whether to stepdown the shard replica sets in a sharded cluster.
            stepdown_interval_ms: the number of milliseconds between stepdowns.
            terminate: shut down the node cleanly as a means of stepping it down.
            kill: With a 50% probability, kill the node instead of shutting it down cleanly.
            use_stepdown_permitted_file: use a file to control if stepdown thread should do a stepdown.
            wait_for_mongos_retarget: whether to run validate on all mongoses for each collection
                in each database, after pausing the stepdown thread.
            stepdown_via_heartbeats: step up secondaries instead of stepping down primary.

        Note that the "terminate" and "kill" arguments are named after the "SIGTERM" and
        "SIGKILL" signals that are used to stop the process. On Windows, there are no signals,
        so we use a different means to achieve the same result as sending SIGTERM or SIGKILL.
        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousStepdown.DESCRIPTION)

        self._fixture = fixture
        self._config_stepdown = config_stepdown
        self._shard_stepdown = shard_stepdown
        self._stepdown_interval_secs = float(stepdown_interval_ms) / 1000
        self._wait_for_mongos_retarget = wait_for_mongos_retarget
        self._stepdown_via_heartbeats = stepdown_via_heartbeats

        self._rs_fixtures = []
        self._mongos_fixtures = []
        self._stepdown_thread = None

        # kill implies terminate.
        self._terminate = terminate or kill
        self._kill = kill

        self._background_reconfig = background_reconfig

        # The stepdown file names need to match the same construction as found in
        # jstests/concurrency/fsm_libs/resmoke_runner.js.
        dbpath_prefix = fixture.get_dbpath_prefix()

        if use_stepdown_permitted_file:
            self.__stepdown_files = StepdownFiles._make(
                [os.path.join(dbpath_prefix, field) for field in StepdownFiles._fields])
        else:
            self.__stepdown_files = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._rs_fixtures:
            self._add_fixture(self._fixture)

        if self.__stepdown_files is not None:
            lifecycle = FileBasedStepdownLifecycle(self.__stepdown_files)
        else:
            lifecycle = FlagBasedStepdownLifecycle()

        self._stepdown_thread = _StepdownThread(
            self.logger, self._mongos_fixtures, self._rs_fixtures, self._stepdown_interval_secs,
            self._terminate, self._kill, lifecycle, self._wait_for_mongos_retarget,
            self._stepdown_via_heartbeats, self._background_reconfig)
        self.logger.info("Starting the stepdown thread.")
        self._stepdown_thread.start()

    def after_suite(self, test_report):
        """After suite."""
        self.logger.info("Stopping the stepdown thread.")
        self._stepdown_thread.stop()
        self.logger.info("Stepdown thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self._check_thread()
        self.logger.info("Resuming the stepdown thread.")
        self._stepdown_thread.pause()
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
            if self._wait_for_mongos_retarget:
                for mongos_fixture in fixture.mongos:
                    self._mongos_fixtures.append(mongos_fixture)


class FlagBasedStepdownLifecycle(object):
    """Class for managing the various states of the stepdown thread.

    The job thread alternates between calling mark_test_started() and mark_test_finished(). The
    stepdown thread is allowed to perform stepdowns at any point between these two calls. Note that
    the job thread synchronizes with the stepdown thread outside the context of this object to know
    it isn't in the process of running a stepdown.
    """

    _TEST_STARTED_STATE = "start"
    _TEST_FINISHED_STATE = "finished"

    def __init__(self):
        """Initialize the FlagBasedStepdownLifecycle instance."""
        self.__lock = threading.Lock()
        self.__cond = threading.Condition(self.__lock)

        self.__test_state = self._TEST_FINISHED_STATE
        self.__should_stop = False

    def mark_test_started(self):
        """Signal to the stepdown thread that a new test has started.

        This function should be called during before_test(). Calling it causes the
        wait_for_stepdown_permitted() function to no longer block and to instead return true.
        """
        with self.__lock:
            self.__test_state = self._TEST_STARTED_STATE
            self.__cond.notify_all()

    def mark_test_finished(self):
        """Signal to the stepdown thread that the current test has finished.

        This function should be called during after_test(). Calling it causes the
        wait_for_stepdown_permitted() function to block until mark_test_started() is called again.
        """
        with self.__lock:
            self.__test_state = self._TEST_FINISHED_STATE
            self.__cond.notify_all()

    def stop(self):
        """Signal to the stepdown thread that it should exit.

        This function should be called during after_suite(). Calling it causes the
        wait_for_stepdown_permitted() function to no longer block and to instead return false.
        """
        with self.__lock:
            self.__should_stop = True
            self.__cond.notify_all()

    def wait_for_stepdown_permitted(self):
        """Block until stepdowns are permitted, or until stop() is called.

        :return: true if stepdowns are permitted, and false if steps are not permitted.
        """
        with self.__lock:
            while not self.__should_stop:
                if self.__test_state == self._TEST_STARTED_STATE:
                    return True

                self.__cond.wait()

        return False

    def wait_for_stepdown_interval(self, timeout):
        """Block for 'timeout' seconds, or until stop() is called."""
        with self.__lock:
            self.__cond.wait(timeout)

    def poll_for_idle_request(self):  # noqa: D205,D400
        """Return true if the stepdown thread should continue running stepdowns, or false if it
        should temporarily stop running stepdowns.
        """
        with self.__lock:
            return self.__test_state == self._TEST_FINISHED_STATE

    def send_idle_acknowledgement(self):
        """No-op.

        This method exists so this class has the same interface as FileBasedStepdownLifecycle.
        """
        pass


StepdownFiles = collections.namedtuple("StepdownFiles", ["permitted", "idle_request", "idle_ack"])


class FileBasedStepdownLifecycle(object):
    """Class for managing the various states of the stepdown thread using files.

    Unlike in the FlagBasedStepdownLifecycle class, the job thread alternating between calls to
    mark_test_started() and mark_test_finished() doesn't automatically grant permission for the
    stepdown thread to perform stepdowns. Instead, the test will part-way through allow stepdowns to
    be performed and then will part-way through disallow stepdowns from continuing to be performed.

    See jstests/concurrency/fsm_libs/resmoke_runner.js for the other half of the file-base protocol.

        Python inside of resmoke.py                     JavaScript inside of the mongo shell
        ---------------------------                     ------------------------------------

                                                        FSM workload starts.
                                                        Call $config.setup() function.
                                                        Create "permitted" file.

        Wait for "permitted" file to be created.

        Stepdown runs.
        Check if "idle_request" file exists.

        Stepdown runs.
        Check if "idle_request" file exists.

                                                        FSM workload threads all finish.
                                                        Create "idle_request" file.

        Stepdown runs.
        Check if "idle_request" file exists.
        Create "idle_ack" file.
        (No more stepdowns run.)

                                                        Wait for "idle_ack" file.
                                                        Call $config.teardown() function.
                                                        FSM workload finishes.

    Note that the job thread still synchronizes with the stepdown thread outside the context of this
    object to know it isn't in the process of running a stepdown.
    """

    def __init__(self, stepdown_files):
        """Initialize the FileBasedStepdownLifecycle instance."""
        self.__stepdown_files = stepdown_files

        self.__lock = threading.Lock()
        self.__cond = threading.Condition(self.__lock)

        self.__should_stop = False

    def mark_test_started(self):
        """Signal to the stepdown thread that a new test has started.

        This function should be called during before_test(). Calling it does nothing because
        permission for running stepdowns is given by the test itself writing the "permitted" file.
        """
        pass

    def mark_test_finished(self):
        """Signal to the stepdown thread that the current test has finished.

        This function should be called during after_test(). Calling it causes the
        wait_for_stepdown_permitted() function to block until the next test gives permission for
        running stepdowns.
        """
        # It is possible something went wrong during the test's execution and prevented the
        # "permitted" and "idle_request" files from being created. We therefore don't consider it an
        # error if they don't exist after the test has finished.
        utils.remove_if_exists(self.__stepdown_files.permitted)
        utils.remove_if_exists(self.__stepdown_files.idle_request)
        utils.remove_if_exists(self.__stepdown_files.idle_ack)

    def stop(self):
        """Signal to the stepdown thread that it should exit.

        This function should be called during after_suite(). Calling it causes the
        wait_for_stepdown_permitted() function to no longer block and to instead return false.
        """
        with self.__lock:
            self.__should_stop = True
            self.__cond.notify_all()

    def wait_for_stepdown_permitted(self):
        """Block until stepdowns are permitted, or until stop() is called.

        :return: true if stepdowns are permitted, and false if steps are not permitted.
        """
        with self.__lock:
            while not self.__should_stop:
                if os.path.isfile(self.__stepdown_files.permitted):
                    return True

                # Wait a little bit before checking for the "permitted" file again.
                self.__cond.wait(0.1)

        return False

    def wait_for_stepdown_interval(self, timeout):
        """Block for 'timeout' seconds, or until stop() is called."""
        with self.__lock:
            self.__cond.wait(timeout)

    def poll_for_idle_request(self):  # noqa: D205,D400
        """Return true if the stepdown thread should continue running stepdowns, or false if it
        should temporarily stop running stepdowns.
        """
        if os.path.isfile(self.__stepdown_files.idle_request):
            with self.__lock:
                return True

        return False

    def send_idle_acknowledgement(self):
        """Signal to the running test that stepdown thread won't continue to run stepdowns."""

        with open(self.__stepdown_files.idle_ack, "w"):
            pass

        # We remove the "permitted" file to revoke permission for the stepdown thread to continue
        # performing stepdowns.
        os.remove(self.__stepdown_files.permitted)


class _StepdownThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    def __init__(  # pylint: disable=too-many-arguments
            self, logger, mongos_fixtures, rs_fixtures, stepdown_interval_secs, terminate, kill,
            stepdown_lifecycle, wait_for_mongos_retarget, stepdown_via_heartbeats,
            background_reconfig):
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
        self._should_wait_for_mongos_retarget = wait_for_mongos_retarget
        self._stepdown_via_heartbeats = stepdown_via_heartbeats
        self._background_reconfig = background_reconfig

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

                permitted = self.__lifecycle.wait_for_stepdown_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                now = time.time()
                if now - self._last_exec > self._stepdown_interval_secs:
                    self.logger.info("Starting stepdown of all primaries")
                    self._step_down_all()
                    # Wait until each replica set has a primary, so the test can make progress.
                    self._await_primaries()
                    self._last_exec = time.time()
                    self.logger.info("Completed stepdown of all primaries in %0d ms",
                                     (self._last_exec - now) * 1000)

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                # The 'wait_secs' is used to wait 'self._stepdown_interval_secs' from the moment
                # the last stepdown command was sent.
                now = time.time()
                wait_secs = max(0, self._stepdown_interval_secs - (now - self._last_exec))
                self.__lifecycle.wait_for_stepdown_interval(wait_secs)
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Stepdown Thread threw exception")

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
        # Wait until we all the replica sets have primaries.
        self._await_primaries()
        # Wait for Mongos to retarget the primary for each shard and the config server.
        self._do_wait_for_mongos_retarget()

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

    def _await_primaries(self):
        for fixture in self._rs_fixtures:
            fixture.get_primary()

    def _step_down_all(self):
        for rs_fixture in self._rs_fixtures:
            self._step_down(rs_fixture)

    # pylint: disable=R0912,R0915
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
            raise errors.ServerFailure("ReplicaSetFixture {} expected to be running in"
                                       " ContinuousStepdown, but wasn't.".format(
                                           rs_fixture.replset_name))

        if self._terminate:
            # If we're running with background reconfigs, it's possible to be in a scenario
            # where we kill a necessary voting node (i.e. in a 5 node repl set), only 2 are
            # voting. In this scenario, we want to avoid killing the primary because no
            # secondary can step up.
            if self._background_reconfig:
                # stagger the kill thread so that it runs a little after the reconfig thread
                time.sleep(1)
                voting_members = rs_fixture.get_voting_members()

                self.logger.info("Current voting members: %s", voting_members)

                if len(voting_members) <= 3:
                    # Do not kill or terminate the primary if we don't have enough voting nodes to
                    # elect a new primary.
                    return

            should_kill = self._kill and random.choice([True, False])
            action = "Killing" if should_kill else "Terminating"
            self.logger.info("%s the primary on port %d of replica set '%s'.", action, primary.port,
                             rs_fixture.replset_name)

            # We send the mongod process the signal to exit but don't immediately wait for it to
            # exit because clean shutdown may take a while and we want to restore write availability
            # as quickly as possible.
            teardown_mode = fixture_interface.TeardownMode.KILL if should_kill else fixture_interface.TeardownMode.TERMINATE
            primary.mongod.stop(mode=teardown_mode)
        elif not self._stepdown_via_heartbeats:
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

        # We have skipped stepping down the primary if we want to step up secondaries instead. Here,
        # we simply need to pick an arbitrary secondary to run for election which will lead to
        # unconditional stepdown on the primary.
        #
        # If we have terminated/killed/stepped down the primary above, write availability has lost.
        # We pick an arbitrary secondary to run for election immediately in order to avoid a long
        # period where the replica set doesn't have write availability. If none of the secondaries
        # are eligible, or their election attempt fails, then we'll run the replSetStepUp command on
        # 'primary' to ensure we have write availability sooner than the
        # self._stepdown_duration_secs duration expires.
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
            except pymongo.errors.AutoReconnect:
                # It is possible for a replSetStepUp to fail with AutoReconnect if that node goes
                # into Rollback (which causes it to close any open connections).
                pass

        if self._terminate:
            self.logger.info("Waiting for the old primary on port %d of replica set '%s' to exit.",
                             primary.port, rs_fixture.replset_name)

            primary.mongod.wait()

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
        elif not self._stepdown_via_heartbeats:
            # If we chose to step up a secondary instead, the primary was never stepped down via the
            # replSetStepDown command and thus will not have a stepdown period. So we can skip
            # running {replSetFreeze: 0}. Otherwise, the replSetStepDown command run earlier
            # introduced a stepdown period on the former primary and so we have to run the
            # {replSetFreeze: 0} command to ensure the former primary is electable in the next
            # round of _step_down().
            client = primary.mongo_client()
            client.admin.command({"replSetFreeze": 0})
        elif secondaries:
            # We successfully stepped up a secondary, wait for the former primary to step down via
            # heartbeats. We need to wait for the former primary to step down to complete this step
            # down round and to avoid races between the ContinuousStepdown hook and other test hooks
            # that may depend on the health of the replica set.
            self.logger.info(
                "Successfully stepped up the secondary on port %d of replica set '%s'.",
                chosen.port, rs_fixture.replset_name)
            while True:
                try:
                    client = primary.mongo_client()
                    is_secondary = client.admin.command("isMaster")["secondary"]
                    if is_secondary:
                        break
                except pymongo.errors.AutoReconnect:
                    pass
                self.logger.info("Waiting for primary on port %d of replica set '%s' to step down.",
                                 primary.port, rs_fixture.replset_name)
                time.sleep(0.2)  # Wait a little bit before trying again.
            self.logger.info("Primary on port %d of replica set '%s' stepped down.", primary.port,
                             rs_fixture.replset_name)

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
                    client = primary.mongo_client()
                    client.admin.command("replSetStepUp")
                    break
                except pymongo.errors.OperationFailure:
                    self._wait(0.2)
                if time.time() - retry_start_time > retry_time_secs:
                    raise errors.ServerFailure(
                        "The old primary on port {} of replica set {} did not step up in"
                        " {} seconds.".format(client.port, rs_fixture.replset_name,
                                              retry_time_secs))

        # Bump the counter for the chosen secondary to indicate that the replSetStepUp command
        # executed successfully.
        key = "{}/{}".format(rs_fixture.replset_name,
                             chosen.get_internal_connection_string() if secondaries else "none")
        self._step_up_stats[key] += 1

    def _do_wait_for_mongos_retarget(self):  # pylint: disable=too-many-branches
        """Run collStats on each collection in each database on each mongos.

        This is to ensure mongos can target the primary for each shard with data, including the
        config servers.
        """
        if not self._should_wait_for_mongos_retarget:
            return

        for mongos_fixture in self._mongos_fixtures:
            mongos_conn_str = mongos_fixture.get_internal_connection_string()
            try:
                client = mongos_fixture.mongo_client()
            except pymongo.errors.AutoReconnect:
                pass
            for db in client.database_names():
                self.logger.info("Waiting for mongos %s to retarget db: %s", mongos_conn_str, db)
                start_time = time.time()
                while True:
                    try:
                        coll_names = client[db].collection_names()
                        break
                    except pymongo.errors.NotMasterError:
                        pass
                    retarget_time = time.time() - start_time
                    if retarget_time >= 60:
                        raise RuntimeError(
                            "Timeout waiting for mongos: {} to retarget to db: {}".format(
                                mongos_conn_str, db))
                    time.sleep(0.2)
                for coll in coll_names:
                    while True:
                        try:
                            client[db].command({"collStats": coll})
                            break
                        except pymongo.errors.NotMasterError:
                            pass
                        retarget_time = time.time() - start_time
                        if retarget_time >= 60:
                            raise RuntimeError(
                                "Timeout waiting for mongos: {} to retarget to db: {}".format(
                                    mongos_conn_str, db))
                        time.sleep(0.2)
                retarget_time = time.time() - start_time
                self.logger.info("Finished waiting for mongos: %s to retarget db: %s, in %d ms",
                                 mongos_conn_str, db, retarget_time * 1000)
