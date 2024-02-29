"""Test hook for periodically running a magic restore process and validating cluster consistency."""

import os

from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import jsfile


class MagicRestoreEveryN(interface.Hook):
    """Open a backup cursor and run magic restore process after 'n' tests have run.

    Requires the use of MagicRestoreFixture.
    """

    IS_BACKGROUND = False

    DEFAULT_N = 20

    def __init__(self, hook_logger, fixture, n=DEFAULT_N):
        """Initialize MagicRestoreEveryN."""
        description = "MagicRestoreEveryN (runs magic restore against a new cluster every `n` tests)"
        interface.Hook.__init__(self, hook_logger, fixture, description)

        self.n = n  # pylint: disable=invalid-name
        self.tests_run = 0

    def after_test(self, test, test_report):
        """After test cleanup."""
        self.tests_run += 1
        if self.tests_run < self.n:
            return

        # The process of doing magic restore requires the following steps

        # Collect backup cursor
        hook_test_case = BackupCursorTestCase.create_after_test(test.logger, test, self)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)

        # Run the magic restore procedure and run a data consistency check
        # TODO SERVER-87172: Make a new test case to run a different js file.


class BackupCursorTestCase(jsfile.DynamicJSTestCase):
    """BackupCursorTestCase class."""

    JS_FILENAME = os.path.join("jstests", "hooks", "magic_restore_backup.js")

    def __init__(self, logger, test_name, description, base_test_name, hook):
        """Initialize BackupCursorTestCase."""
        jsfile.DynamicJSTestCase.__init__(self, logger, test_name, description, base_test_name,
                                          hook, self.JS_FILENAME)

    def run_test(self):
        """Execute test hook."""
        self.logger.info("Beginning backup cursor capture")
        self._js_test_case.run_test()
        self.logger.info("Backup cursor capture complete")
