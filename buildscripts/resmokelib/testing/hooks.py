"""
Customize the behavior of a fixture by allowing special code to be
executed before or after each test, and before or after each suite.
"""

from __future__ import absolute_import

import os
import sys

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
    def start_dynamic_test(test_case, test_report):
        """
        If a CustomBehavior wants to add a test case that will show up
        in the test report, it should use this method to add it to the
        report, since we will need to count it as a dynamic test to get
        the stats in the summary information right.
        """
        test_report.startTest(test_case, dynamic=True)

    def __init__(self, logger, fixture):
        """
        Initializes the CustomBehavior with the specified fixture.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        self.logger = logger
        self.fixture = fixture

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

    def before_test(self, test_report):
        """
        Each test will call this before it executes.

        Raises a TestFailure if the test should be marked as a failure,
        or a ServerFailure if the fixture exits uncleanly or
        unexpectedly.
        """
        pass

    def after_test(self, test_report):
        """
        Each test will call this after it executes.

        Raises a TestFailure if the test should be marked as a failure,
        or a ServerFailure if the fixture exits uncleanly or
        unexpectedly.
        """
        pass


class CleanEveryN(CustomBehavior):
    """
    Restarts the fixture after it has ran 'n' tests.
    On mongod-related fixtures, this will clear the dbpath.
    """

    DEFAULT_N = 20

    def __init__(self, logger, fixture, n=DEFAULT_N):
        CustomBehavior.__init__(self, logger, fixture)

        # Try to isolate what test triggers the leak by restarting the fixture each time.
        if "detect_leaks=1" in os.getenv("ASAN_OPTIONS", ""):
            self.logger.info("ASAN_OPTIONS environment variable set to detect leaks, so restarting"
                             " the fixture after each test instead of after every %d.", n)
            n = 1

        self.n = n
        self.tests_run = 0

    def after_test(self, test_report):
        self.tests_run += 1
        if self.tests_run >= self.n:
            self.logger.info("%d tests have been run against the fixture, stopping it...",
                             self.tests_run)
            self.tests_run = 0

            teardown_success = self.fixture.teardown()
            self.logger.info("Starting the fixture back up again...")
            self.fixture.setup()
            self.fixture.await_ready()

            # Raise this after calling setup in case --continueOnFailure was specified.
            if not teardown_success:
                raise errors.ServerFailure("%s did not exit cleanly" % (self.fixture))


class CheckReplDBHash(CustomBehavior):
    """
    Waits for replication after each test. Checks that the dbhashes of
    the "test" database on the primary and all of its secondaries match.

    Compatible only with ReplFixture subclasses.
    """

    def __init__(self, logger, fixture):
        if not isinstance(fixture, fixtures.ReplFixture):
            raise TypeError("%s does not support replication" % (fixture.__class__.__name__))

        CustomBehavior.__init__(self, logger, fixture)

        self.test_case = testcases.TestCase(self.logger, "Hook", "#dbhash#")

        self.failed = False
        self.started = False

    def after_test(self, test_report):
        """
        After each test, check that the dbhash of the test database is
        the same on all nodes in the replica set or master/slave
        fixture.
        """

        if self.failed:
            # Already failed, so don't check that the dbhash matches anymore.
            return

        try:
            if not self.started:
                CustomBehavior.start_dynamic_test(self.test_case, test_report)
                self.started = True

            # Wait for all operations to have replicated
            self.fixture.await_repl()

            db_name = "test"
            primary_dbhash = CheckReplDBHash._get_dbhash(self.fixture.get_primary().port, db_name)
            for secondary in self.fixture.get_secondaries():
                secondary_dbhash = CheckReplDBHash._get_dbhash(secondary.port, db_name)
                if primary_dbhash != secondary_dbhash:
                    # Adding failures to a TestReport requires traceback information, so we raise
                    # a 'self.test_case.failureException' that we will catch ourselves.
                    raise self.test_case.failureException(
                        "The primary's '%s' database does not match its secondary's '%s'"
                        " database: [ %s ] != [ %s ]"
                        % (db_name, db_name, primary_dbhash, secondary_dbhash))
        except self.test_case.failureException:
            self.test_case.logger.exception("The dbhashes did not match.")
            self.test_case.return_code = 1
            self.failed = True
            test_report.addFailure(self.test_case, sys.exc_info())
            test_report.stopTest(self.test_case)
            raise errors.TestFailure("The dbhashes did not match")

    def after_suite(self, test_report):
        """
        If we get to this point and haven't failed, the #dbhash# test
        is considered a success, so add it to the test report.
        """

        if not self.failed and self.started:
            self.test_case.logger.exception("The dbhashes matched for all tests.")
            self.test_case.return_code = 0
            test_report.addSuccess(self.test_case)
            # TestReport.stopTest() has already been called if there was a failure.
            test_report.stopTest(self.test_case)

        self.failed = False
        self.started = False

    @staticmethod
    def _get_dbhash(port, db_name):
        """
        Returns the dbhash of 'db_name'.
        """
        return utils.new_mongo_client(port=port)[db_name].command("dbHash")["md5"]


_CUSTOM_BEHAVIORS = {
    "CleanEveryN": CleanEveryN,
    "CheckReplDBHash": CheckReplDBHash,
}
