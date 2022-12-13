"""Test hook that runs shard merges continuously."""

import copy
import random
import re
import threading
import time
import uuid

import bson
import pymongo.errors

from bson.objectid import ObjectId

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import shard_merge
from buildscripts.resmokelib.testing.hooks import dbhash_tenant_migration
from buildscripts.resmokelib.testing.hooks import interface


class ContinuousShardMerge(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Starts a shard merge thread at the beginning of each test."""

    DESCRIPTION = ("Continuous shard merges")

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, shell_options):
        """Initialize the ContinuousShardMerge.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target ShardMergeFixture containing two replica sets.
            shell_options: contains the global_vars which contains TestData.tenantId to be used for
                           shard merges.

        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousShardMerge.DESCRIPTION)

        if not isinstance(fixture, shard_merge.ShardMergeFixture):
            raise ValueError("The ContinuousShardMerge hook requires a ShardMergeFixture")
        self._shard_merge_fixture = fixture
        self._shell_options = copy.deepcopy(shell_options)

        self._shard_merge_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._shard_merge_fixture:
            raise ValueError("No ShardMergeFixture to run migrations on")
        self.logger.info("Starting the shard merge thread.")
        self._shard_merge_thread = _ShardMergeThread(self.logger, self._shard_merge_fixture,
                                                     self._shell_options, test_report)
        self._shard_merge_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the shard merge thread.")
        self._shard_merge_thread.stop()
        self.logger.info("Stopped the shard merge thread.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the shard merge thread.")
        self._shard_merge_thread.resume(test)

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the shard merge thread.")
        self._shard_merge_thread.pause()
        self.logger.info("Paused the shard merge thread.")


class ShardMergeLifeCycle(object):
    """Class for managing the various states of the shard merge thread.

    The job thread alternates between calling mark_test_started() and mark_test_finished(). The
    shard merge thread is allowed to perform migrations at any point between these two calls.
    Note that the job thread synchronizes with the shard merge thread outside the context of
    this object to know it isn't in the process of running a migration.
    """

    _TEST_STARTED_STATE = "start"
    _TEST_FINISHED_STATE = "finished"

    def __init__(self):
        """Initialize the ShardMergeLifeCycle instance."""
        self.__lock = threading.Lock()
        self.__cond = threading.Condition(self.__lock)

        self.test_num = 0
        self.__test_state = self._TEST_FINISHED_STATE
        self.__should_stop = False

    def mark_test_started(self):
        """Signal to the shard merge thread that a new test has started.

        This function should be called during before_test(). Calling it causes the
        wait_for_shard_merge_permitted() function to no longer block and to instead return
        true.
        """
        with self.__lock:
            self.test_num += 1
            self.__test_state = self._TEST_STARTED_STATE
            self.__cond.notify_all()

    def mark_test_finished(self):
        """Signal to the shard merge thread that the current test has finished.

        This function should be called during after_test(). Calling it causes the
        wait_for_shard_merge_permitted() function to block until mark_test_started() is called
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
        """Signal to the shard merge thread that it should exit.

        This function should be called during after_suite(). Calling it causes the
        wait_for_shard_merge_permitted() function to no longer block and to instead return
        false.
        """
        with self.__lock:
            self.__should_stop = True
            self.__cond.notify_all()

    def wait_for_shard_merge_permitted(self):
        """Block until migrations are permitted, or until stop() is called.

        Return true if migrations are permitted, and false if migrations are not permitted.
        """
        with self.__lock:
            while not self.__should_stop:
                if self.__test_state == self._TEST_STARTED_STATE:
                    return True

                self.__cond.wait()

        return False

    def wait_for_tenant_migration_interval(self, timeout):
        """Block for 'timeout' seconds, or until stop() is called."""
        with self.__lock:
            self.__cond.wait(timeout)

    def poll_for_idle_request(self):  # noqa: D205,D400
        """Return true if the shard merge thread should continue running migrations, or false
        if it should temporarily stop running migrations.
        """
        with self.__lock:
            return self.__test_state == self._TEST_FINISHED_STATE


def get_certificate_and_private_key(pem_file_path):  # noqa: D205,D400
    """Return a dictionary containing the certificate and private key extracted from the given pem
    file.
    """
    lines = open(pem_file_path, 'rt').read()
    certificate = re.findall(
        re.compile("(-*BEGIN CERTIFICATE-*\n(.*\n)*-*END CERTIFICATE-*\n)", re.MULTILINE),
        lines)[0][0]
    private_key = re.findall(
        re.compile("(-*BEGIN PRIVATE KEY-*\n(.*\n)*-*END PRIVATE KEY-*\n)", re.MULTILINE),
        lines)[0][0]
    return {"certificate": certificate, "privateKey": private_key}


def get_primary(rs, logger, max_tries=5):  # noqa: D205,D400
    """Return the primary from a replica set. Retries up to 'max_tries' times of it fails to get
    the primary within the time limit.
    """
    num_tries = 0
    while num_tries < max_tries:
        num_tries += 1
        try:
            return rs.get_primary()
        except errors.ServerFailure:
            logger.info(
                "Timed out while waiting for a primary for replica set '%s' on try %d." +
                " Retrying." if num_tries < max_tries else "", rs.replset_name, num_tries)


class _ShardMergeOptions:  # pylint:disable=too-many-instance-attributes
    def __init__(  # pylint: disable=too-many-arguments
            self, donor_rs, recipient_rs, tenant_id, read_preference, logger, donor_rs_index,
            recipient_rs_index):
        self.donor_rs = donor_rs
        self.recipient_rs = recipient_rs
        self.migration_id = uuid.uuid4()
        self.tenant_id = tenant_id
        self.read_preference = read_preference
        self.logger = logger
        self.donor_rs_index = donor_rs_index
        self.recipient_rs_index = recipient_rs_index

    def get_donor_name(self):
        """Return the replica set name for the donor."""
        return self.donor_rs.replset_name

    def get_recipient_name(self):
        """Return the replica set name for the recipient."""
        return self.recipient_rs.replset_name

    def get_donor_primary(self):
        """Return a connection to the donor primary."""
        return get_primary(self.donor_rs, self.logger)

    def get_recipient_primary(self):
        """Return a connection to the recipient primary."""
        return get_primary(self.recipient_rs, self.logger)

    def get_donor_nodes(self):
        """Return a list of connections to donor replica set nodes."""
        return self.donor_rs.nodes

    def get_recipient_nodes(self):
        """Return a list of connections to recipient replica set nodes."""
        return self.recipient_rs.nodes

    def __str__(self):
        opts = {
            "donor": self.get_donor_name(), "recipient": self.get_recipient_name(),
            "migration_id": self.migration_id, "tenant_id": self.tenant_id,
            "read_preference": self.read_preference
        }
        return str(opts)


class _ShardMergeThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    THREAD_NAME = "ShardMergeThread"

    WAIT_SECS_RANGES = [[0.05, 0.1], [0.1, 0.5], [1, 5], [5, 15]]
    POLL_INTERVAL_SECS = 0.1
    WAIT_PENDING_IDENT_SECS = 0.3

    NO_SUCH_MIGRATION_ERR_CODE = 327
    INTERNAL_ERR_CODE = 1
    INVALID_SYNC_SOURCE_ERR_CODE = 119
    FAIL_TO_PARSE_ERR_CODE = 9
    NO_SUCH_KEY_ERR_CODE = 4

    def __init__(self, logger, shard_merge_fixture, shell_options, test_report):
        """Initialize _ShardMergeThread."""
        threading.Thread.__init__(self, name=self.THREAD_NAME)
        self.daemon = True
        self.logger = logger
        self._shard_merge_fixture = shard_merge_fixture
        # TODO SERVER-69034 : replace tenantId with tenantIds
        self._tenant_id = shell_options["global_vars"]["TestData"]["tenantId"]
        self._auth_options = shell_options["global_vars"]["TestData"]["authOptions"]
        self._test = None
        self._test_report = test_report
        self._shell_options = shell_options

        self.__lifecycle = ShardMergeLifeCycle()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing migrations.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

    def run(self):
        """Execute the thread."""
        if not self._shard_merge_fixture:
            self.logger.warning("No ShardMergeFixture to run migrations on.")
            return

        test_num = 0
        migration_num = 0
        donor_rs_index = 0

        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_shard_merge_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                if self.__lifecycle.test_num > test_num:
                    # Reset donor_rs_index to 0 since the shell always routes all requests to rs0
                    # at the start of a test.
                    test_num = self.__lifecycle.test_num
                    donor_rs_index = 0

                recipient_rs_index = (
                    donor_rs_index + 1) % self._shard_merge_fixture.get_num_replsets()
                migration_opts = self._create_migration_opts(donor_rs_index, recipient_rs_index)

                # Briefly wait to let the test run before starting the shard merge, so that
                # the first migration is more likely to have data to migrate.
                wait_secs = random.uniform(
                    *self.WAIT_SECS_RANGES[migration_num % len(self.WAIT_SECS_RANGES)])
                self.logger.info("Waiting for %.3f seconds before starting migration.", wait_secs)
                self.__lifecycle.wait_for_tenant_migration_interval(wait_secs)

                self.logger.info("Starting shard merge: %s.", str(migration_opts))
                start_time = time.time()
                is_committed = self._run_migration(migration_opts)
                end_time = time.time()
                self.logger.info("Completed shard merge in %0d ms: %s.",
                                 (end_time - start_time) * 1000, str(migration_opts))

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    continue

                if is_committed:
                    donor_rs_index = recipient_rs_index
                migration_num += 1
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be flushed immediately.
            self.logger.exception("Shard merge thread threw exception")
            # The event should be signaled whenever the thread is not performing migrations.
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

        # Wait until we are no longer executing migrations.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()

        # Check that the fixture is still running.
        if not self._shard_merge_fixture.is_running():
            raise errors.ServerFailure("ShardMergeFixture with pids {} expected to be running in"
                                       " ContinuousShardMerge, but wasn't".format(
                                           self._shard_merge_fixture.pids()))

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
            msg = "Shard merge thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _is_fail_point_abort_reason(self, abort_reason):
        return abort_reason["code"] == self.INTERNAL_ERR_CODE and abort_reason[
            "errmsg"] == "simulate a shard merge error"

    def _create_migration_opts(self, donor_rs_index, recipient_rs_index):
        donor_rs = self._shard_merge_fixture.get_replset(donor_rs_index)
        recipient_rs = self._shard_merge_fixture.get_replset(recipient_rs_index)
        read_preference = {"mode": "primary"}
        return _ShardMergeOptions(donor_rs, recipient_rs, self._tenant_id, read_preference,
                                  self.logger, donor_rs_index, recipient_rs_index)

    def _create_client(self, node):
        return fixture_interface.build_client(node, self._auth_options)

    def _check_tenant_migration_dbhash(self, migration_opts):
        # Set the donor connection string, recipient connection string, and migration uuid string
        # for the shard merge dbhash check script.
        self._shell_options[
            "global_vars"]["TestData"]["donorConnectionString"] = migration_opts.get_donor_primary(
            ).get_internal_connection_string()
        self._shell_options["global_vars"]["TestData"][
            "recipientConnectionString"] = migration_opts.get_recipient_primary(
            ).get_internal_connection_string()
        self._shell_options["global_vars"]["TestData"][
            "migrationIdString"] = migration_opts.migration_id.__str__()

        dbhash_test_case = dbhash_tenant_migration.CheckTenantMigrationDBHash(
            self.logger, self._shard_merge_fixture, self._shell_options)
        dbhash_test_case.before_suite(self._test_report)
        dbhash_test_case.before_test(self._test, self._test_report)
        dbhash_test_case.after_test(self._test, self._test_report)
        dbhash_test_case.after_suite(self._test_report)

    def _run_migration(self, migration_opts):  # noqa: D205,D400
        """Run a shard merge based on 'migration_opts', wait for the migration decision and
        garbage collection. Return true if the migration commits and false otherwise.
        """
        try:
            # Clean up any orphaned tenant databases on the recipient allow next migration to start.
            self._drop_tenant_databases(migration_opts.recipient_rs)
            res = self._start_and_wait_for_migration(migration_opts)
            is_committed = res["state"] == "committed"

            # Garbage collect the migration prior to throwing error to avoid migration conflict
            # in the next test.
            if is_committed:
                # Once we have committed a migration, run a dbhash check before rerouting commands.
                self._check_tenant_migration_dbhash(migration_opts)

                # If the migration committed, to avoid routing commands incorrectly, wait for the
                # donor/proxy to reroute at least one command before doing garbage collection. Stop
                # waiting when the test finishes.
                self._wait_for_reroute_or_test_completion(migration_opts)
            self._forget_migration(migration_opts)
            self._wait_for_migration_garbage_collection(migration_opts)

            if not res["ok"]:
                raise errors.ServerFailure("Shard merge '" + str(migration_opts.migration_id) +
                                           "' with donor replica set '" +
                                           migration_opts.get_donor_name() + "' failed: " +
                                           str(res))

            if is_committed:
                return True

            abort_reason = res["abortReason"]
            if self._is_fail_point_abort_reason(abort_reason):
                self.logger.info(
                    "Shard merge '%s' with donor replica set '%s' aborted due to failpoint: " +
                    "%s.", migration_opts.migration_id, migration_opts.get_donor_name(), str(res))
                return False
            raise errors.ServerFailure(
                "Shard merge '" + str(migration_opts.migration_id) + "' with donor replica set '" +
                migration_opts.get_donor_name() + "' aborted due to an error: " + str(res))
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error running shard merge '%s' with donor primary on replica set '%s'.",
                migration_opts.migration_id, migration_opts.get_donor_name())
            raise

    def _override_abort_failpoint_shard_merge(self, donor_primary):  # noqa: D205,D400
        """Override the abortTenantMigrationBeforeLeavingBlockingState failpoint so the shard merge
        does not abort since it is currently not supported. Only use this method for shard merge.
        """
        while True:
            try:
                donor_primary_client = self._create_client(donor_primary)
                donor_primary_client.admin.command(
                    bson.SON([("configureFailPoint",
                               "pauseTenantMigrationBeforeLeavingBlockingState"), ("mode", "off")]))
                donor_primary_client.admin.command(
                    bson.SON([("configureFailPoint",
                               "abortTenantMigrationBeforeLeavingBlockingState"), ("mode", "off")]))
                return
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError):
                self.logger.info(
                    "Retrying connection to donor primary in order to disable abort failpoint for shard merge."
                )
                continue
            time.sleep(self.POLL_INTERVAL_SECS)

    def _start_and_wait_for_migration(self, migration_opts):  # noqa: D205,D400
        """Run donorStartMigration to start a shard merge based on 'migration_opts', wait for
        the migration decision and return the last response for donorStartMigration.
        """
        cmd_obj = {
            "donorStartMigration":
                1,
            "migrationId":
                bson.Binary(migration_opts.migration_id.bytes, 4),
            "recipientConnectionString":
                migration_opts.recipient_rs.get_driver_connection_url(),
            "readPreference":
                migration_opts.read_preference,
            "donorCertificateForRecipient":
                get_certificate_and_private_key("jstests/libs/tenant_migration_donor.pem"),
            "recipientCertificateForDonor":
                get_certificate_and_private_key("jstests/libs/tenant_migration_recipient.pem"),
            "protocol":
                "shard merge",
            "tenantIds": [ObjectId(migration_opts.tenant_id)],
        }
        donor_primary = migration_opts.get_donor_primary()
        # TODO(SERVER-68643) We no longer need to override the failpoint once milestone 3 is done
        # for shard merge.
        self._override_abort_failpoint_shard_merge(donor_primary)
        # For shard merge protocol we need to wait for the ident to be removed before starting a
        # new migration with a shard merge otherwise, due to the two phase drop, the stored
        # files will be marked to be deleted but not deleted fast enough and we would end up
        # moving a file that still exists.
        while self._get_pending_drop_idents(migration_opts.recipient_rs_index) > 0:
            time.sleep(self.WAIT_PENDING_IDENT_SECS)
        # Some tests also do drops on collection which we need to wait on before doing a new migration
        while self._get_pending_drop_idents(migration_opts.donor_rs_index) > 0:
            time.sleep(self.WAIT_PENDING_IDENT_SECS)

        self.logger.info(
            "Starting shard merge '%s' on donor primary on port %d of replica set '%s'.",
            migration_opts.migration_id, donor_primary.port, migration_opts.get_donor_name())

        while True:
            try:
                # Keep polling the migration state until the migration completes.
                donor_primary_client = self._create_client(donor_primary)
                res = donor_primary_client.admin.command(
                    cmd_obj,
                    bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError):
                donor_primary = migration_opts.get_donor_primary()
                self.logger.info(
                    "Retrying shard merge '%s' against donor primary on port %d of" +
                    " replica set '%s'.", migration_opts.migration_id, donor_primary.port,
                    migration_opts.get_donor_name())
                continue
            if res["state"] == "committed":
                self.logger.info(
                    "Shard merge '%s' with donor primary on port %d of replica set '%s'" +
                    " has committed.", migration_opts.migration_id, donor_primary.port,
                    migration_opts.get_donor_name())
                return res
            if res["state"] == "aborted":
                self.logger.info(
                    "Shard merge '%s' with donor primary on port %d of replica set '%s'" +
                    " has aborted: %s.", migration_opts.migration_id, donor_primary.port,
                    migration_opts.get_donor_name(), str(res))
                return res
            if not res["ok"]:
                self.logger.info(
                    "Shard merge '%s' with donor primary on port %d of replica set '%s'" +
                    " has failed: %s.", migration_opts.migration_id, donor_primary.port,
                    migration_opts.get_donor_name(), str(res))
                return res
            time.sleep(self.POLL_INTERVAL_SECS)

    def _forget_migration(self, migration_opts):
        """Run donorForgetMigration to garbage collection the shard merge denoted by migration_opts'."""
        self.logger.info("Forgetting shard merge: %s.", str(migration_opts))

        cmd_obj = {
            "donorForgetMigration": 1, "migrationId": bson.Binary(migration_opts.migration_id.bytes,
                                                                  4)
        }
        donor_primary = migration_opts.get_donor_primary()

        self.logger.info(
            "Forgetting shard merge '%s' on donor primary on port %d of replica set '%s'.",
            migration_opts.migration_id, donor_primary.port, migration_opts.get_donor_name())

        while True:
            try:
                donor_primary_client = self._create_client(donor_primary)
                donor_primary_client.admin.command(
                    cmd_obj,
                    bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))
                return
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError):
                donor_primary = migration_opts.get_donor_primary()
                self.logger.info(
                    "Retrying forgetting shard merge '%s' against donor primary on port %d of " +
                    "replica set '%s'.", migration_opts.migration_id, donor_primary.port,
                    migration_opts.get_donor_name())
                continue
            except pymongo.errors.OperationFailure as err:
                if err.code != self.NO_SUCH_MIGRATION_ERR_CODE:
                    raise
                # The fixture was restarted.
                self.logger.info(
                    "Could not find shard merge '%s' on donor primary on" +
                    " port %d of replica set '%s': %s.", migration_opts.migration_id,
                    donor_primary.port, migration_opts.get_donor_name(), str(err))
                return
            except pymongo.errors.PyMongoError:
                self.logger.exception(
                    "Error forgetting shard merge '%s' on donor primary on" +
                    " port %d of replica set '%s'.", migration_opts.migration_id,
                    donor_primary.port, migration_opts.get_donor_name())
                raise

    def _wait_for_migration_garbage_collection(self, migration_opts):  # noqa: D205,D400
        """Wait until the persisted state for shard merge denoted by 'migration_opts' has been
        garbage collected on both the donor and recipient.
        """
        try:
            donor_nodes = migration_opts.get_donor_nodes()
            for donor_node in donor_nodes:
                self.logger.info(
                    "Waiting for shard merge '%s' to be garbage collected on donor node on " +
                    "port %d of replica set '%s'.", migration_opts.migration_id, donor_node.port,
                    migration_opts.get_donor_name())

                while True:
                    try:
                        donor_node_client = self._create_client(donor_node)
                        res = donor_node_client.config.command({
                            "count": "tenantMigrationDonors",
                            "query": {"_id": bson.Binary(migration_opts.migration_id.bytes, 4)}
                        })
                        if res["n"] == 0:
                            break
                    except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError):
                        # Ignore NotPrimaryErrors because it's possible to fail with
                        # InterruptedDueToReplStateChange if the donor primary steps down or shuts
                        # down during the garbage collection check.
                        self.logger.info(
                            "Retrying waiting for shard merge '%s' to be garbage collected on" +
                            " donor node on port %d of replica set '%s'.",
                            migration_opts.migration_id, donor_node.port,
                            migration_opts.get_donor_name())
                        continue
                    time.sleep(self.POLL_INTERVAL_SECS)

            recipient_nodes = migration_opts.get_recipient_nodes()
            for recipient_node in recipient_nodes:
                self.logger.info(
                    "Waiting for shard merge '%s' to be garbage collected on recipient node on" +
                    " port %d of replica set '%s'.", migration_opts.migration_id,
                    recipient_node.port, migration_opts.get_recipient_name())

                while True:
                    try:
                        recipient_node_client = self._create_client(recipient_node)
                        res = recipient_node_client.config.command({
                            "count": "tenantMigrationRecipients",
                            "query": {"_id": bson.Binary(migration_opts.migration_id.bytes, 4)}
                        })
                        if res["n"] == 0:
                            break
                    except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError):
                        # Ignore NotPrimaryErrors because it's possible to fail with
                        # InterruptedDueToReplStateChange if the recipient primary steps down or
                        # shuts down during the garbage collection check.
                        self.logger.info(
                            "Retrying waiting for shard merge '%s' to be garbage collected on" +
                            " recipient node on port %d of replica set '%s'.",
                            migration_opts.migration_id, recipient_node.port,
                            migration_opts.get_recipient_name())
                        continue
                    time.sleep(self.POLL_INTERVAL_SECS)
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error waiting for shard merge '%s' from donor replica set '%s" +
                " to recipient replica set '%s' to be garbage collected.",
                migration_opts.migration_id, migration_opts.get_donor_name(),
                migration_opts.get_recipient_name())
            raise

    def _wait_for_reroute_or_test_completion(self, migration_opts):
        start_time = time.time()
        donor_primary = migration_opts.get_donor_primary()

        self.logger.info(
            "Waiting for donor primary on port %d of replica set '%s' for shard merge '%s' " +
            "to reroute at least one conflicting command. Stop waiting when the test finishes.",
            donor_primary.port, migration_opts.get_donor_name(), migration_opts.migration_id)

        while not self.__lifecycle.is_test_finished():
            try:
                donor_primary_client = self._create_client(donor_primary)
                doc = donor_primary_client["local"]["rerouted"].find_one(
                    {"_id": bson.Binary(migration_opts.migration_id.bytes, 4)})
                if doc is not None:
                    return
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError):
                donor_primary = migration_opts.get_donor_primary()
                self.logger.info(
                    "Retrying waiting for donor primary on port '%d' of replica set '%s' for " +
                    "shard merge '%s' to reroute at least one conflicting command.",
                    donor_primary.port, migration_opts.get_donor_name(),
                    migration_opts.migration_id)
                continue
            except pymongo.errors.PyMongoError:
                end_time = time.time()
                self.logger.exception(
                    "Error running find command on donor primary on port %d of replica set '%s' " +
                    "after waiting for reroute for %0d ms", donor_primary.port,
                    migration_opts.get_donor_name(), (end_time - start_time) * 1000)
                raise

            time.sleep(self.POLL_INTERVAL_SECS)

    def _drop_tenant_databases(self, rs):
        self.logger.info("Dropping tenant databases from replica set '%s'.", rs.replset_name)

        primary = get_primary(rs, self.logger)
        self.logger.info("Running dropDatabase commands against primary on port %d.", primary.port)

        while True:
            try:
                primary_client = self._create_client(primary)
                res = primary_client.admin.command({"listDatabases": 1})
                for database in res["databases"]:
                    db_name = database["name"]
                    if db_name.startswith(self._tenant_id + "_"):
                        primary_client.drop_database(db_name)
                return
            # We retry on all write concern errors because we assume the only reason waiting for
            # write concern should fail is because of a failover.
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError,
                    pymongo.errors.WriteConcernError) as err:
                primary = get_primary(rs, self.logger)
                self.logger.info(
                    "Retrying dropDatabase commands against primary on port %d after error %s.",
                    primary.port, str(err))
                continue
            except pymongo.errors.PyMongoError:
                self.logger.exception(
                    "Error dropping databases for tenant id '%s' on primary on" +
                    " port %d of replica set '%s' to be garbage collection.", self._tenant_id,
                    primary.port, rs.replset_name)
                raise

    def _get_pending_drop_idents(self, replica_set_index):  # noqa: D205,D400
        """Returns the number of pending idents to be dropped. This is necessary for the shard
        merge protocol since we need to wait for the idents to be dropped before starting a new
        shard merge.
        """
        primary = self._shard_merge_fixture.get_replset(replica_set_index).get_primary()
        pending_drop_idents = None
        while True:
            try:
                client = self._create_client(primary)
                server_status = client.admin.command({"serverStatus": 1})
                pending_drop_idents = server_status["storageEngine"]["dropPendingIdents"]
                break
            except (pymongo.errors.AutoReconnect, pymongo.errors.NotPrimaryError,
                    pymongo.errors.WriteConcernError) as err:
                self.logger.info(
                    "Retrying getting dropPendingIdents against primary on port %d after error %s.",
                    primary.port, str(err))
                continue
            except pymongo.errors.PyMongoError:
                self.logger.exception(
                    "Error creating client waiting for pending drop idents on " +
                    " primary on port %d.", primary.port)
                raise

        return pending_drop_idents
