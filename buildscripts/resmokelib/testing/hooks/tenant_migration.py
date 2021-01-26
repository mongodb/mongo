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

        self.__test_state = self._TEST_FINISHED_STATE
        self.__should_stop = False

    def mark_test_started(self):
        """Signal to the tenant migration thread that a new test has started.

        This function should be called during before_test(). Calling it causes the
        wait_for_tenant_migration_permitted() function to no longer block and to instead return
        true.
        """
        with self.__lock:
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


class _TenantMigrationThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    MAX_MIGRATION_INTERVAL_SECS = 5
    MIGRATION_STATE_POLL_INTERVAL_SECS = 0.1

    def __init__(self, logger, tenant_migration_fixture, tenant_id):
        """Initialize _TenantMigrationThread."""
        threading.Thread.__init__(self, name="TenantMigrationThread")
        self.daemon = True
        self.logger = logger
        self._tenant_migration_fixture = tenant_migration_fixture
        self._tenant_id = tenant_id

        self.__lifecycle = TenantMigrationLifeCycle()
        self._last_exec = time.time()
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

        donor_rs = self._tenant_migration_fixture.get_replset(0)
        recipient_rs = self._tenant_migration_fixture.get_replset(1)

        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_tenant_migration_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                self.logger.info("Starting tenant migration for tenant id '%s' from %s to %s.",
                                 self._tenant_id, donor_rs.replset_name, recipient_rs.replset_name)
                now = time.time()
                self._run_migration(donor_rs, recipient_rs)
                self._last_exec = time.time()
                self.logger.info(
                    "Completed tenant migration in %0d ms for tenant id '%s' from %s to %s.",
                    (self._last_exec - now) * 1000, self._tenant_id, donor_rs.replset_name,
                    recipient_rs.replset_name)

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    continue

                # The 'wait_secs' is used to wait [0.1, 1.0] * MAX_MIGRATION_INTERVAL_SECS from the moment the last
                # tenant migration completed. We want to sometimes wait for a larger interval to allow long-running
                # commands, like large batched writes, to complete without continuously conflicting with a
                # migration.
                now = time.time()
                wait_secs = max(
                    0,
                    random.uniform(0.1, 1.0) * _TenantMigrationThread.MAX_MIGRATION_INTERVAL_SECS -
                    (now - self._last_exec))
                self.__lifecycle.wait_for_tenant_migration_interval(wait_secs)
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be flushed immediately.
            self.logger.exception("Tenant migration thread threw exception")
            # The event should be signaled whenever the thread is not performing migrations.
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
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _wait(self, timeout):
        """Wait until stop or timeout."""
        self._is_stopped_evt.wait(timeout)

    def _check_thread(self):
        """Throw an error if the thread is not running."""
        if not self.is_alive():
            msg = "The tenant migration thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _wait_for_migration_garbage_collection(self, rs):
        primary = rs.get_primary()
        primary_client = primary.mongo_client()

        try:
            while True:
                res = primary_client.config.command(
                    {"count": "tenantMigrationDonors", "query": {"tenantId": self._tenant_id}})
                if res["n"] == 0:
                    break
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error waiting for tenant migration for tenant id '%s' on primary on" +
                " port %d of replica set '%s' to be garbage collection.", self._tenant_id,
                primary.port, rs.replset_name)
            raise

    def _drop_tenant_databases(self, rs):
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

    def _forget_migration(self, donor_rs, migration_id):
        donor_primary = donor_rs.get_primary()
        donor_primary_client = donor_primary.mongo_client()

        self.logger.info(
            "Forgeting tenant migration with donor primary on port %d of replica set '%s'.",
            donor_primary.port, donor_rs.replset_name)

        try:
            donor_primary_client.admin.command(
                {"donorForgetMigration": 1, "migrationId": migration_id},
                bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error forgetting tenant migration with donor primary on port %d of replica set '%s'.",
                donor_primary.port, donor_rs.replset_name)
            raise

    def _run_migration(self, donor_rs, recipient_rs):
        donor_primary = donor_rs.get_primary()
        donor_primary_client = donor_primary.mongo_client()

        self.logger.info(
            "Starting tenant migration with donor primary on port %d of replica set '%s'.",
            donor_primary.port, donor_rs.replset_name)

        migration_id = bson.Binary(uuid.uuid4().bytes, 4)
        cmd_obj = {
            "donorStartMigration":
                1,
            "migrationId":
                migration_id,
            "recipientConnectionString":
                recipient_rs.get_driver_connection_url(),
            "tenantId":
                self._tenant_id,
            "readPreference": {"mode": "primary"},
            "donorCertificateForRecipient":
                get_certificate_and_private_key("jstests/libs/rs0_tenant_migration.pem"),
            "recipientCertificateForDonor":
                get_certificate_and_private_key("jstests/libs/rs1_tenant_migration.pem"),
        }

        try:
            while True:
                # Keep polling the migration state until the migration completes, otherwise we
                # might end up disabling 'pauseTenantMigrationBeforeLeavingBlockingStateWithTimeout'
                # before the tenant migration enters the blocking state and aborts.
                res = donor_primary_client.admin.command(
                    cmd_obj,
                    bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))

                if res["state"] == "committed":
                    # TODO (SERVER-50495): Make tenant_migration_jscore_passthrough simulate a
                    # migration that commits.
                    errors.ServerFailure("Tenant migration with donor primary on port " +
                                         str(donor_primary.port) + " of replica set '" +
                                         donor_rs.replset_name + "' has committed.")
                elif res["state"] == "aborted":
                    self.logger.info("Tenant migration with donor primary on port " +
                                     str(donor_primary.port) + " of replica set '" +
                                     donor_rs.replset_name + "' has aborted: " + str(res))
                    break
                elif not res["ok"]:
                    errors.ServerFailure("Tenant migration with donor primary on port " +
                                         str(donor_primary.port) + " of replica set '" +
                                         donor_rs.replset_name + "' has failed: " + str(res))

                time.sleep(_TenantMigrationThread.MIGRATION_STATE_POLL_INTERVAL_SECS)

            self._forget_migration(donor_rs, migration_id)
            # Wait for the donor to garbage the migration state.
            self._wait_for_migration_garbage_collection(donor_rs)
            # Drop any tenant databases that the recipient cloned during the migration.
            self._drop_tenant_databases(recipient_rs)
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error running tenant migration with donor primary on port %d of replica set '%s'.",
                donor_primary.port, donor_rs.replset_name)
            raise
