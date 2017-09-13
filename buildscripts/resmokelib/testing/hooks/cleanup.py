"""
Testing hook for cleaning up data files created by the fixture.
"""

from __future__ import absolute_import

import os

from . import interface
from ..testcases import interface as testcase
from ... import errors


class CleanEveryN(interface.CustomBehavior):
    """
    Restarts the fixture after it has ran 'n' tests.
    On mongod-related fixtures, this will clear the dbpath.
    """

    DEFAULT_N = 20

    def __init__(self, hook_logger, fixture, n=DEFAULT_N):
        description = "CleanEveryN (restarts the fixture after running `n` tests)"
        interface.CustomBehavior.__init__(self, hook_logger, fixture, description)
        self.hook_test_case = testcase.TestCase(hook_logger, "Hook", "CleanEveryN")

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
        interface.CustomBehavior.start_dynamic_test(self.hook_test_case, test_report)
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
