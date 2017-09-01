"""
Testing hook for verifying correctness of initial sync.
"""

from __future__ import absolute_import

import os.path
import random

import bson
import pymongo
import pymongo.errors

from . import cleanup
from . import jsfile
from ..fixtures import replicaset
from ... import errors
from ... import utils


class BackgroundInitialSync(jsfile.JsCustomBehavior):
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

    DEFAULT_N = cleanup.CleanEveryN.DEFAULT_N

    def __init__(self, hook_logger, fixture, use_resync=False, n=DEFAULT_N, shell_options=None):
        if not isinstance(fixture, replicaset.ReplicaSetFixture):
            raise ValueError("`fixture` must be an instance of ReplicaSetFixture, not {}".format(
                fixture.__class__.__name__))

        description = "Background Initial Sync"
        js_filename = os.path.join("jstests", "hooks", "run_initial_sync_node_validation.js")
        jsfile.JsCustomBehavior.__init__(self, hook_logger, fixture, js_filename,
                                         description, shell_options)

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
        sync_node_conn = sync_node.mongo_client()

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


class IntermediateInitialSync(jsfile.JsCustomBehavior):
    """
    This hook accepts a parameter 'n' that specifies a number of tests after which it will start up
    a node to initial sync, wait for replication to finish, and then validate the data. It also
    accepts a parameter 'use_resync' for whether to restart the initial sync node with resync or by
    shutting it down and restarting it.

    This requires the ReplicaSetFixture to be started with 'start_initial_sync_node=True'.
    """

    DEFAULT_N = cleanup.CleanEveryN.DEFAULT_N

    def __init__(self, hook_logger, fixture, use_resync=False, n=DEFAULT_N):
        if not isinstance(fixture, replicaset.ReplicaSetFixture):
            raise ValueError("`fixture` must be an instance of ReplicaSetFixture, not {}".format(
                fixture.__class__.__name__))

        description = "Intermediate Initial Sync"
        js_filename = os.path.join("jstests", "hooks", "run_initial_sync_node_validation.js")
        jsfile.JsCustomBehavior.__init__(self, hook_logger, fixture, js_filename, description)

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
        sync_node_conn = sync_node.mongo_client()

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
