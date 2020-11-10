"""unittest.TestCase for multi-statement transaction passthrough tests."""

from __future__ import absolute_import

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class MultiStmtTxnTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """Test case for mutli statement transactions."""

    REGISTERED_NAME = "multi_stmt_txn_passthrough"

    def __init__(self, logger, multi_stmt_txn_test_file, shell_executable=None, shell_options=None):
        """Initilize MultiStmtTxnTestCase."""
        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self, logger, "Multi-statement Transaction Passthrough", multi_stmt_txn_test_file,
            test_runner_file="jstests/libs/txns/txn_passthrough_runner.js",
            shell_executable=shell_executable, shell_options=shell_options)

    @property
    def multi_stmt_txn_test_file(self):
        """Return the name of the test file."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["multiStmtTxnTestFile"] = self.multi_stmt_txn_test_file


class AbortMultiStmtTxnTestCase(MultiStmtTxnTestCase):
    """Test case for aborting multi statement transactions."""

    REGISTERED_NAME = "abort_multi_stmt_txn_passthrough"

    def __init__(self, logger, multi_stmt_txn_test_file, shell_executable=None, shell_options=None):
        """Initialize AbortMultiStmtTxnTestCase to be used to test transaction expiration logic."""
        # pylint: disable=non-parent-init-called,super-init-not-called
        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self, logger, "Abort Multi-statement Transaction Passthrough", multi_stmt_txn_test_file,
            test_runner_file="jstests/libs/txns/txn_passthrough_runner.js",
            shell_executable=shell_executable, shell_options=shell_options)

    def _execute(self, process):
        """Run the specified process."""
        self.logger.info("Starting %s...\n%s", self.short_description(), process.as_command())

        process.start()
        self.logger.info("%s started with pid %s.", self.short_description(), process.pid)

        self.return_code = process.wait()
        # This test case is intended to randomly abort transactions in the core passthrough. We only
        # expect to return a failure when the system crashes. This is different from the base
        # implementation where we will throw in a non-zero return code.
        if self.return_code != 0:
            self.logger.info("Returning quietly instead of throwing failure: %s" %
                             (self.short_description()))

        self.logger.info("%s finished.", self.short_description())
