"""Test hook for cleaning up data files created by the fixture."""

import os

from . import interface


class CleanEveryN(interface.Hook):
    """Restart the fixture after it has ran 'n' tests.

    On mongod-related fixtures, this will clear the dbpath.
    """

    DEFAULT_N = 20

    def __init__(self, hook_logger, fixture, n=DEFAULT_N):
        """Initialize CleanEveryN."""
        description = "CleanEveryN (restarts the fixture after running `n` tests)"
        interface.Hook.__init__(self, hook_logger, fixture, description)

        # Try to isolate what test triggers the leak by restarting the fixture each time.
        if "detect_leaks=1" in os.getenv("ASAN_OPTIONS", ""):
            self.logger.info(
                "ASAN_OPTIONS environment variable set to detect leaks, so restarting"
                " the fixture after each test instead of after every %d.", n)
            n = 1

        self.n = n  # pylint: disable=invalid-name
        self.tests_run = 0

    def after_test(self, test, test_report):
        """After test cleanup."""
        self.tests_run += 1
        if self.tests_run < self.n:
            return

        hook_test_case = CleanEveryNTestCase.create_after_test(self.logger.test_case_logger, test,
                                                               self)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class CleanEveryNTestCase(interface.DynamicTestCase):
    """CleanEveryNTestCase class."""

    def run_test(self):
        """Execute test hook."""
        try:
            self.logger.info("%d tests have been run against the fixture, stopping it...",
                             self._hook.tests_run)
            self._hook.tests_run = 0

            self.fixture.teardown()

            self.logger.info("Starting the fixture back up again...")
            self.fixture.setup()
            self.fixture.await_ready()
        except:
            self.logger.exception("Encountered an error while restarting the fixture.")
            raise
