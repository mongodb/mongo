"""unittest.TestCase for multi-statement transaction passthrough tests."""

from typing import Optional

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class MultiStmtTxnTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """Test case for mutli statement transactions."""

    REGISTERED_NAME = "multi_stmt_txn_passthrough"

    def __init__(
        self,
        logger: logging.Logger,
        multi_stmt_txn_test_files: list[str],
        shell_executable: Optional[str] = None,
        shell_options: Optional[dict] = None,
    ):
        """Initilize MultiStmtTxnTestCase."""

        assert len(multi_stmt_txn_test_files) == 1
        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self,
            logger,
            "Multi-statement Transaction Passthrough",
            multi_stmt_txn_test_files[0],
            test_runner_file="jstests/libs/txns/txn_passthrough_runner.js",
            shell_executable=shell_executable,
            shell_options=shell_options,
        )

    @property
    def multi_stmt_txn_test_file(self):
        """Return the name of the test file."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["multiStmtTxnTestFile"] = self.multi_stmt_txn_test_file
        test_data["peerPids"] = self.fixture.pids()
        test_data["implicitlyShardOnCreateCollectionOnly"] = (
            "/timeseries/" in self.test_name or "\\timeseries\\" in self.test_name
        )
