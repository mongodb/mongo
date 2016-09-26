"""
Customize the behavior of a fixture by allowing special code to be
executed before or after each test, and before or after each suite.
"""

from __future__ import absolute_import

import os
import sys
import time

import bson
import pymongo
import random

from . import fixtures
from . import testcases
from .. import errors
from .. import logging
from .. import utils


def make_custom_behavior(class_name, *args, **kwargs):
    """
    Factory function for creating CustomBehavior instances.
    """

    if class_name not in _CUSTOM_BEHAVIORS:
        raise ValueError("Unknown custom behavior class '%s'" % (class_name))
    return _CUSTOM_BEHAVIORS[class_name](*args, **kwargs)


class CustomBehavior(object):
    """
    The common interface all CustomBehaviors will inherit from.
    """

    @staticmethod
    def start_dynamic_test(hook_test_case, test_report):
        """
        If a CustomBehavior wants to add a test case that will show up
        in the test report, it should use this method to add it to the
        report, since we will need to count it as a dynamic test to get
        the stats in the summary information right.
        """
        test_report.startTest(hook_test_case, dynamic=True)

    def __init__(self, logger, fixture, description):
        """
        Initializes the CustomBehavior with the specified fixture.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        self.logger = logger
        self.fixture = fixture
        self.hook_test_case = None
        self.logger_name = self.__class__.__name__
        self.description = description

    def before_suite(self, test_report):
        """
        The test runner calls this exactly once before they start
        running the suite.
        """
        pass

    def after_suite(self, test_report):
        """
        The test runner calls this exactly once after all tests have
        finished executing. Be sure to reset the behavior back to its
        original state so that it can be run again.
        """
        pass

    def before_test(self, test, test_report):
        """
        Each test will call this before it executes.
        """
        pass

    def after_test(self, test, test_report):
        """
        Each test will call this after it executes.
        """
        pass


class CleanEveryN(CustomBehavior):
    """
    Restarts the fixture after it has ran 'n' tests.
    On mongod-related fixtures, this will clear the dbpath.
    """

    DEFAULT_N = 20

    def __init__(self, logger, fixture, n=DEFAULT_N):
        description = "CleanEveryN (restarts the fixture after running `n` tests)"
        CustomBehavior.__init__(self, logger, fixture, description)
        self.hook_test_case = testcases.TestCase(logger, "Hook", "CleanEveryN")

        # Try to isolate what test triggers the leak by restarting the fixture each time.
        if "detect_leaks=1" in os.getenv("ASAN_OPTIONS", ""):
            self.logger.info("ASAN_OPTIONS environment variable set to detect leaks, so restarting"
                             " the fixture after each test instead of after every %d.", n)
            n = 1

        self.n = n
        self.tests_run = 0

    def after_test(self, test, test_report):
        self.tests_run += 1
        if self.tests_run < self.n:
            return

        self.hook_test_case.test_name = test.short_name() + ":" + self.logger_name
        CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)
        try:
            self.logger.info("%d tests have been run against the fixture, stopping it...",
                             self.tests_run)
            self.tests_run = 0

            if not self.fixture.teardown():
                raise errors.ServerFailure("%s did not exit cleanly" % (self.fixture))

            self.logger.info("Starting the fixture back up again...")
            self.fixture.setup()
            self.fixture.await_ready()

            self.hook_test_case.return_code = 0
            test_report.addSuccess(self.hook_test_case)
        finally:
            test_report.stopTest(self.hook_test_case)


class JsCustomBehavior(CustomBehavior):
    def __init__(self, logger, fixture, js_filename, description, shell_options=None):
        CustomBehavior.__init__(self, logger, fixture, description)
        self.hook_test_case = testcases.JSTestCase(logger,
                                                   js_filename,
                                                   shell_options=shell_options,
                                                   test_kind="Hook")
        self.test_case_is_configured = False

    def before_suite(self, test_report):
        if not self.test_case_is_configured:
            # Configure the test case after the fixture has been set up.
            self.hook_test_case.configure(self.fixture)
            self.test_case_is_configured = True

    def _should_run_after_test_impl(self):
        return True

    def _after_test_impl(self, test, test_report, description):
        self.hook_test_case.run_test()

    def after_test(self, test, test_report):
        if not self._should_run_after_test_impl():
            return

        # Change test_name and description to be more descriptive.
        description = "{0} after running '{1}'".format(self.description, test.short_name())
        self.hook_test_case.test_name = test.short_name() + ":" + self.logger_name
        CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)

        try:
            self._after_test_impl(test, test_report, description)
        except pymongo.errors.OperationFailure as err:
            self.hook_test_case.logger.exception("{0} failed".format(description))
            self.hook_test_case.return_code = 1
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.StopExecution(err.args[0])
        except self.hook_test_case.failureException as err:
            self.hook_test_case.logger.exception("{0} failed".format(description))
            test_report.addFailure(self.hook_test_case, sys.exc_info())
            raise errors.StopExecution(err.args[0])
        else:
            self.hook_test_case.return_code = 0
            test_report.addSuccess(self.hook_test_case)
        finally:
            test_report.stopTest(self.hook_test_case)


class BackgroundInitialSync(JsCustomBehavior):
    """
    After every test, this hook checks if a background node has finished initial sync and if so,
    validates it, tears it down, and restarts it.

    This test accepts a parameter 'n' that specifies a number of tests after which it will wait for
    replication to finish before validating and restarting the initial sync node. It also accepts
    a parameter 'use_resync' for whether to restart the initial sync node with resync or by
    shutting it down and restarting it.

    This requires the ReplicaSetFixture to be started with 'start_initial_sync_node=True'. If used
    at the same time as CleanEveryN, the 'n' value passed to this hook should be equal to the 'n'
    value for CleanEveryN.
    """

    DEFAULT_N = CleanEveryN.DEFAULT_N

    def __init__(self, logger, fixture, use_resync=False, n=DEFAULT_N, shell_options=None):
        description = "Background Initial Sync"
        js_filename = os.path.join("jstests", "hooks", "run_initial_sync_node_validation.js")
        JsCustomBehavior.__init__(self, logger, fixture, js_filename, description, shell_options)

        self.use_resync = use_resync
        self.n = n
        self.tests_run = 0
        self.random_restarts = 0

    # Restarts initial sync by shutting down the node, clearing its data, and restarting it,
    # or by calling resync if use_resync is specified.
    def __restart_init_sync(self, test_report, sync_node, sync_node_conn):
        if self.use_resync:
            self.hook_test_case.logger.info("Calling resync on initial sync node...")
            cmd = bson.SON([("resync", 1), ("wait", 0)])
            sync_node_conn.admin.command(cmd)
        else:
            # Tear down and restart the initial sync node to start initial sync again.
            if not sync_node.teardown():
                raise errors.ServerFailure("%s did not exit cleanly" % (sync_node))

            self.hook_test_case.logger.info("Starting the initial sync node back up again...")
            sync_node.setup()
            sync_node.await_ready()

    def _after_test_impl(self, test, test_report, description):
        self.tests_run += 1
        sync_node = self.fixture.get_initial_sync_node()
        sync_node_conn = utils.new_mongo_client(port=sync_node.port)

        # If it's been 'n' tests so far, wait for the initial sync node to finish syncing.
        if self.tests_run >= self.n:
            self.hook_test_case.logger.info(
                "%d tests have been run against the fixture, waiting for initial sync"
                " node to go into SECONDARY state",
                self.tests_run)
            self.tests_run = 0

            cmd = bson.SON([("replSetTest", 1),
                            ("waitForMemberState", 2),
                            ("timeoutMillis", 20 * 60 * 1000)])
            sync_node_conn.admin.command(cmd)

        # Check if the initial sync node is in SECONDARY state. If it's been 'n' tests, then it
        # should have waited to be in SECONDARY state and the test should be marked as a failure.
        # Otherwise, we just skip the hook and will check again after the next test.
        try:
            state = sync_node_conn.admin.command("replSetGetStatus").get("myState")
            if state != 2:
                if self.tests_run == 0:
                    msg = "Initial sync node did not catch up after waiting 20 minutes"
                    self.hook_test_case.logger.exception("{0} failed: {1}".format(description, msg))
                    raise errors.TestFailure(msg)

                self.hook_test_case.logger.info(
                    "Initial sync node is in state %d, not state SECONDARY (2)."
                    " Skipping BackgroundInitialSync hook for %s",
                    state,
                    test.short_name())

                # If we have not restarted initial sync since the last time we ran the data
                # validation, restart initial sync with a 20% probability.
                if self.random_restarts < 1 and random.random() < 0.2:
                    hook_type = "resync" if self.use_resync else "initial sync"
                    self.hook_test_case.logger.info("randomly restarting " + hook_type +
                                             " in the middle of " + hook_type)
                    self.__restart_init_sync(test_report, sync_node, sync_node_conn)
                    self.random_restarts += 1
                return
        except pymongo.errors.OperationFailure:
            # replSetGetStatus can fail if the node is in STARTUP state. The node will soon go into
            # STARTUP2 state and replSetGetStatus will succeed after the next test.
            self.hook_test_case.logger.info(
                "replSetGetStatus call failed in BackgroundInitialSync hook, skipping hook for %s",
                test.short_name())
            return

        self.random_restarts = 0

        # Run data validation and dbhash checking.
        self.hook_test_case.run_test()

        self.__restart_init_sync(test_report, sync_node, sync_node_conn)


class IntermediateInitialSync(JsCustomBehavior):
    """
    This hook accepts a parameter 'n' that specifies a number of tests after which it will start up
    a node to initial sync, wait for replication to finish, and then validate the data. It also
    accepts a parameter 'use_resync' for whether to restart the initial sync node with resync or by
    shutting it down and restarting it.

    This requires the ReplicaSetFixture to be started with 'start_initial_sync_node=True'.
    """

    DEFAULT_N = CleanEveryN.DEFAULT_N

    def __init__(self, logger, fixture, use_resync=False, n=DEFAULT_N):
        description = "Intermediate Initial Sync"
        js_filename = os.path.join("jstests", "hooks", "run_initial_sync_node_validation.js")
        JsCustomBehavior.__init__(self, logger, fixture, js_filename, description)

        self.use_resync = use_resync
        self.n = n
        self.tests_run = 0

    def _should_run_after_test_impl(self):
        self.tests_run += 1

        # If we have not run 'n' tests yet, skip this hook.
        if self.tests_run < self.n:
            return False

        self.tests_run = 0
        return True

    def _after_test_impl(self, test, test_report, description):
        sync_node = self.fixture.get_initial_sync_node()
        sync_node_conn = utils.new_mongo_client(port=sync_node.port)

        if self.use_resync:
            self.hook_test_case.logger.info("Calling resync on initial sync node...")
            cmd = bson.SON([("resync", 1)])
            sync_node_conn.admin.command(cmd)
        else:
            if not sync_node.teardown():
                raise errors.ServerFailure("%s did not exit cleanly" % (sync_node))

            self.hook_test_case.logger.info("Starting the initial sync node back up again...")
            sync_node.setup()
            sync_node.await_ready()

        # Do initial sync round.
        self.hook_test_case.logger.info("Waiting for initial sync node to go into SECONDARY state")
        cmd = bson.SON([("replSetTest", 1),
                        ("waitForMemberState", 2),
                        ("timeoutMillis", 20 * 60 * 1000)])
        sync_node_conn.admin.command(cmd)

        # Run data validation and dbhash checking.
        self.hook_test_case.run_test()



class ValidateCollections(JsCustomBehavior):
    """
    Runs full validation on all collections in all databases on every stand-alone
    node, primary replica-set node, or primary shard node.
    """
    def __init__(self, logger, fixture, shell_options=None):
        description = "Full collection validation"
        js_filename = os.path.join("jstests", "hooks", "run_validate_collections.js")
        JsCustomBehavior.__init__(self,
                                  logger,
                                  fixture,
                                  js_filename,
                                  description,
                                  shell_options=shell_options)


class CheckReplDBHash(JsCustomBehavior):
    """
    Checks that the dbhashes of all non-local databases and non-replicated system collections
    match on the primary and secondaries.
    """
    def __init__(self, logger, fixture, shell_options=None):
        description = "Check dbhashes of all replica set or master/slave members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_dbhash.js")
        JsCustomBehavior.__init__(self,
                                  logger,
                                  fixture,
                                  js_filename,
                                  description,
                                  shell_options=shell_options)


class CheckReplOplogs(JsCustomBehavior):
    """
    Checks that local.oplog.rs matches on the primary and secondaries.
    """
    def __init__(self, logger, fixture, shell_options=None):
        description = "Check oplogs of all replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_oplogs.js")
        JsCustomBehavior.__init__(self,
                                  logger,
                                  fixture,
                                  js_filename,
                                  description,
                                  shell_options=shell_options)


class PeriodicKillSecondaries(CustomBehavior):
    """
    Periodically kills the secondaries in a replica set and verifies
    that they can reach the SECONDARY state without having connectivity
    to the primary after an unclean shutdown.
    """

    DEFAULT_PERIOD_SECS = 30

    def __init__(self, logger, fixture, period_secs=DEFAULT_PERIOD_SECS):
        if not isinstance(fixture, fixtures.ReplicaSetFixture):
            raise TypeError("%s either does not support replication or does not support writing to"
                            " its oplog early"
                            % (fixture.__class__.__name__))

        if fixture.num_nodes <= 1:
            raise ValueError("PeriodicKillSecondaries requires the replica set to contain at least"
                             " one secondary")

        description = ("PeriodicKillSecondaries (kills the secondary after running tests for a"
                       " configurable period of time)")
        CustomBehavior.__init__(self, logger, fixture, description)

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
        self.hook_test_case = testcases.TestCase(
            self.logger,
            "Hook",
            "%s:%s" % (self._last_test_name, self.logger_name))
        CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)

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
        validate_test_case = ValidateCollections(self.logger, self.fixture)
        validate_test_case.before_suite(test_report)
        validate_test_case.before_test(self.hook_test_case, test_report)
        validate_test_case.after_test(self.hook_test_case, test_report)
        validate_test_case.after_suite(test_report)

    def _check_repl_dbhash(self, test_report):
        dbhash_test_case = CheckReplDBHash(self.logger, self.fixture)
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
                ("timeoutMillis", fixtures.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60 * 1000)]))
        except pymongo.errors.OperationFailure as err:
            self.hook_test_case.logger.exception(
                "mongod on port %d failed to reach state SECONDARY after %d seconds",
                secondary.port,
                fixtures.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60)
            raise errors.ServerFailure(
                "mongod on port %d failed to reach state SECONDARY after %d seconds: %s"
                % (secondary.port, fixtures.ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60, err.args[0]))


_CUSTOM_BEHAVIORS = {
    "CleanEveryN": CleanEveryN,
    "CheckReplDBHash": CheckReplDBHash,
    "CheckReplOplogs": CheckReplOplogs,
    "ValidateCollections": ValidateCollections,
    "IntermediateInitialSync": IntermediateInitialSync,
    "BackgroundInitialSync": BackgroundInitialSync,
    "PeriodicKillSecondaries": PeriodicKillSecondaries,
}
