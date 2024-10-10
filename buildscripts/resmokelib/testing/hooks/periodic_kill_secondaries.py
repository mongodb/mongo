"""Test hook for verifying correctness of secondary's behavior during an unclean shutdown."""

import time

import bson
import pymongo
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.hooks import (
    change_collection_consistency,
    dbhash,
    interface,
    oplog,
    preimages_consistency,
    validate,
)


class PeriodicKillSecondaries(interface.Hook):
    """Periodically kills the secondaries in a replica set.

    Also verifies that the secondaries can reach the SECONDARY state without having connectivity
    to the primary after an unclean shutdown.
    """

    IS_BACKGROUND = False

    DEFAULT_PERIOD_SECS = 30

    def __init__(self, hook_logger, rs_fixture, period_secs=DEFAULT_PERIOD_SECS):
        """Initialize PeriodicKillSecondaries."""
        if not isinstance(rs_fixture, replicaset.ReplicaSetFixture):
            raise TypeError(
                "{} either does not support replication or does not support writing to"
                " its oplog early".format(rs_fixture.__class__.__name__)
            )

        if rs_fixture.num_nodes <= 1:
            raise ValueError(
                "PeriodicKillSecondaries requires the replica set to contain at least"
                " one secondary"
            )

        description = (
            "PeriodicKillSecondaries (kills the secondary after running tests for a"
            " configurable period of time)"
        )
        interface.Hook.__init__(self, hook_logger, rs_fixture, description)

        self._period_secs = period_secs
        self._start_time = None
        self._last_test = None

    def after_suite(self, test_report, teardown_flag=None):
        """Run after suite."""
        if self._start_time is not None:
            # Ensure that we test killing the secondary and having it reach state SECONDARY after
            # being restarted at least once when running the suite.
            # The if condition ensures we haven't already run after the last test, making this
            # equivalent to an 'after_test' call for the last test.
            self._run(test_report)

    def before_test(self, test, test_report):
        """Run before test."""
        if self._start_time is not None:
            # The "rsSyncApplyStop" failpoint is already enabled.
            return

        for secondary in self.fixture.get_secondaries():
            # Enable the "rsSyncApplyStop" failpoint on the secondary to prevent them from
            # applying any oplog entries while the test is running.
            self._enable_rssyncapplystop(secondary)

        self._start_time = time.time()

    def after_test(self, test, test_report):
        """Run after test."""
        self._last_test = test

        # Kill the secondaries and verify that they can reach the SECONDARY state if the specified
        # period has elapsed.
        should_check_secondaries = time.time() - self._start_time >= self._period_secs
        if not should_check_secondaries:
            return

        self._run(test_report)

    def _run(self, test_report):
        try:
            hook_test_case = PeriodicKillSecondariesTestCase.create_after_test(
                self.logger, self._last_test, self, test_report
            )
            hook_test_case.configure(self.fixture)
            hook_test_case.run_dynamic_test(test_report)
        finally:
            # Set the hook back into a state where it will disable oplog application at the start
            # of the next test that runs.
            # Always reset _start_time to prevent the hook from running in after_suite immediately
            # after failing in after_test.
            self._start_time = None

    def _enable_rssyncapplystop(self, secondary):
        # Enable the "rsSyncApplyStop" failpoint on the secondary to prevent them from
        # applying any oplog entries while the test is running.
        client = secondary.mongo_client()
        try:
            client.admin.command(
                bson.SON([("configureFailPoint", "rsSyncApplyStop"), ("mode", "alwaysOn")])
            )
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Unable to disable oplog application on the mongod on port %d", secondary.port
            )
            raise errors.ServerFailure(
                "Unable to disable oplog application on the mongod on port {}: {}".format(
                    secondary.port, err.args[0]
                )
            )

    def _disable_rssyncapplystop(self, secondary):
        # Disable the "rsSyncApplyStop" failpoint on the secondary to have it resume applying
        # oplog entries.
        client = secondary.mongo_client()
        try:
            client.admin.command(
                bson.SON([("configureFailPoint", "rsSyncApplyStop"), ("mode", "off")])
            )
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Unable to re-enable oplog application on the mongod on port %d", secondary.port
            )
            raise errors.ServerFailure(
                "Unable to re-enable oplog application on the mongod on port {}: {}".format(
                    secondary.port, err.args[0]
                )
            )


class PeriodicKillSecondariesTestCase(interface.DynamicTestCase):
    """PeriodicKillSecondariesTestCase class."""

    INTERRUPTED_DUE_TO_REPL_STATE_CHANGE = 11602
    INTERRUPTED_DUE_TO_STORAGE_CHANGE = 355

    def __init__(self, logger, test_name, description, base_test_name, hook, test_report):
        """Initialize PeriodicKillSecondariesTestCase."""
        interface.DynamicTestCase.__init__(
            self, logger, test_name, description, base_test_name, hook
        )
        self._test_report = test_report

    def run_test(self):
        """Run the test."""
        self._kill_secondaries()
        self._check_secondaries_and_restart_fixture()

        # The CheckReplOplogs hook checks that the local.oplog.rs matches on the primary and
        # the secondaries.
        self._check_repl_oplog(self._test_report)

        # The CheckReplPreImagesConsistency hook checks that config.system.preimages is compatible
        # across nodes and that there are no holes in the collection.
        self._check_pre_images_consistency(self._test_report)

        # The CheckReplChangeCollectionConsistency hook checks that config.system.change_collection
        # for each tenant is compatible across nodes and that there are no holes in the collection.
        self._check_change_collection_consistency(self._test_report)

        # The CheckReplDBHash hook waits until all operations have replicated to and have been
        # applied on the secondaries, so we run the ValidateCollections hook after it to ensure
        # we're validating the entire contents of the collection.
        #
        # Verify that the dbhashes match across all nodes after having the secondaries reconcile
        # the end of their oplogs.
        self._check_repl_dbhash(self._test_report)

        # Validate all collections on all nodes after having the secondaries reconcile the end
        # of their oplogs.
        self._validate_collections(self._test_report)

        self._restart_and_clear_fixture()

    def _kill_secondaries(self):
        for secondary in self.fixture.get_secondaries():
            # Disable the "rsSyncApplyStop" failpoint on the secondary to have it resume applying
            # oplog entries.
            self._hook._disable_rssyncapplystop(secondary)

            # Wait a little bit for the secondary to start apply oplog entries so that we are more
            # likely to kill the mongod process while it is partway into applying a batch.
            time.sleep(0.1)

            # Check that the secondary is still running before forcibly terminating it. This ensures
            # we still detect some cases in which the secondary has already crashed.
            if not secondary.is_running():
                raise errors.ServerFailure(
                    "mongod on port {} was expected to be running in"
                    " PeriodicKillSecondaries.after_test(), but wasn't.".format(secondary.port)
                )

            self.logger.info("Killing the secondary on port %d...", secondary.port)
            secondary.mongod.stop(mode=fixture.TeardownMode.KILL)

        try:
            self.fixture.teardown()
        except errors.ServerFailure:
            # Teardown may or may not be considered a success as a result of killing a secondary,
            # so we ignore ServerFailure raised during teardown.
            pass

    def _check_secondaries_and_restart_fixture(self):
        preserve_dbpaths = []
        for node in self.fixture.nodes:
            preserve_dbpaths.append(node.preserve_dbpath)
            node.preserve_dbpath = True

        for secondary in self.fixture.get_secondaries():
            self._check_invariants_as_standalone(secondary)
            # We disable the remover for pre-images. Otherwise the remover might run and fix any
            # potential consistency issues before we validate the config.system.preimages
            # collection.
            if "set_parameters" in secondary.mongod_options:
                secondary.mongod_options["set_parameters"]["disableExpiredPreImagesRemover"] = True
                secondary.mongod_options["set_parameters"][
                    "disableExpiredChangeCollectionRemover"
                ] = True
            else:
                secondary.mongod_options["set_parameters"] = {
                    "disableExpiredPreImagesRemover": True,
                    "disableExpiredChangeCollectionRemover": True,
                }

            self.logger.info(
                "Restarting the secondary on port %d as a replica set node with"
                " its data files intact...",
                secondary.port,
            )
            # Start the 'secondary' mongod back up as part of the replica set and wait for it to
            # reach state SECONDARY.
            secondary.setup()
            self.logger.info(fixture.create_fixture_table(self.fixture))
            secondary.await_ready()
            self._await_secondary_state(secondary)

            try:
                secondary.teardown()
            except errors.ServerFailure:
                raise errors.ServerFailure(
                    "{} did not exit cleanly after reconciling the end of its oplog".format(
                        secondary
                    )
                )

        self.logger.info(
            "Starting the fixture back up again with its data files intact for final"
            " validation..."
        )

        try:
            self.fixture.setup()
            self.logger.info(fixture.create_fixture_table(self.fixture))
            self.fixture.await_ready()
        finally:
            for i, node in enumerate(self.fixture.nodes):
                node.preserve_dbpath = preserve_dbpaths[i]

    def _validate_collections(self, test_report):
        validate_test_case = validate.ValidateCollections(
            self._hook.logger,
            self.fixture,
            {"global_vars": {"TestData": {"skipEnforceFastCountOnValidate": True}}},
        )
        validate_test_case.before_suite(test_report)
        validate_test_case.before_test(self, test_report)
        validate_test_case.after_test(self, test_report)
        validate_test_case.after_suite(test_report)

    def _check_repl_dbhash(self, test_report):
        dbhash_test_case = dbhash.CheckReplDBHash(self._hook.logger, self.fixture)
        dbhash_test_case.before_suite(test_report)
        dbhash_test_case.before_test(self, test_report)
        dbhash_test_case.after_test(self, test_report)
        dbhash_test_case.after_suite(test_report)

    def _check_repl_oplog(self, test_report):
        oplog_test_case = oplog.CheckReplOplogs(self._hook.logger, self.fixture)
        oplog_test_case.before_suite(test_report)
        oplog_test_case.before_test(self, test_report)
        oplog_test_case.after_test(self, test_report)
        oplog_test_case.after_suite(test_report)

    def _check_pre_images_consistency(self, test_report):
        preimages_test_case = preimages_consistency.CheckReplPreImagesConsistency(
            self._hook.logger, self.fixture
        )
        preimages_test_case.before_suite(test_report)
        preimages_test_case.before_test(self, test_report)
        preimages_test_case.after_test(self, test_report)
        preimages_test_case.after_suite(test_report)

    def _check_change_collection_consistency(self, test_report):
        change_collection_test_case = (
            change_collection_consistency.CheckReplChangeCollectionConsistency(
                self._hook.logger, self.fixture
            )
        )
        change_collection_test_case.before_suite(test_report)
        change_collection_test_case.before_test(self, test_report)
        change_collection_test_case.after_test(self, test_report)
        change_collection_test_case.after_suite(test_report)

    def _restart_and_clear_fixture(self):
        # We restart the fixture after setting 'preserve_dbpath' back to its original value in order
        # to clear the contents of the data directory if desired. The CleanEveryN hook cannot be
        # used in combination with the PeriodicKillSecondaries hook because we may attempt to call
        # Fixture.teardown() while the "rsSyncApplyStop" failpoint is still enabled on the
        # secondaries, causing them to exit with a non-zero return code.
        self.logger.info("Finished verifying data consistency, stopping the fixture...")

        try:
            self.fixture.teardown()
        except errors.ServerFailure:
            raise errors.ServerFailure(
                "{} did not exit cleanly after verifying data consistency".format(self.fixture)
            )

        for secondary in self.fixture.get_secondaries():
            # We re-enable the removers for pre-images and change collections. These were disabled
            # before re-joining the replSet as a secondary during the consistency checks.
            secondary.mongod_options["set_parameters"].pop("disableExpiredPreImagesRemover")
            secondary.mongod_options["set_parameters"].pop("disableExpiredChangeCollectionRemover")

        self.logger.info("Starting the fixture back up again with no data...")
        self.fixture.setup()
        self.logger.info(fixture.create_fixture_table(self.fixture))
        self.fixture.await_ready()

    def _check_invariants_as_standalone(self, secondary):
        # We remove the --replSet option in order to start the node as a standalone.
        def try_pop_default_to_none(option):
            try:
                return secondary.mongod_options.pop(option)
            except KeyError:
                return None

        # On serverless, replSet is not set, but the serverless flag still enables replication.
        replset_name = try_pop_default_to_none("replSet")
        serverless = try_pop_default_to_none("serverless")

        self.logger.info(
            "Restarting the secondary on port %d as a standalone node with"
            " its data files intact...",
            secondary.port,
        )

        try:
            secondary.setup()
            self.logger.info(fixture.create_fixture_table(self.fixture))
            secondary.await_ready()

            client = secondary.mongo_client()
            oplog_truncate_after_doc = client.local["replset.oplogTruncateAfterPoint"].find_one()
            recovery_timestamp_res = client.admin.command(
                "replSetTest", getLastStableRecoveryTimestamp=True
            )
            latest_oplog_doc = client.local["oplog.rs"].find_one(
                sort=[("$natural", pymongo.DESCENDING)]
            )

            self.logger.info(
                "Checking replication invariants. oplogTruncateAfterPoint: {},"
                " stable recovery timestamp: {}, latest oplog doc: {}".format(
                    oplog_truncate_after_doc, recovery_timestamp_res, latest_oplog_doc
                )
            )

            null_ts = bson.Timestamp(0, 0)

            # We wait for a stable recovery timestamp at setup, so we must have an oplog.
            latest_oplog_entry_ts = null_ts
            if latest_oplog_doc is None:
                raise errors.ServerFailure("No latest oplog entry")
            latest_oplog_entry_ts = latest_oplog_doc.get("ts")
            if latest_oplog_entry_ts is None:
                raise errors.ServerFailure(
                    "Latest oplog entry had no 'ts' field: {}".format(latest_oplog_doc)
                )

            # The "lastStableRecoveryTimestamp" field is present if the storage engine supports
            # "recover to a timestamp". If it's a null timestamp on a durable storage engine, that
            # means we do not yet have a stable checkpoint timestamp and must be restarting at the
            # top of the oplog. Since we wait for a stable recovery timestamp at test fixture setup,
            # we should never encounter a null timestamp here.
            recovery_timestamp = recovery_timestamp_res.get("lastStableRecoveryTimestamp")
            if recovery_timestamp == null_ts:
                raise errors.ServerFailure(
                    "Received null stable recovery timestamp {}".format(recovery_timestamp_res)
                )
            # On a storage engine that doesn't support "recover to a timestamp", we default to null.
            if recovery_timestamp is None:
                recovery_timestamp = null_ts

            # last stable recovery timestamp <= top of oplog
            if not recovery_timestamp <= latest_oplog_entry_ts:
                raise errors.ServerFailure(
                    "The condition last stable recovery timestamp <= top"
                    " of oplog ({} <= {}) doesn't hold:"
                    " getLastStableRecoveryTimestamp result={},"
                    " latest oplog entry={}".format(
                        recovery_timestamp,
                        latest_oplog_entry_ts,
                        recovery_timestamp_res,
                        latest_oplog_doc,
                    )
                )

            try:
                secondary.teardown()
            except errors.ServerFailure:
                raise errors.ServerFailure(
                    "{} did not exit cleanly after being started up as a standalone".format(
                        secondary
                    )
                )
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Failed to read the minValid document, the oplogTruncateAfterPoint document,"
                " the last stable recovery timestamp, or the latest oplog entry from the"
                " mongod on port %d",
                secondary.port,
            )
            raise errors.ServerFailure(
                "Failed to read the minValid document, the oplogTruncateAfterPoint document,"
                " the last stable recovery timestamp, or the latest oplog entry from the"
                " mongod on port {}: {}".format(secondary.port, err.args[0])
            )
        finally:
            # Set the secondary's options back to their original values.
            if replset_name:
                secondary.mongod_options["replSet"] = replset_name
            if serverless:
                secondary.mongod_options["serverless"] = serverless

    def _await_secondary_state(self, secondary):
        client = secondary.mongo_client()
        while True:
            try:
                client.admin.command(
                    bson.SON(
                        [
                            ("replSetTest", 1),
                            ("waitForMemberState", 2),  # 2 = SECONDARY
                            (
                                "timeoutMillis",
                                fixture.ReplFixture.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60 * 1000,
                            ),
                        ]
                    )
                )
                break
            except pymongo.errors.OperationFailure as err:
                if err.code not in (
                    self.INTERRUPTED_DUE_TO_REPL_STATE_CHANGE,
                    self.INTERRUPTED_DUE_TO_STORAGE_CHANGE,
                ):
                    self.logger.exception(
                        "mongod on port %d failed to reach state SECONDARY after %d seconds",
                        secondary.port,
                        fixture.ReplFixture.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60,
                    )
                    raise errors.ServerFailure(
                        "mongod on port {} failed to reach state SECONDARY after {} seconds: {}".format(
                            secondary.port,
                            fixture.ReplFixture.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60,
                            err.args[0],
                        )
                    )

                msg = (
                    "Interrupted while waiting for node to reach secondary state, retrying: {}"
                ).format(err)
                self.logger.error(msg)
