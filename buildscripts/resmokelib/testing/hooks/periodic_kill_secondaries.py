"""
Testing hook for verifying correctness of a secondary's behavior during
an unclean shutdown.
"""

from __future__ import absolute_import

import sys
import time

import bson
import pymongo
import pymongo.errors

from . import dbhash
from . import interface
from . import validate
from ..fixtures import interface as fixture
from ..fixtures import replicaset
from ..testcases import interface as testcase
from ... import errors


class PeriodicKillSecondaries(interface.CustomBehavior):
    """
    Periodically kills the secondaries in a replica set and verifies
    that they can reach the SECONDARY state without having connectivity
    to the primary after an unclean shutdown.
    """

    DEFAULT_PERIOD_SECS = 30

    def __init__(self, hook_logger, rs_fixture, period_secs=DEFAULT_PERIOD_SECS):
        if not isinstance(rs_fixture, replicaset.ReplicaSetFixture):
            raise TypeError("{} either does not support replication or does not support writing to"
                            " its oplog early".format(rs_fixture.__class__.__name__))

        if rs_fixture.num_nodes <= 1:
            raise ValueError("PeriodicKillSecondaries requires the replica set to contain at least"
                             " one secondary")

        description = ("PeriodicKillSecondaries (kills the secondary after running tests for a"
                       " configurable period of time)")
        interface.CustomBehavior.__init__(self, hook_logger, rs_fixture, description)

        self._period_secs = period_secs
        self._start_time = None

    def after_suite(self, test_report):
        if self._start_time is not None:
            # Ensure that we test killing the secondary and having it reach state SECONDARY after
            # being restarted at least once when running the suite.
            self._run(test_report)

    def before_test(self, test, test_report):
        if self._start_time is not None:
            # The "rsSyncApplyStop" failpoint is already enabled.
            return

        for secondary in self.fixture.get_secondaries():
            # Enable the "rsSyncApplyStop" failpoint on the secondary to prevent them from
            # applying any oplog entries while the test is running.
            self._enable_rssyncapplystop(secondary)

        self._start_time = time.time()

    def after_test(self, test, test_report):
        self._last_test_name = test.short_name()

        # Kill the secondaries and verify that they can reach the SECONDARY state if the specified
        # period has elapsed.
        should_check_secondaries = time.time() - self._start_time >= self._period_secs
        if not should_check_secondaries:
            return

        self._run(test_report)

    def _run(self, test_report):
        self.hook_test_case = testcase.TestCase(
            self.logger,
            "Hook",
            "{}:{}".format(self._last_test_name, self.logger_name))
        interface.CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)

        try:
            self._kill_secondaries()
            self._check_secondaries_and_restart_fixture()

            # Validate all collections on all nodes after having the secondaries reconcile the end
            # of their oplogs.
            self._validate_collections(test_report)

            # Verify that the dbhashes match across all nodes after having the secondaries reconcile
            # the end of their oplogs.
            self._check_repl_dbhash(test_report)

            self._restart_and_clear_fixture()
        except Exception as err:
            self.hook_test_case.logger.exception(
                "Encountered an error running PeriodicKillSecondaries.")
            self.hook_test_case.return_code = 2
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.StopExecution(err.args[0])
        else:
            self.hook_test_case.return_code = 0
            test_report.addSuccess(self.hook_test_case)
        finally:
            test_report.stopTest(self.hook_test_case)

            # Set the hook back into a state where it will disable oplog application at the start
            # of the next test that runs.
            self._start_time = None

    def _kill_secondaries(self):
        for secondary in self.fixture.get_secondaries():
            # Disable the "rsSyncApplyStop" failpoint on the secondary to have it resume applying
            # oplog entries.
            self._disable_rssyncapplystop(secondary)

            # Wait a little bit for the secondary to start apply oplog entries so that we are more
            # likely to kill the mongod process while it is partway into applying a batch.
            time.sleep(0.1)

            # Check that the secondary is still running before forcibly terminating it. This ensures
            # we still detect some cases in which the secondary has already crashed.
            if not secondary.is_running():
                raise errors.ServerFailure(
                    "mongod on port {} was expected to be running in"
                    " PeriodicKillSecondaries.after_test(), but wasn't.".format(secondary.port))

            self.hook_test_case.logger.info(
                "Killing the secondary on port %d...", secondary.port)
            secondary.mongod.stop(kill=True)

        # Teardown may or may not be considered a success as a result of killing a secondary, so we
        # ignore the return value of Fixture.teardown().
        self.fixture.teardown()

    def _check_secondaries_and_restart_fixture(self):
        preserve_dbpaths = []
        for node in self.fixture.nodes:
            preserve_dbpaths.append(node.preserve_dbpath)
            node.preserve_dbpath = True

        for secondary in self.fixture.get_secondaries():
            self._check_invariants_as_standalone(secondary)

            # Start the 'secondary' mongod back up as part of the replica set and wait for it to
            # reach state SECONDARY.
            secondary.setup()
            secondary.await_ready()
            self._await_secondary_state(secondary)

            teardown_success = secondary.teardown()
            if not teardown_success:
                raise errors.ServerFailure(
                    "{} did not exit cleanly after reconciling the end of its oplog".format(
                        secondary))

        self.hook_test_case.logger.info(
            "Starting the fixture back up again with its data files intact...")

        try:
            self.fixture.setup()
            self.fixture.await_ready()
        finally:
            for (i, node) in enumerate(self.fixture.nodes):
                node.preserve_dbpath = preserve_dbpaths[i]

    def _validate_collections(self, test_report):
        validate_test_case = validate.ValidateCollections(self.logger, self.fixture)
        validate_test_case.before_suite(test_report)
        validate_test_case.before_test(self.hook_test_case, test_report)
        validate_test_case.after_test(self.hook_test_case, test_report)
        validate_test_case.after_suite(test_report)

    def _check_repl_dbhash(self, test_report):
        dbhash_test_case = dbhash.CheckReplDBHash(self.logger, self.fixture)
        dbhash_test_case.before_suite(test_report)
        dbhash_test_case.before_test(self.hook_test_case, test_report)
        dbhash_test_case.after_test(self.hook_test_case, test_report)
        dbhash_test_case.after_suite(test_report)

    def _restart_and_clear_fixture(self):
        # We restart the fixture after setting 'preserve_dbpath' back to its original value in order
        # to clear the contents of the data directory if desired. The CleanEveryN hook cannot be
        # used in combination with the PeriodicKillSecondaries hook because we may attempt to call
        # Fixture.teardown() while the "rsSyncApplyStop" failpoint is still enabled on the
        # secondaries, causing them to exit with a non-zero return code.
        self.hook_test_case.logger.info(
            "Finished verifying data consistency, stopping the fixture...")

        teardown_success = self.fixture.teardown()
        if not teardown_success:
            raise errors.ServerFailure(
                "{} did not exit cleanly after verifying data consistency".format(self.fixture))

        self.hook_test_case.logger.info("Starting the fixture back up again...")
        self.fixture.setup()
        self.fixture.await_ready()

    def _check_invariants_as_standalone(self, secondary):
        # We remove the --replSet option in order to start the node as a standalone.
        replset_name = secondary.mongod_options.pop("replSet")

        try:
            secondary.setup()
            secondary.await_ready()

            client = secondary.mongo_client()
            minvalid_doc = client.local["replset.minvalid"].find_one()
            oplog_truncate_after_doc = client.local["replset.oplogTruncateAfterPoint"].find_one()

            latest_oplog_doc = client.local["oplog.rs"].find_one(
                sort=[("$natural", pymongo.DESCENDING)])

            null_ts = bson.Timestamp(0, 0)

            # The oplog could be empty during initial sync. If so, we default it to null.
            latest_oplog_entry_ts = null_ts
            if latest_oplog_doc is not None:
                latest_oplog_entry_ts = latest_oplog_doc.get("ts")
                if latest_oplog_entry_ts is None:
                    raise errors.ServerFailure("Latest oplog entry had no 'ts' field: {}".format(
                        latest_oplog_doc))

            # The "oplogTruncateAfterPoint" document may not exist at startup. If so, we default
            # it to null.
            oplog_truncate_after_ts = null_ts
            if oplog_truncate_after_doc is not None:
                oplog_truncate_after_ts = oplog_truncate_after_doc.get(
                    "oplogTruncateAfterPoint", null_ts)

            if minvalid_doc is not None:
                applied_through_ts = minvalid_doc.get("begin", {}).get("ts", null_ts)
                minvalid_ts = minvalid_doc.get("ts", null_ts)

                # This hook never runs upgrades, so it should never have an "oplogDeleteFromPoint".
                if minvalid_doc.get("oplogDeleteFromPoint") is not None:
                    raise errors.ServerFailure(
                        "The condition oplogDeleteFromPoint != null doesn't hold:"
                        " minValid document={}, oplogTruncateAfterPoint document={},"
                        " last oplog entry={}".format(
                            minvalid_doc, oplog_truncate_after_doc, latest_oplog_doc))

                if minvalid_ts == null_ts:
                    # The server treats the "ts" field in the minValid document as missing when its
                    # value is the null timestamp.
                    minvalid_ts = applied_through_ts

                if latest_oplog_entry_ts == null_ts:
                    # If the oplog is empty, we treat the "minValid" as the latest oplog entry.
                    latest_oplog_entry_ts = minvalid_ts

                if oplog_truncate_after_ts == null_ts:
                    # The server treats the "oplogTruncateAfterPoint" field as missing when its
                    # value is the null timestamp. When it is null, the oplog is complete and
                    # should not be truncated, so it is effectively the top of the oplog.
                    oplog_truncate_after_ts = latest_oplog_entry_ts

                # Check the ordering invariants before the secondary has reconciled the end of
                # its oplog.
                # The "oplogTruncateAfterPoint" is set to the first timestamp of each batch of
                # oplog entries before they are written to the oplog. Thus, it can be ahead
                # of the top of the oplog before any oplog entries are written, and behind it
                # after some are written. Thus, we cannot compare it to the top of the oplog.

                # appliedThrough <= minValid
                # appliedThrough represents the end of the previous batch, so it is always the
                # earliest.
                if not applied_through_ts <= minvalid_ts:
                    raise errors.ServerFailure(
                        "The condition appliedThrough <= minValid ({} <= {}) doesn't hold: minValid"
                        " document={}, latest oplog entry={}".format(
                            applied_through_ts, minvalid_ts, minvalid_doc, latest_oplog_doc))

                # minValid <= oplogTruncateAfterPoint
                # This is true because this hook is never run after a rollback. Thus, we only
                # move "minValid" to the end of each batch after the batch is written to the oplog.
                # We reset the "oplogTruncateAfterPoint" to null before we move "minValid" from
                # the end of the previous batch to the end of the current batch. Thus "minValid"
                # must be less than or equal to the "oplogTruncateAfterPoint".
                if not minvalid_ts <= oplog_truncate_after_ts:
                    raise errors.ServerFailure(
                        "The condition minValid <= oplogTruncateAfterPoint ({} <= {}) doesn't"
                        " hold: minValid document={}, oplogTruncateAfterPoint document={},"
                        " latest oplog entry={}".format(
                            minvalid_ts, oplog_truncate_after_ts, minvalid_doc,
                            oplog_truncate_after_doc, latest_oplog_doc))

                # minvalid <= latest oplog entry
                # "minValid" is set to the end of a batch after the batch is written to the oplog.
                # Thus it is always less than or equal to the top of the oplog.
                if not minvalid_ts <= latest_oplog_entry_ts:
                    raise errors.ServerFailure(
                        "The condition minValid <= top of oplog ({} <= {}) doesn't"
                        " hold: minValid document={}, latest oplog entry={}".format(
                            minvalid_ts, latest_oplog_entry_ts, minvalid_doc,
                            latest_oplog_doc))

            teardown_success = secondary.teardown()
            if not teardown_success:
                raise errors.ServerFailure(
                    "{} did not exit cleanly after being started up as a standalone".format(
                        secondary))
        except pymongo.errors.OperationFailure as err:
            self.hook_test_case.logger.exception(
                "Failed to read the minValid document, the oplogTruncateAfterPoint document,"
                " or the latest oplog entry from the mongod on"
                " port %d", secondary.port)
            raise errors.ServerFailure(
                "Failed to read the minValid document, the oplogTruncateAfterPoint document,"
                " or the latest oplog entry from the mongod on"
                " port {}: {}".format(secondary.port, err.args[0]))
        finally:
            # Set the secondary's options back to their original values.
            secondary.mongod_options["replSet"] = replset_name

    def _await_secondary_state(self, secondary):
        client = secondary.mongo_client()
        try:
            client.admin.command(bson.SON([
                ("replSetTest", 1),
                ("waitForMemberState", 2),  # 2 = SECONDARY
                ("timeoutMillis", fixture.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60 * 1000)]))
        except pymongo.errors.OperationFailure as err:
            self.hook_test_case.logger.exception(
                "mongod on port %d failed to reach state SECONDARY after %d seconds",
                secondary.port,
                fixture.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60)
            raise errors.ServerFailure(
                "mongod on port {} failed to reach state SECONDARY after {} seconds: {}".format(
                    secondary.port, fixture.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60, err.args[0]))

    def _enable_rssyncapplystop(self, secondary):
        # Enable the "rsSyncApplyStop" failpoint on the secondary to prevent them from
        # applying any oplog entries while the test is running.
        client = secondary.mongo_client()
        try:
            client.admin.command(bson.SON([
                ("configureFailPoint", "rsSyncApplyStop"),
                ("mode", "alwaysOn")]))
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Unable to disable oplog application on the mongod on port %d", secondary.port)
            raise errors.ServerFailure(
                "Unable to disable oplog application on the mongod on port {}: {}".format(
                    secondary.port, err.args[0]))

    def _disable_rssyncapplystop(self, secondary):
        # Disable the "rsSyncApplyStop" failpoint on the secondary to have it resume applying
        # oplog entries.
        client = secondary.mongo_client()
        try:
            client.admin.command(bson.SON([
                ("configureFailPoint", "rsSyncApplyStop"),
                ("mode", "off")]))
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Unable to re-enable oplog application on the mongod on port %d",
                secondary.port)
            raise errors.ServerFailure(
                "Unable to re-enable oplog application on the mongod on port {}: {}".format(
                    secondary.port, err.args[0]))
