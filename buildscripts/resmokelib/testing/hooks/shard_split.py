"""Test hook that runs shard splits continuously."""

import copy
import random
import threading
import time
import uuid

import bson
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import shard_split
from buildscripts.resmokelib.testing.fixtures.replicaset import ReplicaSetFixture
from buildscripts.resmokelib.testing.hooks import interface


class ContinuousShardSplit(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Starts a shard split thread at the beginning of each test."""

    DESCRIPTION = ("Continuous shard split operations")

    IS_BACKGROUND = True
    AWAIT_REPL_TIMEOUT_MINS = ReplicaSetFixture.AWAIT_REPL_TIMEOUT_MINS

    def __init__(self, hook_logger, fixture, shell_options):
        """Initialize the ContinuousShardSplit.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target ShardSplitFixture containing the donor replica set.
            shell_options: contains the global_vars which contains TestData.tenantIds to be used for
                           shard splits.

        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousShardSplit.DESCRIPTION)

        if not isinstance(fixture, shard_split.ShardSplitFixture):
            raise ValueError("The ContinuousShardSplit hook requires a ShardSplitFixture")
        self._shard_split_fixture = fixture
        self._shell_options = copy.deepcopy(shell_options)
        self._shard_split_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._shard_split_fixture:
            raise ValueError("No ShardSplitFixture to run shard splits on")
        self.logger.info("Starting the shard split thread.")
        self._shard_split_thread = _ShardSplitThread(self.logger, self._shard_split_fixture,
                                                     self._shell_options, test_report)
        self._shard_split_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the shard split thread.")
        self._shard_split_thread.stop()
        self.logger.info("Stopped the shard split thread.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the shard split thread.")
        self._shard_split_thread.resume(test)

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the shard split thread.")
        self._shard_split_thread.pause()
        self.logger.info("Paused the shard split thread.")


class ShardSplitLifeCycle(object):
    """Class for managing the various states of the shard split thread.

    The job thread alternates between calling mark_test_started() and mark_test_finished(). The
    shard split thread is allowed to perform splits at any point between these two calls.
    Note that the job thread synchronizes with the shard split thread outside the context of
    this object to know it isn't in the process of running a split.
    """

    _TEST_STARTED_STATE = "start"
    _TEST_FINISHED_STATE = "finished"

    def __init__(self):
        """Initialize the ShardSplitLifeCycle instance."""
        self.__lock = threading.Lock()
        self.__cond = threading.Condition(self.__lock)

        self.test_num = 0
        self.__test_state = self._TEST_FINISHED_STATE
        self.__should_stop = False

    def mark_test_started(self):
        """Signal to the shard split thread that a new test has started.

        This function should be called during before_test(). Calling it causes the
        wait_for_shard_split_permitted() function to no longer block and to instead return
        true.
        """
        with self.__lock:
            self.test_num += 1
            self.__test_state = self._TEST_STARTED_STATE
            self.__cond.notify_all()

    def mark_test_finished(self):
        """Signal to the shard split thread that the current test has finished.

        This function should be called during after_test(). Calling it causes the
        wait_for_shard_split_permitted() function to block until mark_test_started() is called
        again.
        """
        with self.__lock:
            self.__test_state = self._TEST_FINISHED_STATE
            self.__cond.notify_all()

    def is_test_finished(self):
        """Return true if the current test has finished."""
        with self.__lock:
            return self.__test_state == self._TEST_FINISHED_STATE

    def stop(self):
        """Signal to the shard split thread that it should exit.

        This function should be called during after_suite(). Calling it causes the
        wait_for_shard_split_permitted() function to no longer block and to instead return
        false.
        """
        with self.__lock:
            self.__should_stop = True
            self.__cond.notify_all()

    def wait_for_shard_split_permitted(self):
        """Block until splits are permitted, or until stop() is called."""
        with self.__lock:
            while not self.__should_stop:
                if self.__test_state == self._TEST_STARTED_STATE:
                    return True

                self.__cond.wait()

        return False

    def wait_for_shard_split_interval(self, timeout):
        """Block for 'timeout' seconds, or until stop() is called."""
        with self.__lock:
            self.__cond.wait(timeout)

    def poll_for_idle_request(self):  # noqa: D205,D400
        """Return true if the shard split thread should continue running splits, or false
        if it should temporarily stop running splits.
        """
        with self.__lock:
            return self.__test_state == self._TEST_FINISHED_STATE


class _ShardSplitOptions:
    def __init__(  # pylint: disable=too-many-arguments
            self, logger, shard_split_fixture, tenant_ids, recipient_tag_name, recipient_set_name):
        self.logger = logger
        self.migration_id = uuid.uuid4()
        self.shard_split_fixture = shard_split_fixture
        self.tenant_ids = tenant_ids
        self.recipient_tag_name = recipient_tag_name
        self.recipient_set_name = recipient_set_name

    def get_migration_id_as_binary(self):
        """Return the migration id as BSON Binary."""
        return bson.Binary(self.migration_id.bytes, 4)

    def get_donor_rs(self):
        """Return the current donor for the split fixture."""
        return self.shard_split_fixture.get_donor_rs()

    def get_donor_name(self):
        """Return the replica set name for the donor."""
        return self.get_donor_rs().replset_name

    def get_donor_primary(self):
        """Return a connection to the donor primary."""
        return self.get_donor_rs().get_primary(timeout_secs=self.AWAIT_REPL_TIMEOUT_MINS)

    def get_donor_nodes(self):
        """Return the nodes for the current shard split fixture donor."""
        return self.get_donor_rs().nodes

    def get_recipient_nodes(self):
        """Return the recipient nodes for the shard split fixture."""
        return self.shard_split_fixture.get_recipient_nodes()

    def __str__(self):
        opts = {
            "migration_id": self.migration_id, "tenant_ids": self.tenant_ids,
            "donor": self.get_donor_name(), "recipientSetName": self.recipient_set_name,
            "recipientTagName": self.recipient_tag_name
        }
        return str(opts)


class _ShardSplitThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    THREAD_NAME = "ShardSplitThread"

    WAIT_SECS_RANGES = [[0.05, 0.1], [0.1, 0.5], [1, 5], [5, 15]]
    POLL_INTERVAL_SECS = 0.1

    NO_SUCH_MIGRATION_ERR_CODE = 327
    INTERNAL_ERR_CODE = 1

    def __init__(self, logger, shard_split_fixture, shell_options, test_report):
        """Initialize _ShardSplitThread."""
        threading.Thread.__init__(self, name=self.THREAD_NAME)
        self.daemon = True
        self.logger = logger
        self._shard_split_fixture = shard_split_fixture
        self._tenant_ids = shell_options["global_vars"]["TestData"]["tenantIds"]
        self._auth_options = shell_options["global_vars"]["TestData"]["authOptions"]
        self._test = None
        self._test_report = test_report
        self._shell_options = shell_options

        self.__lifecycle = ShardSplitLifeCycle()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing shard splits.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

    def run(self):
        """Execute the thread."""
        if not self._shard_split_fixture:
            self.logger.warning("No ShardSplitFixture to run shard splits on.")
            return

        split_count = 0

        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_shard_split_permitted()
                if not permitted:
                    break

                if split_count >= 1:  # TODO(SERVER-65042): Remove this check and run multiple splits
                    time.sleep(self.POLL_INTERVAL_SECS)
                    continue

                self._is_idle_evt.clear()

                split_opts = self._create_split_opts(split_count)

                # Set up the donor for a split
                self._shard_split_fixture.add_recipient_nodes(split_opts.recipient_set_name)

                # Briefly wait to let the test run before starting the split operation, so that
                # the first split is more likely to have data to migrate.
                wait_secs = random.uniform(
                    *self.WAIT_SECS_RANGES[split_count % len(self.WAIT_SECS_RANGES)])
                self.logger.info(f"Waiting for {wait_secs} seconds before starting split.")
                self.__lifecycle.wait_for_shard_split_interval(wait_secs)

                self.logger.info(f"Starting shard split: {str(split_opts)}.")
                start_time = time.time()
                is_committed = self._run_shard_split(split_opts)
                end_time = time.time()

                split_count += 1
                self.logger.info(
                    f"Completed shard split {str(split_opts)} in {(end_time - start_time) * 1000} ms."
                )

                # set up the fixture for the next split operation
                if is_committed:
                    self._shard_split_fixture.replace_donor_with_recipient(
                        split_opts.recipient_set_name)
                else:
                    self._shard_split_fixture.remove_recipient_nodes()

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    continue
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be flushed immediately.
            self.logger.exception("Shard split thread threw exception")
            # The event should be signaled whenever the thread is not performing shard splits.
            self._is_idle_evt.set()

    def stop(self):
        """Stop the thread when the suite finishes."""
        self.__lifecycle.stop()
        self._is_stopped_evt.set()
        # Unpause to allow the thread to finish.
        self.resume(self._test)
        self.join()

    def pause(self):
        """Pause the thread after test."""
        self.__lifecycle.mark_test_finished()

        # Wait until we are no longer executing splits.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()

        # Check that the fixture is still running.
        if not self._shard_split_fixture.is_running():
            raise errors.ServerFailure(
                f"ShardSplitFixture with pids {self._shard_split_fixture.pids()} expected to be running in"
                " ContinuousShardSplit, but wasn't.")

    def resume(self, test):
        """Resume the thread before test."""
        self._test = test
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        """Wait until stop or timeout."""
        self._is_stopped_evt.wait(timeout)

    def short_name(self):
        """Return the name of the thread."""
        return self.THREAD_NAME

    def _check_thread(self):
        """Throw an error if the thread is not running."""
        if not self.is_alive():
            msg = "Shard split thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _is_fail_point_abort_reason(self, abort_reason):
        return abort_reason["code"] == self.INTERNAL_ERR_CODE and abort_reason[
            "errmsg"] == "simulate a shard split error"

    def _create_split_opts(self, split_count):
        recipient_set_name = f"rs{split_count+1}"
        recipient_tag_name = "recipientNode"
        return _ShardSplitOptions(self.logger, self._shard_split_fixture, self._tenant_ids,
                                  recipient_tag_name, recipient_set_name)

    def _create_client(self, node):
        return fixture_interface.authenticate(node.mongo_client(), self._auth_options)

    def _run_shard_split(self, split_opts):  # noqa: D205,D400
        try:
            donor_client = self._create_client(split_opts.get_donor_rs())
            res = self._commit_shard_split(donor_client, split_opts)
            is_committed = res["state"] == "committed"

            # Garbage collect the split prior to throwing error to avoid conflicting operations
            # in the next test.
            if is_committed:
                # Wait for the donor/proxy to reroute at least one command before doing garbage
                # collection. Stop waiting when the test finishes.
                self._wait_for_reroute_or_test_completion(donor_client, split_opts)

            self._forget_shard_split(donor_client, split_opts)
            self._wait_for_garbage_collection(split_opts)

            if not res["ok"]:
                raise errors.ServerFailure(
                    f"Shard split '{split_opts.migration_id}' on replica set "
                    f"'{split_opts.get_donor_name()}' failed: {str(res)}")

            if is_committed:
                return True

            abort_reason = res["abortReason"]
            if self._is_fail_point_abort_reason(abort_reason):
                self.logger.info(
                    f"Shard split '{split_opts.migration_id}' on replica set "
                    f"'{split_opts.get_donor_name()}' aborted due to failpoint: {str(res)}.")
                return False
            raise errors.ServerFailure(
                f"Shard split '{str(split_opts.migration_id)}' with donor replica set "
                f"'{split_opts.get_donor_name()}' aborted due to an error: {str(res)}")
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                f"Error running shard split '{split_opts.migration_id}' with donor primary on "
                f"replica set '{split_opts.get_donor_name()}'.")
            raise

    def _commit_shard_split(self, donor_client, split_opts):  # noqa: D205,D400
        self.logger.info(f"Starting shard split '{split_opts.migration_id}' on replica set "
                         f"'{split_opts.get_donor_name()}'.")

        while True:
            try:
                res = donor_client.admin.command({
                    "commitShardSplit": 1, "migrationId": split_opts.get_migration_id_as_binary(),
                    "tenantIds": split_opts.tenant_ids,
                    "recipientTagName": split_opts.recipient_tag_name, "recipientSetName":
                        split_opts.recipient_set_name
                }, bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))

                if res["state"] == "committed":
                    self.logger.info(f"Shard split '{split_opts.migration_id}' on replica set "
                                     f"'{split_opts.get_donor_name()}' has committed.")
                    return res
                if res["state"] == "aborted":
                    self.logger.info(f"Shard split '{split_opts.migration_id}' on replica set "
                                     f"'{split_opts.get_donor_name()}' has aborted: {str(res)}.")
                    return res
                if not res["ok"]:
                    self.logger.info(f"Shard split '{split_opts.migration_id}' on replica set "
                                     f"'{split_opts.get_donor_name()}' has failed: {str(res)}.")
                    return res
            except pymongo.errors.ConnectionFailure:
                self.logger.info(
                    f"Retrying shard split '{split_opts.migration_id}' against replica set "
                    f"'{split_opts.get_donor_name()}'.")

    def _forget_shard_split(self, donor_client, split_opts):
        self.logger.info(f"Forgetting shard split '{split_opts.migration_id}' on replica set "
                         f"'{split_opts.get_donor_name()}'.")

        while True:
            try:
                donor_client.admin.command(
                    {"forgetShardSplit": 1, "migrationId": split_opts.get_migration_id_as_binary()},
                    bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))
                return
            except pymongo.errors.ConnectionFailure:
                self.logger.info(
                    f"Retrying forget shard split '{split_opts.migration_id}' against replica "
                    f"set '{split_opts.get_donor_name()}'.")
                continue
            except pymongo.errors.OperationFailure as err:
                if err.code != self.NO_SUCH_MIGRATION_ERR_CODE:
                    raise

                # The fixture was restarted.
                self.logger.info(
                    f"Could not find shard split '{split_opts.migration_id}' on donor primary on "
                    f"replica set '{split_opts.get_donor_name()}': {str(err)}.")
                return
            except pymongo.errors.PyMongoError:
                self.logger.exception(
                    f"Error forgetting shard split '{split_opts.migration_id}' on donor primary on "
                    f"replica set '{split_opts.get_donor_name()}'.")
                raise

    def _wait_for_garbage_collection(self, split_opts):  # noqa: D205,D400
        try:
            donor_nodes = split_opts.get_donor_nodes()
            for donor_node in donor_nodes:
                self.logger.info(
                    f"Waiting for shard split '{split_opts.migration_id}' to be garbage collected on donor node on port {donor_node.port} of replica set '{split_opts.get_donor_name()}'."
                )

                donor_node_client = self._create_client(donor_node)
                while True:
                    try:
                        res = donor_node_client.config.command({
                            "count": "tenantSplitDonors",
                            "query": {"tenantIds": split_opts.tenant_ids}
                        })
                        if res["n"] == 0:
                            break
                    except pymongo.errors.ConnectionFailure:
                        self.logger.info(
                            f"Retrying waiting for shard split '{split_opts.migration_id}' to be garbage collected on donor node on port {donor_node.port} of replica set '{split_opts.get_donor_name()}'."
                        )
                        continue
                    time.sleep(self.POLL_INTERVAL_SECS)

            recipient_nodes = split_opts.get_recipient_nodes()
            for recipient_node in recipient_nodes:
                self.logger.info(
                    f"Waiting for shard split '{split_opts.migration_id}' to be garbage collected on recipient node on port {recipient_node.port} of replica set '{split_opts.recipient_set_name}'."
                )

                recipient_node_client = self._create_client(recipient_node)
                while True:
                    try:
                        hello = recipient_node_client.admin.command("hello")
                        print(f"recipient hello: {hello}")

                        res = recipient_node_client.config.command({
                            "count": "tenantSplitDonors",
                            "query": {"tenantIds": split_opts.tenant_ids}
                        })
                        if res["n"] == 0:
                            break

                        docs = recipient_node_client.config.tenantSplitDonors.find()
                        print(f"recipient tenantSplitDonors docs: {list(docs)}")
                    except pymongo.errors.ConnectionFailure:
                        self.logger.info(
                            f"Retrying waiting for shard split '{split_opts.migration_id}' to be garbage collected on recipient node on port {recipient_node.port} of replica set '{split_opts.recipient_set_name}'."
                        )
                        continue
                    time.sleep(self.POLL_INTERVAL_SECS)

        except pymongo.errors.PyMongoError:
            self.logger.exception(
                f"Error waiting for shard split '{split_opts.migration_id}' from donor replica set '{split_opts.get_donor_name()} to recipient replica set '{split_opts.recipient_set_name}' to be garbage collected."
            )
            raise

    def _wait_for_reroute_or_test_completion(self, donor_client, split_opts):
        start_time = time.time()

        self.logger.info(
            f"Waiting for donor primary of replica set '{split_opts.get_donor_name()}' for shard split '{split_opts.migration_id}' to reroute at least one conflicting command. Stop waiting when the test finishes."
        )

        while not self.__lifecycle.is_test_finished():
            try:
                # We are reusing the infrastructure originally developed for tenant migrations,
                # and aren't concerned about conflicts because we don't expect the tenant migration
                # and shard split hooks to run concurrently.
                doc = donor_client["testTenantMigration"]["rerouted"].find_one(
                    {"_id": split_opts.get_migration_id_as_binary()})
                if doc is not None:
                    return
            except pymongo.errors.ConnectionFailure:
                self.logger.info(
                    f"Retrying waiting for donor primary of replica set '{split_opts.get_donor_name()}' for shard split '{split_opts.migration_id}' to reroute at least one conflicting command."
                )
                continue
            except pymongo.errors.PyMongoError:
                end_time = time.time()
                self.logger.exception(
                    f"Error running find command on donor primary replica set '{split_opts.get_donor_name()}' after waiting for reroute for {(end_time - start_time) * 1000} ms"
                )
                raise

            time.sleep(self.POLL_INTERVAL_SECS)
