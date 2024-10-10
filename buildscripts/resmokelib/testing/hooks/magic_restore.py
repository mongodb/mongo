"""Test hook for periodically running a magic restore process and validating cluster consistency."""

import os
import random

from buildscripts.resmokelib.testing.hooks import interface, jsfile


class MagicRestoreEveryN(interface.Hook):
    """Open a backup cursor and run magic restore process after 'n' tests have run.

    Requires the use of MagicRestoreFixture.
    """

    IS_BACKGROUND = False

    DEFAULT_N = 20

    def __init__(self, hook_logger, fixture, n=DEFAULT_N, randomize_pit=False):
        """Initialize MagicRestoreEveryN."""
        description = (
            "MagicRestoreEveryN (runs magic restore against a new cluster every `n` tests)"
        )
        interface.Hook.__init__(self, hook_logger, fixture, description)

        self.n = n  # pylint: disable=invalid-name
        self.tests_run = 0

        self.pit = False

        if randomize_pit:
            self.pit = random.choice([True, False])

        self.logger.info("Magic Restore: PIT Restore: " + str(self.pit))

    def should_run_backup_or_restore(self):
        # For point in time restores we want to take the backup at n/2 tests and do the restore at n tests.
        if self.pit:
            return (self.tests_run == self.n / 2, self.tests_run == self.n)
        # For non point in time restores we want to do the backup and restore after the same test.
        else:
            return (self.tests_run < self.n, self.tests_run < self.n)

    def after_test(self, test, test_report):
        """After test cleanup."""
        self.tests_run += 1
        run_backup, run_restore = self.should_run_backup_or_restore()

        if (not run_backup) and (not run_restore):
            return

        # The process of doing magic restore requires the following steps

        if run_backup:
            # Collect data files from backup cursor
            hook_test_case = BackupCursorTestCase.create_after_test(test.logger, test, self)
            hook_test_case.configure(self.fixture)
            hook_test_case.run_dynamic_test(test_report)

        if run_restore:
            # Run the magic restore procedure and run a data consistency check
            magic_restore_test_case = MagicRestoreTestCase.create_after_test(
                test.logger, test, self
            )
            magic_restore_test_case.configure(self.fixture)
            magic_restore_test_case.run_dynamic_test(test_report)

            self.logger.info("Tearing the fixture down to clear its data.")
            self.fixture.teardown()

            self.logger.info("Starting the fixture back up again.")
            self.fixture.setup()
            self.fixture.await_ready()

            # Reset tests_run
            self.tests_run = 0


class BackupCursorTestCase(jsfile.DynamicJSTestCase):
    """BackupCursorTestCase class."""

    JS_FILENAME = os.path.join("jstests", "hooks", "magic_restore_backup.js")

    def __init__(self, logger, test_name, description, base_test_name, hook):
        """Initialize BackupCursorTestCase."""
        jsfile.DynamicJSTestCase.__init__(
            self, logger, test_name, description, base_test_name, hook, self.JS_FILENAME
        )

    def run_test(self):
        """Execute test hook."""
        self.logger.info("Beginning backup cursor capture")
        self._js_test_case.run_test()
        self.logger.info("Backup cursor capture complete")


class MagicRestoreTestCase(jsfile.DynamicJSTestCase):
    """MagicRestoreTestCase class."""

    JS_FILENAME = os.path.join("jstests", "hooks", "magic_restore.js")

    def __init__(self, logger, test_name, description, base_test_name, hook):
        """Initialize MagicRestoreTestCase."""
        jsfile.DynamicJSTestCase.__init__(
            self, logger, test_name, description, base_test_name, hook, self.JS_FILENAME
        )

    def run_test(self):
        """Execute test hook."""
        self.logger.info("Beginning magic restore")
        self._js_test_case.run_test()
        self.logger.info("Magic restore complete")
