"""The unittest.TestCase for QueryTester server correctness tests."""

import os
import time

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core, logging
from buildscripts.resmokelib.testing.testcases import interface


def wait_for_file(file_path, sleep_interval=2):
    """Wait for `file_path` to be present on the filesystem. Sleep for
    `sleep_interval` between each existence check and fail if we have
    waited longer than `timeout` seconds."""
    # Returns time in seconds
    while True:
        if os.path.exists(file_path):
            return
        time.sleep(sleep_interval)


class QueryTesterServerTestCase(interface.ProcessTestCase):
    """A QueryTester server test to execute."""

    REGISTERED_NAME = "query_tester_server_test"

    def __init__(self, logger: logging.Logger, test_dir: list[str]):
        """Initialize QueryTesterServerTestCase.
        test_dir must contain one dir that contains .test files, their corresponding .results and a .coll file
        To run multiple test dirs, you would create an instance of QueryTesterServerTestCase for each one.
        """
        assert len(test_dir) == 1
        interface.ProcessTestCase.__init__(self, logger, "QueryTesterServerTest", test_dir[0])

        self.test_dir = test_dir[0]

    def _make_process(self):
        # Ensure the test files are all available. See
        # query_tester_test_sparse_checkout.sh for how this process
        # works. This must be done outside of the constructor to allow
        # resmoke to proceed with initializing the complete set of
        # tests before beginning execution.
        wait_for_file(os.path.join(self.test_dir, ".sparse-checkout-done"))

        test_files = [
            os.path.join(self.test_dir, f) for f in os.listdir(self.test_dir) if f.endswith(".test")
        ]

        return core.programs.generic_program(
            self.logger,
            [
                _config.DEFAULT_MONGOTEST_EXECUTABLE,
                "--uri",
                self.fixture.get_internal_connection_string(),
                *[cmd for f in test_files for cmd in ("-t", f)],
                "--drop",
                "--load",
                "--mode",
                "compare",
                "-v",
                "--diff",
                "plain",
            ],
        )
