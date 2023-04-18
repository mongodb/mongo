"""Test hook that runs tenant migrations continuously."""

import copy
import random
import re
import threading
import time
import uuid

from bson.binary import Binary, UUID_SUBTYPE
from pymongo.errors import OperationFailure, PyMongoError

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import tenant_migration
from buildscripts.resmokelib.testing.fixtures.fixturelib import with_naive_retry
from buildscripts.resmokelib.testing.hooks import dbhash_tenant_migration
from buildscripts.resmokelib.testing.hooks import interface


class ContinuousTenantMigration(interface.Hook):
    """Starts a tenant migration thread at the beginning of each test."""

    DESCRIPTION = ("Continuous tenant migrations")

    IS_BACKGROUND = True

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
        self._shell_options = copy.deepcopy(shell_options)

        self._tenant_migration_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._tenant_migration_fixture:
            raise ValueError("No TenantMigrationFixture to run migrations on")
        self.logger.info("Starting the tenant migration thread.")
        self._tenant_migration_thread = _TenantMigrationThread(
            self.logger, self._tenant_migration_fixture, self._shell_options, test_report)
        self._tenant_migration_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the tenant migration thread.")
        self._tenant_migration_thread.stop()
        self.logger.info("Stopped the tenant migration thread.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the tenant migration thread.")
        self._tenant_migration_thread.resume(test)

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


class _TenantMigrationOptions:
    def __init__(self, donor_rs, recipient_rs, tenant_id, read_preference, logger):
        self.donor_rs = donor_rs
        self.recipient_rs = recipient_rs
        self.migration_id = uuid.uuid4()
        self.tenant_id = tenant_id
        self.read_preference = read_preference
        self.logger = logger

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


class _TenantMigrationThread(threading.Thread):
    THREAD_NAME = "TenantMigrationThread"

    WAIT_SECS_RANGES = [[0.05, 0.1], [0.1, 0.5], [1, 5], [5, 15]]
    POLL_INTERVAL_SECS = 0.1

    MIGRATION_ABORTED_ERR_CODE = 325
    NO_SUCH_MIGRATION_ERR_CODE = 327
    INTERNAL_ERR_CODE = 1
    INVALID_SYNC_SOURCE_ERR_CODE = 119
    FAIL_TO_PARSE_ERR_CODE = 9
    NO_SUCH_KEY_ERR_CODE = 4

    def __init__(self, logger, tenant_migration_fixture, shell_options, test_report):
        """Initialize _TenantMigrationThread."""
        threading.Thread.__init__(self, name=self.THREAD_NAME)
        self.daemon = True
        self.logger = logger
        self._tenant_migration_fixture = tenant_migration_fixture
        self._tenant_id = shell_options["global_vars"]["TestData"]["tenantId"]
        self._auth_options = shell_options["global_vars"]["TestData"]["authOptions"]
        self._test = None
        self._test_report = test_report
        self._shell_options = shell_options

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

                # Briefly wait to let the test run before starting the tenant migration, so that
                # the first migration is more likely to have data to migrate.
                wait_secs = random.uniform(
                    *self.WAIT_SECS_RANGES[migration_num % len(self.WAIT_SECS_RANGES)])
                self.logger.info("Waiting for %.3f seconds before starting migration.", wait_secs)
                self.__lifecycle.wait_for_tenant_migration_interval(wait_secs)

                self.logger.info("Starting tenant migration: %s.", str(migration_opts))
                start_time = time.time()
                is_committed = self._run_migration(migration_opts)
                end_time = time.time()
                self.logger.info("Completed tenant migration in %0d ms: %s.",
                                 (end_time - start_time) * 1000, str(migration_opts))

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    continue

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
        if not self._tenant_migration_fixture.is_running():
            raise errors.ServerFailure(
                "TenantMigrationFixture with pids {} expected to be running in"
                " ContinuousTenantMigration, but wasn't".format(
                    self._tenant_migration_fixture.pids()))

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
            msg = "Tenant migration thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _is_fail_point_abort_reason(self, abort_reason):
        return abort_reason["code"] == self.INTERNAL_ERR_CODE and abort_reason[
            "errmsg"] == "simulate a tenant migration error"

    def _is_recipient_failover_abort_reason(self, abort_reason):
        return abort_reason["code"] == self.MIGRATION_ABORTED_ERR_CODE and abort_reason[
            "errmsg"].find("Recipient failover happened during migration")

    def _create_migration_opts(self, donor_rs_index, recipient_rs_index):
        donor_rs = self._tenant_migration_fixture.get_replset(donor_rs_index)
        recipient_rs = self._tenant_migration_fixture.get_replset(recipient_rs_index)
        read_preference = {"mode": "primary"} if random.randint(0, 1) else {"mode": "secondary"}
        return _TenantMigrationOptions(donor_rs, recipient_rs, self._tenant_id, read_preference,
                                       self.logger)

    def _create_client(self, fixture, **kwargs):
        return fixture.mongo_client(username=self._auth_options["username"],
                                    password=self._auth_options["password"],
                                    authSource=self._auth_options["authenticationDatabase"],
                                    authMechanism=self._auth_options["authenticationMechanism"],
                                    uuidRepresentation='standard', **kwargs)

    def _check_tenant_migration_dbhash(self, migration_opts):
        # Set the donor connection string, recipient connection string, and migration uuid string
        # for the tenant migration dbhash check script.
        self._shell_options[
            "global_vars"]["TestData"]["donorConnectionString"] = migration_opts.get_donor_primary(
            ).get_internal_connection_string()
        self._shell_options["global_vars"]["TestData"][
            "recipientConnectionString"] = migration_opts.get_recipient_primary(
            ).get_internal_connection_string()
        self._shell_options["global_vars"]["TestData"][
            "migrationIdString"] = migration_opts.migration_id.__str__()

        dbhash_test_case = dbhash_tenant_migration.CheckTenantMigrationDBHash(
            self.logger, self._tenant_migration_fixture, self._shell_options)
        dbhash_test_case.before_suite(self._test_report)
        dbhash_test_case.before_test(self._test, self._test_report)
        dbhash_test_case.after_test(self._test, self._test_report)
        dbhash_test_case.after_suite(self._test_report)

    def _run_migration(self, migration_opts):  # noqa: D205,D400
        """Run a tenant migration based on 'migration_opts', wait for the migration decision and
        garbage collection. Return true if the migration commits and false otherwise.
        """
        try:
            # Clean up any orphaned tenant databases on the recipient allow next migration to start.
            self._drop_tenant_databases_on_recipient(migration_opts)

            donor_client = self._create_client(migration_opts.donor_rs)
            res = self._start_and_wait_for_migration(donor_client, migration_opts)
            is_committed = res["state"] == "committed"

            # Garbage collect the migration prior to throwing error to avoid migration conflict
            # in the next test.
            if is_committed:
                # Once we have committed a migration, run a dbhash check before rerouting commands.
                self._check_tenant_migration_dbhash(migration_opts)

                # If the migration committed, to avoid routing commands incorrectly, wait for the
                # donor/proxy to reroute at least one command before doing garbage collection. Stop
                # waiting when the test finishes.
                self._wait_for_reroute_or_test_completion(donor_client, migration_opts)
            self._forget_migration(donor_client, migration_opts)
            self._wait_for_migration_garbage_collection(migration_opts)

            if not res["ok"]:
                raise errors.ServerFailure("Tenant migration '" + str(migration_opts.migration_id) +
                                           "' with donor replica set '" +
                                           migration_opts.get_donor_name() + "' failed: " +
                                           str(res))

            if is_committed:
                return True

            abort_reason = res["abortReason"]
            if self._is_recipient_failover_abort_reason(abort_reason):
                self.logger.info("Tenant migration '%s' aborted due to recipient failover: %s",
                                 migration_opts.migration_id, str(res))
                return False
            elif self._is_fail_point_abort_reason(abort_reason):
                self.logger.info(
                    "Tenant migration '%s' with donor replica set '%s' aborted due to failpoint: " +
                    "%s.", migration_opts.migration_id, migration_opts.get_donor_name(), str(res))
                return False
            raise errors.ServerFailure("Tenant migration '" + str(migration_opts.migration_id) +
                                       "' with donor replica set '" +
                                       migration_opts.get_donor_name() +
                                       "' aborted due to an error: " + str(res))
        except PyMongoError:
            self.logger.exception(
                "Error running tenant migration '%s' with donor primary on replica set '%s'.",
                migration_opts.migration_id, migration_opts.get_donor_name())
            raise

    def _start_and_wait_for_migration(self, donor_client, migration_opts):  # noqa: D205,D400
        """Run donorStartMigration to start a tenant migration based on 'migration_opts', wait for
        the migration decision and return the last response for donorStartMigration.
        """

        self.logger.info(
            f"Starting tenant migration '{migration_opts.migration_id}' on replica set "
            f"'{migration_opts.get_donor_name()}'.")

        while True:
            # Keep polling the migration state until the migration completes.
            res = with_naive_retry(lambda: donor_client.admin.command({
                "donorStartMigration":
                    1,
                "migrationId":
                    Binary(migration_opts.migration_id.bytes, UUID_SUBTYPE),
                "recipientConnectionString":
                    migration_opts.recipient_rs.get_driver_connection_url(),
                "tenantId":
                    migration_opts.tenant_id,
                "readPreference":
                    migration_opts.read_preference,
                "donorCertificateForRecipient":
                    get_certificate_and_private_key("jstests/libs/tenant_migration_donor.pem"),
                "recipientCertificateForDonor":
                    get_certificate_and_private_key("jstests/libs/tenant_migration_recipient.pem"),
            }))

            if res["state"] == "committed":
                self.logger.info(f"Tenant migration '{migration_opts.migration_id}' on replica set "
                                 f"'{migration_opts.get_donor_name()}' has committed.")
                return res
            if res["state"] == "aborted":
                self.logger.info(f"Tenant migration '{migration_opts.migration_id}' on replica set "
                                 f"'{migration_opts.get_donor_name()}' has aborted: '{str(res)}'.")
                return res
            if not res["ok"]:
                self.logger.info(f"Tenant migration '{migration_opts.migration_id}' on replica set "
                                 f"'{migration_opts.get_donor_name()}' has failed: '{str(res)}'.")
                return res

            time.sleep(self.POLL_INTERVAL_SECS)

    def _forget_migration(self, donor_client, migration_opts):
        """Run donorForgetMigration to garbage collection the tenant migration denoted by migration_opts'."""
        self.logger.info(
            f"Forgetting tenant migration '{migration_opts.migration_id}' on replica set "
            f"'{migration_opts.get_donor_name()}'.")

        try:
            with_naive_retry(lambda: donor_client.admin.command({
                "donorForgetMigration": 1, "migrationId": Binary(migration_opts.migration_id.bytes,
                                                                 UUID_SUBTYPE)
            }))
        except OperationFailure as err:
            if err.code != self.NO_SUCH_MIGRATION_ERR_CODE:
                raise

            self.logger.info(f"Could not find tenant migration '{migration_opts.migration_id}' on "
                             f"replica set '{migration_opts.get_donor_name()}': {str(err)}.")
        except PyMongoError:
            self.logger.exception(
                f"Error forgetting tenant migration '{migration_opts.migration_id}' on "
                f"replica set '{migration_opts.get_donor_name()}'.")
            raise

    def _wait_for_migration_garbage_collection(self, migration_opts):  # noqa: D205,D400
        """Wait until the persisted state for tenant migration denoted by 'migration_opts' has been
        garbage collected on both the donor and recipient.
        """
        timeout_secs = self._tenant_migration_fixture.AWAIT_REPL_TIMEOUT_MINS * 60

        def wait_for_gc_on_node(node, rs_name, collection_name):
            self.logger.info(
                "Waiting for tenant migration '%s' to be garbage collected on donor node on " +
                "port %d of replica set '%s'.", migration_opts.migration_id, node.port, rs_name)

            node_client = self._create_client(node)

            start = time.monotonic()
            while time.monotonic() - start < timeout_secs:
                res = with_naive_retry(lambda: node_client.config.command(
                    {"count": collection_name, "query": {"tenantId": migration_opts.tenant_id}}))
                if res["n"] == 0:
                    return
                time.sleep(self.POLL_INTERVAL_SECS)

            raise errors.ServerFailure(
                f"Timed out while waiting for garbage collection of node on port {node.port}.")

        try:
            donor_nodes = migration_opts.get_donor_nodes()
            for donor_node in donor_nodes:
                wait_for_gc_on_node(donor_node, migration_opts.get_donor_name(),
                                    "tenantMigrationDonors")

            recipient_nodes = migration_opts.get_recipient_nodes()
            for recipient_node in recipient_nodes:
                wait_for_gc_on_node(recipient_node, migration_opts.get_recipient_name(),
                                    "tenantMigrationRecipients")
        except PyMongoError:
            self.logger.exception(
                "Error waiting for tenant migration '%s' from donor replica set '%s" +
                " to recipient replica set '%s' to be garbage collected.",
                migration_opts.migration_id, migration_opts.get_donor_name(),
                migration_opts.get_recipient_name())
            raise

    def _wait_for_reroute_or_test_completion(self, donor_client, migration_opts):
        self.logger.info(
            f"Waiting for tenant migration '{migration_opts.migration_id}' on replica set "
            f"'{migration_opts.get_donor_name()}' to reroute at least one conflicting command. "
            f"Stop waiting when the test finishes.")

        start_time = time.time()
        while not self.__lifecycle.is_test_finished():
            try:
                doc = donor_client["testTenantMigration"]["rerouted"].find_one(
                    {"_id": Binary(migration_opts.migration_id.bytes, UUID_SUBTYPE)})
                if doc is not None:
                    return
            except PyMongoError:
                end_time = time.time()
                self.logger.exception(
                    f"Error running find command on replica set '{migration_opts.get_donor_name()}' "
                    f"after waiting for reroute for {(end_time - start_time) * 1000} ms")
                raise

            time.sleep(self.POLL_INTERVAL_SECS)

    def _drop_tenant_databases_on_recipient(self, migration_opts):
        self.logger.info(
            f"Dropping tenant databases from replica set '{migration_opts.get_recipient_name()}'.")
        recipient_client = self._create_client(migration_opts.recipient_rs)

        try:
            self.logger.info(
                f"Running dropDatabase commands against replica set '{migration_opts.get_recipient_name()}'"
            )
            res = with_naive_retry(lambda: recipient_client.admin.command({"listDatabases": 1}))
            for database in res["databases"]:
                db_name = database["name"]
                if db_name.startswith(self._tenant_id + "_"):
                    recipient_client.drop_database(db_name)
        except PyMongoError as err:
            self.logger.exception(
                f"Error dropping databases for tenant '{self._tenant_id}' on replica set '{migration_opts.get_recipient_name()}': '{str(err)}'."
            )
            raise
