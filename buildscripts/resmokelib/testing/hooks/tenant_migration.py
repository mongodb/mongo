"""Test hook that runs tenant migrations continuously."""

import random
import re
import threading
import time
import uuid

import bson
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import tenant_migration
from buildscripts.resmokelib.testing.hooks import interface


class ContinuousTenantMigration(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Starts a tenant migration thread at the beginning of each test."""

    DESCRIPTION = ("Continuous tenant migrations")

    def __init__(self, hook_logger, fixture, shell_options):
        """Initialize the ContinuousTenantMigration.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target TenantMigrationFixture containing two replica sets.
            shell_options: contains the global_vars which contains TestData.tenantId to be used for
                           tenant migrations.

        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousTenantMigration.DESCRIPTION)

        if not isinstance(fixture, tenant_migration.TenantMigrationFixture):
            raise ValueError("The ContinuousTenantMigration hook requires a TenantMigrationFixture")
        self._tenant_migration_fixture = fixture
        self._tenant_id = shell_options["global_vars"]["TestData"]["tenantId"]

        self._tenant_migration_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._tenant_migration_fixture:
            raise ValueError("No TenantMigrationFixture to run migrations on")
        self.logger.info("Starting the tenant migration thread.")
        self._tenant_migration_thread = _TenantMigrationThread(
            self.logger, self._tenant_migration_fixture, self._tenant_id)
        self._tenant_migration_thread.start()

    def after_suite(self, test_report):
        """After suite."""
        self.logger.info("Stopping the tenant migration thread.")
        self._tenant_migration_thread.stop()
        self.logger.info("Stopped the tenant migration thread.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the tenant migration thread.")
        self._tenant_migration_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the tenant migration thread.")
        self._tenant_migration_thread.pause()
        self.logger.info("Paused the tenant migration thread.")


class TenantMigrationLifeCycle(object):
    """Class for managing the various states of the tenant migration thread.

    The job thread alternates between calling mark_test_started() and mark_test_finished(). The
    tenant migration thread is allowed to perform migrations at any point between these two calls.
    Note that the job thread synchronizes with the tenant migration thread outside the context of
    this object to know it isn't in the process of running a migration.
    """

    _TEST_STARTED_STATE = "start"
    _TEST_FINISHED_STATE = "finished"

    def __init__(self):
        """Initialize the MigrationLifecycle instance."""
        self.__lock = threading.Lock()
        self.__cond = threading.Condition(self.__lock)

        self.test_num = 0
        self.__test_state = self._TEST_FINISHED_STATE
        self.__should_stop = False

    def mark_test_started(self):
        """Signal to the tenant migration thread that a new test has started.

        This function should be called during before_test(). Calling it causes the
        wait_for_tenant_migration_permitted() function to no longer block and to instead return
        true.
        """
        with self.__lock:
            self.test_num += 1
            self.__test_state = self._TEST_STARTED_STATE
            self.__cond.notify_all()

    def mark_test_finished(self):
        """Signal to the tenant migration thread that the current test has finished.

        This function should be called during after_test(). Calling it causes the
        wait_for_tenant_migration_permitted() function to block until mark_test_started() is called
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
        """Signal to the tenant migration thread that it should exit.

        This function should be called during after_suite(). Calling it causes the
        wait_for_tenant_migration_permitted() function to no longer block and to instead return
        false.
        """
        with self.__lock:
            self.__should_stop = True
            self.__cond.notify_all()

    def wait_for_tenant_migration_permitted(self):
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
        """Return true if the tenant migration thread should continue running migrations, or false
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


class _TenantMigrationOptions:
    def __init__(self, donor_rs, recipient_rs, tenant_id, read_preference):
        self.donor_rs = donor_rs
        self.recipient_rs = recipient_rs
        self.migration_id = uuid.uuid4()
        self.tenant_id = tenant_id
        self.read_preference = read_preference

    def get_donor_name(self):
        """Return the replica set name for the donor."""
        return self.donor_rs.replset_name

    def get_recipient_name(self):
        """Return the replica set name for the recipient."""
        return self.recipient_rs.replset_name

    def get_donor_primary(self):
        """Return a connection to the donor primary."""
        return self.donor_rs.get_primary()

    def get_recipient_primary(self):
        """Return a connection to the recipient primary."""
        return self.recipient_rs.get_primary()

    def __str__(self):
        opts = {
            "donor": self.get_donor_name(), "recipient": self.get_recipient_name(),
            "migration_id": self.migration_id, "tenant_id": self.tenant_id,
            "read_preference": self.read_preference
        }
        return str(opts)


class _TenantMigrationThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    WAIT_SECS_RANGES = [[0.1, 0.5], [1, 5], [5, 15]]
    MIGRATION_STATE_POLL_INTERVAL_SECS = 0.1
    NO_SUCH_MIGRATION_ERR_CODE = 327

    def __init__(self, logger, tenant_migration_fixture, tenant_id):
        """Initialize _TenantMigrationThread."""
        threading.Thread.__init__(self, name="TenantMigrationThread")
        self.daemon = True
        self.logger = logger
        self._tenant_migration_fixture = tenant_migration_fixture
        self._tenant_id = tenant_id

        self.__lifecycle = TenantMigrationLifeCycle()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing migrations.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

    def run(self):
        """Execute the thread."""
        if not self._tenant_migration_fixture:
            self.logger.warning("No TenantMigrationFixture to run migrations on.")
            return

        test_num = 0
        migration_num = 0
        donor_rs_index = 0

        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_tenant_migration_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                if self.__lifecycle.test_num > test_num:
                    # Reset donor_rs_index to 0 since the shell always routes all requests to rs0
                    # at the start of a test.
                    test_num = self.__lifecycle.test_num
                    donor_rs_index = 0

                recipient_rs_index = (
                    donor_rs_index + 1) % self._tenant_migration_fixture.get_num_replsets()
                migration_opts = self._create_migration_opts(donor_rs_index, recipient_rs_index)

                self.logger.info("Starting tenant migration: %s.", str(migration_opts))
                start_time = time.time()
                is_committed = self._run_migration(migration_opts)
                end_time = time.time()
                self.logger.info("Completed tenant migration in %0d ms: %s.",
                                 (end_time - start_time) * 1000, str(migration_opts))

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    continue

                wait_secs = random.uniform(*_TenantMigrationThread.WAIT_SECS_RANGES[
                    migration_num % len(_TenantMigrationThread.WAIT_SECS_RANGES)])
                self.__lifecycle.wait_for_tenant_migration_interval(wait_secs)

                if is_committed:
                    donor_rs_index = recipient_rs_index
                migration_num += 1
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be flushed immediately.
            self.logger.exception("Tenant migration thread threw exception")
            # The event should be signaled whenever the thread is not performing migrations.
            self._is_idle_evt.set()

    def stop(self):
        """Stop the thread when the suite finishes."""
        self.__lifecycle.stop()
        self._is_stopped_evt.set()
        # Unpause to allow the thread to finish.
        self.resume()
        self.join()

    def pause(self):
        """Pause the thread after test."""
        self.__lifecycle.mark_test_finished()

        # Wait until we are no longer executing migrations.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()

        # Check that the fixture is still running.
        if not self._tenant_migration_fixture.is_running():
            raise errors.ServerFailure(
                "TenantMigrationFixture with pids {} expected to be running in"
                " ContinuousTenantMigration, but wasn't".format(
                    self._tenant_migration_fixture.pids()))

    def resume(self):
        """Resume the thread before test."""
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        """Wait until stop or timeout."""
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        """Throw an error if the thread is not running."""
        if not self.is_alive():
            msg = "Tenant migration thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _create_migration_opts(self, donor_rs_index, recipient_rs_index):
        donor_rs = self._tenant_migration_fixture.get_replset(donor_rs_index)
        recipient_rs = self._tenant_migration_fixture.get_replset(recipient_rs_index)
        read_preference = {"mode": "primary"} if random.randint(0, 1) else {"mode": "secondary"}
        return _TenantMigrationOptions(donor_rs, recipient_rs, self._tenant_id, read_preference)

    def _run_migration(self, migration_opts):  # noqa: D205,D400
        """Run donorStartMigration to start a tenant migration based on 'migration_opts', wait for
        the migration decision. Returns true if the migration commits and false otherwise.
        """
        donor_primary = migration_opts.get_donor_primary()
        donor_primary_client = donor_primary.mongo_client()

        self.logger.info(
            "Starting tenant migration with donor primary on port %d of replica set '%s'.",
            donor_primary.port, migration_opts.get_donor_name())

        cmd_obj = {
            "donorStartMigration":
                1,
            "migrationId":
                bson.Binary(migration_opts.migration_id.bytes, 4),
            "recipientConnectionString":
                migration_opts.recipient_rs.get_driver_connection_url(),
            "tenantId":
                migration_opts.tenant_id,
            "readPreference":
                migration_opts.read_preference,
            "donorCertificateForRecipient":
                get_certificate_and_private_key("jstests/libs/rs0_tenant_migration.pem"),
            "recipientCertificateForDonor":
                get_certificate_and_private_key("jstests/libs/rs1_tenant_migration.pem"),
        }
        is_committed = False

        try:
            # Clean up any orphaned tenant databases on the recipient allow next migration to start.
            self._drop_tenant_databases(migration_opts.recipient_rs)

            while True:
                # Keep polling the migration state until the migration completes.
                res = donor_primary_client.admin.command(
                    cmd_obj,
                    bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))

                if res["state"] == "committed":
                    self.logger.info("Tenant migration with donor primary on port " +
                                     str(donor_primary.port) + " of replica set '" +
                                     migration_opts.get_donor_name() + "' has committed.")
                    is_committed = True
                    break
                elif res["state"] == "aborted":
                    self.logger.info("Tenant migration with donor primary on port " +
                                     str(donor_primary.port) + " of replica set '" +
                                     migration_opts.get_donor_name() + "' has aborted: " + str(res))
                    break
                elif not res["ok"]:
                    self.errors.ServerFailure("Tenant migration with donor primary on port " +
                                              str(donor_primary.port) + " of replica set '" +
                                              migration_opts.get_donor_name() + "' has failed: " +
                                              str(res))

                time.sleep(_TenantMigrationThread.MIGRATION_STATE_POLL_INTERVAL_SECS)

            # Garbage collect the migration.
            if is_committed:
                # If the migration committed, to avoid routing commands incorrectly, wait for the
                # donor/proxy to reroute at least one command before doing garbage collection. Stop
                # waiting when the test finishes.
                self._wait_for_reroute_or_test_completion(migration_opts)
            self._forget_migration(migration_opts)
            self._wait_for_migration_garbage_collection(migration_opts)

            return is_committed
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error running tenant migration with donor primary on port %d of replica set '%s'.",
                donor_primary.port, migration_opts.get_donor_name())
            raise

    def _forget_migration(self, migration_opts):
        """Run donorForgetMigration to garbage collection the tenant migration denoted by migration_opts'."""
        self.logger.info("Forgetting tenant migration: %s.", str(migration_opts))

        donor_primary = migration_opts.get_donor_primary()
        donor_primary_client = donor_primary.mongo_client()

        try:
            donor_primary_client.admin.command({
                "donorForgetMigration": 1, "migrationId": bson.Binary(
                    migration_opts.migration_id.bytes, 4)
            }, bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))
        except pymongo.errors.OperationFailure as err:
            if err.code != _TenantMigrationThread.NO_SUCH_MIGRATION_ERR_CODE:
                raise
            # The fixture was restarted.
            self.logger.info(
                "Could not find tenant migration '%s' on donor primary on" +
                " port %d of replica set '%s'.", migration_opts.migration_id, donor_primary.port,
                migration_opts.get_donor_name())
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error forgetting tenant migration '%s' on donor primary on" +
                " port %d of replica set '%s'.", migration_opts.migration_id, donor_primary.port,
                migration_opts.get_donor_name())
            raise

    def _wait_for_migration_garbage_collection(self, migration_opts):  # noqa: D205,D400
        """Wait until the persisted state for migration denoted by 'migration_opts' has been
        garbage collected on both the donor and recipient.
        """
        donor_primary = migration_opts.get_donor_primary()
        donor_primary_client = donor_primary.mongo_client()

        recipient_primary = migration_opts.get_recipient_primary()
        recipient_primary_client = recipient_primary.mongo_client()

        try:
            while True:
                res = donor_primary_client.config.command({
                    "count": "tenantMigrationDonors",
                    "query": {"tenantId": migration_opts.tenant_id}
                })
                if res["n"] == 0:
                    break
            while True:
                res = recipient_primary_client.config.command({
                    "count": "tenantMigrationRecipients",
                    "query": {"tenantId": migration_opts.tenant_id}
                })
                if res["n"] == 0:
                    break
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error waiting for tenant migration for tenant id '%s' from donor replica set '%s" +
                " to recipient replica set '%s' to be garbage collection.", self._tenant_id,
                migration_opts.get_donor_name(), migration_opts.get_recipient_name())
            raise

    def _wait_for_reroute_or_test_completion(self, migration_opts):
        self.logger.info(
            "Waiting for donor replica set '%s' for migration '%s' to reroute at least one " +
            "conflicting command. Stop waiting when the test finishes.",
            migration_opts.get_donor_name(), migration_opts.migration_id)

        start_time = time.time()

        while not self.__lifecycle.is_test_finished():
            donor_primary = migration_opts.get_donor_primary()
            donor_primary_client = donor_primary.mongo_client()

            try:
                doc = donor_primary_client["testTenantMigration"]["rerouted"].find_one(
                    {"_id": bson.Binary(migration_opts.migration_id.bytes, 4)})
                if doc is not None:
                    return
            except pymongo.errors.PyMongoError:
                end_time = time.time()
                self.logger.exception(
                    "Error running find command on donor primary on port %d of replica set '%s' " +
                    "after waiting for reroute for %0d ms", donor_primary.port,
                    migration_opts.get_donor_name(), (end_time - start_time) * 1000)
                raise

            time.sleep(_TenantMigrationThread.MIGRATION_STATE_POLL_INTERVAL_SECS)

    def _drop_tenant_databases(self, rs):
        self.logger.info("Dropping tenant databases from replica set '%s'.", rs.replset_name)
        primary = rs.get_primary()
        primary_client = primary.mongo_client()

        try:
            res = primary_client.admin.command({"listDatabases": 1})
            for database in res["databases"]:
                db_name = database["name"]
                if db_name.startswith(self._tenant_id + "_"):
                    primary_client.drop_database(db_name)
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error dropping databases for tenant id '%s' on primary on" +
                " port %d of replica set '%s' to be garbage collection.", self._tenant_id,
                primary.port, rs.replset_name)
            raise
