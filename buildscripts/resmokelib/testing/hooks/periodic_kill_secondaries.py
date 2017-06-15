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
from ... import utils


class PeriodicKillSecondaries(interface.CustomBehavior):
    """
    Periodically kills the secondaries in a replica set and verifies
    that they can reach the SECONDARY state without having connectivity
    to the primary after an unclean shutdown.
    """

    DEFAULT_PERIOD_SECS = 30

    def __init__(self, hook_logger, fixture, period_secs=DEFAULT_PERIOD_SECS):
        if not isinstance(fixture, replicaset.ReplicaSetFixture):
            raise TypeError("%s either does not support replication or does not support writing to"
                            " its oplog early"
                            % (fixture.__class__.__name__))

        if fixture.num_nodes <= 1:
            raise ValueError("PeriodicKillSecondaries requires the replica set to contain at least"
                             " one secondary")

        description = ("PeriodicKillSecondaries (kills the secondary after running tests for a"
                       " configurable period of time)")
        interface.CustomBehavior.__init__(self, hook_logger, fixture, description)

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

        # Enable the "rsSyncApplyStop" failpoint on each of the secondaries to prevent them from
        # applying any oplog entries while the test is running.
        for secondary in self.fixture.get_secondaries():
            client = utils.new_mongo_client(port=secondary.port)
            try:
                client.admin.command(bson.SON([
                    ("configureFailPoint", "rsSyncApplyStop"),
                    ("mode", "alwaysOn")]))
            except pymongo.errors.OperationFailure as err:
                self.logger.exception(
                    "Unable to disable oplog application on the mongod on port %d", secondary.port)
                raise errors.ServerFailure(
                    "Unable to disable oplog application on the mongod on port %d: %s"
                    % (secondary.port, err.args[0]))

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
            "%s:%s" % (self._last_test_name, self.logger_name))
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
            for secondary in self.fixture.get_secondaries():
                client = utils.new_mongo_client(port=secondary.port)
                try:
                    client.admin.command(bson.SON([
                        ("configureFailPoint", "rsSyncApplyStop"),
                        ("mode", "off")]))
                except pymongo.errors.OperationFailure as err:
                    self.logger.exception(
                        "Unable to re-enable oplog application on the mongod on port %d",
                        secondary.port)
                    raise errors.ServerFailure(
                        "Unable to re-enable oplog application on the mongod on port %d: %s"
                        % (secondary.port, err.args[0]))

            # Wait a little bit for the secondary to start apply oplog entries so that we are more
            # likely to kill the mongod process while it is partway into applying a batch.
            time.sleep(0.1)

            # Check that the secondary is still running before forcibly terminating it. This ensures
            # we still detect some cases in which the secondary has already crashed.
            if not secondary.is_running():
                raise errors.ServerFailure(
                    "mongod on port %d was expected to be running in"
                    " PeriodicKillSecondaries.after_test(), but wasn't."
                    % (secondary.port))

            self.hook_test_case.logger.info(
                "Killing the secondary on port %d..." % (secondary.port))
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
                    "%s did not exit cleanly after reconciling the end of its oplog" % (secondary))

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
                "%s did not exit cleanly after verifying data consistency"
                % (self.fixture))

        self.hook_test_case.logger.info("Starting the fixture back up again...")
        self.fixture.setup()
        self.fixture.await_ready()

    def _check_invariants_as_standalone(self, secondary):
        # We remove the --replSet option in order to start the node as a standalone.
        replset_name = secondary.mongod_options.pop("replSet")

        try:
            secondary.setup()
            secondary.await_ready()

            client = utils.new_mongo_client(port=secondary.port)
            minvalid_doc = client.local["replset.minvalid"].find_one()

            latest_oplog_doc = client.local["oplog.rs"].find_one(
                sort=[("$natural", pymongo.DESCENDING)])

            if minvalid_doc is not None:
                # Check the invariants 'begin <= minValid', 'minValid <= oplogDeletePoint', and
                # 'minValid <= top of oplog' before the secondary has reconciled the end of its
                # oplog.
                null_ts = bson.Timestamp(0, 0)
                begin_ts = minvalid_doc.get("begin", {}).get("ts", null_ts)
                minvalid_ts = minvalid_doc.get("ts", begin_ts)
                oplog_delete_point_ts = minvalid_doc.get("oplogDeleteFromPoint", minvalid_ts)

                if minvalid_ts == null_ts:
                    # The server treats the "ts" field in the minValid document as missing when its
                    # value is the null timestamp.
                    minvalid_ts = begin_ts

                if oplog_delete_point_ts == null_ts:
                    # The server treats the "oplogDeleteFromPoint" field as missing when its value
                    # is the null timestamp.
                    oplog_delete_point_ts = minvalid_ts

                latest_oplog_entry_ts = latest_oplog_doc.get("ts", oplog_delete_point_ts)

                if not begin_ts <= minvalid_ts:
                    raise errors.ServerFailure(
                        "The condition begin <= minValid (%s <= %s) doesn't hold: minValid"
                        " document=%s, latest oplog entry=%s"
                        % (begin_ts, minvalid_ts, minvalid_doc, latest_oplog_doc))

                if not minvalid_ts <= oplog_delete_point_ts:
                    raise errors.ServerFailure(
                        "The condition minValid <= oplogDeletePoint (%s <= %s) doesn't hold:"
                        " minValid document=%s, latest oplog entry=%s"
                        % (minvalid_ts, oplog_delete_point_ts, minvalid_doc, latest_oplog_doc))

                if not minvalid_ts <= latest_oplog_entry_ts:
                    raise errors.ServerFailure(
                        "The condition minValid <= top of oplog (%s <= %s) doesn't hold: minValid"
                        " document=%s, latest oplog entry=%s"
                        % (minvalid_ts, latest_oplog_entry_ts, minvalid_doc, latest_oplog_doc))

            teardown_success = secondary.teardown()
            if not teardown_success:
                raise errors.ServerFailure(
                    "%s did not exit cleanly after being started up as a standalone" % (secondary))
        except pymongo.errors.OperationFailure as err:
            self.hook_test_case.logger.exception(
                "Failed to read the minValid document or the latest oplog entry from the mongod on"
                " port %d",
                secondary.port)
            raise errors.ServerFailure(
                "Failed to read the minValid document or the latest oplog entry from the mongod on"
                " port %d: %s"
                % (secondary.port, err.args[0]))
        finally:
            # Set the secondary's options back to their original values.
            secondary.mongod_options["replSet"] = replset_name

    def _await_secondary_state(self, secondary):
        client = utils.new_mongo_client(port=secondary.port)
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
                "mongod on port %d failed to reach state SECONDARY after %d seconds: %s"
                % (secondary.port, fixture.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60, err.args[0]))
